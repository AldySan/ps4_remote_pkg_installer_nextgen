/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * System module load/unload (mirrors the original application).
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

static bool s_loaded = false;

bool modules_load(void)
{
    size_t i;

    if (s_loaded) {
        return true;
    }

    for (i = 0; i < sizeof(s_module_ids) / sizeof(s_module_ids[0]); i++) {
        int ret = sceSysmoduleLoadModuleInternal(s_module_ids[i]);
        if (ret != 0) {
            LOG_ERROR("sceSysmoduleLoadModuleInternal(0x%08X) failed: 0x%08X", s_module_ids[i], ret);
            goto err_unload;
        }
    }

    s_loaded = true;
    return true;

err_unload:
    while (i-- > 0) {
        int ret = sceSysmoduleUnloadModuleInternal(s_module_ids[i]);
        if (ret != 0) {
            LOG_ERROR("sceSysmoduleUnloadModuleInternal(0x%08X) failed: 0x%08X", s_module_ids[i], ret);
        }
    }
    return false;
}

void modules_unload(void)
{
    size_t i;

    if (!s_loaded) {
        return;
    }

    for (i = sizeof(s_module_ids) / sizeof(s_module_ids[0]); i-- > 0; ) {
        int ret = sceSysmoduleUnloadModuleInternal(s_module_ids[i]);
        if (ret != 0) {
            LOG_ERROR("sceSysmoduleUnloadModuleInternal(0x%08X) failed: 0x%08X", s_module_ids[i], ret);
        }
    }

    s_loaded = false;
}
