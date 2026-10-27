/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Task registry: tracks every install job (source URL, BGFT task id,
 * progress snapshots, state machine) for the on-screen dashboard and the
 * HTTP API. A background poller refreshes progress once per second.
 */
#pragma once

#include "bgft.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum task_state {
    TASK_STATE_UNKNOWN = 0,
    TASK_STATE_PENDING,          /* created, prerequisites not fetched yet */
    TASK_STATE_PREPARING,        /* fetching pkg header/sfo/icon */
    TASK_STATE_REGISTERED,       /* BGFT task created (auto-starts) */
    TASK_STATE_RUNNING,          /* transferring */
    TASK_STATE_PAUSED,
    TASK_STATE_COMPLETED,
    TASK_STATE_ALREADY_INSTALLED,
    TASK_STATE_FAILED,
    TASK_STATE_STOPPED,
} task_state_t;

#define TASK_TITLE_MAX   256
#define TASK_URL_MAX     512
#define TASK_CONTENT_ID_MAX 40

typedef struct task_info {
    int id;                            /* our job id (v2 API) */
    int bgft_task_id;                  /* BGFT task id, -1 until registered */
    task_state_t state;
    int error;                         /* last BGFT/system error, 0 = none */
    char url[TASK_URL_MAX];
    bool is_ref;                      /* url is a reference manifest */
    char content_id[TASK_CONTENT_ID_MAX];
    char title[TASK_TITLE_MAX];
    uint64_t package_size;
    bool is_patch;
    bool has_icon;
    char manifest_path[64];            /* /data/tmp_xxx.json */
    char icon_path[64];                /* /user/data/tmp_xxx.png */
    /* Progress snapshot (BGFT). */
    uint32_t bits;
    int32_t error_result;
    uint64_t length;
    uint64_t transferred;
    uint64_t length_total;
    uint64_t transferred_total;
    uint32_t num_index;
    uint32_t num_total;
    uint32_t rest_sec;
    uint32_t rest_sec_total;
    int32_t preparing_percent;
    int32_t local_copy_percent;
    /* Derived. */
    double speed_bps;                  /* bytes/second (EMA) */
    int percent;                       /* 0..100 over transferred_total */
} task_info_t;

/* Starts/stops the registry and the progress poller thread. */
bool taskman_start(void);
void taskman_stop(void);

/* Creates a job (state PENDING) and returns its id (or -1). */
int taskman_create(const char* url, bool is_ref);

/* Looks a job up by our id or by BGFT task id; copies a snapshot. */
bool taskman_get(int id, task_info_t* out);
bool taskman_find_by_bgft(int bgft_task_id, task_info_t* out);
int taskman_count(void);

/* Iterates all jobs (snapshot copies); callback returns false to stop. */
typedef bool (*taskman_visit_t)(const task_info_t* info, void* arg);
void taskman_foreach(taskman_visit_t cb, void* arg);

/* State transitions used by the installer and the API layer. */
bool taskman_set_state(int id, task_state_t state, int error);
bool taskman_set_registered(int id, int bgft_task_id, const char* content_id,
                            const char* title, uint64_t package_size,
                            bool is_patch, bool has_icon,
                            const char* manifest_path, const char* icon_path);

/* Terminal-state event hook (invoked outside the registry lock). */
typedef void (*taskman_event_t)(int job_id, task_state_t state, void* arg);
void taskman_on_event(taskman_event_t cb, void* arg);

/* Control ops (map to BGFT calls and update state accordingly). */
bool taskman_pause(int id, int* error);
bool taskman_resume(int id, int* error);
bool taskman_stop_task(int id, int* error);
