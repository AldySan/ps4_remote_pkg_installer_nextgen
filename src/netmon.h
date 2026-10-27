/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Network monitor: polls the netctl state and manages the net stack lifetime.
 *
 * This is the fix for the original application's offline crash: instead of
 * exiting when no IP is available, the app keeps running and simply waits
 * until the link comes up (and tears the stack down when it goes away).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Not declared by the toolchain's NetCtl.h, but exported by libSceNetCtl. */
int sceNetCtlGetState(int* state);

/*
 * State codes returned by sceNetCtlGetState.
 * (Values verified against firmware behaviour, see netmon.c.)
 */
#define NETMON_STATE_DISCONNECTED 0
#define NETMON_STATE_CONNECTING   1
#define NETMON_STATE_IPOBTAINING  2
#define NETMON_STATE_IPOBTAINED   3

typedef void (*netmon_callback_t)(void* arg, bool up);

/* Starts the monitor thread. Blocks ~instantly; polling happens in the
 * background. Returns false only on thread creation failure. */
bool netmon_start(void);

/* Stops the monitor thread and tears down the network stack. */
void netmon_stop(void);

/* Subscribes to link up/down notifications (invoked from the monitor
 * thread). Returns a subscription id or -1. */
int netmon_subscribe(netmon_callback_t cb, void* arg);

/* Unsubscribes a previously returned subscription id. */
void netmon_unsubscribe(int sub_id);

/* True while the link is up and an IP has been obtained. */
bool netmon_is_up(void);

/* Copies the current IPv4 address (dotted quad) into out. False if down. */
bool netmon_get_ip(char* out, size_t outsz);

/* Net pool memory id for sockets/http (valid only while up, else -1). */
int netmon_mem_id(void);
