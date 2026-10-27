/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "ui.h"
#include "font.h"
#include "installer.h"
#include "log.h"
#include "netmon.h"
#include "server.h"
#include "storage.h"
#include "taskman.h"
#include "util.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

/* Panel colors (4 components, for rect fills). */
#define COL_BG      0x0D, 0x11, 0x17, 0xFF
#define COL_PANEL   0x16, 0x1B, 0x22, 0xFF
#define COL_PANEL2  0x1C, 0x23, 0x2D, 0xFF
#define COL_TRACK   0x22, 0x2A, 0x33, 0xFF
#define COL_BORDER  0x30, 0x36, 0x3D, 0xFF

/* Text colors (0xRRGGBB, single value for font_draw). */
#define TXT_TEXT    0xE6EDF3u
#define TXT_DIM     0x8B949Eu
#define TXT_ACCENT  0x39D2C0u
#define TXT_GREEN   0x3FB950u
#define TXT_RED     0xF85149u
#define TXT_YELLOW  0xD29922u

#define UI_SCALE     2
#define UI_SCALE_SM  1
#define UI_W         1920
#define UI_H         1080

#define UI_HEADER_H  84
#define UI_STATUS_H  64
#define UI_FOOTER_H  56
#define UI_CARD_H    140
#define UI_CARD_GAP  16
#define UI_LOG_LINES 7
#define UI_MAX_JOBS  32

static int s_selected = 0;
static int s_scroll = 0;
static Uint32 s_last_action = 0;

static void fill_rect(SDL_Renderer* r, int x, int y, int w, int h,
                      int cr, int cg, int cb, int ca)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, (Uint8)cr, (Uint8)cg, (Uint8)cb, (Uint8)ca);
    SDL_RenderFillRect(r, &(SDL_Rect){ x, y, w, h });
}

static void draw_rect(SDL_Renderer* r, int x, int y, int w, int h,
                      int cr, int cg, int cb, int ca)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, (Uint8)cr, (Uint8)cg, (Uint8)cb, (Uint8)ca);
    SDL_RenderDrawRect(r, &(SDL_Rect){ x, y, w, h });
}

static uint32_t state_text_color(task_state_t state)
{
    switch (state) {
        case TASK_STATE_RUNNING:
        case TASK_STATE_REGISTERED:
            return TXT_ACCENT;
        case TASK_STATE_COMPLETED:
            return TXT_GREEN;
        case TASK_STATE_PAUSED:
        case TASK_STATE_PREPARING:
        case TASK_STATE_PENDING:
            return TXT_YELLOW;
        case TASK_STATE_FAILED:
            return TXT_RED;
        default:
            return TXT_DIM;
    }
}

static void state_rgb(task_state_t state, int* r, int* g, int* b)
{
    uint32_t c = state_text_color(state);
    *r = (int)((c >> 16) & 0xFF);
    *g = (int)((c >> 8) & 0xFF);
    *b = (int)(c & 0xFF);
}

static const char* state_name(task_state_t state)
{
    switch (state) {
        case TASK_STATE_PENDING: return "PENDING";
        case TASK_STATE_PREPARING: return "PREPARING";
        case TASK_STATE_REGISTERED: return "STARTING";
        case TASK_STATE_RUNNING: return "DOWNLOADING";
        case TASK_STATE_PAUSED: return "PAUSED";
        case TASK_STATE_COMPLETED: return "COMPLETED";
        case TASK_STATE_ALREADY_INSTALLED: return "INSTALLED";
        case TASK_STATE_FAILED: return "FAILED";
        case TASK_STATE_STOPPED: return "STOPPED";
        default: return "UNKNOWN";
    }
}

static void format_eta(uint32_t rest_sec, char* out, size_t outsz)
{
    if (rest_sec == 0 || rest_sec >= 0xFFFFFFF0u) {
        snprintf(out, outsz, "--:--");
    } else if (rest_sec >= 3600) {
        snprintf(out, outsz, "%uh%02um", rest_sec / 3600, (rest_sec % 3600) / 60);
    } else {
        snprintf(out, outsz, "%02u:%02u", rest_sec / 60, rest_sec % 60);
    }
}

