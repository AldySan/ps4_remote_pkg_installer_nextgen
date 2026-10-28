/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Install flows: "direct" (list of pkg piece URLs) and "ref_pkg_url"
 * (reference package manifest). Both build a local manifest served back
 * to BGFT via the app's HTTP server (/static/), exactly like the
 * original application.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct install_result {
    int job_id;          /* taskman job id (v2 API) */
    int task_id;         /* BGFT task id, -1 = already installed */
    int bgft_error;      /* BGFT registration error code (0 = none/transport) */
    char title[256];
    char content_id[40];
    uint64_t package_size;
    bool is_patch;
} install_result_t;

/*
 * Installs a package split into one or more pieces (each URL must be
 * http:// or https://). Piece 0 must be the beginning of the pkg.
 * err/errsz receive a human readable message on failure.
 */
bool installer_install_direct(const char* const* urls, size_t url_count,
                              install_result_t* out, char* err, size_t errsz);

/*
 * Installs from a reference package manifest (JSON with piece urls,
 * offsets and sizes — produced by tools such as PS4 PKG Linker).
 */
bool installer_install_ref_pkg_url(const char* url, install_result_t* out,
                                   char* err, size_t errsz);
