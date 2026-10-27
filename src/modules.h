/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * System module load/unload (mirrors the original application).
 */
#pragma once

#include <stdbool.h>

bool modules_load(void);
void modules_unload(void);
