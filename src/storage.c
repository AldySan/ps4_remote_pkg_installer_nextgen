/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "storage.h"
#include "log.h"

#include <sys/statvfs.h>

#define STORAGE_MOUNT "/data"

bool storage_get_free(uint64_t* free_bytes)
{
    struct statvfs st;

    if (!free_bytes) {
        return false;
    }
    if (statvfs(STORAGE_MOUNT, &st) != 0) {
        LOG_WARN("statvfs(" STORAGE_MOUNT ") failed");
        return false;
    }
    *free_bytes = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
    return true;
}

bool storage_get_total(uint64_t* total_bytes)
{
    struct statvfs st;

    if (!total_bytes) {
        return false;
    }
    if (statvfs(STORAGE_MOUNT, &st) != 0) {
        return false;
    }
    *total_bytes = (uint64_t)st.f_blocks * (uint64_t)st.f_frsize;
    return true;
}
