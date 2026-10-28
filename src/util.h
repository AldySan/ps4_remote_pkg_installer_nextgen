/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Small portable helpers: big-endian readers, formatting, hex.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Big-endian readers (pkg/sfo on-disk layouts are big-endian). */
static inline uint16_t util_be16(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)((b[0] << 8) | b[1]);
}

static inline uint32_t util_be32(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static inline uint64_t util_be64(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return ((uint64_t)util_be32(b) << 32) | (uint64_t)util_be32(b + 4);
}

/* Little-endian readers (PARAM.SFO is little-endian). */
static inline uint16_t util_le16(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)((b[1] << 8) | b[0]);
}

static inline uint32_t util_le32(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[1] << 8) | (uint32_t)b[0];
}

/* Lowercase hex encoding of data into out (needs 2*len+1 bytes). */
void util_hex_encode(const uint8_t* data, size_t len, char* out, size_t outsz);

/* Human-readable size, e.g. "13.4 MB" / "1.02 GB". */
void util_format_size(uint64_t bytes, char* out, size_t outsz);

/* True if s starts with prefix. */
bool util_starts_with(const char* s, const char* prefix);
