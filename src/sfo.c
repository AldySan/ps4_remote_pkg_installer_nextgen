/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "sfo.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define SFO_MAGIC 0x46535000u /* "\0PSF" little-endian */

enum sfo_value_format {
    SFO_FORMAT_STRING_SPECIAL = 0x0004,
    SFO_FORMAT_STRING         = 0x0204,
    SFO_FORMAT_UINT32         = 0x0404,
};

struct sfo_header {
    uint32_t magic;
    uint32_t version;
    uint32_t key_table_start;
    uint32_t data_table_start;
    uint32_t entry_count;
};

struct sfo_raw_entry {
    uint16_t key_offset;
    uint16_t data_fmt;
    uint32_t data_len;
    uint32_t data_max_len;
    uint32_t data_offset;
};

static bool sfo_parse_header(const void* sfo, size_t size, struct sfo_header* hdr)
{
    const uint8_t* p = (const uint8_t*)sfo;

    if (!sfo || size < 20) {
        return false;
    }
    if (util_le32(p + 0x00) != SFO_MAGIC) {
        return false;
    }

    hdr->magic            = util_le32(p + 0x00);
    hdr->version          = util_le32(p + 0x04);
    hdr->key_table_start  = util_le32(p + 0x08);
    hdr->data_table_start = util_le32(p + 0x0C);
    hdr->entry_count      = util_le32(p + 0x10);

    if (hdr->key_table_start >= size || hdr->data_table_start >= size) {
        return false;
    }
    if (hdr->entry_count == 0 || hdr->entry_count > 0x1000) {
        return false;
    }
    return true;
}

static bool sfo_find(const void* sfo, size_t size, const char* key,
                     struct sfo_raw_entry* out_entry)
{
    const uint8_t* p = (const uint8_t*)sfo;
    struct sfo_header hdr;
    uint32_t i;

    if (!sfo_parse_header(sfo, size, &hdr)) {
        return false;
    }

    for (i = 0; i < hdr.entry_count; ++i) {
        const uint8_t* e = p + 0x14 + (size_t)i * 0x10;
        struct sfo_raw_entry entry;
        const char* entry_key;

        if ((size_t)(e + 0x10 - p) > size) {
            return false;
        }

        entry.key_offset   = util_le16(e + 0x00);
        entry.data_fmt     = util_le16(e + 0x02);
        entry.data_len     = util_le32(e + 0x04);
        entry.data_max_len = util_le32(e + 0x08);
        entry.data_offset  = util_le32(e + 0x0C);

        if ((size_t)hdr.key_table_start + entry.key_offset + 1 > size) {
            continue;
        }
        entry_key = (const char*)(p + hdr.key_table_start + entry.key_offset);
        if (strncmp(entry_key, key, 0x40) == 0) {
            *out_entry = entry;
            return true;
        }
    }
    return false;
}

bool sfo_get_string(const void* sfo, size_t size, const char* key,
                    char* out, size_t outsz)
{
    const uint8_t* p = (const uint8_t*)sfo;
    struct sfo_header hdr;
    struct sfo_raw_entry entry;
    size_t offset;

    if (!out || outsz == 0 || !key) {
        return false;
    }
    out[0] = '\0';

    if (!sfo_parse_header(sfo, size, &hdr)) {
        return false;
    }
    if (!sfo_find(sfo, size, key, &entry)) {
        return false;
    }
    if (entry.data_fmt != SFO_FORMAT_STRING && entry.data_fmt != SFO_FORMAT_STRING_SPECIAL) {
        return false;
    }

    offset = (size_t)hdr.data_table_start + entry.data_offset;
    if (offset + 1 > size || entry.data_len == 0) {
        return false;
    }

    /* Copy at most outsz-1 bytes, stopping at the stored length. */
    {
        size_t n = entry.data_len - 1; /* exclude trailing NUL */
        size_t avail = size - offset;
        if (n > avail) {
            n = avail;
        }
        if (n > outsz - 1) {
            n = outsz - 1;
        }
        memcpy(out, p + offset, n);
        out[n] = '\0';
    }
    return true;
}

bool sfo_get_u32(const void* sfo, size_t size, const char* key, uint32_t* out)
{
    const uint8_t* p = (const uint8_t*)sfo;
    struct sfo_header hdr;
    struct sfo_raw_entry entry;
    size_t offset;

    if (!out || !key) {
        return false;
    }
    if (!sfo_parse_header(sfo, size, &hdr)) {
        return false;
    }
    if (!sfo_find(sfo, size, key, &entry)) {
        return false;
    }
    if (entry.data_fmt != SFO_FORMAT_UINT32) {
        return false;
    }

    offset = (size_t)hdr.data_table_start + entry.data_offset;
    if (offset + 4 > size) {
        return false;
    }
    *out = util_le32(p + offset);
    return true;
}

bool sfo_get_title(const void* sfo, size_t size, int lang_index,
                   char* out, size_t outsz)
{
    char key[24];

    if (lang_index > 0) {
        snprintf(key, sizeof(key), "TITLE_%02d", lang_index);
        if (sfo_get_string(sfo, size, key, out, outsz)) {
            return true;
        }
    }
    return sfo_get_string(sfo, size, "TITLE", out, outsz);
}
