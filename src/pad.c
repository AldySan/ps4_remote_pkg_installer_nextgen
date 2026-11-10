/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Based on "Remote Package Installer" by flatz.
 *
 * Local controller input, polled straight from the pad service.
 *
 * The pad is read with scePadReadState() once per frame and edge-detected in
 * software. This keeps the input path independent from SDL's joystick
 * subsystem, whose initialization interacts with the user service behind
 * SDL_Init(); the video side of SDL needs none of that. Long-lived PS4 UI
 * apps (Apollo Save Tool, PS4 Cheats Manager) drive their menus the same way.
 */

#include "pad.h"
#include "log.h"

#include <orbis/Pad.h>
#include <orbis/_types/pad.h>

#include <string.h>

/* Left-stick synthesis thresholds around the resting position (0x78). */
#define STICK_CENTER   0x78
#define STICK_THRESH   0x68
#define STICK_MIN      (STICK_CENTER - STICK_THRESH)
#define STICK_MAX      (STICK_CENTER + STICK_THRESH)

static int s_handle = -1;
static uint32_t s_last_buttons = 0;

bool pad_init(int user_id)
{
    OrbisPadData data;
    int ret;

    ret = scePadInit();
    if (ret < 0) {
        LOG_WARN("scePadInit failed: 0x%08X", ret);
        return false;
    }

    s_handle = scePadOpen(user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    if (s_handle < 0) {
        LOG_WARN("scePadOpen failed: 0x%08X", s_handle);
        return false;
    }

    /*
     * Baseline read so the first poll reports an edge only for buttons
     * pressed after startup, not for whatever state the pad happened to
     * be in when the handle was opened.
     */
    memset(&data, 0, sizeof(data));
    if (scePadReadState(s_handle, &data) == 0 && data.connected) {
        s_last_buttons = data.buttons;
    } else {
        s_last_buttons = 0;
    }

    LOG_INFO("pad: polling handle %d for user %d", s_handle, user_id);
    return true;
}

void pad_fini(void)
{
    if (s_handle >= 0) {
        scePadClose(s_handle);
        s_handle = -1;
    }
    s_last_buttons = 0;
}

uint32_t pad_poll(void)
{
    OrbisPadData data;
    uint32_t current;
    uint32_t pressed;

    if (s_handle < 0) {
        return 0;
    }

    memset(&data, 0, sizeof(data));
    if (scePadReadState(s_handle, &data) != 0 || !data.connected) {
        /* Disconnected pad: drop the baseline so a reconnect starts fresh. */
        s_last_buttons = 0;
        return 0;
    }

    current = data.buttons;

    /* The left stick scrolls the list as well, mirroring the d-pad. */
    if (data.leftStick.y < STICK_MIN) {
        current |= ORBIS_PAD_BUTTON_UP;
    }
    if (data.leftStick.y > STICK_MAX) {
        current |= ORBIS_PAD_BUTTON_DOWN;
    }
    if (data.leftStick.x < STICK_MIN) {
        current |= ORBIS_PAD_BUTTON_LEFT;
    }
    if (data.leftStick.x > STICK_MAX) {
        current |= ORBIS_PAD_BUTTON_RIGHT;
    }

    pressed = current & ~s_last_buttons;
    s_last_buttons = current;
    return pressed;
}
