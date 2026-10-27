/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "netmon.h"
#include "log.h"
#include "version.h"

#include <orbis/NetCtl.h>
#include <orbis/Net.h>

#include <pthread.h>
#include <unistd.h>
#include <string.h>

#define NETMON_POLL_INTERVAL_MS 1000
#define NETMON_HEAP_SIZE (1 * 1024 * 1024)
#define NETMON_MAX_SUBSCRIBERS 8

struct netmon_sub {
    bool active;
    netmon_callback_t cb;
    void* arg;
};

static pthread_t s_thread;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_started = false;
static bool s_up = false;
static bool s_stack_ready = false;
static int s_mem_id = -1;
static int s_netctl_state = NETMON_STATE_DISCONNECTED;
static struct netmon_sub s_subs[NETMON_MAX_SUBSCRIBERS];

static void stack_init(void)
{
    int ret;

    if (s_stack_ready) {
        return;
    }

    ret = sceNetCtlInit();
    if (ret != 0) {
        /* netctl init failing is unusual but not fatal; retry next cycle. */
        LOG_WARN("sceNetCtlInit failed: 0x%08X", ret);
        return;
    }

    ret = sceNetInit();
    if (ret != 0) {
        LOG_ERROR("sceNetInit failed: 0x%08X", ret);
        sceNetCtlTerm();
        return;
    }

    ret = sceNetPoolCreate(APP_SHORT_NAME "_net", NETMON_HEAP_SIZE, 0);
    if (ret < 0) {
        LOG_ERROR("sceNetPoolCreate failed: 0x%08X", ret);
        sceNetTerm();
        sceNetCtlTerm();
        return;
    }
    s_mem_id = ret;

    s_stack_ready = true;
    LOG_INFO("network stack ready (mem id %d)", s_mem_id);
}

static void stack_term(void)
{
    int mem_id;

    if (!s_stack_ready) {
        return;
    }
    s_stack_ready = false;
    mem_id = s_mem_id;
    s_mem_id = -1;

    sceNetPoolDestroy(mem_id);
    sceNetTerm();
    sceNetCtlTerm();
}

static void notify(bool up)
{
    int i;

    pthread_mutex_lock(&s_lock);
    for (i = 0; i < NETMON_MAX_SUBSCRIBERS; ++i) {
        if (s_subs[i].active && s_subs[i].cb) {
            s_subs[i].cb(s_subs[i].arg, up);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

static void set_up(bool up)
{
    if (s_up == up) {
        return;
    }
    s_up = up;
    if (up) {
        char ip[16] = { 0 };
        OrbisNetCtlInfo info;
        memset(&info, 0, sizeof(info));
        if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) == 0) {
            strlcpy(ip, info.ip_address, sizeof(ip));
        }
        LOG_INFO("network up: %s", ip[0] ? ip : "ip unknown");
    } else {
        LOG_WARN("network down");
    }
    notify(up);
}

static void* netmon_thread(void* arg)
{
    (void)arg;

    while (s_started) {
        int state = NETMON_STATE_DISCONNECTED;
        int ret;

        if (!s_stack_ready) {
            stack_init();
        }

        ret = sceNetCtlGetState(&state);
        if (ret == 0) {
            s_netctl_state = state;
            set_up(state == NETMON_STATE_IPOBTAINED);
        }

        usleep(NETMON_POLL_INTERVAL_MS * 1000);
    }
    return NULL;
}

bool netmon_start(void)
{
    int ret;

    if (s_started) {
        return true;
    }

    memset(s_subs, 0, sizeof(s_subs));
    s_started = true;

    ret = pthread_create(&s_thread, NULL, netmon_thread, NULL);
    if (ret != 0) {
        LOG_ERROR("pthread_create(netmon) failed: %d", ret);
        s_started = false;
        return false;
    }
    return true;
}

void netmon_stop(void)
{
    if (!s_started) {
        return;
    }
    s_started = false;
    pthread_join(s_thread, NULL);

    set_up(false);
    stack_term();
    memset(s_subs, 0, sizeof(s_subs));
}

int netmon_subscribe(netmon_callback_t cb, void* arg)
{
    int i;

    if (!cb) {
        return -1;
    }
    pthread_mutex_lock(&s_lock);
    for (i = 0; i < NETMON_MAX_SUBSCRIBERS; ++i) {
        if (!s_subs[i].active) {
            s_subs[i].active = true;
            s_subs[i].cb = cb;
            s_subs[i].arg = arg;
            pthread_mutex_unlock(&s_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return -1;
}

void netmon_unsubscribe(int sub_id)
{
    if (sub_id < 0 || sub_id >= NETMON_MAX_SUBSCRIBERS) {
        return;
    }
    pthread_mutex_lock(&s_lock);
    s_subs[sub_id].active = false;
    s_subs[sub_id].cb = NULL;
    s_subs[sub_id].arg = NULL;
    pthread_mutex_unlock(&s_lock);
}

bool netmon_is_up(void)
{
    return s_up;
}

bool netmon_get_ip(char* out, size_t outsz)
{
    OrbisNetCtlInfo info;

    if (!out || outsz == 0) {
        return false;
    }
    out[0] = '\0';

    if (!s_up) {
        return false;
    }
    memset(&info, 0, sizeof(info));
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) != 0) {
        return false;
    }
    strlcpy(out, info.ip_address, outsz);
    return out[0] != '\0';
}

int netmon_mem_id(void)
{
    return s_stack_ready ? s_mem_id : -1;
}