static void format_speed(double bps, char* out, size_t outsz)
{
    if (bps <= 0.0) {
        snprintf(out, outsz, "--");
    } else if (bps >= 1024.0 * 1024.0) {
        snprintf(out, outsz, "%.1f MB/s", bps / (1024.0 * 1024.0));
    } else if (bps >= 1024.0) {
        snprintf(out, outsz, "%.0f KB/s", bps / 1024.0);
    } else {
        snprintf(out, outsz, "%.0f B/s", bps);
    }
}

/* ------------------------------------------------------------------ */
/* Job list snapshot                                                   */
/* ------------------------------------------------------------------ */

struct ui_jobs {
    task_info_t items[UI_MAX_JOBS];
    int count;
};

static bool ui_collect_cb(const task_info_t* t, void* arg);

static void ui_collect(struct ui_jobs* jobs)
{
    jobs->count = 0;
    taskman_foreach(ui_collect_cb, jobs);
}

static bool ui_collect_cb(const task_info_t* t, void* arg)
{
    struct ui_jobs* jobs = (struct ui_jobs*)arg;
    if (jobs->count < UI_MAX_JOBS) {
        jobs->items[jobs->count++] = *t;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Panels                                                             */
/* ------------------------------------------------------------------ */

static void draw_header(SDL_Renderer* r)
{
    fill_rect(r, 0, 0, UI_W, UI_HEADER_H, COL_PANEL);
    fill_rect(r, 0, UI_HEADER_H - 3, UI_W, 3, 0x39, 0xD2, 0xC0, 0xFF);

    font_draw(r, 48, 22, UI_SCALE, TXT_ACCENT, "REMOTE PKG INSTALLER");
    font_draw(r, 48 + 21 * font_advance(UI_SCALE), 22, UI_SCALE, TXT_TEXT, " NEXTGEN");

    {
        char ver[32];
        snprintf(ver, sizeof(ver), "v%s", APP_VERSION);
        font_draw(r, UI_W - 48 - (int)strlen(ver) * font_advance(UI_SCALE_SM), 34,
                  UI_SCALE_SM, TXT_DIM, ver);
    }
}

static void draw_status(SDL_Renderer* r)
{
    char ip[16];
    char free_str[32];
    uint64_t free_b = 0;
    int y = UI_HEADER_H;
    int jobs = taskman_count();

    fill_rect(r, 0, y, UI_W, UI_STATUS_H, COL_PANEL2);
    draw_rect(r, 0, y, UI_W, UI_STATUS_H, COL_BORDER);

    if (netmon_is_up() && netmon_get_ip(ip, sizeof(ip))) {
        font_draw(r, 48, y + 20, UI_SCALE, TXT_DIM, "NET ");
        font_drawf(r, 48 + 4 * font_advance(UI_SCALE), y + 20, UI_SCALE, TXT_TEXT,
                   "%s:%d", ip, server_port());
        if (server_is_running()) {
            font_draw(r, 48 + 24 * font_advance(UI_SCALE), y + 20, UI_SCALE,
                      TXT_GREEN, " [ONLINE]");
        }
    } else {
        font_draw(r, 48, y + 20, UI_SCALE, TXT_DIM, "NET ");
        font_draw(r, 48 + 4 * font_advance(UI_SCALE), y + 20, UI_SCALE,
                  TXT_YELLOW, "OFFLINE - waiting for link...");
    }

    if (storage_get_free(&free_b)) {
        util_format_size(free_b, free_str, sizeof(free_str));
    } else {
        snprintf(free_str, sizeof(free_str), "n/a");
    }
    font_drawf(r, 1180, y + 20, UI_SCALE, TXT_DIM, "FREE %s", free_str);

    font_drawf(r, 1500, y + 20, UI_SCALE, TXT_DIM, "JOBS %d", jobs);
}

static void draw_card(SDL_Renderer* r, const task_info_t* t, int y, bool selected)
{
    char title[256];
    char line[256];
    char size_str[48];
    char trans_str[48];
    char speed_str[48];
    char eta_str[16];
    uint32_t state_col = state_text_color(t->state);
    int sr, sg, sb;
    int x = 48;
    int w = UI_W - 96;
    int bar_y;
    int bar_w;
    int i;

    state_rgb(t->state, &sr, &sg, &sb);

    if (selected) {
        fill_rect(r, x - 10, y - 8, w + 20, UI_CARD_H + 16, COL_PANEL2);
        fill_rect(r, x - 10, y - 8, 6, UI_CARD_H + 16, 0x39, 0xD2, 0xC0, 0xFF);
        draw_rect(r, x - 10, y - 8, w + 20, UI_CARD_H + 16, 0x39, 0xD2, 0xC0, 0xFF);
    } else {
        fill_rect(r, x, y, w, UI_CARD_H, COL_PANEL);
        draw_rect(r, x, y, w, UI_CARD_H, COL_BORDER);
    }

    /* Title + state. */
    snprintf(title, sizeof(title), "%s", t->title[0] ? t->title : "(untitled)");
    font_fit(title, sizeof(title), w - 500, UI_SCALE);
    font_draw(r, x + 24, y + 12, UI_SCALE, TXT_TEXT, title);
    {
        const char* st = state_name(t->state);
        font_draw(r, x + w - 24 - (int)strlen(st) * font_advance(UI_SCALE),
                  y + 12, UI_SCALE, state_col, st);
    }

    /* Info line. */
    util_format_size(t->package_size, size_str, sizeof(size_str));
    snprintf(line, sizeof(line), "%s%s   %s   job #%d   bgft %d",
             t->content_id[0] ? t->content_id : "-",
             t->is_patch ? "  [PATCH]" : "",
             size_str, t->id, t->bgft_task_id);
    font_fit(line, sizeof(line), w - 48, UI_SCALE_SM);
    font_draw(r, x + 24, y + 60, UI_SCALE_SM, TXT_DIM, line);

    /* Progress bar. */
    bar_y = y + 92;
    bar_w = w - 320;
    fill_rect(r, x + 24, bar_y, bar_w, 18, COL_TRACK);
    if (t->percent > 0) {
        int fill_w = (bar_w * t->percent) / 100;
        if (fill_w > bar_w) {
            fill_w = bar_w;
        }
        fill_rect(r, x + 24, bar_y, fill_w, 18, sr, sg, sb, 0xFF);
        for (i = 24; i < fill_w; i += 28) {
            fill_rect(r, x + 24 + i, bar_y, 3, 18, COL_BG);
        }
    }

    /* Stats. */
    util_format_size(t->transferred_total, trans_str, sizeof(trans_str));
    format_speed(t->speed_bps, speed_str, sizeof(speed_str));
    format_eta(t->rest_sec_total, eta_str, sizeof(eta_str));
    font_drawf(r, x + 24 + bar_w + 24, y + 60, UI_SCALE_SM, TXT_TEXT,
               "%d%%", t->percent);
    font_drawf(r, x + 24 + bar_w + 24, y + 90, UI_SCALE_SM, TXT_DIM,
               "%s %s", speed_str, eta_str);
    font_drawf(r, x + 24 + bar_w + 24, y + 114, UI_SCALE_SM, TXT_DIM,
               "%s of %s", trans_str, size_str);

    if (selected) {
        font_draw(r, x + 24, y + UI_CARD_H - 6, UI_SCALE_SM, TXT_DIM,
                  "X pause/resume   O stop   [ ] re-transfer");
    }
}

static void draw_jobs(SDL_Renderer* r)
{
    struct ui_jobs jobs;
    int y = UI_HEADER_H + UI_STATUS_H + 20;
    int log_h = UI_LOG_LINES * (font_height(UI_SCALE_SM) + 6) + 34;
    int area_bottom = UI_H - UI_FOOTER_H - log_h - 12 - 20;
    int visible = (area_bottom - y) / (UI_CARD_H + UI_CARD_GAP + 16);
    int i;

    if (visible < 1) {
        visible = 1;
    }

    ui_collect(&jobs);

    if (s_selected >= jobs.count) {
        s_selected = jobs.count > 0 ? jobs.count - 1 : 0;
    }
    if (s_selected < s_scroll) {
        s_scroll = s_selected;
    }
    if (s_selected >= s_scroll + visible) {
        s_scroll = s_selected - visible + 1;
    }

    if (jobs.count == 0) {
        int line_h = font_height(UI_SCALE) + 18;
        font_draw(r, 48, y + 12, UI_SCALE, TXT_DIM, "No transfers yet.");
        font_draw(r, 48, y + 12 + line_h, UI_SCALE, TXT_DIM,
                  "Send installs from any device on this network:");
        font_draw(r, 48, y + 12 + 2 * line_h, UI_SCALE, TXT_ACCENT,
                  "  curl http://<PS4-IP>:12800/api/v2/system");
        font_draw(r, 48, y + 12 + 3 * line_h, UI_SCALE, TXT_ACCENT,
                  "  curl http://<PS4-IP>:12800/api/install --data "
                  "'{\"type\":\"direct\",\"packages\":[\"http://<PC>/game.pkg\"]}'");
        return;
    }

    for (i = s_scroll; i < jobs.count && i < s_scroll + visible; ++i) {
        draw_card(r, &jobs.items[i], y, i == s_selected);
        y += UI_CARD_H + UI_CARD_GAP + 16;
    }

    if (jobs.count > visible) {
        char nav[64];
        int shown = (s_scroll + visible < jobs.count) ? s_scroll + visible
                                                      : jobs.count;
        snprintf(nav, sizeof(nav), "%d-%d of %d", s_scroll + 1, shown, jobs.count);
        font_draw(r, UI_W - 48 - (int)strlen(nav) * font_advance(UI_SCALE_SM),
                  UI_H - UI_FOOTER_H - 26, UI_SCALE_SM, TXT_DIM, nav);
    }
}

static void draw_logs(SDL_Renderer* r)
{
    char entry[LOG_RING_ENTRY_MAX];
    int lines = UI_LOG_LINES;
    int count = log_ring_count();
    int first = count > lines ? count - lines : 0;
    int log_h = lines * (font_height(UI_SCALE_SM) + 6) + 34;
    int y = UI_H - UI_FOOTER_H - log_h - 12;
    int i;

    fill_rect(r, 48, y, UI_W - 96, log_h, COL_PANEL);
    draw_rect(r, 48, y, UI_W - 96, log_h, COL_BORDER);

    font_draw(r, 64, y + 10, UI_SCALE_SM, TXT_ACCENT, "ACTIVITY LOG");

    y += 36;
    for (i = first; i < count; ++i) {
        if (!log_ring_get(i, entry, sizeof(entry))) {
            break;
        }
        font_fit(entry, sizeof(entry), UI_W - 128, UI_SCALE_SM);
        if (strncmp(entry, "ERROR", 5) == 0) {
            font_draw(r, 64, y, UI_SCALE_SM, TXT_RED, entry);
        } else if (strncmp(entry, "WARN", 4) == 0) {
            font_draw(r, 64, y, UI_SCALE_SM, TXT_YELLOW, entry);
        } else {
            font_draw(r, 64, y, UI_SCALE_SM, TXT_DIM, entry);
        }
        y += font_height(UI_SCALE_SM) + 6;
    }
}

static void draw_footer(SDL_Renderer* r)
{
    fill_rect(r, 0, UI_H - UI_FOOTER_H, UI_W, UI_FOOTER_H, COL_PANEL);
    fill_rect(r, 0, UI_H - UI_FOOTER_H, UI_W, 2, COL_BORDER);
    font_draw(r, 48, UI_H - UI_FOOTER_H + 18, UI_SCALE_SM, TXT_DIM,
              "dpad select    X pause/resume    O stop    [ ] re-transfer    OPTIONS exit");
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static const task_info_t* selected_snapshot(struct ui_jobs* jobs)
{
    ui_collect(jobs);
    if (s_selected >= jobs->count) {
        return NULL;
    }
    return &jobs->items[s_selected];
}

static void action_pause_resume(void)
{
    struct ui_jobs jobs;
    const task_info_t* t = selected_snapshot(&jobs);
    int error = 0;

    if (!t) {
        return;
    }
    if (t->state == TASK_STATE_PAUSED) {
        if (!taskman_resume(t->id, &error)) {
            LOG_WARN("ui: resume job %d failed: 0x%08X", t->id, error);
        }
    } else if (t->state == TASK_STATE_RUNNING || t->state == TASK_STATE_REGISTERED) {
        if (!taskman_pause(t->id, &error)) {
            LOG_WARN("ui: pause job %d failed: 0x%08X", t->id, error);
        }
    }
}

static void action_stop(void)
{
    struct ui_jobs jobs;
    const task_info_t* t = selected_snapshot(&jobs);
    int error = 0;

    if (!t) {
        return;
    }
    if (t->state == TASK_STATE_RUNNING || t->state == TASK_STATE_PAUSED ||
        t->state == TASK_STATE_REGISTERED) {
        if (!taskman_stop_task(t->id, &error)) {
            LOG_WARN("ui: stop job %d failed: 0x%08X", t->id, error);
        }
    }
}

static void action_retry(void)
{
    struct ui_jobs jobs;
    const task_info_t* t = selected_snapshot(&jobs);
    install_result_t result;
    char err[256];

    if (!t) {
        return;
    }
    switch (t->state) {
        case TASK_STATE_COMPLETED:
        case TASK_STATE_FAILED:
        case TASK_STATE_STOPPED:
        case TASK_STATE_ALREADY_INSTALLED:
            break;
        default:
            return;
    }
    if (t->url[0] == '\0') {
        return;
    }

    LOG_INFO("ui: re-transferring '%s'", t->title[0] ? t->title : t->url);
    if (t->is_ref) {
        if (!installer_install_ref_pkg_url(t->url, &result, err, sizeof(err))) {
            LOG_WARN("ui: re-transfer failed: %s", err);
        }
    } else {
        const char* one[1] = { t->url };
        if (!installer_install_direct(one, 1, &result, err, sizeof(err))) {
            LOG_WARN("ui: re-transfer failed: %s", err);
        }
    }
}

void ui_handle_button(int button)
{
    Uint32 now = SDL_GetTicks();

    if (now - s_last_action < 200) {
        return; /* debounce */
    }
    s_last_action = now;

    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (s_selected > 0) {
                --s_selected;
            }
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            ++s_selected;
            break;
        case SDL_CONTROLLER_BUTTON_A:
            action_pause_resume();
            break;
        case SDL_CONTROLLER_BUTTON_B:
            action_stop();
            break;
        case SDL_CONTROLLER_BUTTON_X:
            action_retry();
            break;
        default:
            break;
    }
}

bool ui_init(SDL_Renderer* renderer)
{
    return font_init(renderer);
}

void ui_fini(void)
{
    font_fini();
}

void ui_frame(SDL_Renderer* r)
{
    SDL_SetRenderDrawColor(r, 0x0D, 0x11, 0x17, 0xFF);
    SDL_RenderClear(r);

    draw_header(r);
    draw_status(r);
    draw_jobs(r);
    draw_logs(r);
    draw_footer(r);

    SDL_RenderPresent(r);
}
