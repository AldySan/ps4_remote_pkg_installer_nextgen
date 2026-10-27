/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * PARAM.SFO lookup over a raw in-memory image (fetched from a remote pkg).
 * The parser is pure portable C and is unit-tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Finds `key` in the SFO image and copies its NUL-terminated string value
 * into `out`. Returns false when the image is invalid or the key is absent.
 */
bool sfo_get_string(const void* sfo, size_t size, const char* key,
                    char* out, size_t outsz);

/*
 * Finds `key` in the SFO image and stores its 32-bit integer value.
 * Returns false when the image is invalid or the key is absent.
 */
bool sfo_get_u32(const void* sfo, size_t size, const char* key, uint32_t* out);

/*
 * Reads the localized TITLE_%02d entry for `lang_index` (0 = default TITLE).
 * Falls back to plain "TITLE". PS4 language indices follow the system order
 * (0=jp, 1=en, ... 11=[default]), matching the original application.
 */
bool sfo_get_title(const void* sfo, size_t size, int lang_index,
                   char* out, size_t outsz);
