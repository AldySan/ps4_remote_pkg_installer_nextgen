/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "appinst.h"
#include "log.h"
#include "pkg.h"

#include <orbis/AppInstUtil.h>

static bool s_initialized = false;

bool appinst_init(void)
{
    int ret;

    if (s_initialized) {
        return true;
    }

    ret = sceAppInstUtilInitialize();
    if (ret != 0) {
        LOG_ERROR("sceAppInstUtilInitialize failed: 0x%08X", ret);
        return false;
    }

    s_initialized = true;
    return true;
}

void appinst_fini(void)
{
    if (!s_initialized) {
        return;
    }
    sceAppInstUtilTerminate();
    s_initialized = false;
}

static bool check_ready(int* error)
{
    if (!s_initialized) {
        if (error) {
            *error = 0x80010002; /* ENXIO */
        }
        return false;
    }
    return true;
}

bool appinst_uninstall_game(const char* title_id, int* error)
{
    int ret;

    if (!check_ready(error)) {
        return false;
    }
    if (!title_id) {
        if (error) *error = 0x80010016;
        return false;
    }

    ret = sceAppInstUtilAppUnInstall(title_id);
    if (ret != 0) {
        LOG_ERROR("sceAppInstUtilAppUnInstall failed: 0x%08X", ret);
        if (error) *error = ret;
        return false;
    }
    return true;
}

bool appinst_uninstall_patch(const char* title_id, int* error)
{
    int ret;

    if (!check_ready(error)) {
        return false;
    }
    if (!title_id) {
        if (error) *error = 0x80010016;
        return false;
    }

    ret = sceAppInstUtilAppUnInstallPat(title_id);
    if (ret != 0) {
        LOG_ERROR("sceAppInstUtilAppUnInstallPat failed: 0x%08X", ret);
        if (error) *error = ret;
        return false;
    }
    return true;
}

bool appinst_uninstall_ac(const char* content_id, int* error)
{
    pkg_content_id_info_t info;
    int ret;

    if (!check_ready(error)) {
        return false;
    }
    if (!content_id || !pkg_parse_content_id(content_id, &info)) {
        if (error) *error = 0x80010016;
        return false;
    }

    ret = sceAppInstUtilAppUnInstallAddcont(info.title_id, info.label);
    if (ret != 0) {
        LOG_ERROR("sceAppInstUtilAppUnInstallAddcont failed: 0x%08X", ret);
        if (error) *error = ret;
        return false;
    }
    return true;
}

bool appinst_uninstall_theme(const char* content_id, int* error)
{
    int ret;

    if (!check_ready(error)) {
        return false;
    }
    if (!content_id) {
        if (error) *error = 0x80010016;
        return false;
    }

    ret = sceAppInstUtilAppUnInstallTheme(content_id);
    if (ret != 0) {
        LOG_ERROR("sceAppInstUtilAppUnInstallTheme failed: 0x%08X", ret);
        if (error) *error = ret;
        return false;
    }
    return true;
}

bool appinst_is_exists(const char* title_id, bool* exists, uint64_t* size_out,
                       int* error)
{
    int flag = 0;
    int ret;

    if (!check_ready(error)) {
        return false;
    }
    if (!title_id || !exists) {
        if (error) *error = 0x80010016;
        return false;
    }

    *exists = false;
    if (size_out) {
        *size_out = 0;
    }

    ret = sceAppInstUtilAppExists(title_id, &flag);
    if (ret != 0) {
        LOG_ERROR("sceAppInstUtilAppExists failed: 0x%08X", ret);
        if (error) *error = ret;
        return false;
    }
    if (!flag) {
        return true;
    }
    *exists = true;

    if (size_out) {
        unsigned long size = 0;
        ret = sceAppInstUtilAppGetSize(title_id, &size);
        if (ret != 0) {
            LOG_WARN("sceAppInstUtilAppGetSize failed: 0x%08X", ret);
        } else {
            *size_out = (uint64_t)size;
        }
    }
    return true;
}
