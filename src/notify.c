/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * System notifications for errors the user would otherwise never see:
 * the request goes straight to the kernel, so it keeps working in every
 * degraded state of the app, exactly when it is needed.
 */

#include "notify.h"

#include <orbis/libkernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void notify_send(const char* fmt, ...)
{
    OrbisNotificationRequest req;
    va_list ap;

    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;

    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);

    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
