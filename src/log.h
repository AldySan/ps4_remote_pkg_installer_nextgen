/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Logging facade: stdout (kernel log) + dbglogger (UDP/file sink) + an
 * in-memory ring buffer served by the v2 API and shown on the dashboard.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <dbglogger.h>

/* Ring buffer sink (fixed size, no allocation; safe from any thread). */
#define LOG_RING_CAPACITY 64
#define LOG_RING_ENTRY_MAX 200

void log_ring_add(const char* level, const char* fmt, ...);

/* Copies the entry at index (0 = oldest) into out; false when exhausted.
 * Use log_ring_count() for iteration. */
bool log_ring_get(int index, char* out, size_t outsz);
int log_ring_count(void);

#define LOG_INFO(...)                                    \
    do {                                                 \
        printf("[INFO] " __VA_ARGS__);                   \
        printf("\n");                                    \
        dbglogger_log("[INFO] " __VA_ARGS__);            \
        dbglogger_log("\n");                             \
        log_ring_add("INFO", __VA_ARGS__);               \
    } while (0)

#define LOG_WARN(...)                                    \
    do {                                                 \
        printf("[WARN] " __VA_ARGS__);                   \
        printf("\n");                                    \
        dbglogger_log("[WARN] " __VA_ARGS__);            \
        dbglogger_log("\n");                             \
        log_ring_add("WARN", __VA_ARGS__);               \
    } while (0)

#define LOG_ERROR(...)                                   \
    do {                                                 \
        printf("[ERROR] " __VA_ARGS__);                  \
        printf("\n");                                    \
        dbglogger_log("[ERROR] " __VA_ARGS__);           \
        dbglogger_log("\n");                             \
        log_ring_add("ERROR", __VA_ARGS__);              \
    } while (0)

#define LOG_DEBUG(...)                                   \
    do {                                                 \
        printf("[DEBUG] " __VA_ARGS__);                  \
        printf("\n");                                    \
        dbglogger_log("[DEBUG] " __VA_ARGS__);           \
        dbglogger_log("\n");                             \
        log_ring_add("DEBUG", __VA_ARGS__);              \
    } while (0)
