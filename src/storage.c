/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "storage.h"
#include "log.h"

#include <sys/statvfs.h>
#include <time.h>

#define STORAGE_MOUNT "/data"

/* The dashboard reads the free space every frame; statvfs is a filesystem
 * round-trip, so the value is cached for five seconds instead. */
#define STORAGE_CACHE_S 5.0

static bool s_free_valid = false;
static uint64_t s_free_cached = 0;
static double s_free_at = 0.0;

static double mono_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

bool storage_get_free(uint64_t* free_bytes)
{
    struct statvfs st;
    double now;

    if (!free_bytes) {
        return false;
    }
    now = mono_sec();
    if (s_free_valid && now - s_free_at < STORAGE_CACHE_S) {
        *free_bytes = s_free_cached;
        return true;
    }
    if (statvfs(STORAGE_MOUNT, &st) != 0) {
        LOG_WARN("statvfs(" STORAGE_MOUNT ") failed");
        return false;
    }
    s_free_cached = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
    s_free_at = now;
    s_free_valid = true;
    *free_bytes = s_free_cached;
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
