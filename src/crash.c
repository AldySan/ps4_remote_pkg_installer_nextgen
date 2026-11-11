/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Fault recovery net.
 *
 * Hardware faults on the PS4 reach handlers installed through
 * sceKernelInstallExceptionHandler, not sigaction handlers, so every
 * service thread arms its own recovery point here and a fault turns into
 * a jump back into that thread instead of a killed process. A thread that
 * never armed one, or the main thread faulting past its strike limit,
 * takes the app down cleanly with a notification on the way out.
 */

#include "crash.h"
#include "log.h"
#include "notify.h"
#include "version.h"

#include <orbis/libkernel.h>
#include <pthread.h>
#include <signal.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* One slot per armed thread. The worst case already sums past 32:
 * 16 server connections + two prefetch streams x 8 workers + the
 * long-lived services (server, taskman, netmon, proxy, UI) + main.
 * A thread that fails to register loses the recovery net entirely,
 * so its first fault ends the app; 64 leaves headroom for that mix. */
#define CRASH_MAX_THREADS 64

/* The kernel takes FreeBSD signal numbers while the toolchain headers
 * carry Linux ones (SIGBUS is 10 on the console, 7 in the headers). */
#define CRASH_SIGILL  4
#define CRASH_SIGFPE  8
#define CRASH_SIGBUS  10
#define CRASH_SIGSEGV 11

/* PS4 ucontext: 16-byte mask, then a FreeBSD amd64 mcontext. The length
 * field gates the fault-address read so a layout surprise logs zeros
 * rather than garbage from a wrong offset. */
#define CRASH_MC_OFF   64
#define CRASH_MC_LEN   200
#define CRASH_MC_ADDR  136
#define CRASH_MC_RIP   160

#define CRASH_MAIN_STRIKE_LIMIT 3

struct crash_slot {
    pthread_t tid;
    sigjmp_buf* buf;
    bool used;
};

/* Exported by the toolchain's libkernel stub but missing from its headers. */
int sceKernelInstallExceptionHandler(int signum, void (*handler)(int, void*));

/* Registration is deliberately lock-free: a slot is fully written before
 * its used flag, entries are only ever scanned, and the handler may run
 * while the heap is broken. */
static struct crash_slot s_slots[CRASH_MAX_THREADS];
static pthread_t s_main_thread;
static volatile sig_atomic_t s_in_handler = 0;
static volatile sig_atomic_t s_main_strikes = 0;
static volatile sig_atomic_t s_pending_sig = 0;

static uint64_t crash_read_u64(const void* base, size_t off)
{
    uint64_t v;
    memcpy(&v, (const unsigned char*)base + off, sizeof(v));
    return v;
}

static struct crash_slot* crash_find_slot(void)
{
    pthread_t self = pthread_self();
    int i;

    for (i = 0; i < CRASH_MAX_THREADS; ++i) {
        if (s_slots[i].used && pthread_equal(s_slots[i].tid, self)) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static void crash_note_last_words(void)
{
    OrbisNotificationRequest req;

    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;
    strlcpy(req.message, APP_SHORT_NAME ": closing after an internal error",
            sizeof(req.message));
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void crash_handler(int sig, void* uc)
{
    uint64_t mc_len = 0;
    uint64_t fault_addr = 0;
    uint64_t fault_rip = 0;
    struct crash_slot* slot;
    char line[128];

    if (s_in_handler) {
        _exit(0);
    }
    s_in_handler = 1;

    if (uc) {
        mc_len = crash_read_u64(uc, CRASH_MC_OFF + CRASH_MC_LEN);
        if (mc_len >= 256 && mc_len <= 8192 && (mc_len % 16) == 0) {
            fault_addr = crash_read_u64(uc, CRASH_MC_OFF + CRASH_MC_ADDR);
            fault_rip = crash_read_u64(uc, CRASH_MC_OFF + CRASH_MC_RIP);
        }
    }

    snprintf(line, sizeof(line),
             "[WARN] fault sig=%d addr=0x%llx rip=0x%llx; recovering\n",
             sig, (unsigned long long)fault_addr, (unsigned long long)fault_rip);
    sceKernelDebugOutText(0, line);

    slot = crash_find_slot();
    if (!slot) {
        crash_note_last_words();
        _exit(0);
    }

    if (pthread_equal(pthread_self(), s_main_thread)) {
        if (++s_main_strikes > CRASH_MAIN_STRIKE_LIMIT) {
            crash_note_last_words();
            _exit(0);
        }
    }

    s_pending_sig = sig;
    s_in_handler = 0;
    siglongjmp(*slot->buf, 1);
}

void crash_init(void)
{
    static const int signals[] = {
        CRASH_SIGILL, CRASH_SIGFPE, CRASH_SIGBUS, CRASH_SIGSEGV,
    };
    size_t i;

    s_main_thread = pthread_self();

    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        int ret = sceKernelInstallExceptionHandler(signals[i], crash_handler);
        if (ret != 0) {
            LOG_WARN("exception handler for signal %d failed: 0x%08X", signals[i], ret);
        }
    }
}

void crash_arm(sigjmp_buf* buf)
{
    pthread_t self = pthread_self();
    int i;

    for (i = 0; i < CRASH_MAX_THREADS; ++i) {
        if (s_slots[i].used && pthread_equal(s_slots[i].tid, self)) {
            s_slots[i].buf = buf;
            return;
        }
    }
    for (i = 0; i < CRASH_MAX_THREADS; ++i) {
        if (!s_slots[i].used) {
            s_slots[i].tid = self;
            s_slots[i].buf = buf;
            s_slots[i].used = true;
            return;
        }
    }
}

void crash_disarm(void)
{
    struct crash_slot* slot = crash_find_slot();

    if (slot) {
        slot->used = false;
    }
}

int crash_pending_signal(void)
{
    return s_pending_sig;
}
