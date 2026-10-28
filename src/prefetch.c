/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "prefetch.h"
#include "http_client.h"
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * Design: all workers pull from a SHARED sequential chunk frontier.
 *
 * The resource is cut into fixed-size chunks (2 MiB). Each worker picks the
 * next unclaimed chunk nearest the consumer frontier (so N workers fetch N
 * consecutive chunks in parallel), fetches it with one HTTP Range request,
 * and stores it into the ring. The consumer reads strictly sequentially and
 * only ever waits for its next chunks. This gives a true N-times speedup
 * for sequential consumers (BGFT piece downloads) even when each connection
 * is throttled.
 */

#define PREFETCH_RING_SIZE   (64u * 1024u * 1024u) /* 64 MiB ring */
#define PREFETCH_CHUNK_SIZE  (2u * 1024u * 1024u)  /* per-request granularity */
#define PREFETCH_MAX_CONNS   8

enum chunk_state {
    CHUNK_PENDING = 0,
    CHUNK_DONE = 1,
    CHUNK_FAILED = 2,
    CHUNK_CLAIMED = 3,
};

struct prefetch {
    char url[2048];
    uint64_t total_size;
    int connections;

    uint8_t* ring;
    size_t ring_size;

    pthread_mutex_t lock;
    pthread_cond_t data_avail;   /* a chunk completed (or abort/failure) */
    pthread_cond_t work_avail;   /* window advanced / chunks became claimable */

    bool aborting;

    uint8_t* chunk_state;
    size_t chunk_count;
    uint64_t filled_bytes;
    uint64_t consumed_bytes;     /* sequential consumer frontier */
    bool read_failed;

    pthread_t threads[PREFETCH_MAX_CONNS];
    int thread_count;
    bool threads_spawned;
};

struct worker_ctx {
    prefetch_t* pf;
};

static uint64_t chunk_start(size_t chunk)
{
    return (uint64_t)chunk * PREFETCH_CHUNK_SIZE;
}

static size_t chunk_index(uint64_t pos)
{
    return (size_t)(pos / PREFETCH_CHUNK_SIZE);
}

