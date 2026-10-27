/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Adaptive segmented prefetch engine.
 *
 * Downloads a remote resource with N parallel HTTP Range connections into
 * a bounded ring buffer while the consumer reads it back sequentially.
 * Used by the /static/ piece proxy so BGFT effectively pulls at up to
 * N-times single-stream speed, even when the origin server throttles
 * per connection.
 *
 * Strategy (adaptive):
 *   - start with 1 connection
 *   - if the server supports Range requests and the resource is large,
 *     split into N segments (default 4)
 *   - small resources stay on a single connection
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct prefetch prefetch_t;

/* Statistics snapshot for the dashboard/API. */
typedef struct prefetch_stats {
    int      connections;    /* active worker count */
    uint64_t filled_bytes;   /* bytes fetched so far */
    uint64_t consumed_bytes; /* bytes read by the consumer */
    uint64_t total_bytes;    /* resource size */
    bool     ranges_ok;      /* server supports Range */
} prefetch_stats_t;

/*
 * Starts prefetching `url` (total_size bytes known by the caller).
 * connections == 1 performs a plain single-stream download.
 * Returns NULL on failure (allocation/thread error).
 */
prefetch_t* prefetch_start(const char* url, uint64_t total_size, int connections);

/*
 * Reads up to len bytes sequentially (blocking until data is available).
 * Returns bytes read (0 at EOF), -1 on error/abort.
 */
int64_t prefetch_read(prefetch_t* pf, uint8_t* buf, size_t len);

/* Aborts workers and wakes the consumer (pending reads return -1). */
void prefetch_abort(prefetch_t* pf);

/* Joins workers and frees resources. */
void prefetch_free(prefetch_t* pf);

/* Snapshots current stats. */
void prefetch_stats(const prefetch_t* pf, prefetch_stats_t* out);

/* Default parallelism used by callers that probe first. */
#define PREFETCH_DEFAULT_CONNECTIONS 4
