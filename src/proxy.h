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

/* Drops a job's proxy state. A live stream is aborted (its serving
 * connection owns it and frees it via proxy_stream_close); the piece
 * table is released. */
void proxy_unregister(int job_id);

/*
 * Opens a streaming handle for a piece (starts the prefetch engine).
 * Connection count is chosen adaptively from size + Range support.
 * Only one piece streams per job at a time (BGFT reads sequentially);
 * opening a new piece aborts the previous one.
 */
void* proxy_stream_open(int job_id, size_t piece, uint64_t* total_size);

/* Reads the next block of the stream (0 at EOF, -1 on error). */
int64_t proxy_stream_read(void* stream, uint8_t* buf, size_t len);

/*
 * Frees a stream handle (its serving connection owns the stream).
 * job_id lets the registry drop its reference first, so a concurrent
 * piece change or unregister can never abort or double-free it.
 */
void proxy_stream_close(int job_id, void* stream);
