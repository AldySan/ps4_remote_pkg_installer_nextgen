/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * BGFT download wrappers: task registration with legacy semantics
 * (already-installed reports task id -1 as success), control ops and
 * progress snapshots.
 */
#include "bgft.h"
#include "log.h"

#include <orbis/UserService.h>

#include <stdlib.h>
#include <string.h>

static bool s_initialized = false;
static SceBgftInitParams s_init_params;

bool bgft_init(void)
{
    int ret;

    if (s_initialized) {
        return true;
    }

    memset(&s_init_params, 0, sizeof(s_init_params));
    s_init_params.heapSize = SCE_BGFT_HEAP_SIZE;
    s_init_params.heap = (uint8_t*)malloc(s_init_params.heapSize);
    if (!s_init_params.heap) {
        LOG_ERROR("bgft: out of memory for heap");
        return false;
    }
    memset(s_init_params.heap, 0, s_init_params.heapSize);

    ret = sceBgftInitialize(&s_init_params);
    if (ret != 0) {
        LOG_ERROR("sceBgftInitialize failed: 0x%08X", ret);
        free(s_init_params.heap);
        s_init_params.heap = NULL;
        s_init_params.heapSize = 0;
        return false;
    }

    s_initialized = true;
    return true;
}

void bgft_fini(void)
{
    if (!s_initialized) {
        return;
    }

    sceBgftFinalize();

    free(s_init_params.heap);
    memset(&s_init_params, 0, sizeof(s_init_params));

    s_initialized = false;
}

int bgft_get_user_id(void)
{
    int user_id = -1;
    int ret;

    ret = sceUserServiceGetForegroundUser(&user_id);
    if (ret != 0) {
        LOG_ERROR("sceUserServiceGetForegroundUser failed: 0x%08X", ret);
        return -1;
    }
    return user_id;
}

bool bgft_register(const char* content_id, const char* content_url,
                   const char* content_name, const char* icon_path,
                   const char* package_type, const char* package_sub_type,
                   uint64_t package_size, bool is_patch,
                   int* out_task_id, int* error)
{
    SceBgftDownloadParam params;
    SceBgftTaskId task_id = SCE_BGFT_INVALID_TASK_ID;
    int ret;

    if (!s_initialized) {
        ret = 0x80010002; /* ENXIO-ish */
        goto err;
    }
    if (!content_id || !content_url || !content_name || !package_type) {
        ret = 0x80010016; /* EINVAL */
        goto err;
    }

    memset(&params, 0, sizeof(params));
    params.userId = bgft_get_user_id();
    if (params.userId < 0) {
        ret = 0x80010002;
        goto err;
    }
    params.entitlementType = 5; /* same as the original application */
    params.id = content_id;
    params.contentUrl = content_url;
    params.contentName = content_name;
    params.iconPath = icon_path ? icon_path : "";
    params.playgoScenarioId = "0";
    params.option = SCE_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM;
    params.packageType = package_type;
    params.packageSubType = package_sub_type ? package_sub_type : "";
    params.packageSize = (unsigned long)package_size;

    if (!is_patch) {
        ret = sceBgftDownloadRegisterTask(&params, &task_id);
    } else {
        ret = sceBgftDebugDownloadRegisterPkg(&params, &task_id);
    }
    if (ret != 0) {
        if (ret == (int)SCE_BGFT_ERROR_SAME_APPLICATION_ALREADY_INSTALLED) {
            /* Legacy behaviour: report success with task id -1. */
            task_id = -1;
            goto done;
        }
        LOG_ERROR("bgft register failed: 0x%08X", ret);
        goto err;
    }

done:
    if (out_task_id) {
        *out_task_id = (int)task_id;
    }
    return true;

err:
    if (error) {
        *error = ret;
    }
    if (out_task_id) {
        *out_task_id = SCE_BGFT_INVALID_TASK_ID;
    }
    return false;
}

bool bgft_start_task(int task_id, int* error)
{
    int ret;

    if (!s_initialized) {
        if (error) *error = 0x80010002;
        return false;
    }
    ret = sceBgftDownloadStartTask((SceBgftTaskId)task_id);
    if (ret != 0) {
        if (error) *error = ret;
        return false;
    }
    return true;
}

bool bgft_pause_task(int task_id, int* error)
{
    int ret;

    if (!s_initialized) {
        if (error) *error = 0x80010002;
        return false;
    }
    ret = sceBgftDownloadPauseTask((SceBgftTaskId)task_id);
    if (ret != 0) {
        if (error) *error = ret;
        return false;
    }
    return true;
}

bool bgft_resume_task(int task_id, int* error)
{
    int ret;

    if (!s_initialized) {
        if (error) *error = 0x80010002;
        return false;
    }
    ret = sceBgftDownloadResumeTask((SceBgftTaskId)task_id);
    if (ret != 0) {
        if (error) *error = ret;
        return false;
    }
    return true;
}

bool bgft_stop_task(int task_id, int* error)
{
    int ret;

    if (!s_initialized) {
        if (error) *error = 0x80010002;
        return false;
    }
    ret = sceBgftDownloadStopTask((SceBgftTaskId)task_id);
    if (ret != 0) {
        if (error) *error = ret;
        return false;
    }
    return true;
}

bool bgft_unregister_task(int task_id, int* error)
{
    int ret;

    if (!s_initialized) {
        if (error) *error = 0x80010002;
        return false;
    }
    ret = sceBgftDownloadUnregisterTask((SceBgftTaskId)task_id);
    if (ret != 0) {
        if (error) *error = ret;
        return false;
    }
    return true;
}

bool bgft_reregister_patch(int old_task_id, int* new_task_id, int* error)
{
    SceBgftTaskId task_id = SCE_BGFT_INVALID_TASK_ID;
    int ret;

    if (!s_initialized || !new_task_id) {
        if (error) *error = 0x80010002;
        return false;
    }
    ret = sceBgftDownloadReregisterTaskPatch((SceBgftTaskId)old_task_id, &task_id);
    if (ret != 0) {
        if (error) *error = ret;
        return false;
    }
    *new_task_id = (int)task_id;
    return true;
}

bool bgft_get_progress(int task_id, SceBgftTaskProgress* progress, int* error)
{
    int ret;

    if (!s_initialized || !progress) {
        if (error) *error = 0x80010002;
        return false;
    }
    memset(progress, 0, sizeof(*progress));
    ret = sceBgftDownloadGetProgress((SceBgftTaskId)task_id, progress);
    if (ret != 0) {
        if (error) *error = ret;
        return false;
    }
    return true;
}

bool bgft_find_task(const char* content_id, int sub_type, int* task_id, int* error)
{
    SceBgftTaskId id = SCE_BGFT_INVALID_TASK_ID;
    int ret;

    if (!s_initialized || !content_id || !task_id) {
        if (error) *error = 0x80010016;
        return false;
    }
    ret = sceBgftServiceDownloadFindTaskByContentId(
        content_id, (enum SceBgftTaskSubType)sub_type, &id);
    if (ret != 0) {
        if (error) *error = ret;
        return false;
    }
    *task_id = (int)id;
    return true;
}
