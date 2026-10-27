/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * AppInstUtil wrappers: uninstall family, installed checks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool appinst_init(void);
void appinst_fini(void);

/* Uninstall a game/app or a patch by title id (legacy API ops). */
bool appinst_uninstall_game(const char* title_id, int* error);
bool appinst_uninstall_patch(const char* title_id, int* error);

/* Uninstall additional content by content id, or a theme by content id. */
bool appinst_uninstall_ac(const char* content_id, int* error);
bool appinst_uninstall_theme(const char* content_id, int* error);

/* Installed check by title id; size_out receives the installed size. */
bool appinst_is_exists(const char* title_id, bool* exists, uint64_t* size_out,
                       int* error);
