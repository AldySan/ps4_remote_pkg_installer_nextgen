/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "taskman.h"
#include "log.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TASKMAN_MAX_TASKS   32
#define TASKMAN_POLL_MS     1000

struct taskman_slot {
    bool used;
    task_info_t info;
    /* internal bookkeeping */
    uint64_t last_poll_transferred;
    uint64_t last_poll_tick_ms;
    bool last_progress_ok;
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_thread;
static bool s_running = false;
static int s_next_id = 1;
static struct taskman_slot s_slots[TASKMAN_MAX_TASKS];

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void slot_refresh_derived(struct taskman_slot* slot)
{
    task_info_t* t = &slot->info;

    if (t->length_total > 0) {
        uint64_t pct = t->transferred_total * 100u / t->length_total;
        t->percent = (pct > 100u) ? 100 : (int)pct;
    } else {
        t->percent = 0;
    }

    if (slot->last_poll_tick_ms > 0 && t->transferred_total >= slot->last_poll_transferred) {
        uint64_t dt = now_ms() - slot->last_poll_tick_ms;
        if (dt > 200) {
            double inst = (double)(t->transferred_total - slot->last_poll_transferred) *
                          1000.0 / (double)dt;
            /* Exponential moving average to smooth jitter. */
            t->speed_bps = (t->speed_bps <= 0.0) ? inst : (t->speed_bps * 0.5 + inst * 0.5);
        }
    }
    slot->last_poll_transferred = t->transferred_total;
    slot->last_poll_tick_ms = now_ms();
}

static void task_cleanup_files(const task_info_t* t)
{
    if (t->manifest_path[0]) {
        unlink(t->manifest_path);
    }
    if (t->has_icon && t->icon_path[0]) {
        /* icon_path is /user/data/... — strip the /user mount prefix. */
        char local[64];
        if (util_starts_with(t->icon_path, "/user")) {
            snprintf(local, sizeof(local), "%s", t->icon_path + 5);
            unlink(local);
        }
    }
}

static void poll_task(struct taskman_slot* slot)
{
    task_info_t* t = &slot->info;
    SceBgftTaskProgress progress;
    int error = 0;

    if (t->bgft_task_id < 0) {
        return;
    }

    if (!bgft_get_progress(t->bgft_task_id, &progress, &error)) {
        if (slot->last_progress_ok && t->state == TASK_STATE_RUNNING) {
            /* Task finished and was unregistered by BGFT. */
            if (t->length_total > 0 && t->transferred_total >= t->length_total) {
                t->state = TASK_STATE_COMPLETED;
                task_cleanup_files(t);
            } else if (t->state != TASK_STATE_STOPPED) {
                t->state = TASK_STATE_STOPPED;
                task_cleanup_files(t);
            }
            LOG_INFO("task #%d (bgft %d) left the queue: state=%d",
                     t->id, t->bgft_task_id, (int)t->state);
        }
        slot->last_progress_ok = false;
        return;
    }
    slot->last_progress_ok = true;

    t->bits = progress.bits;
    t->error_result = progress.errorResult;
    t->length = (uint64_t)progress.length;
    t->transferred = (uint64_t)progress.transferred;
    t->length_total = (uint64_t)progress.lengthTotal;
    t->transferred_total = (uint64_t)progress.transferredTotal;
    t->num_index = progress.numIndex;
    t->num_total = progress.numTotal;
    t->rest_sec = progress.restSec;
    t->rest_sec_total = progress.restSecTotal;
    t->preparing_percent = progress.preparingPercent;
    t->local_copy_percent = progress.localCopyPercent;

    if (progress.errorResult != 0) {
        if (t->state == TASK_STATE_RUNNING || t->state == TASK_STATE_PAUSED ||
            t->state == TASK_STATE_REGISTERED) {
            t->state = TASK_STATE_FAILED;
            t->error = progress.errorResult;
            task_cleanup_files(t);
            LOG_ERROR("task #%d (bgft %d) failed: 0x%08X",
                      t->id, t->bgft_task_id, progress.errorResult);
        }
        return;
    }

    if (t->state == TASK_STATE_REGISTERED || t->state == TASK_STATE_PAUSED) {
        if (t->transferred_total > 0 || t->preparing_percent >= 100) {
            t->state = TASK_STATE_RUNNING;
        }
    }

    slot_refresh_derived(slot);
}

static void* taskman_thread(void* arg)
{
    (void)arg;

    while (s_running) {
        int i;

        pthread_mutex_lock(&s_lock);
        for (i = 0; i < TASKMAN_MAX_TASKS; ++i) {
            if (!s_slots[i].used) {
                continue;
            }
            switch (s_slots[i].info.state) {
                case TASK_STATE_REGISTERED:
                case TASK_STATE_RUNNING:
                case TASK_STATE_PAUSED:
                    poll_task(&s_slots[i]);
                    break;
                default:
                    break;
            }
        }
        pthread_mutex_unlock(&s_lock);

        usleep(TASKMAN_POLL_MS * 1000);
    }
    return NULL;
}

bool taskman_start(void)
{
    int ret;

    if (s_running) {
        return true;
    }

    memset(s_slots, 0, sizeof(s_slots));
    s_running = true;

    ret = pthread_create(&s_thread, NULL, taskman_thread, NULL);
    if (ret != 0) {
        LOG_ERROR("pthread_create(taskman) failed: %d", ret);
        s_running = false;
        return false;
    }
    return true;
}

void taskman_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    pthread_join(s_thread, NULL);
    /* NOTE: tmp manifests/icons of active tasks are intentionally kept —
     * BGFT keeps downloading after the app exits (same as the original). */
}

