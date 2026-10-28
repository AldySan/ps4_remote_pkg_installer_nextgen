/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Piece proxy: rewrites BGFT piece URLs to point at this app
 * (/static/proxy/<job>/<piece>) and streams each piece through the
 * adaptive prefetch engine, so downloads run at up to N-times
 * single-connection speed even against throttling origin servers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Registers a job's pieces for proxying. Returns false on bad args or
 * registry exhaustion. urls/sizes are copied. */
bool proxy_register(int job_id, const char* const* urls,
                    const uint64_t* sizes, const bool* ranges_ok,
                    size_t count);

/* Drops a job's proxy state (aborts an active stream if any). */
void proxy_unregister(int job_id);

/* Piece info for the HTTP handler. */
bool proxy_get_piece(int job_id, size_t piece, char* url_out, size_t url_sz,
                     uint64_t* size_out);

/*
 * Opens a streaming handle for a piece (starts the prefetch engine).
 * Connection count is chosen adaptively from size + Range support.
 * Only one piece streams per job at a time (BGFT reads sequentially);
 * opening a new piece aborts the previous one.
 */
void* proxy_stream_open(int job_id, size_t piece, uint64_t* total_size);

/* Reads the next block of the stream (0 at EOF, -1 on error). */
int64_t proxy_stream_read(void* stream, uint8_t* buf, size_t len);

/* Aborts and frees a stream handle. */
void proxy_stream_close(void* stream);
