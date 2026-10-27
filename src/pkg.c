/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "pkg.h"
#include "util.h"

#include <string.h>

static const uint8_t s_pkg_magic[4] = { 0x7F, 'C', 'N', 'T' };

bool pkg_parse_header(const void* buf, size_t size, pkg_header_info_t* out)
{
    const uint8_t* p = (const uint8_t*)buf;

    if (!buf || !out || size < PKG_HEADER_SIZE) {
        return false;
    }
    if (memcmp(p + 0x00, s_pkg_magic, sizeof(s_pkg_magic)) != 0) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    out->entry_count        = util_be32(p + 0x10);
    out->entry_table_offset = util_be32(p + 0x18);

    memcpy(out->content_id, p + 0x40, PKG_CONTENT_ID_SIZE);
    out->content_id[PKG_CONTENT_ID_SIZE] = '\0';

    out->content_type  = util_be32(p + 0x74);
    out->content_flags = util_be32(p + 0x78);
    out->package_size  = util_be64(p + 0x430);
    memcpy(out->digest, p + 0xFE0, PKG_DIGEST_SIZE);

    if (out->entry_count == 0 || out->entry_count > 0x1000) {
        return false;
    }
    return true;
}

bool pkg_find_entry(const void* buf, size_t size, uint32_t entry_id,
                    pkg_entry_t* out)
{
    const uint8_t* p = (const uint8_t*)buf;
    pkg_header_info_t hdr;
    uint32_t i;

    if (!buf || !out) {
        return false;
    }
    if (!pkg_parse_header(buf, size, &hdr)) {
        return false;
    }

    for (i = 0; i < hdr.entry_count; ++i) {
        const uint8_t* e;
        uint32_t id;

        /* Guard against a hostile table offset/count pair. */
        if ((uint64_t)hdr.entry_table_offset + (uint64_t)(i + 1) * PKG_ENTRY_TABLE_SIZE > size) {
            return false;
        }

        e = p + hdr.entry_table_offset + (size_t)i * PKG_ENTRY_TABLE_SIZE;
        id = util_be32(e + 0x00);
        if (id == entry_id) {
            out->id     = id;
            out->offset = util_be32(e + 0x10);
            out->size   = util_be32(e + 0x14);
            if (out->offset + out->size > size) {
                /* The entry data lives beyond the provided buffer (that is
                 * normal when only the header was fetched); still report the
                 * entry so the caller can range-fetch the data. */
            }
            return true;
        }
    }
    return false;
}

bool pkg_is_patch(uint32_t content_flags)
{
    if (content_flags & PKG_CONTENT_FLAGS_FIRST_PATCH) {
        return true;
    }
    if (content_flags & PKG_CONTENT_FLAGS_SUBSEQUENT_PATCH) {
        return true;
    }
    if (content_flags & PKG_CONTENT_FLAGS_DELTA_PATCH) {
        return true;
    }
    if (content_flags & PKG_CONTENT_FLAGS_CUMULATIVE_PATCH) {
        return true;
    }
    return false;
}

const char* pkg_content_type_name(uint32_t content_type)
{
    switch (content_type) {
        case PKG_CONTENT_TYPE_GD:
            return "GD";
        case PKG_CONTENT_TYPE_AC:
            return "AC";
        case PKG_CONTENT_TYPE_AL:
            return "AL";
        case PKG_CONTENT_TYPE_DP:
            return "DP";
        default:
            return "??";
    }
}

bool pkg_is_supported_content_type(uint32_t content_type)
{
    switch (content_type) {
        case PKG_CONTENT_TYPE_GD:
        case PKG_CONTENT_TYPE_AC:
        case PKG_CONTENT_TYPE_AL:
        case PKG_CONTENT_TYPE_DP:
            return true;
        default:
            return false;
    }
}

bool pkg_parse_content_id(const char* content_id, pkg_content_id_info_t* out)
{
    const char* p1;
    const char* p2;
    size_t service_len;

    if (!content_id || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (strlen(content_id) != PKG_CONTENT_ID_SIZE) {
        return false;
    }

    p1 = strchr(content_id, '-');
    if (!p1) {
        return false;
    }
    p2 = strchr(p1 + 1, '-');
    if (!p2) {
        return false;
    }
    service_len = (size_t)(p2 - content_id);
    if (service_len != PKG_SERVICE_ID_SIZE) {
        return false;
    }
    if (strlen(p2 + 1) + 1 > sizeof(out->label)) {
        return false;
    }
    if ((size_t)(p1 - content_id) + 1 + PKG_TITLE_ID_SIZE > service_len) {
        return false;
    }

    memcpy(out->content_id, content_id, PKG_CONTENT_ID_SIZE);
    memcpy(out->service_id, content_id, service_len);
    memcpy(out->title_id, p1 + 1, PKG_TITLE_ID_SIZE);
    out->title_id[PKG_TITLE_ID_SIZE] = '\0';
    memcpy(out->label, p2 + 1, strlen(p2 + 1));

    return true;
}
