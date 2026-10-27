/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Storage (the /data partition used for fPKG installs).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Free bytes on the /data partition. Returns false on statvfs failure. */
bool storage_get_free(uint64_t* free_bytes);

/* Total capacity in bytes of the /data partition (best effort). */
bool storage_get_total(uint64_t* total_bytes);
