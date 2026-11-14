/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Flight recorder: the startup sequence and every recovered fault are noted
 * on screen (small HUD), in a persistent file and in the system
 * notification feed, so a failure on a real console leaves evidence behind
 * even when the process dies.
 */

#pragma once

#include <stddef.h>

/* Records a startup/runtime step. Persisted to /data/rpi_nextgen_boot.txt. */
void diag_note(const char* step);

/* Last recorded step (for the crash journal and the HUD). */
const char* diag_step(void);

/* Seconds since the first note. */
double diag_uptime(void);

/* One-line HUD string for the dashboard (uptime + last step). */
void diag_hud(char* out, size_t outsz);

/* Appends one line to /data/rpi_nextgen_crash.txt with a wall-clock stamp.
 * Used from the fault handler right before recovery or clean exit; the
 * write goes through a pre-opened fd so it stays legal in that context. */
void diag_crash_note(const char* fmt, ...);

/* The pre-opened, append-only fd of the crash journal (-1 when the file
 * cannot be created). Warm it once before any fault can occur; the fault
 * handler then writes to it without allocating or locking. */
int diag_crash_fd(void);
