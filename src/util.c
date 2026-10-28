/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "util.h"

#include <stdio.h>
#include <string.h>

void util_hex_encode(const uint8_t* data, size_t len, char* out, size_t outsz)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    if (!out || outsz == 0) {
        return;
    }
    out[0] = '\0';
    if (!data) {
        return;
    }
    for (i = 0; i < len; ++i) {
        if ((i * 2 + 2) >= outsz) {
            break;
        }
        out[i * 2 + 0] = hex[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[data[i] & 0xF];
    }
    out[(i * 2) < outsz ? (i * 2) : (outsz - 1)] = '\0';
}

void util_format_size(uint64_t bytes, char* out, size_t outsz)
{
    static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double value = (double)bytes;
    int unit = 0;

    if (!out || outsz == 0) {
        return;
    }
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        snprintf(out, outsz, "%u B", (unsigned)bytes);
    } else {
        snprintf(out, outsz, "%.2f %s", value, units[unit]);
    }
}

bool util_starts_with(const char* s, const char* prefix)
{
    size_t plen;

    if (!s || !prefix) {
        return false;
    }
    plen = strlen(prefix);
    return strncmp(s, prefix, plen) == 0;
}
