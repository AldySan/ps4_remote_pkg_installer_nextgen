/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Fault recovery net.
 *
 * Hardware faults on the PS4 reach handlers installed through
 * sceKernelInstallExceptionHandler, not sigaction handlers. The handler
 * below must stay async-signal-safe: no allocation, no stdio, no locks,
 * because the fault may have interrupted the allocator or any lock
 * holder mid-update.
 *
 * Only the main thread jumps back to its armed recovery point (the
 * background safe mode). Any other thread leaves the app through
 * _exit(0) with the fault journaled first: resuming a worker thread on
 * possibly corrupted allocator or lock state is what used to surface as
 * the allocator __bin_chunk trap (sig=10) on a later, unrelated
 * malloc/free, so a worker fault is contained by a clean exit instead.
 */

#include "crash.h"
#include "diag.h"
#include "log.h"
#include "version.h"

#include <orbis/libkernel.h>
#include <pthread.h>
#include <signal.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Only long-lived recovery points register (main today); keep headroom
 * so future callers can arm without resizing the table. */
#define CRASH_MAX_THREADS 8

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

/* arm/disarm serialize on a plain static mutex; the handler scans the
 * table WITHOUT it (a fault inside arm/disarm must not deadlock on the
 * lock the faulting thread holds). The scan may miss a slot that is
 * mid-registration, which only costs the journal the thread name. */
static pthread_mutex_t s_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static struct crash_slot s_slots[CRASH_MAX_THREADS];
static pthread_t s_main_thread;
static volatile int s_journal_fd = -1;
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

static void crash_notify(const char* message)
{
    OrbisNotificationRequest req;

    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;
    strlcpy(req.message, message, sizeof(req.message));
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void crash_handler(int sig, void* uc)
{
    uint64_t mc_len = 0;
    uint64_t fault_addr = 0;
    uint64_t fault_rip = 0;
    struct crash_slot* slot;
    bool is_main;
    char line[192];
    char note[128];
    int len;

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

    slot = crash_find_slot();
    is_main = slot && pthread_equal(pthread_self(), s_main_thread);

    /* Persist the fault before any recovery or exit: the journal survives
     * the process, so a crash on a real console leaves evidence even when
     * nothing else does. Raw write() on a pre-opened fd only - fopen and
     * friends allocate, which is illegal here. */
    len = snprintf(line, sizeof(line),
                   "%s fault sig=%d addr=0x%llx rip=0x%llx at %s\n",
                   is_main ? "main" : (slot ? "worker" : "unregistered"),
                   sig, (unsigned long long)fault_addr,
                   (unsigned long long)fault_rip, diag_step());
    sceKernelDebugOutText(0, line);
    if (s_journal_fd >= 0 && len > 0) {
        write(s_journal_fd, line, (size_t)len);
    }

    if (!is_main) {
        snprintf(note, sizeof(note),
                 "%s: internal error (sig=%d); see the crash journal; closing",
                 APP_SHORT_NAME, sig);
        crash_notify(note);
        _exit(0);
    }
    if (++s_main_strikes > CRASH_MAIN_STRIKE_LIMIT) {
        snprintf(note, sizeof(note),
                 "%s: too many internal errors; closing", APP_SHORT_NAME);
        crash_notify(note);
        _exit(0);
    }

    snprintf(note, sizeof(note), "%s (recovered)", line);
    crash_notify(note);
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
    s_journal_fd = diag_crash_fd();

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

    pthread_mutex_lock(&s_slot_lock);
    for (i = 0; i < CRASH_MAX_THREADS; ++i) {
        if (s_slots[i].used && pthread_equal(s_slots[i].tid, self)) {
            s_slots[i].buf = buf;
            pthread_mutex_unlock(&s_slot_lock);
            return;
        }
    }
    for (i = 0; i < CRASH_MAX_THREADS; ++i) {
        if (!s_slots[i].used) {
            s_slots[i].tid = self;
            s_slots[i].buf = buf;
            s_slots[i].used = true;
            pthread_mutex_unlock(&s_slot_lock);
            return;
        }
    }
    pthread_mutex_unlock(&s_slot_lock);
}

void crash_disarm(void)
{
    struct crash_slot* slot;

    pthread_mutex_lock(&s_slot_lock);
    slot = crash_find_slot();
    if (slot) {
        slot->used = false;
    }
    pthread_mutex_unlock(&s_slot_lock);
}

int crash_pending_signal(void)
{
    return s_pending_sig;
}
