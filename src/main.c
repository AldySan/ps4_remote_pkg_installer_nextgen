/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Based on "Remote Package Installer" by flatz.
 *
 * Application entry point.
 */

#include "version.h"
#include "appinst.h"
#include "bgft.h"
#include "http_client.h"
#include "installer.h"
#include "log.h"
#include "modules.h"
#include "netmon.h"
#include "proxy.h"
#include "server.h"
#include "storage.h"
#include "taskman.h"
#include "ui.h"
#include "util.h"

#include <orbis/Sysmodule.h>
#include <orbis/_types/sysmodule.h>
#include <orbis/UserService.h>

#include <SDL2/SDL.h>

#include <stdint.h>
#include <stdio.h>

size_t sceLibcHeapSize = 256 * 1024 * 1024;

static SDL_Window* s_window = NULL;
static SDL_Renderer* s_renderer = NULL;
static SDL_GameController* s_controller = NULL;

static bool s_running = true;

static void on_netmon_event(void* arg, bool up)
{
    (void)arg;
    if (up) {
        if (!server_is_running()) {
            server_start(APP_DEFAULT_PORT);
        }
    } else {
        http_client_term();
        if (server_is_running()) {
            server_stop();
        }
    }
}

static void on_task_event(int job_id, task_state_t state, void* arg)
{
    (void)arg;
    switch (state) {
        case TASK_STATE_COMPLETED:
        case TASK_STATE_FAILED:
        case TASK_STATE_STOPPED:
        case TASK_STATE_ALREADY_INSTALLED:
            proxy_unregister(job_id);
            break;
        default:
            break;
    }
}

static void handle_event(const SDL_Event* ev)
{
    if (ev->type == SDL_QUIT) {
        s_running = false;
        return;
    }

    switch (ev->type) {
        case SDL_CONTROLLERDEVICEADDED:
            if (!s_controller) {
                s_controller = SDL_GameControllerOpen(ev->cdevice.which);
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (s_controller && ev->cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_controller))) {
                SDL_GameControllerClose(s_controller);
                s_controller = NULL;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                s_running = false; /* OPTIONS exits */
            } else {
                ui_handle_button(ev->cbutton.button);
            }
            break;
        default:
            break;
    }
}

static void draw_frame(void)
{
    ui_frame(s_renderer);
}

int main(void)
{
    int ret;

    /* Kernel log sink is always available; UDP sink activates when network is up. */
    dbglogger_init();

    LOG_INFO("%s v%s starting", APP_NAME, APP_VERSION);

    if (!modules_load()) {
        LOG_ERROR("failed to load system modules");
        goto err;
    }

    ret = sceUserServiceInitialize(NULL);
    if (ret != 0) {
        LOG_ERROR("sceUserServiceInitialize failed: 0x%08X", ret);
        goto err_modules_unload;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        goto err_user_service_terminate;
    }

    s_window = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1920, 1080, 0);
    if (!s_window) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        goto err_sdl_quit;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        LOG_ERROR("SDL_CreateRenderer failed: %s", SDL_GetError());
        goto err_window_destroy;
    }

    if (!ui_init(s_renderer)) {
        LOG_ERROR("ui_init failed: %s", SDL_GetError());
        goto err_window_destroy;
    }

    /*
     * Network monitor: keeps polling in the background. The app stays
     * alive with no network (the original build exited here) and services
     * start automatically once a link is available.
     */
    if (!netmon_start()) {
        LOG_WARN("netmon failed to start; continuing without network");
    }

    /* Install stack: AppInstUtil + BGFT + task registry with progress poller. */
    if (!appinst_init()) {
        LOG_WARN("appinst init failed; uninstall APIs disabled");
    }
    if (!bgft_init()) {
        LOG_WARN("bgft init failed; installs disabled");
    }
    if (!taskman_start()) {
        LOG_WARN("taskman failed to start; progress tracking disabled");
    }
    taskman_on_event(on_task_event, NULL);
    netmon_subscribe(on_netmon_event, NULL);
    if (netmon_is_up()) {
        on_netmon_event(NULL, true);
    }

    {
        uint64_t free_bytes = 0;
        char free_str[32];
        if (storage_get_free(&free_bytes)) {
            util_format_size(free_bytes, free_str, sizeof(free_str));
            LOG_INFO("storage: %s free on /data", free_str);
        } else {
            LOG_WARN("storage: free space unknown");
        }
    }

    LOG_INFO("dashboard running");

    while (s_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            handle_event(&ev);
        }
        draw_frame();
    }

    LOG_INFO("shutting down");

    if (s_controller) {
        SDL_GameControllerClose(s_controller);
    }
    SDL_DestroyRenderer(s_renderer);
    SDL_DestroyWindow(s_window);
    SDL_Quit();

    server_stop();
    ui_fini();
    taskman_stop();
    bgft_fini();
    appinst_fini();
    netmon_stop();

    sceUserServiceTerminate();
    modules_unload();

    dbglogger_stop();

    return 0;

err_window_destroy:
    SDL_DestroyWindow(s_window);
    s_window = NULL;

err_sdl_quit:
    SDL_Quit();

err_user_service_terminate:
    sceUserServiceTerminate();

err_modules_unload:
    modules_unload();

err:
    dbglogger_stop();
    return 1;
}
