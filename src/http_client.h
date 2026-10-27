/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * libSceHttp client wrapper for range GETs against remote pkg hosts.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HTTP status range helpers. */
#define HTTP_IS_OK(status)        ((status) >= 200 && (status) < 300)
#define HTTP_IS_REDIRECT(status)  ((status) >= 300 && (status) < 400)
#define HTTP_IS_CLIENT_ERR(status) ((status) >= 400 && (status) < 500)
#define HTTP_IS_SERVER_ERR(status) ((status) >= 500 && (status) < 600)

/* Output of a completed GET. */
typedef struct http_result {
    int      status;     /* HTTP status code */
    uint64_t body_len;   /* bytes written into the caller buffer */
    uint64_t total_len;  /* Content-Length (or total for range responses) */
    bool     partial;    /* true when the server answered 206 */
} http_result_t;

/*
 * GETs `length` bytes starting at `offset` from `url` into buf.
 *
 * - length == 0: plain GET of the whole resource (no Range header).
 * - The response body is read in chunks; only up to `bufsz` bytes are
 *   stored (body_len reflects what was stored, total_len the full size).
 *
 * Returns false only on transport errors (DNS, connect, send, read);
 * HTTP error statuses still return true with result->status set.
 * Requires the network stack to be up (netmon).
 */
bool http_client_get(const char* url, uint64_t offset, uint64_t length,
                     uint8_t* buf, size_t bufsz, http_result_t* result);

/*
 * Probes a URL: fetches just the first byte to learn the total size and
 * whether the server supports Range requests (result->partial == true).
 */
bool http_client_probe(const char* url, uint64_t* total_size, bool* accepts_ranges);

/* Tears down the SSL/HTTP contexts (safe to call when never initialized,
 * e.g. after the network went down mid-run). */
void http_client_term(void);
