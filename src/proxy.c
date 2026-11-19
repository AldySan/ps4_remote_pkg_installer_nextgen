/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "proxy.h"
#include "http_client.h"
#include "log.h"
#include "prefetch.h"
#include "taskman.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROXY_MAX_JOBS     32
#define PROXY_URL_MAX      2048  /* must hold long signed piece URLs */

#define PROXY_BIG_PIECE    (8u * 1024u * 1024u)   /* switch to parallel above */
#define PROXY_CONN_SMALL   1
#define PROXY_CONN_BIG     PREFETCH_DEFAULT_CONNECTIONS

struct proxy_piece {
    char url[PROXY_URL_MAX];
    uint64_t size;
    bool ranges_ok;
};

struct proxy_stream {
    bool direct;
    prefetch_t* pf;
    http_stream_t* hs;
    uint64_t size;
    uint64_t pos;
    bool aborted;
};

struct proxy_job {
    bool used;
    int job_id;
    size_t count;
    struct proxy_piece* pieces;
    /* Reference to the stream currently serving this job (NOT owned by
     * the registry): the serving connection owns the stream and frees it
     * via proxy_stream_close. Kept only so a piece change or job teardown
     * can abort it; always cleared under s_lock before it is freed. */
    struct proxy_stream* stream;
    int stream_piece;
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static struct proxy_job s_jobs[PROXY_MAX_JOBS];

static struct proxy_job* find_job(int job_id)
{
    int i;

