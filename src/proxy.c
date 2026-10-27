/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "proxy.h"
#include "log.h"
#include "prefetch.h"
#include "taskman.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROXY_MAX_JOBS     32
#define PROXY_URL_MAX      512

#define PROXY_BIG_PIECE    (8u * 1024u * 1024u)   /* switch to parallel above */
#define PROXY_CONN_SMALL   1
#define PROXY_CONN_BIG     PREFETCH_DEFAULT_CONNECTIONS

struct proxy_piece {
    char url[PROXY_URL_MAX];
    uint64_t size;
    bool ranges_ok;
};

struct proxy_job {
    bool used;
    int job_id;
    size_t count;
    struct proxy_piece* pieces;
    /* Active stream (one per job; BGFT reads pieces sequentially). */
    bool stream_open;
    int stream_piece;
    prefetch_t* stream_pf;
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
        if (job->stream_pf) {
            prefetch_t* pf = job->stream_pf;
            job->stream_pf = NULL;
            job->stream_open = false;
            job->stream_piece = -1;
            /* prefetch_free joins workers; call without the lock. */
            pthread_mutex_unlock(&s_lock);
            prefetch_free(pf);
            pthread_mutex_lock(&s_lock);
        }
        free(job->pieces);
        job->pieces = NULL;
        job->used = false;
        job->job_id = 0;
    }
    pthread_mutex_unlock(&s_lock);
}

bool proxy_get_piece(int job_id, size_t piece, char* url_out, size_t url_sz,
                     uint64_t* size_out)
{
    struct proxy_job* job;
    bool ok = false;

    if (!url_out || url_sz == 0) {
        return false;
    }
    url_out[0] = '\0';

    pthread_mutex_lock(&s_lock);
    job = find_job(job_id);
    if (job && piece < job->count) {
        snprintf(url_out, url_sz, "%s", job->pieces[piece].url);
        if (size_out) {
            *size_out = job->pieces[piece].size;
        }
        ok = true;
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

void* proxy_stream_open(int job_id, size_t piece, uint64_t* total_size)
{
    char url[PROXY_URL_MAX];
    uint64_t size = 0;
    bool ranges_ok = false;
    prefetch_t* old_pf = NULL;
    prefetch_t* pf;
    struct proxy_job* job;
    int conns = PROXY_CONN_SMALL;

    url[0] = '\0';

    pthread_mutex_lock(&s_lock);
    job = find_job(job_id);
    if (job && piece < job->count) {
        snprintf(url, sizeof(url), "%s", job->pieces[piece].url);
        size = job->pieces[piece].size;
        ranges_ok = job->pieces[piece].ranges_ok;
        if (job->stream_pf && job->stream_piece != (int)piece) {
            /* Retire the previous stream (BGFT moved on). */
            old_pf = job->stream_pf;
            job->stream_pf = NULL;
            job->stream_open = false;
            job->stream_piece = -1;
        }
    }
    pthread_mutex_unlock(&s_lock);

    if (old_pf) {
        prefetch_abort(old_pf);
        prefetch_free(old_pf);
    }
    if (size == 0 || url[0] == '\0') {
        return NULL;
    }

    /* Adaptive: parallel segments only when the server supports Range
     * requests and the piece is big enough to amortize the connections. */
    if (ranges_ok && size >= PROXY_BIG_PIECE) {
        conns = PROXY_CONN_BIG;
    }

    pf = prefetch_start(url, size, conns);
    if (!pf) {
        LOG_ERROR("proxy: prefetch_start failed for job %d piece %zu",
                  job_id, piece);
        return NULL;
    }

    pthread_mutex_lock(&s_lock);
    job = find_job(job_id);
    if (!job) {
        pthread_mutex_unlock(&s_lock);
        prefetch_free(pf);
        return NULL;
    }
    if (!job->stream_pf) {
        job->stream_pf = pf;
        job->stream_open = true;
        job->stream_piece = (int)piece;
        pthread_mutex_unlock(&s_lock);
    } else {
        /* Another stream for the same piece won the race; serve this one
         * detached (freed by proxy_stream_close). */
        pthread_mutex_unlock(&s_lock);
    }

    if (total_size) {
        *total_size = size;
    }
    return pf;
}

int64_t proxy_stream_read(void* stream, uint8_t* buf, size_t len)
{
    if (!stream) {
        return -1;
    }
    return prefetch_read((prefetch_t*)stream, buf, len);
}

void proxy_stream_close(void* stream)
{
    if (!stream) {
        return;
    }
    prefetch_free((prefetch_t*)stream);
}