int taskman_create(const char* url)
{
    int i;
    int id = -1;

    if (!url) {
        return -1;
    }

    pthread_mutex_lock(&s_lock);
    for (i = 0; i < TASKMAN_MAX_TASKS; ++i) {
        if (!s_slots[i].used) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            s_slots[i].used = true;
            id = s_next_id++;
            s_slots[i].info.id = id;
            s_slots[i].info.bgft_task_id = -1;
            s_slots[i].info.state = TASK_STATE_PENDING;
            s_slots[i].info.speed_bps = 0.0;
            snprintf(s_slots[i].info.url, sizeof(s_slots[i].info.url), "%s", url);
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);

    if (id < 0) {
        LOG_WARN("taskman: registry full");
    }
    return id;
}

static struct taskman_slot* find_slot(int id)
{
    int i;

    for (i = 0; i < TASKMAN_MAX_TASKS; ++i) {
        if (s_slots[i].used && s_slots[i].info.id == id) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static struct taskman_slot* find_slot_by_bgft(int bgft_task_id)
{
    int i;

    for (i = 0; i < TASKMAN_MAX_TASKS; ++i) {
        if (s_slots[i].used && s_slots[i].info.bgft_task_id == bgft_task_id) {
            return &s_slots[i];
        }
    }
    return NULL;
}

bool taskman_get(int id, task_info_t* out)
{
    struct taskman_slot* slot;
    bool ok = false;

    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&s_lock);
    slot = find_slot(id);
    if (slot) {
        *out = slot->info;
        ok = true;
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool taskman_find_by_bgft(int bgft_task_id, task_info_t* out)
{
    struct taskman_slot* slot;
    bool ok = false;

    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&s_lock);
    slot = find_slot_by_bgft(bgft_task_id);
    if (slot) {
        *out = slot->info;
        ok = true;
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

int taskman_count(void)
{
    int i;
    int count = 0;

    pthread_mutex_lock(&s_lock);
    for (i = 0; i < TASKMAN_MAX_TASKS; ++i) {
        if (s_slots[i].used) {
            ++count;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return count;
}

void taskman_foreach(taskman_visit_t cb, void* arg)
{
    int i;

    if (!cb) {
        return;
    }
    pthread_mutex_lock(&s_lock);
    for (i = 0; i < TASKMAN_MAX_TASKS; ++i) {
        if (s_slots[i].used) {
            if (!cb(&s_slots[i].info, arg)) {
                break;
            }
        }
    }
    pthread_mutex_unlock(&s_lock);
}

bool taskman_set_state(int id, task_state_t state, int error)
{
    struct taskman_slot* slot;
    bool ok = false;

    pthread_mutex_lock(&s_lock);
    slot = find_slot(id);
    if (slot) {
        slot->info.state = state;
        slot->info.error = error;
        ok = true;
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool taskman_set_registered(int id, int bgft_task_id, const char* content_id,
                            const char* title, uint64_t package_size,
                            bool is_patch, bool has_icon,
                            const char* manifest_path, const char* icon_path)
{
    struct taskman_slot* slot;
    bool ok = false;

    pthread_mutex_lock(&s_lock);
    slot = find_slot(id);
    if (slot) {
        slot->info.bgft_task_id = bgft_task_id;
        slot->info.state = (bgft_task_id == -1) ? TASK_STATE_ALREADY_INSTALLED
                                                : TASK_STATE_REGISTERED;
        if (content_id) {
            snprintf(slot->info.content_id, sizeof(slot->info.content_id), "%s", content_id);
        }
        if (title) {
            snprintf(slot->info.title, sizeof(slot->info.title), "%s", title);
        }
        slot->info.package_size = package_size;
        slot->info.is_patch = is_patch;
        slot->info.has_icon = has_icon;
        if (manifest_path) {
            snprintf(slot->info.manifest_path, sizeof(slot->info.manifest_path),
                     "%s", manifest_path);
        }
        if (icon_path) {
            snprintf(slot->info.icon_path, sizeof(slot->info.icon_path), "%s", icon_path);
        }
        ok = true;
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool taskman_pause(int id, int* error)
{
    struct taskman_slot* slot;
    bool ok = false;

    pthread_mutex_lock(&s_lock);
    slot = find_slot(id);
    if (slot && slot->info.bgft_task_id >= 0) {
        if (bgft_pause_task(slot->info.bgft_task_id, error)) {
            slot->info.state = TASK_STATE_PAUSED;
            ok = true;
        }
    } else if (error) {
        *error = 0x80010016;
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool taskman_resume(int id, int* error)
{
    struct taskman_slot* slot;
    bool ok = false;

    pthread_mutex_lock(&s_lock);
    slot = find_slot(id);
    if (slot && slot->info.bgft_task_id >= 0) {
        if (bgft_resume_task(slot->info.bgft_task_id, error)) {
            slot->info.state = TASK_STATE_RUNNING;
            ok = true;
        }
    } else if (error) {
        *error = 0x80010016;
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool taskman_stop_task(int id, int* error)
{
    struct taskman_slot* slot;
    bool ok = false;

    pthread_mutex_lock(&s_lock);
    slot = find_slot(id);
    if (slot && slot->info.bgft_task_id >= 0) {
        if (bgft_stop_task(slot->info.bgft_task_id, error)) {
            slot->info.state = TASK_STATE_STOPPED;
            task_cleanup_files(&slot->info);
            ok = true;
        }
    } else if (error) {
        *error = 0x80010016;
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}
