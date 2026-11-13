/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Flight recorder implementation. File writes go to the app sandbox
 * (/data) so they survive the process and can be pulled over FTP/USB.
 */

#include "diag.h"
#include "version.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DIAG_BOOT_FILE  APP_WORK_DIR "/rpi_nextgen_boot.txt"
#define DIAG_CRASH_FILE APP_WORK_DIR "/rpi_nextgen_crash.txt"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec s_t0;
static bool s_t0_set = false;
static char s_step[40] = "pre-start";

static double mono_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static double uptime_locked(void)
{
    return s_t0_set ? mono_sec() - ((double)s_t0.tv_sec +
               (double)s_t0.tv_nsec / 1000000000.0) : 0.0;
}

void diag_note(const char* step)
{
    FILE* f;
    double t;

    if (!step) {
        return;
    }

    pthread_mutex_lock(&s_lock);
    if (!s_t0_set) {
        clock_gettime(CLOCK_MONOTONIC, &s_t0);
        s_t0_set = true;
        f = fopen(DIAG_BOOT_FILE, "w");
        if (f) {
            fprintf(f, "%s v%s boot\n", APP_SHORT_NAME, APP_VERSION);
            fclose(f);
        }
    }
    t = uptime_locked();
    snprintf(s_step, sizeof(s_step), "%s", step);
    pthread_mutex_unlock(&s_lock);

    f = fopen(DIAG_BOOT_FILE, "a");
    if (f) {
        fprintf(f, "t=%8.3fs %s\n", t, step);
        fclose(f);
    }
}

const char* diag_step(void)
{
    return s_step;
}

double diag_uptime(void)
{
    double t;

    pthread_mutex_lock(&s_lock);
    t = uptime_locked();
    pthread_mutex_unlock(&s_lock);
    return t;
}

void diag_hud(char* out, size_t outsz)
{
    if (!out || outsz == 0) {
        return;
    }
    snprintf(out, outsz, "v%s  t=%3.0fs  %s", APP_VERSION, diag_uptime(),
             diag_step());
}

void diag_crash_note(const char* fmt, ...)
{
    FILE* f;
    char line[256];
    char stamp[40];
    struct timespec now;
    struct tm tm;
    va_list ap;

    if (!fmt) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    clock_gettime(CLOCK_REALTIME, &now);
    gmtime_r(&now.tv_sec, &tm);
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    f = fopen(DIAG_CRASH_FILE, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", stamp, line);
        fclose(f);
    }
}