static void* worker_thread(void* arg)
{
    struct worker_ctx* ctx = (struct worker_ctx*)arg;
    prefetch_t* pf = ctx->pf;
    http_session_t* sess = NULL;
    uint8_t* buf = NULL;

    free(ctx);

    buf = (uint8_t*)malloc(PREFETCH_CHUNK_SIZE);
    if (!buf) {
        pthread_mutex_lock(&pf->lock);
        pf->read_failed = true;
        pthread_cond_broadcast(&pf->data_avail);
        pthread_mutex_unlock(&pf->lock);
        return NULL;
    }

    for (;;) {
        size_t c = (size_t)-1;
        uint64_t off;
        size_t want;
        http_result_t r;
        bool aborting;

        /* Claim the next pending chunk inside the ring window. */
        pthread_mutex_lock(&pf->lock);
        for (;;) {
            size_t first = chunk_index(pf->consumed_bytes);
            size_t last = first + pf->ring_size / PREFETCH_CHUNK_SIZE; /* exclusive */
            size_t i;
            bool any_pending = false;

            if (pf->aborting) {
                pthread_mutex_unlock(&pf->lock);
                http_session_close(sess);
                free(buf);
                return NULL;
            }

            for (i = first; i < last && i < pf->chunk_count; ++i) {
                if (pf->chunk_state[i] == CHUNK_PENDING) {
                    c = i;
                    break;
                }
                if (pf->chunk_state[i] == CHUNK_CLAIMED) {
                    any_pending = true; /* work in flight; more may follow */
                }
            }
            if (c != (size_t)-1) {
                break;
            }

            /* Nothing claimable: window is exhausted or all claimed/done. */
            {
                size_t k;
                bool all_done = true;
                for (k = 0; k < pf->chunk_count; ++k) {
                    if (pf->chunk_state[k] != CHUNK_DONE &&
                        pf->chunk_state[k] != CHUNK_FAILED) {
                        all_done = false;
                        break;
                    }
                }
                if (all_done) {
                    pthread_mutex_unlock(&pf->lock);
                    http_session_close(sess);
                    free(buf);
                    return NULL; /* finished */
                }
            }
            if (any_pending || first + pf->ring_size / PREFETCH_CHUNK_SIZE < pf->chunk_count) {
                /* In-flight work may free the window; wait for progress. */
                pthread_cond_wait(&pf->work_avail, &pf->lock);
                continue;
            }
            /* Everything remaining is done or failed and the window is
             * ahead of it: the reader stopped consuming. Retire. */
            pthread_cond_wait(&pf->work_avail, &pf->lock);
        }
        pf->chunk_state[c] = CHUNK_CLAIMED;
        pthread_mutex_unlock(&pf->lock);

        off = chunk_start(c);
        want = (size_t)(pf->total_size - off);
        if (want > PREFETCH_CHUNK_SIZE) {
            want = PREFETCH_CHUNK_SIZE;
        }

        if (!http_client_get_session(&sess, pf->url, off, want, buf, want, &r) ||
            !HTTP_IS_OK(r.status) || r.body_len != want) {
            pthread_mutex_lock(&pf->lock);
            if (pf->chunk_state[c] == CHUNK_CLAIMED) {
                pf->chunk_state[c] = CHUNK_FAILED;
            }
            pf->read_failed = true;
            pthread_cond_broadcast(&pf->data_avail);
            pthread_cond_broadcast(&pf->work_avail);
            pthread_mutex_unlock(&pf->lock);
            LOG_WARN("prefetch: chunk %zu failed at offset %llu (HTTP %d)",
                     c, (unsigned long long)off, r.status);
            http_session_close(sess);
            free(buf);
            return NULL;
        }

        pthread_mutex_lock(&pf->lock);
        {
            size_t ring_pos = (size_t)(off % pf->ring_size);
            size_t first_part = pf->ring_size - ring_pos;
            if (first_part > want) {
                first_part = want;
            }
            memcpy(pf->ring + ring_pos, buf, first_part);
            if (want > first_part) {
                memcpy(pf->ring, buf + first_part, want - first_part);
            }
            if (pf->chunk_state[c] == CHUNK_CLAIMED) {
                pf->chunk_state[c] = CHUNK_DONE;
                {
                    uint64_t end = off + want;
                    if (end > pf->filled_bytes) {
                        pf->filled_bytes = end;
                    }
                }
            }
            aborting = pf->aborting;
            pthread_cond_broadcast(&pf->data_avail);
            pthread_cond_broadcast(&pf->work_avail);
        }
        pthread_mutex_unlock(&pf->lock);

        if (aborting) {
            http_session_close(sess);
            free(buf);
            return NULL;
        }
    }
}

prefetch_t* prefetch_start(const char* url, uint64_t total_size, int connections)
{
    prefetch_t* pf;

    if (!url || total_size == 0) {
        return NULL;
    }
    if (connections < 1) {
        connections = 1;
    }
    if (connections > PREFETCH_MAX_CONNS) {
        connections = PREFETCH_MAX_CONNS;
    }

    pf = (prefetch_t*)calloc(1, sizeof(*pf));
    if (!pf) {
        return NULL;
    }

    snprintf(pf->url, sizeof(pf->url), "%s", url);
    pf->total_size = total_size;
    pf->connections = connections;
    pf->ring_size = PREFETCH_RING_SIZE;
    if (pf->ring_size > (size_t)total_size) {
        pf->ring_size = (size_t)((total_size + 0xFFFFFULL) & ~0xFFFFFULL);
    }

    pf->ring = (uint8_t*)malloc(pf->ring_size);
    pf->chunk_count = (size_t)((total_size + PREFETCH_CHUNK_SIZE - 1) / PREFETCH_CHUNK_SIZE);
    pf->chunk_state = (uint8_t*)calloc(pf->chunk_count, 1);
    if (!pf->ring || !pf->chunk_state) {
        goto err;
    }

    pthread_mutex_init(&pf->lock, NULL);
    pthread_cond_init(&pf->data_avail, NULL);
    pthread_cond_init(&pf->work_avail, NULL);

    {
        int i;
        for (i = 0; i < connections; ++i) {
            struct worker_ctx* ctx = (struct worker_ctx*)malloc(sizeof(*ctx));
            if (!ctx) {
                goto err;
            }
            ctx->pf = pf;
            if (pthread_create(&pf->threads[i], NULL, worker_thread, ctx) != 0) {
                free(ctx);
                goto err;
            }
            ++pf->thread_count;
        }
    }
    pf->threads_spawned = true;
    return pf;

err:
    pthread_mutex_lock(&pf->lock);
    pf->aborting = true;
    pthread_cond_broadcast(&pf->data_avail);
    pthread_cond_broadcast(&pf->work_avail);
    pthread_mutex_unlock(&pf->lock);
    {
        int i;
        for (i = 0; i < pf->thread_count; ++i) {
            pthread_join(pf->threads[i], NULL);
        }
    }
    pthread_mutex_destroy(&pf->lock);
    pthread_cond_destroy(&pf->data_avail);
    pthread_cond_destroy(&pf->work_avail);
    free(pf->ring);
    free(pf->chunk_state);
    free(pf);
    return NULL;
}

