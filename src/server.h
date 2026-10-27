/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * HTTP server: legacy API (byte-compatible with the original application),
 * the new v2 API, /static/ file serving and the accelerated piece proxy.
 *
 * Legacy wire format (kept 1:1):
 *   - every response: Content-Type: application/json,
 *     Access-Control-Allow-Origin: *, Connection: close
 *   - errors: {"status": "fail", "error": "..."} (HTTP 400/500) or
 *             {"status": "fail", "error_code": 0x........} (HTTP 200)
 *   - success: {"status": "success", ...} with spaces after colons
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Starts the server on INADDR_ANY:port (threaded). Returns false when
 * the socket could not be created/bound. */
bool server_start(int port);

/* Stops the server (listener closed; in-flight requests finish). */
void server_stop(void);

bool server_is_running(void);

int server_port(void);
