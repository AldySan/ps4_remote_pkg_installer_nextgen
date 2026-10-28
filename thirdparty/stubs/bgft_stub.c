/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Custom libSceBgft stub shared object.
 *
 * The stock OpenOrbis toolchain stub only exports the VSH-facing
 * sceBgftService* functions. This project also needs the app-facing
 * sceBgftDownload* family, so we build our own stub .so exporting every
 * BGFT symbol the application imports. Dummy bodies are never executed:
 * at load time the runtime resolves each import by name-derived NID
 * against the real libSceBgft.sprx (loaded via sceSysmoduleLoadModuleInternal),
 * exactly as it does for every other stub import.
 *
 * Keep this list in sync with src/bgft.h.
 */

#define BGFT_STUB(name) \
    void name(void);    \
    void name(void)     \
    {                   \
        for (;;) {      \
        }               \
    }

BGFT_STUB(sceBgftInitialize)
BGFT_STUB(sceBgftFinalize)

BGFT_STUB(sceBgftDownloadRegisterTask)
BGFT_STUB(sceBgftDebugDownloadRegisterPkg)
BGFT_STUB(sceBgftDownloadRegisterTaskStoreWithErrorInfo)
BGFT_STUB(sceBgftDownloadUnregisterTask)
BGFT_STUB(sceBgftDownloadReregisterTaskPatch)

BGFT_STUB(sceBgftDownloadStartTask)
BGFT_STUB(sceBgftDownloadStopTask)
BGFT_STUB(sceBgftDownloadPauseTask)
BGFT_STUB(sceBgftDownloadResumeTask)

BGFT_STUB(sceBgftDownloadGetTaskInfo)
BGFT_STUB(sceBgftDownloadGetProgress)
BGFT_STUB(sceBgftDownloadFindActiveTask)

BGFT_STUB(sceBgftServiceDownloadFindTaskByContentId)
