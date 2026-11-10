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
#include "pad.h"
#include "proxy.h"
#include "server.h"
#include "storage.h"
#include "taskman.h"
#include "ui.h"
#include "util.h"

#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/_types/pad.h>
#include <orbis/_types/sysmodule.h>
#include <orbis/UserService.h>

#include <SDL2/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

size_t sceLibcHeapSize = 256 * 1024 * 1024;

static SDL_Window* s_window = NULL;
static SDL_Renderer* s_renderer = NULL;
static bool s_ui_ok = false;   /* false = running headless (API only) */

static bool s_running = true;

/* The process may arrive with libSceUserService already initialized (the
 * loader or the environment can set it up before main() runs). That is a
 * healthy state to run in, not a failure: the established console input
 * helpers treat this return code as benign and keep going. */
#define SCE_USER_SERVICE_ERROR_ALREADY_INITIALIZED ((int)0x80960003)

/* True only when this app performed the user service initialization, so the
 * shutdown path never terminates a service it does not own. */
static bool s_user_service_owned = false;

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

/* Orbis pad buttons onto the button ids the dashboard consumes. */
static const struct {
    uint32_t orbis;
    int sdl;
} PAD_MAP[] = {
    { ORBIS_PAD_BUTTON_UP,       SDL_CONTROLLER_BUTTON_DPAD_UP },
    { ORBIS_PAD_BUTTON_DOWN,     SDL_CONTROLLER_BUTTON_DPAD_DOWN },
    { ORBIS_PAD_BUTTON_LEFT,     SDL_CONTROLLER_BUTTON_DPAD_LEFT },
    { ORBIS_PAD_BUTTON_RIGHT,    SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
    { ORBIS_PAD_BUTTON_CROSS,    SDL_CONTROLLER_BUTTON_A },
    { ORBIS_PAD_BUTTON_CIRCLE,   SDL_CONTROLLER_BUTTON_B },
    { ORBIS_PAD_BUTTON_SQUARE,   SDL_CONTROLLER_BUTTON_X },
    { ORBIS_PAD_BUTTON_TRIANGLE, SDL_CONTROLLER_BUTTON_Y },
};

/*
 * One frame of local input: the pad is polled directly (scePadReadState
 * under pad_poll) instead of going through SDL's joystick subsystem, whose
 * initialization reaches into the user service. Every subsystem SDL needs
 * to start is one more thing that can fail on launch; the pad service is
 * the single dependency left for local input.
 */
static void handle_pad_frame(void)
{
    uint32_t pressed = pad_poll();
    size_t i;

    if (pressed & ORBIS_PAD_BUTTON_OPTIONS) {
        s_running = false; /* OPTIONS exits */
        return;
    }

    for (i = 0; i < sizeof(PAD_MAP) / sizeof(PAD_MAP[0]); ++i) {
        if (pressed & PAD_MAP[i].orbis) {
            ui_handle_button(PAD_MAP[i].sdl);
        }
    }
}

static void draw_frame(void)
{
    if (s_ui_ok) {
        ui_frame(s_renderer);
    }
}

int main(void)
{
    int ret;
    int user_id = -1;

    /* Kernel log sink is always available; UDP sink activates when network is up. */
    dbglogger_init();

    LOG_INFO("%s v%s starting", APP_NAME, APP_VERSION);

    if (!modules_load()) {
        LOG_ERROR("failed to load system modules");
        goto err;
    }

    ret = sceUserServiceInitialize(NULL);
    if (ret != 0 && ret != SCE_USER_SERVICE_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("sceUserServiceInitialize failed: 0x%08X", ret);
        goto err_modules_unload;
    }
    s_user_service_owned = (ret == 0);

    if (sceUserServiceGetInitialUser(&user_id) != 0) {
        LOG_WARN("userService: no initial user; local input disabled");
        user_id = -1;
    }

    /*
     * Local input is opened before the UI so a missing pad never stands
     * between the user and the app. Failure is not fatal: control stays
     * available through the web dashboard.
     */
    if (user_id >= 0 && !pad_init(user_id)) {
        LOG_WARN("pad unavailable; control continues through the web dashboard");
    }

    if (SDL_Init(SDL_INIT_VIDEO) == 0) {
        s_window = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1920, 1080, 0);
        if (s_window) {
            s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!s_renderer) {
                /*
                 * The toolchain's SDL2 build registers exactly one driver:
                 * a GLES2 renderer backed by Piglet (the console's OpenGL ES
                 * module), so the request above selects the GPU and presents
                 * with vsync. There is no CPU fallback tier in this build:
                 * the software renderer is not registered and the fork's
                 * video core implements no window framebuffer anyway, so the
                 * retry below just re-asks GLES2 without flag requirements.
                 * When that fails as well, the app degrades to headless
                 * below and the web dashboard keeps serving installs.
                 */
                s_renderer = SDL_CreateRenderer(s_window, -1, 0);
            }
            if (s_renderer && ui_init(s_renderer)) {
                s_ui_ok = true;
            }
        }
    }

    if (!s_ui_ok) {
        /*
         * The dashboard is the local view of the app, not its life
         * support. When any video-side step fails (SDL_Init, window,
         * renderer, font atlas) the app degrades to running headless
         * instead of aborting: the HTTP API and the web dashboard keep
         * serving installs, and OPTIONS on the pad still exits. Tearing
         * the partial video state down keeps the shutdown path simple.
         */
        char reason[128];
        snprintf(reason, sizeof(reason), "%s", SDL_GetError());
        if (s_renderer) {
            SDL_DestroyRenderer(s_renderer);
            s_renderer = NULL;
        }
        if (s_window) {
            SDL_DestroyWindow(s_window);
            s_window = NULL;
        }
        if (SDL_WasInit(SDL_INIT_VIDEO)) {
            SDL_Quit();
        }
        if (reason[0]) {
            LOG_WARN("ui unavailable (%s); running headless, use the web dashboard", reason);
        } else {
            LOG_WARN("ui unavailable; running headless, use the web dashboard");
        }
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

    if (s_ui_ok) {
        LOG_INFO("dashboard running");
    } else {
        LOG_INFO("running headless (web dashboard only)");
    }

    while (s_running) {
        handle_pad_frame();
        if (s_ui_ok) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) {
                    s_running = false;
                }
            }
            draw_frame();
        } else {
            usleep(100 * 1000); /* 10 Hz is plenty when nobody is watching */
        }
    }

    LOG_INFO("shutting down");

    /* Stop services first. ui_fini() destroys the icon textures and the
     * font atlas, which must happen while their renderer still exists:
     * SDL_DestroyRenderer frees every texture it created, and SDL_Quit()
     * tears the subsystem down entirely: freeing them afterwards (as the
     * old order did) is a use-after-free at exit. */
    server_stop();
    if (s_ui_ok) {
        ui_fini();
    }
    taskman_stop();
    bgft_fini();
    appinst_fini();
    netmon_stop();

    if (s_ui_ok) {
        SDL_DestroyRenderer(s_renderer);
        SDL_DestroyWindow(s_window);
        SDL_Quit();
    }
    pad_fini();

    if (s_user_service_owned) {
        sceUserServiceTerminate();
    }
    modules_unload();

    dbglogger_stop();

    return 0;

err_modules_unload:
    modules_unload();

err:
    dbglogger_stop();
    return 1;
}
