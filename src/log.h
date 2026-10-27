/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Logging facade: stdout (kernel log) + dbglogger (UDP/file sink).
 */
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <dbglogger.h>

#define LOG_INFO(...)                                    \
    do {                                                 \
        printf("[INFO] " __VA_ARGS__);                   \
        printf("\n");                                    \
        dbglogger_log("[INFO] " __VA_ARGS__);            \
        dbglogger_log("\n");                             \
    } while (0)

#define LOG_WARN(...)                                    \
    do {                                                 \
        printf("[WARN] " __VA_ARGS__);                   \
        printf("\n");                                    \
        dbglogger_log("[WARN] " __VA_ARGS__);            \
        dbglogger_log("\n");                             \
    } while (0)

#define LOG_ERROR(...)                                   \
    do {                                                 \
        printf("[ERROR] " __VA_ARGS__);                  \
        printf("\n");                                    \
        dbglogger_log("[ERROR] " __VA_ARGS__);           \
        dbglogger_log("\n");                             \
    } while (0)

#define LOG_DEBUG(...)                                   \
    do {                                                 \
        printf("[DEBUG] " __VA_ARGS__);                  \
        printf("\n");                                    \
        dbglogger_log("[DEBUG] " __VA_ARGS__);           \
        dbglogger_log("\n");                             \
    } while (0)
