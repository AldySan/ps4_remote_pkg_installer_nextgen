/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Fault recovery net.
 */

#pragma once

#include <setjmp.h>

void crash_init(void);
void crash_arm(sigjmp_buf* buf);
void crash_disarm(void);
int crash_pending_signal(void);