int64_t prefetch_read(prefetch_t* pf, uint8_t* buf, size_t len)
{
    if (!pf || !buf || len == 0) {
        return -1;
    }

    pthread_mutex_lock(&pf->lock);
    for (;;) {
        uint64_t pos = pf->consumed_bytes;
        uint64_t end = pos + len;
        size_t c0, c1, c;
        bool ready = true;

        if (pos >= pf->total_size) {
            pthread_mutex_unlock(&pf->lock);
            return 0; /* EOF */
        }
        if (end > pf->total_size) {
            end = pf->total_size;
            len = (size_t)(end - pos);
        }

        c0 = chunk_index(pos);
        c1 = chunk_index(end - 1);
        for (c = c0; c <= c1; ++c) {
            if (pf->chunk_state[c] != CHUNK_DONE) {
                ready = false;
                break;
            }
        }

        if (ready) {
            size_t ring_pos = (size_t)(pos % pf->ring_size);
            size_t first_part = pf->ring_size - ring_pos;
            if (first_part > len) {
                first_part = len;
            }
            memcpy(buf, pf->ring + ring_pos, first_part);
            if (len > first_part) {
                memcpy(buf + first_part, pf->ring, len - first_part);
            }
            pf->consumed_bytes = end;
            pthread_cond_broadcast(&pf->work_avail);
            pthread_mutex_unlock(&pf->lock);
            return (int64_t)len;
        }

        if (pf->chunk_state[c] == CHUNK_FAILED || pf->read_failed || pf->aborting) {
            pthread_mutex_unlock(&pf->lock);
            return -1;
        }

        pthread_cond_wait(&pf->data_avail, &pf->lock);
    }
}

void prefetch_abort(prefetch_t* pf)
{
    if (!pf) {
        return;
    }
    pthread_mutex_lock(&pf->lock);
    pf->aborting = true;
    pthread_cond_broadcast(&pf->data_avail);
    pthread_cond_broadcast(&pf->work_avail);
    pthread_mutex_unlock(&pf->lock);
}

void prefetch_free(prefetch_t* pf)
{
    int i;

    if (!pf) {
        return;
    }
    prefetch_abort(pf);
    for (i = 0; i < pf->thread_count; ++i) {
        pthread_join(pf->threads[i], NULL);
    }
    pthread_mutex_destroy(&pf->lock);
    pthread_cond_destroy(&pf->data_avail);
    pthread_cond_destroy(&pf->work_avail);
    free(pf->ring);
    free(pf->chunk_state);
    free(pf);
}

void prefetch_stats(const prefetch_t* pf, prefetch_stats_t* out)
{
    if (!pf || !out) {
        return;
    }
    pthread_mutex_lock((pthread_mutex_t*)&pf->lock);
    out->connections = pf->connections;
    out->filled_bytes = pf->filled_bytes;
    out->consumed_bytes = pf->consumed_bytes;
    out->total_bytes = pf->total_size;
    out->ranges_ok = pf->connections > 1;
    pthread_mutex_unlock((pthread_mutex_t*)&pf->lock);
}
