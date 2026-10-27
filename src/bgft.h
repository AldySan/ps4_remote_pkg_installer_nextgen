/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * BGFT (Background Function Task) download API — app-facing declarations.
 *
 * Struct layouts and prototypes follow the BGFT ABI as documented in
 * flatz's ps4_stub_lib_maker_v2 (include/bgft.h), which matches the
 * original "Remote Package Installer" build. Note: length/size fields are
 * `unsigned long` (64-bit on PS4) — do not narrow them to 32-bit types.
 *
 * The toolchain's stock libSceBgft stub only exports the VSH-facing
 * sceBgftService* functions, so this project builds its own stub shared
 * object (thirdparty/stubs) that exports the app-facing names below.
 * The runtime loader resolves them from libSceBgft.sprx by name-derived
 * NID, exactly like every other import.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_BGFT_ERROR_SAME_APPLICATION_ALREADY_INSTALLED (0x80990088)
#define SCE_BGFT_INVALID_TASK_ID (-1)
#define SCE_BGFT_HEAP_SIZE (1 * 1024 * 1024)

typedef int32_t SceBgftTaskId;

enum SceBgftTaskSubType {
    SCE_BGFT_TASK_SUB_TYPE_UNKNOWN     = 0,
    SCE_BGFT_TASK_SUB_TYPE_PHOTO       = 1,
    SCE_BGFT_TASK_SUB_TYPE_MUSIC       = 2,
    SCE_BGFT_TASK_SUB_TYPE_VIDEO       = 3,
    SCE_BGFT_TASK_SUB_TYPE_MARLIN_VIDEO = 4,
    SCE_BGFT_TASK_SUB_TYPE_UPDATA_ORBIS = 5,
    SCE_BGFT_TASK_SUB_TYPE_GAME        = 6,
    SCE_BGFT_TASK_SUB_TYPE_GAME_AC     = 7,
    SCE_BGFT_TASK_SUB_TYPE_GAME_PATCH  = 8,
    SCE_BGFT_TASK_SUB_TYPE_GAME_LICENSE = 9,
    SCE_BGFT_TASK_SUB_TYPE_SAVE_DATA   = 10,
    SCE_BGFT_TASK_SUB_TYPE_CRASH_REPORT = 11,
    SCE_BGFT_TASK_SUB_TYPE_PACKAGE     = 12,
    SCE_BGFT_TASK_SUB_TYPE_MAX         = 13,
};

enum SceBgftTaskOpt {
    SCE_BGFT_TASK_OPT_NONE                     = 0x0,
    SCE_BGFT_TASK_OPT_DELETE_AFTER_UPLOAD      = 0x1,
    SCE_BGFT_TASK_OPT_INVISIBLE                = 0x2,
    SCE_BGFT_TASK_OPT_ENABLE_PLAYGO            = 0x4,
    SCE_BGFT_TASK_OPT_FORCE_UPDATE             = 0x8,
    SCE_BGFT_TASK_OPT_REMOTE                   = 0x10,
    SCE_BGFT_TASK_OPT_COPY_CRASH_REPORT_FILES  = 0x20,
    SCE_BGFT_TASK_OPT_DISABLE_INSERT_POPUP     = 0x40,
    SCE_BGFT_TASK_OPT_INTERNAL                 = 0x80,
    SCE_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM  = 0x10000,
};

typedef struct SceBgftInitParams {
    void* heap;
    size_t heapSize;
} SceBgftInitParams;

typedef struct SceBgftDownloadRegisterErrorInfo {
    uint8_t buf[0x100];
} SceBgftDownloadRegisterErrorInfo;

typedef struct SceBgftDownloadParam {
    int32_t userId;
    int32_t entitlementType;
    const char* id;                /* content id, max 0x30 */
    const char* contentUrl;        /* max 0x800 */
    const char* contentExUrl;
    const char* contentName;       /* max 0x259 */
    const char* iconPath;          /* max 0x800 */
    const char* skuId;
    enum SceBgftTaskOpt option;
    const char* playgoScenarioId;
    const char* releaseDate;
    const char* packageType;
    const char* packageSubType;
    unsigned long packageSize;
} SceBgftDownloadParam;

typedef struct SceBgftDownloadParamEx {
    SceBgftDownloadParam params;
    uint32_t slot;
} SceBgftDownloadParamEx;

typedef struct SceBgftDownloadTaskInfo {
    char* contentTitle;
    char* iconPath;
    unsigned long notificationUtcTick;
} SceBgftDownloadTaskInfo;

typedef struct SceBgftTaskProgress {
    uint32_t bits;
    int32_t errorResult;
    unsigned long length;
    unsigned long transferred;
    unsigned long lengthTotal;
    unsigned long transferredTotal;
    uint32_t numIndex;
    uint32_t numTotal;
    uint32_t restSec;
    uint32_t restSecTotal;
    int32_t preparingPercent;
    int32_t localCopyPercent;
} SceBgftTaskProgress;

/* Initialization / teardown. */
int sceBgftInitialize(SceBgftInitParams* params);
int sceBgftFinalize(void);

/* Task registration (regular vs debug/fake pkg). */
int sceBgftDownloadRegisterTask(SceBgftDownloadParam* params, SceBgftTaskId* taskId);
int sceBgftDebugDownloadRegisterPkg(SceBgftDownloadParam* params, SceBgftTaskId* taskId);
int sceBgftDownloadRegisterTaskStoreWithErrorInfo(SceBgftDownloadParam* params,
                                                  SceBgftTaskId* taskId,
                                                  SceBgftDownloadRegisterErrorInfo* errorInfo);
int sceBgftDownloadUnregisterTask(SceBgftTaskId taskId);
int sceBgftDownloadReregisterTaskPatch(SceBgftTaskId oldTaskId, SceBgftTaskId* newTaskId);

/* Task control. */
int sceBgftDownloadStartTask(SceBgftTaskId taskId);
int sceBgftDownloadStopTask(SceBgftTaskId taskId);
int sceBgftDownloadPauseTask(SceBgftTaskId taskId);
int sceBgftDownloadResumeTask(SceBgftTaskId taskId);

/* Progress / lookup. */
int sceBgftDownloadGetTaskInfo(SceBgftTaskId taskId, SceBgftDownloadTaskInfo* taskInfo);
int sceBgftDownloadGetProgress(SceBgftTaskId taskId, SceBgftTaskProgress* progress);
int sceBgftDownloadFindActiveTask(const char* contentId,
                                  enum SceBgftTaskSubType subType,
                                  SceBgftTaskId* taskId);

/* VSH-facing find used by the legacy /api/find_task endpoint. */
int sceBgftServiceDownloadFindTaskByContentId(const char* contentId,
                                              enum SceBgftTaskSubType subType,
                                              SceBgftTaskId* taskId);

#ifdef __cplusplus
}
#endif