    for (i = 0; i < PROXY_MAX_JOBS; ++i) {
        if (s_jobs[i].used && s_jobs[i].job_id == job_id) {
            return &s_jobs[i];
        }
    }
    return NULL;
}

/* Caller must hold s_lock (the stream is aborted, never freed here). */
static void stream_abort(struct proxy_stream* st)
{
    if (!st) {
        return;
    }
    if (st->direct) {
        /* Written under s_lock, polled lock-free by the reader: atomic
         * so the reader sees the flag without a data race. */
        __atomic_store_n(&st->aborted, true, __ATOMIC_RELEASE);
    } else {
        prefetch_abort(st->pf);
    }
}

static void stream_destroy(struct proxy_stream* st)
{
    if (!st) {
        return;
    }
    if (st->direct) {
        http_stream_close(st->hs);
    } else {
        prefetch_free(st->pf);
    }
    free(st);
}

bool proxy_register(int job_id, const char* const* urls,
                    const uint64_t* sizes, const bool* ranges_ok,
                    size_t count)
{
    struct proxy_job* job = NULL;
    int i;

    if (!urls || !sizes || count == 0) {
        return false;
    }

    pthread_mutex_lock(&s_lock);
    if (find_job(job_id)) {
        pthread_mutex_unlock(&s_lock);
        return false; /* already registered */
    }
    for (i = 0; i < PROXY_MAX_JOBS; ++i) {
        if (!s_jobs[i].used) {
            job = &s_jobs[i];
            break;
        }
    }
    if (!job) {
        pthread_mutex_unlock(&s_lock);
        LOG_WARN("proxy: registry full");
        return false;
    }

    memset(job, 0, sizeof(*job));
    job->pieces = (struct proxy_piece*)calloc(count, sizeof(*job->pieces));
    if (!job->pieces) {
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    for (i = 0; i < (int)count; ++i) {
        if (!urls[i]) {
            free(job->pieces);
            job->pieces = NULL;
            pthread_mutex_unlock(&s_lock);
            return false;
        }
        snprintf(job->pieces[i].url, sizeof(job->pieces[i].url), "%s", urls[i]);
        job->pieces[i].size = sizes[i];
        job->pieces[i].ranges_ok = ranges_ok ? ranges_ok[i] : false;
    }
    job->used = true;
    job->job_id = job_id;
    job->count = count;
    job->stream_piece = -1;
    pthread_mutex_unlock(&s_lock);
    return true;
}

void proxy_unregister(int job_id)
{
    struct proxy_job* job;

    pthread_mutex_lock(&s_lock);
    job = find_job(job_id);
    if (job) {
        if (job->stream) {
            /* Abort the live stream so its serving connection unwinds and
             * frees it (it owns the stream). Never free it here: the poll
             * thread calls this, and prefetch_free joins the workers. */
            stream_abort(job->stream);
            job->stream = NULL;
        }
        job->stream_piece = -1;
        free(job->pieces);
        job->pieces = NULL;
        job->used = false;
        job->job_id = 0;
    }
    pthread_mutex_unlock(&s_lock);
}

void* proxy_stream_open(int job_id, size_t piece, uint64_t* total_size)
{
    char url[PROXY_URL_MAX];
    uint64_t size = 0;
    bool ranges_ok = false;
    struct proxy_stream* st;
    struct proxy_job* job;

    url[0] = '\0';

    pthread_mutex_lock(&s_lock);
    job = find_job(job_id);
    if (job && piece < job->count) {
        snprintf(url, sizeof(url), "%s", job->pieces[piece].url);
        size = job->pieces[piece].size;
        ranges_ok = job->pieces[piece].ranges_ok;
        if (job->stream && job->stream_piece != (int)piece) {
            /* BGFT moved on: abort the previous stream (aborted reads
             * fail immediately, so its connection unwinds and frees it).
             * Aborting under the lock orders this against the owner's
             * proxy_stream_close, which clears the ref before freeing. */
            stream_abort(job->stream);
            job->stream = NULL;
            job->stream_piece = -1;
        }
    }
    pthread_mutex_unlock(&s_lock);

    if (size == 0 || url[0] == '\0') {
        return NULL;
    }

    st = (struct proxy_stream*)calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }
    st->size = size;

    /* The prefetch engine only works against servers that honour Range
     * requests (every worker fetch is a range GET). No-Range origins go
     * straight to the sequential stream, and so does any piece whose
     * parallel engine could not start: a valid piece must never 404 just
     * because memory is tight, or BGFT fails the whole task. */
    if (ranges_ok) {
        int conns = PROXY_CONN_SMALL;

        if (size >= PROXY_BIG_PIECE) {
            conns = PROXY_CONN_BIG;
        }
        st->pf = prefetch_start(url, size, conns);
    }
    st->direct = (st->pf == NULL);
    if (st->direct) {
        st->hs = http_stream_open(url);
        if (!st->hs) {
            free(st);
            return NULL;
        }
    }

    pthread_mutex_lock(&s_lock);
    job = find_job(job_id);
    if (!job) {
        pthread_mutex_unlock(&s_lock);
        stream_destroy(st);
        return NULL;
    }
    if (!job->stream) {
        job->stream = st;
        job->stream_piece = (int)piece;
    }
    /* else: another stream for the same piece is already registered; this
     * one is detached and freed by its own connection via
     * proxy_stream_close. */
    pthread_mutex_unlock(&s_lock);

    if (total_size) {
        *total_size = size;
    }
    return st;
}

int64_t proxy_stream_read(void* stream, uint8_t* buf, size_t len)
{
    struct proxy_stream* st = (struct proxy_stream*)stream;
    int64_t n;

    if (!st || !buf || len == 0) {
        return -1;
    }
    if (!st->direct) {
        return prefetch_read(st->pf, buf, len);
    }
    if (__atomic_load_n(&st->aborted, __ATOMIC_ACQUIRE)) {
        return -1;
    }
    if (st->pos >= st->size) {
        return 0;
    }
    n = http_stream_read(st->hs, buf, len);
    if (n < 0) {
        return -1;
    }
    if (n == 0) {
        /* The response headers promised `size` bytes: EOF before that is
         * a short body and must surface as an error, not a clean end. */
        return (st->pos < st->size) ? -1 : 0;
    }
    st->pos += (uint64_t)n;
    return n;
}

void proxy_stream_close(int job_id, void* stream)
{
    struct proxy_stream* st = (struct proxy_stream*)stream;
    struct proxy_job* job;

    if (!st) {
        return;
    }
    /* Drop the registry's reference first (under the lock) so nothing can
     * abort or double-free the stream we are about to own-free. */
    pthread_mutex_lock(&s_lock);
    job = find_job(job_id);
    if (job && job->stream == st) {
        job->stream = NULL;
        job->stream_piece = -1;
    }
    pthread_mutex_unlock(&s_lock);
    stream_destroy(st);
}
