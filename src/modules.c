/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * System module load/unload (mirrors the original application).
 *
 * Loading is fail-soft: an Orbis stub for a module that failed to load
 * never resolves, and calling into it hangs the process until the system
 * kills it, so the callers ask modules_is_loaded() before touching a
 * module's functions and the app keeps running on whatever did load.
 */

#include "modules.h"
#include "log.h"

#include <orbis/Sysmodule.h>
#include <orbis/_types/sysmodule.h>

static const uint32_t s_module_ids[] = {
    ORBIS_SYSMODULE_INTERNAL_SYSCORE,
    ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE,
    ORBIS_SYSMODULE_INTERNAL_USER_SERVICE,
    ORBIS_SYSMODULE_INTERNAL_NETCTL,
    ORBIS_SYSMODULE_INTERNAL_NET,
    ORBIS_SYSMODULE_INTERNAL_HTTP,
    ORBIS_SYSMODULE_INTERNAL_SSL,
    ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL,
    ORBIS_SYSMODULE_INTERNAL_BGFT,
    ORBIS_SYSMODULE_INTERNAL_NP_COMMON,
};

static bool s_module_loaded[sizeof(s_module_ids) / sizeof(s_module_ids[0])];
static bool s_any_unload = false;

bool modules_load(void)
{
    size_t i;
    bool all_loaded = true;

    for (i = 0; i < sizeof(s_module_ids) / sizeof(s_module_ids[0]); i++) {
        int ret = sceSysmoduleLoadModuleInternal(s_module_ids[i]);
        if (ret != 0) {
            LOG_ERROR("sceSysmoduleLoadModuleInternal(0x%08X) failed: 0x%08X",
                      s_module_ids[i], ret);
            all_loaded = false;
        } else {
            s_module_loaded[i] = true;
            s_any_unload = true;
        }
    }

    return all_loaded;
}

bool modules_is_loaded(uint32_t module_id)
{
    size_t i;

    for (i = 0; i < sizeof(s_module_ids) / sizeof(s_module_ids[0]); i++) {
        if (s_module_ids[i] == module_id) {
            return s_module_loaded[i];
        }
    }
    return false;
}

void modules_unload(void)
{
    size_t i;

    if (!s_any_unload) {
        return;
    }

    for (i = sizeof(s_module_ids) / sizeof(s_module_ids[0]); i-- > 0; ) {
        if (!s_module_loaded[i]) {
            continue;
        }
        int ret = sceSysmoduleUnloadModuleInternal(s_module_ids[i]);
        if (ret != 0) {
            LOG_ERROR("sceSysmoduleUnloadModuleInternal(0x%08X) failed: 0x%08X",
                      s_module_ids[i], ret);
        }
        s_module_loaded[i] = false;
    }

    s_any_unload = false;
}
