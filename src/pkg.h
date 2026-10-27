/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * PKG container header/entry-table parsing over a raw in-memory image
 * (fetched via HTTP Range requests). Pure portable C, unit-tested on host.
 *
 * Field offsets follow the PS4 pkg format used by the original application:
 *   magic[4] @0x00 ("\x7FCNT"), entry_count u32 @0x10,
 *   entry_table_offset u32 @0x18, content_id[36] @0x40,
 *   content_type u32 @0x74, content_flags u32 @0x78,
 *   package_size u64 @0x430, digest[32] @0xFE0,
 *   header size 0x2000; table entries 0x20 bytes each:
 *   id u32 @0x00, offset u32 @0x10, size u32 @0x14.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PKG_HEADER_SIZE       0x2000u
#define PKG_ENTRY_TABLE_SIZE  0x20u
#define PKG_CONTENT_ID_SIZE   0x24u /* 36 chars */
#define PKG_TITLE_ID_SIZE     0x09u /* 9 chars incl. NUL */
#define PKG_SERVICE_ID_SIZE   0x13u /* 19 chars incl. NUL */
#define PKG_LABEL_SIZE        0x10u
#define PKG_DIGEST_SIZE       0x20u

enum pkg_entry_id {
    PKG_ENTRY_ID_PARAM_SFO = 0x1000,
    PKG_ENTRY_ID_ICON0_PNG = 0x1200,
};

enum pkg_content_type {
    PKG_CONTENT_TYPE_GD = 0x1A, /* apps, patches, remasters */
    PKG_CONTENT_TYPE_AC = 0x1B, /* additional content, themes */
    PKG_CONTENT_TYPE_AL = 0x1C, /* additional content (no data) */
    PKG_CONTENT_TYPE_DP = 0x1E, /* delta patches */
};

#define PKG_CONTENT_FLAGS_FIRST_PATCH       0x00100000u
#define PKG_CONTENT_FLAGS_PATCHGO           0x00200000u
#define PKG_CONTENT_FLAGS_REMASTER          0x00400000u
#define PKG_CONTENT_FLAGS_PS_CLOUD          0x00800000u
#define PKG_CONTENT_FLAGS_GD_AC             0x02000000u
#define PKG_CONTENT_FLAGS_NON_GAME          0x04000000u
#define PKG_CONTENT_FLAGS_SUBSEQUENT_PATCH  0x40000000u
#define PKG_CONTENT_FLAGS_DELTA_PATCH       0x41000000u
#define PKG_CONTENT_FLAGS_CUMULATIVE_PATCH  0x60000000u

/* Parsed view of a pkg header (all endian conversion done). */
typedef struct pkg_header_info {
    uint32_t entry_count;
    uint32_t entry_table_offset;
    char     content_id[PKG_CONTENT_ID_SIZE + 1];
    uint32_t content_type;
    uint32_t content_flags;
    uint64_t package_size;
    uint8_t  digest[PKG_DIGEST_SIZE];
} pkg_header_info_t;

/* A single pkg table entry (endianness converted). */
typedef struct pkg_entry {
    uint32_t id;     /* enum pkg_entry_id */
    uint64_t offset; /* absolute offset in the pkg file */
    uint64_t size;
} pkg_entry_t;

/* Parsed pieces of a content id, e.g. "IV0000-RPIN00001_00-RPINEXTGEN000000". */
typedef struct pkg_content_id_info {
    char content_id[PKG_CONTENT_ID_SIZE + 1];
    char service_id[PKG_SERVICE_ID_SIZE + 1]; /* e.g. "IV0000-RPIN00001_00" (19 chars) */
    char title_id[PKG_TITLE_ID_SIZE + 1];     /* e.g. "RPIN00001" (9 chars) */
    char label[PKG_LABEL_SIZE + 1];           /* e.g. "RPINEXTGEN000000" (16 chars) */
} pkg_content_id_info_t;

/* Parses the fixed 0x2000-byte header. Returns false on bad magic/size. */
bool pkg_parse_header(const void* buf, size_t size, pkg_header_info_t* out);

/* Finds an entry by id in the pkg table. Requires the header to be valid. */
bool pkg_find_entry(const void* buf, size_t size, uint32_t entry_id,
                    pkg_entry_t* out);

/* Patch detection from content flags (matches the original application). */
bool pkg_is_patch(uint32_t content_flags);

/* "GD" / "AC" / "AL" / "DP" / "??" for logging and API payloads. */
const char* pkg_content_type_name(uint32_t content_type);

/* Content type accepted by the installer. */
bool pkg_is_supported_content_type(uint32_t content_type);

/* Splits a content id into its parts. Returns false on malformed input. */
bool pkg_parse_content_id(const char* content_id, pkg_content_id_info_t* out);
