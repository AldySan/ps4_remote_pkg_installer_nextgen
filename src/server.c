/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 */
#include "server.h"
#include "appinst.h"
#include "bgft.h"
#include "http_client.h"
#include "installer.h"
#include "log.h"
#include "netmon.h"
#include "proxy.h"
#include "storage.h"
#include "taskman.h"
#include "util.h"
#include "version.h"

#include <orbis/Net.h>
#include <orbis/_types/net.h>

#include <cJSON.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Platform socket bits missing from the toolchain headers.            */
/* ------------------------------------------------------------------ */

#define SERVER_SOL_SOCKET  0xffff
#define SERVER_SO_REUSEADDR 0x0004
#define SERVER_SO_RCVTIMEO  0x1006

struct server_sockaddr_in {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
};

/* ------------------------------------------------------------------ */
/* Limits / state                                                      */
/* ------------------------------------------------------------------ */

#define SERVER_BACKLOG        8
#define SERVER_MAX_CONNS      16
#define SERVER_REQ_HEAD_MAX   (16 * 1024)
#define SERVER_REQ_BODY_MAX   (256 * 1024)
#define SERVER_SEND_CHUNK     (64 * 1024)

static pthread_t s_thread;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool s_running = false;
static volatile OrbisNetId s_listen_fd = -1;
static int s_port = 0;
static int s_active_conns = 0;

/* ------------------------------------------------------------------ */
/* Connection write helpers                                            */
/* ------------------------------------------------------------------ */

/* Sends a buffer; returns false when the connection is gone so callers
 * can stop pulling data for a dead client (e.g. a stopped download). */
static bool conn_send(OrbisNetId fd, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    while (len > 0) {
        int n = sceNetSend(fd, p, len, 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool conn_send_str(OrbisNetId fd, const char* s)
{
    return conn_send(fd, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* Response writers (legacy byte-compatible)                           */
/* ------------------------------------------------------------------ */

static void resp_headers_json(OrbisNetId fd)
{
    conn_send_str(fd, "HTTP/1.1 200 OK\r\n");
    conn_send_str(fd, "Content-Type: application/json\r\n");
    conn_send_str(fd, "Access-Control-Allow-Origin: *\r\n");
    conn_send_str(fd, "Connection: close\r\n");
    conn_send_str(fd, "\r\n");
}

static void resp_error(OrbisNetId fd, int code, const char* title, const char* error)
{
    char head[128];
    char body[900];

    if (!error) {
        error = "Unknown";
    }
    snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n", code, title);
    conn_send_str(fd, head);
    conn_send_str(fd, "Content-Type: application/json\r\n");
    conn_send_str(fd, "Access-Control-Allow-Origin: *\r\n");
    conn_send_str(fd, "Connection: close\r\n");
    conn_send_str(fd, "\r\n");
    snprintf(body, sizeof(body), "{ \"status\": \"fail\", \"error\": \"%s\" }\n", error);
    conn_send_str(fd, body);
}

static void resp_error_code(OrbisNetId fd, int code)
{
    char body[64];

    resp_headers_json(fd);
    snprintf(body, sizeof(body), "{ \"status\": \"fail\", \"error_code\": 0x%08X }\n", code);
    conn_send_str(fd, body);
}

static void resp_success(OrbisNetId fd)
{
    resp_headers_json(fd);
    conn_send_str(fd, "{ \"status\": \"success\" }\n");
}

/* JSON string escaping for embedding values in responses. */
static void json_escape(const char* in, char* out, size_t outsz)
{
    size_t o = 0;

    if (!in) {
        in = "";
    }
    for (; *in && o + 7 < outsz; ++in) {
        unsigned char c = (unsigned char)*in;
        switch (c) {
            case '"':  out[o++] = '\\'; out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
            case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
            case '\t': out[o++] = '\\'; out[o++] = 't'; break;
            default:
                if (c < 0x20) {
                    o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
                } else {
                    out[o++] = (char)c;
                }
                break;
        }
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* /static/ file serving + accelerated piece proxy                     */
/* ------------------------------------------------------------------ */

static const char* static_content_type(const char* name)
{
    size_t n = strlen(name);

    if (n >= 5 && strcasecmp(name + n - 5, ".json") == 0) {
        return "application/json";
    }
    if (n >= 4 && strcasecmp(name + n - 4, ".png") == 0) {
        return "image/png";
    }
    return "application/octet-stream";
}

static void serve_static_file(OrbisNetId fd, const char* name)
{
    char path[128];
    char head[256];
    struct stat st;
    FILE* f;
    uint8_t* buf;
    size_t n;

    if (strstr(name, "..") || strstr(name, ":/") || name[0] == '/') {
        resp_error(fd, 400, "Bad request", "Invalid path");
        return;
    }

    snprintf(path, sizeof(path), "/data/%s", name);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        resp_error(fd, 404, "Not found", "Page not found");
        return;
    }

    f = fopen(path, "rb");
    if (!f) {
        resp_error(fd, 403, "Forbidden", "Access is denied");
        return;
    }

    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\n"
             "Connection: close\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %lld\r\n"
             "\r\n",
             static_content_type(name), (long long)st.st_size);
    conn_send_str(fd, head);

    buf = (uint8_t*)malloc(SERVER_SEND_CHUNK);
    if (buf) {
        while ((n = fread(buf, 1, SERVER_SEND_CHUNK, f)) > 0) {
            if (!conn_send(fd, buf, n)) {
                break; /* client is gone */
            }
        }
        free(buf);
    }
    fclose(f);
}

/* GET /static/proxy/<job>/<piece> — streams a piece through the
 * adaptive prefetch engine (transfer acceleration). */
static void serve_proxy_piece(OrbisNetId fd, int job_id, size_t piece)
{
    uint64_t total = 0;
    void* stream;
    char head[256];
    uint8_t* buf;
    int64_t n;

    stream = proxy_stream_open(job_id, piece, &total);
    if (!stream) {
        resp_error(fd, 404, "Not found", "Piece not found");
        return;
    }

    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\n"
             "Connection: close\r\n"
             "Content-Type: application/octet-stream\r\n"
             "Content-Length: %llu\r\n"
             "\r\n",
             (unsigned long long)total);
    conn_send_str(fd, head);

    buf = (uint8_t*)malloc(SERVER_SEND_CHUNK);
    if (!buf) {
        proxy_stream_close(job_id, stream);
        return;
    }
    while ((n = proxy_stream_read(stream, buf, SERVER_SEND_CHUNK)) > 0) {
        if (!conn_send(fd, buf, (size_t)n)) {
            break; /* client is gone: stop pulling from the origin */
        }
    }
    free(buf);
    proxy_stream_close(job_id, stream);

    if (n < 0) {
        LOG_WARN("proxy: stream for job %d piece %zu failed mid-transfer",
                 job_id, piece);
    }
}

static void handle_static(OrbisNetId fd, const char* path)
{
    /* path starts with "/static/" */
    const char* name = path + 8;

    if (util_starts_with(name, "proxy/")) {
        int job_id;
        int piece;
        char extra;

        if (sscanf(name + 6, "%d/%d%c", &job_id, &piece, &extra) != 2 || piece < 0) {
            resp_error(fd, 400, "Bad request", "Invalid path");
            return;
        }
        serve_proxy_piece(fd, job_id, (size_t)piece);
        return;
    }

    serve_static_file(fd, name);
}

/* ------------------------------------------------------------------ */
/* JSON body helpers                                                   */
/* ------------------------------------------------------------------ */

static cJSON* parse_body(const char* body, size_t body_len)
{
    if (!body || body_len == 0 || body_len > SERVER_REQ_BODY_MAX) {
        return NULL;
    }
    return cJSON_ParseWithLength(body, body_len);
}

static bool get_string(const cJSON* root, const char* key, char* out, size_t outsz)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (!cJSON_IsString(item) || item->valuestring[0] == '\0') {
        return false;
    }
    snprintf(out, outsz, "%s", item->valuestring);
    return true;
}

static bool get_int(const cJSON* root, const char* key, int* out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = (int)item->valuedouble;
    return true;
}

/* ------------------------------------------------------------------ */
/* Legacy API handlers                                                 */
/* ------------------------------------------------------------------ */

static void handle_api_install(OrbisNetId fd, const char* body, size_t body_len)
{
    cJSON* root = parse_body(body, body_len);
    const cJSON* type;
    install_result_t result;
    char err[256];
    char title_esc[512];

    if (!root) {
        resp_error(fd, 500, "Internal server error", "Invalid JSON.");
        return;
    }

    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error", "No 'type' parameter specified.");
        return;
    }

    bool ok = false;
    if (strcmp(type->valuestring, "direct") == 0) {
        const cJSON* packages = cJSON_GetObjectItemCaseSensitive(root, "packages");
        size_t count = 0;
        size_t i = 0;
        char** urls = NULL;

        if (cJSON_IsArray(packages)) {
            count = (size_t)cJSON_GetArraySize(packages);
        }
        if (count == 0) {
            cJSON_Delete(root);
            resp_error(fd, 500, "Internal server error", "No packages.");
            return;
        }
        urls = (char**)calloc(count, sizeof(char*));
        if (!urls) {
            cJSON_Delete(root);
            resp_error(fd, 500, "Internal server error", "Out of memory.");
            return;
        }
        const cJSON* it = NULL;
        cJSON_ArrayForEach(it, packages) {
            if (cJSON_IsString(it) && it->valuestring[0] != '\0') {
                urls[i++] = strdup(it->valuestring);
            }
        }
        if (i != count) {
            for (i = 0; i < count; ++i) {
                free(urls[i]);
            }
            free(urls);
            cJSON_Delete(root);
            resp_error(fd, 500, "Internal server error",
                       "Invalid type for element of parameter 'packages'.");
            return;
        }
        ok = installer_install_direct((const char* const*)urls, count, &result,
                                      err, sizeof(err));
        for (i = 0; i < count; ++i) {
            free(urls[i]);
        }
        free(urls);
    } else if (strcmp(type->valuestring, "ref_pkg_url") == 0) {
        char url[TASK_URL_MAX];

        if (!get_string(root, "url", url, sizeof(url))) {
            cJSON_Delete(root);
            resp_error(fd, 500, "Internal server error",
                       "No 'url' parameter specified.");
            return;
        }
        ok = installer_install_ref_pkg_url(url, &result, err, sizeof(err));
    } else {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error",
                   "Invalid type of parameter 'type'.");
        return;
    }

    if (ok) {
        char resp[900];
        json_escape(result.title, title_esc, sizeof(title_esc));
        resp_headers_json(fd);
        snprintf(resp, sizeof(resp),
                 "{ \"status\": \"success\", \"task_id\": %d, \"title\": \"%s\" }\n",
                 result.task_id, title_esc);
        conn_send_str(fd, resp);
    } else {
        if (result.bgft_error != 0) {
            /* BGFT-level failure: byte-compatible with the original
             * (HTTP 200 + error_code, e.g. registration rejected). */
            resp_error_code(fd, result.bgft_error);
        } else if (result.job_id >= 0) {
            /* transport-level failure with a message */
            char esc[600];
            json_escape(err, esc, sizeof(esc));
            resp_error(fd, 500, "Internal server error", esc);
        } else {
            resp_error_code(fd, 0x80010016);
        }
    }
    cJSON_Delete(root);
}

static void handle_api_uninstall_game(OrbisNetId fd, const char* body, size_t body_len)
{
    cJSON* root = parse_body(body, body_len);
    char title_id[16];
    int error = 0;
    bool ok;

    if (!root || !get_string(root, "title_id", title_id, sizeof(title_id))) {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error", "Invalid parameter.");
        return;
    }
    ok = appinst_uninstall_game(title_id, &error);
    if (ok) {
        resp_success(fd);
    } else {
        resp_error_code(fd, error);
    }
    cJSON_Delete(root);
}

static void handle_api_uninstall_patch(OrbisNetId fd, const char* body, size_t body_len)
{
    cJSON* root = parse_body(body, body_len);
    char title_id[16];
    int error = 0;
    bool ok;

    if (!root || !get_string(root, "title_id", title_id, sizeof(title_id))) {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error", "Invalid parameter.");
        return;
    }
    ok = appinst_uninstall_patch(title_id, &error);
    if (ok) {
        resp_success(fd);
    } else {
        resp_error_code(fd, error);
    }
    cJSON_Delete(root);
}

static void handle_api_uninstall_ac(OrbisNetId fd, const char* body, size_t body_len)
{
    cJSON* root = parse_body(body, body_len);
    char content_id[40];
    int error = 0;
    bool ok;

    if (!root || !get_string(root, "content_id", content_id, sizeof(content_id))) {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error", "Invalid parameter.");
        return;
    }
    ok = appinst_uninstall_ac(content_id, &error);
    if (ok) {
        resp_success(fd);
    } else {
        resp_error_code(fd, error);
    }
    cJSON_Delete(root);
}

static void handle_api_uninstall_theme(OrbisNetId fd, const char* body, size_t body_len)
{
    cJSON* root = parse_body(body, body_len);
    char content_id[40];
    int error = 0;
    bool ok;

    if (!root || !get_string(root, "content_id", content_id, sizeof(content_id))) {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error", "Invalid parameter.");
        return;
    }
    ok = appinst_uninstall_theme(content_id, &error);
    if (ok) {
        resp_success(fd);
    } else {
        resp_error_code(fd, error);
    }
    cJSON_Delete(root);
}

static void handle_api_is_exists(OrbisNetId fd, const char* body, size_t body_len)
{
    cJSON* root = parse_body(body, body_len);
    char title_id[16];
    bool exists = false;
    uint64_t size = 0;
    int error = 0;
    char resp[160];

    if (!root || !get_string(root, "title_id", title_id, sizeof(title_id))) {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error", "Invalid parameter.");
        return;
    }

    if (!appinst_is_exists(title_id, &exists, &size, &error)) {
        cJSON_Delete(root);
        resp_error_code(fd, error);
        return;
    }

    resp_headers_json(fd);
    if (exists) {
        if (size == 0) {
            size = (uint64_t)-1;
        }
        snprintf(resp, sizeof(resp),
                 "{ \"status\": \"success\", \"exists\": \"true\", \"size\": 0x%llX }\n",
                 (unsigned long long)size);
    } else {
        snprintf(resp, sizeof(resp),
                 "{ \"status\": \"success\", \"exists\": \"false\" }\n");
    }
    conn_send_str(fd, resp);
    cJSON_Delete(root);
}

static void handle_api_task_op(OrbisNetId fd, const char* body, size_t body_len,
                               int op)
{
    cJSON* root = parse_body(body, body_len);
    int task_id;
    int error = 0;
    bool ok = false;

    if (!root || !get_int(root, "task_id", &task_id)) {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error", "Invalid parameter.");
        return;
    }

    switch (op) {
        case 0: ok = bgft_start_task(task_id, &error); break;
        case 1: ok = bgft_stop_task(task_id, &error); break;
        case 2: ok = bgft_pause_task(task_id, &error); break;
        case 3: ok = bgft_resume_task(task_id, &error); break;
        case 4: ok = bgft_unregister_task(task_id, &error); break;
        default: break;
    }

    if (ok) {
        resp_success(fd);
    } else {
        resp_error_code(fd, error);
    }
    cJSON_Delete(root);
}

static void handle_api_get_task_progress(OrbisNetId fd, const char* body, size_t body_len)
{
    cJSON* root = parse_body(body, body_len);
    int task_id;
    int error = 0;
    SceBgftTaskProgress progress;

    if (!root || !get_int(root, "task_id", &task_id)) {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error", "Invalid parameter.");
        return;
    }

    if (!bgft_get_progress(task_id, &progress, &error)) {
        cJSON_Delete(root);
        resp_error_code(fd, error);
        return;
    }

    {
        char resp[640];
        resp_headers_json(fd);
        snprintf(resp, sizeof(resp),
                 "{ \"status\": \"success\", \"bits\": 0x%X, \"error\": %d, "
                 "\"length\": 0x%llX, \"transferred\": 0x%llX, "
                 "\"length_total\": 0x%llX, \"transferred_total\": 0x%llX, "
                 "\"num_index\": %u, \"num_total\": %u, "
                 "\"rest_sec\": %u, \"rest_sec_total\": %u, "
                 "\"preparing_percent\": %d, \"local_copy_percent\": %d }\n",
                 progress.bits, progress.errorResult,
                 (unsigned long long)progress.length,
                 (unsigned long long)progress.transferred,
                 (unsigned long long)progress.lengthTotal,
                 (unsigned long long)progress.transferredTotal,
                 progress.numIndex, progress.numTotal,
                 progress.restSec, progress.restSecTotal,
                 progress.preparingPercent, progress.localCopyPercent);
        conn_send_str(fd, resp);
    }
    cJSON_Delete(root);
}

static void handle_api_find_task(OrbisNetId fd, const char* body, size_t body_len)
{
    cJSON* root = parse_body(body, body_len);
    char content_id[40];
    int sub_type;
    int task_id;
    int error = 0;
    char resp[96];

    if (!root || !get_string(root, "content_id", content_id, sizeof(content_id)) ||
        !get_int(root, "sub_type", &sub_type)) {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error", "Invalid parameter.");
        return;
    }
    if (sub_type <= 0 || sub_type >= SCE_BGFT_TASK_SUB_TYPE_MAX) {
        cJSON_Delete(root);
        resp_error(fd, 500, "Internal server error", "Invalid sub_type.");
        return;
    }

    if (!bgft_find_task(content_id, sub_type, &task_id, &error)) {
        cJSON_Delete(root);
        resp_error_code(fd, error);
        return;
    }

    resp_headers_json(fd);
    snprintf(resp, sizeof(resp),
             "{ \"status\": \"success\", \"task_id\": %d }\n", task_id);
    conn_send_str(fd, resp);
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* v2 API handlers                                                     */
/* ------------------------------------------------------------------ */

static void v2_task_json(const task_info_t* t, char* out, size_t outsz)
{
    char title_esc[512];
    char state[24];

    json_escape(t->title, title_esc, sizeof(title_esc));
    switch (t->state) {
        case TASK_STATE_PENDING: snprintf(state, sizeof(state), "pending"); break;
        case TASK_STATE_PREPARING: snprintf(state, sizeof(state), "preparing"); break;
        case TASK_STATE_REGISTERED: snprintf(state, sizeof(state), "registered"); break;
        case TASK_STATE_RUNNING: snprintf(state, sizeof(state), "running"); break;
        case TASK_STATE_PAUSED: snprintf(state, sizeof(state), "paused"); break;
        case TASK_STATE_COMPLETED: snprintf(state, sizeof(state), "completed"); break;
        case TASK_STATE_ALREADY_INSTALLED: snprintf(state, sizeof(state), "already_installed"); break;
        case TASK_STATE_FAILED: snprintf(state, sizeof(state), "failed"); break;
        case TASK_STATE_STOPPED: snprintf(state, sizeof(state), "stopped"); break;
        default: snprintf(state, sizeof(state), "unknown"); break;
    }

    snprintf(out, outsz,
             "{\"id\":%d,\"bgft_task_id\":%d,\"state\":\"%s\","
             "\"title\":\"%s\",\"content_id\":\"%s\","
             "\"app_ver\":\"%s\",\"content_type\":\"%s\",\"pieces\":%u,"
             "\"package_size\":%llu,"
             "\"transferred\":%llu,\"length_total\":%llu,"
             "\"percent\":%d,\"speed_bps\":%.0f,"
             "\"is_patch\":%s,\"error\":%d,"
             "\"rest_sec\":%u,\"num_index\":%u,\"num_total\":%u}",
             t->id, t->bgft_task_id, state, title_esc, t->content_id,
             t->app_ver[0] ? t->app_ver : "",
             t->content_type[0] ? t->content_type : "",
             t->piece_count,
             (unsigned long long)t->package_size,
             (unsigned long long)t->transferred_total,
             (unsigned long long)t->length_total,
             t->percent, t->speed_bps,
             t->is_patch ? "true" : "false", t->error,
             t->rest_sec_total, t->num_index, t->num_total);
}

struct jobs_list_ctx {
    OrbisNetId fd;
    bool first;
};

static bool jobs_list_cb(const task_info_t* t, void* arg)
{
    struct jobs_list_ctx* ctx = (struct jobs_list_ctx*)arg;
    char item[900];

    if (!ctx->first) {
        conn_send_str(ctx->fd, ",");
    }
    ctx->first = false;
    v2_task_json(t, item, sizeof(item));
    conn_send_str(ctx->fd, item);
    return true;
}

static void handle_v2_jobs(OrbisNetId fd, const char* path)
{
    struct jobs_list_ctx ctx;

    /* /api/v2/jobs or /api/v2/jobs/<id> */
    if (path[0] == '\0') {
        resp_headers_json(fd);
        conn_send_str(fd, "{\"status\":\"success\",\"data\":{\"jobs\":[");
        ctx.fd = fd;
        ctx.first = true;
        taskman_foreach(jobs_list_cb, &ctx);
        conn_send_str(fd, "]}}\n");
        return;
    }

    {
        int id = atoi(path);
        task_info_t t;
        char item[900];
        char resp[1100];

        if (!taskman_get(id, &t)) {
            resp_error(fd, 404, "Not found", "Job not found");
            return;
        }
        v2_task_json(&t, item, sizeof(item));
        resp_headers_json(fd);
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"success\",\"data\":%s}\n", item);
        conn_send_str(fd, resp);
    }
}

static void handle_v2_job_op(OrbisNetId fd, int id, const char* op)
{
    int error = 0;
    bool ok = false;

    if (strcmp(op, "pause") == 0) {
        ok = taskman_pause(id, &error);
    } else if (strcmp(op, "resume") == 0) {
        ok = taskman_resume(id, &error);
    } else if (strcmp(op, "stop") == 0) {
        ok = taskman_stop_task(id, &error);
    } else {
        resp_error(fd, 400, "Bad request", "Unknown operation");
        return;
    }

    if (ok) {
        resp_success(fd);
    } else {
        resp_error_code(fd, error);
    }
}

static void handle_v2_system(OrbisNetId fd)
{
    char ip[16] = "";
    uint64_t free_b = 0;
    uint64_t total_b = 0;
    char resp[640];

    netmon_get_ip(ip, sizeof(ip));
    storage_get_free(&free_b);
    storage_get_total(&total_b);

    resp_headers_json(fd);
    snprintf(resp, sizeof(resp),
             "{\"status\":\"success\",\"data\":{"
             "\"name\":\"%s\",\"version\":\"%s\","
             "\"title_id\":\"%s\","
             "\"ip\":\"%s\",\"port\":%d,"
             "\"network\":%s,"
             "\"storage\":{\"free\":%llu,\"total\":%llu},"
             "\"active_jobs\":%d}}\n",
             APP_NAME, APP_VERSION, APP_TITLE_ID,
             ip, s_port,
             netmon_is_up() ? "true" : "false",
             (unsigned long long)free_b, (unsigned long long)total_b,
             taskman_count());
    conn_send_str(fd, resp);
}

static void handle_v2_storage(OrbisNetId fd)
{
    uint64_t free_b = 0;
    uint64_t total_b = 0;
    char resp[256];
    char free_str[32];
    char total_str[32];

    storage_get_free(&free_b);
    storage_get_total(&total_b);
    util_format_size(free_b, free_str, sizeof(free_str));
    util_format_size(total_b, total_str, sizeof(total_str));

    resp_headers_json(fd);
    snprintf(resp, sizeof(resp),
             "{\"status\":\"success\",\"data\":{\"free\":%llu,\"total\":%llu,"
             "\"free_hr\":\"%s\",\"total_hr\":\"%s\"}}\n",
             (unsigned long long)free_b, (unsigned long long)total_b,
             free_str, total_str);
    conn_send_str(fd, resp);
}

static void handle_v2_logs(OrbisNetId fd)
{
    char entry[LOG_RING_ENTRY_MAX];
    char esc[LOG_RING_ENTRY_MAX * 2];
    int count = log_ring_count();
    int i;

    resp_headers_json(fd);
    conn_send_str(fd, "{\"status\":\"success\",\"data\":{\"logs\":[");
    for (i = 0; i < count; ++i) {
        if (!log_ring_get(i, entry, sizeof(entry))) {
            break;
        }
        if (i > 0) {
            conn_send_str(fd, ",");
        }
        json_escape(entry, esc, sizeof(esc));
        conn_send_str(fd, "\"");
        conn_send_str(fd, esc);
        conn_send_str(fd, "\"");
    }
    conn_send_str(fd, "]}}\n");
}

static void handle_v2_install(OrbisNetId fd, const char* body, size_t body_len)
{
    cJSON* root = parse_body(body, body_len);
    install_result_t result;
    char err[256];
    char resp[900];

    if (!root) {
        resp_error(fd, 500, "Internal server error", "Invalid JSON.");
        return;
    }

    {
        char url[TASK_URL_MAX];
        const cJSON* urls = cJSON_GetObjectItemCaseSensitive(root, "urls");
        bool ok = false;

        if (get_string(root, "ref_pkg_url", url, sizeof(url))) {
            ok = installer_install_ref_pkg_url(url, &result, err, sizeof(err));
        } else if (cJSON_IsArray(urls) && cJSON_GetArraySize(urls) > 0) {
            size_t count = (size_t)cJSON_GetArraySize(urls);
            char** list = (char**)calloc(count, sizeof(char*));
            size_t i = 0;
            const cJSON* it = NULL;
            if (!list) {
                cJSON_Delete(root);
                resp_error(fd, 500, "Internal server error", "Out of memory.");
                return;
            }
            cJSON_ArrayForEach(it, urls) {
                if (cJSON_IsString(it) && it->valuestring[0] != '\0' && i < count) {
                    list[i++] = strdup(it->valuestring);
                }
            }
            ok = installer_install_direct((const char* const*)list, i, &result,
                                          err, sizeof(err));
            for (i = 0; i < count; ++i) {
                free(list[i]);
            }
            free(list);
        } else if (get_string(root, "url", url, sizeof(url))) {
            const char* one[1] = { url };
            ok = installer_install_direct(one, 1, &result, err, sizeof(err));
        } else {
            cJSON_Delete(root);
            resp_error(fd, 500, "Internal server error",
                       "Provide 'url', 'urls' or 'ref_pkg_url'.");
            return;
        }

        if (!ok) {
            cJSON_Delete(root);
            resp_error(fd, 500, "Internal server error", err[0] ? err : "install failed");
            return;
        }
    }

    {
        char title_esc[512];
        json_escape(result.title, title_esc, sizeof(title_esc));
        resp_headers_json(fd);
        snprintf(resp, sizeof(resp),
                 "{\"status\":\"success\",\"data\":{\"job_id\":%d,"
                 "\"task_id\":%d,\"title\":\"%s\",\"content_id\":\"%s\","
                 "\"package_size\":%llu}}\n",
                 result.job_id, result.task_id, title_esc, result.content_id,
                 (unsigned long long)result.package_size);
        conn_send_str(fd, resp);
    }
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* Request routing                                                     */
/* ------------------------------------------------------------------ */

static void route(OrbisNetId fd, const char* method, const char* path,
                  const char* body, size_t body_len)
{
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        if (strcmp(path, "/") == 0) {
            handle_v2_system(fd);
            return;
        }
        if (util_starts_with(path, "/static/")) {
            handle_static(fd, path);
            return;
        }
        if (strcmp(path, "/api/v2/system") == 0) {
            handle_v2_system(fd);
            return;
        }
        if (strcmp(path, "/api/v2/jobs") == 0) {
            handle_v2_jobs(fd, "");
            return;
        }
        if (util_starts_with(path, "/api/v2/jobs/")) {
            const char* rest = path + 13;
            int id = atoi(rest);
            const char* slash = strchr(rest, '/');
            if (slash) {
                handle_v2_job_op(fd, id, slash + 1);
            } else {
                handle_v2_jobs(fd, rest);
            }
            return;
        }
        if (strcmp(path, "/api/v2/storage") == 0) {
            handle_v2_storage(fd);
            return;
        }
        if (strcmp(path, "/api/v2/logs") == 0) {
            handle_v2_logs(fd);
            return;
        }
        resp_error(fd, 404, "Not found", "Page not found");
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/install") == 0) {
            handle_api_install(fd, body, body_len);
            return;
        }
        if (strcmp(path, "/api/uninstall_game") == 0) {
            handle_api_uninstall_game(fd, body, body_len);
            return;
        }
        if (strcmp(path, "/api/uninstall_patch") == 0) {
            handle_api_uninstall_patch(fd, body, body_len);
            return;
        }
        if (strcmp(path, "/api/uninstall_ac") == 0) {
            handle_api_uninstall_ac(fd, body, body_len);
            return;
        }
        if (strcmp(path, "/api/uninstall_theme") == 0) {
            handle_api_uninstall_theme(fd, body, body_len);
            return;
        }
        if (strcmp(path, "/api/is_exists") == 0) {
            handle_api_is_exists(fd, body, body_len);
            return;
        }
        if (strcmp(path, "/api/start_task") == 0) {
            handle_api_task_op(fd, body, body_len, 0);
            return;
        }
        if (strcmp(path, "/api/stop_task") == 0) {
            handle_api_task_op(fd, body, body_len, 1);
            return;
        }
        if (strcmp(path, "/api/pause_task") == 0) {
            handle_api_task_op(fd, body, body_len, 2);
            return;
        }
        if (strcmp(path, "/api/resume_task") == 0) {
            handle_api_task_op(fd, body, body_len, 3);
            return;
        }
        if (strcmp(path, "/api/unregister_task") == 0) {
            handle_api_task_op(fd, body, body_len, 4);
            return;
        }
        if (strcmp(path, "/api/get_task_progress") == 0) {
            handle_api_get_task_progress(fd, body, body_len);
            return;
        }
        if (strcmp(path, "/api/find_task") == 0) {
            handle_api_find_task(fd, body, body_len);
            return;
        }
        if (strcmp(path, "/api/v2/install") == 0) {
            handle_v2_install(fd, body, body_len);
            return;
        }
        resp_error(fd, 404, "Not found", "Page not found");
        return;
    }

    resp_error(fd, 400, "Bad request", "Unsupported method");
}

/* ------------------------------------------------------------------ */
/* HTTP request parsing (one request per connection, Connection: close) */
/* ------------------------------------------------------------------ */

struct http_request {
    char method[8];
    char path[1024];
    char* body;
    size_t body_len;
};

static bool conn_read_request(OrbisNetId fd, struct http_request* req)
{
    char* buf = NULL;
    size_t used = 0;
    size_t cap = SERVER_REQ_HEAD_MAX + 16;
    char* header_end = NULL;

    buf = (char*)malloc(cap);
    if (!buf) {
        return false;
    }

    /* Read until the end of the headers. */
    while (!header_end && used < SERVER_REQ_HEAD_MAX) {
        int n = sceNetRecv(fd, buf + used, cap - used - 1, 0);
        if (n <= 0) {
            free(buf);
            return false;
        }
        used += (size_t)n;
        buf[used] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
    }
    if (!header_end) {
        free(buf);
        return false;
    }

    /* Request line. */
    {
        char* line_end = strstr(buf, "\r\n");
        if (!line_end) {
            free(buf);
            return false;
        }
        *line_end = '\0';
        {
            char* sp1 = strchr(buf, ' ');
            char* sp2;
            size_t path_len;

            if (!sp1) {
                free(buf);
                return false;
            }
            *sp1 = '\0';
            snprintf(req->method, sizeof(req->method), "%s", buf);

            sp2 = strchr(sp1 + 1, ' ');
            if (sp2) {
                *sp2 = '\0';
            }
            path_len = strlen(sp1 + 1);
            if (path_len == 0 || path_len >= sizeof(req->path)) {
                free(buf);
                return false;
            }
            snprintf(req->path, sizeof(req->path), "%s", sp1 + 1);
        }
        /* restore for body handling below (not needed, header_end is set) */
    }

    /* Content-Length + body. */
    req->body = NULL;
    req->body_len = 0;
    {
        const char* cl = NULL;
        char* p;

        /* Case-insensitive search for Content-Length. */
        for (p = buf; p < header_end; ) {
            if (strncasecmp(p, "Content-Length:", 15) == 0) {
                cl = p + 15;
                break;
            }
            p = strchr(p, '\n');
            if (!p) {
                break;
            }
            ++p;
        }

        if (cl) {
            long long len = strtoll(cl, NULL, 10);
            size_t header_len = (size_t)(header_end + 4 - buf);

            if (len < 0 || (size_t)len > SERVER_REQ_BODY_MAX) {
                free(buf);
                return false;
            }
            req->body = (char*)malloc((size_t)len + 1);
            if (!req->body) {
                free(buf);
                return false;
            }
            req->body_len = (size_t)len;

            {
                size_t have = used - header_len;
                size_t taken = have > req->body_len ? req->body_len : have;
                memcpy(req->body, buf + header_len, taken);
                while (taken < req->body_len) {
                    int n = sceNetRecv(fd, req->body + taken, req->body_len - taken, 0);
                    if (n <= 0) {
                        free(req->body);
                        req->body = NULL;
                        free(buf);
                        return false;
                    }
                    taken += (size_t)n;
                }
            }
            req->body[req->body_len] = '\0';
        }
    }

    free(buf);
    return true;
}

/* ------------------------------------------------------------------ */
/* Connection worker + accept loop                                     */
/* ------------------------------------------------------------------ */

static void* conn_thread(void* arg)
{
    OrbisNetId fd = (OrbisNetId)(intptr_t)arg;
    struct http_request req;

    if (conn_read_request(fd, &req)) {
        route(fd, req.method, req.path, req.body, req.body_len);
        free(req.body);
    } else {
        resp_error(fd, 400, "Bad request", "Malformed request");
    }

    sceNetSocketClose(fd);

    pthread_mutex_lock(&s_lock);
    --s_active_conns;
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

static void* accept_thread(void* arg)
{
    (void)arg;

    while (s_running) {
        struct server_sockaddr_in addr;
        OrbisNetId fd;
        pthread_t tid;
        bool spawn = false;

        {
            OrbisNetSocklen_t alen = sizeof(addr);
            memset(&addr, 0, sizeof(addr));
            fd = sceNetAccept(s_listen_fd, (OrbisNetSockaddr*)&addr, &alen);
        }
        if (fd < 0) {
            if (!s_running) {
                break;
            }
            usleep(50 * 1000);
            continue;
        }

        pthread_mutex_lock(&s_lock);
        if (s_active_conns < SERVER_MAX_CONNS) {
            ++s_active_conns;
            spawn = true;
        }
        pthread_mutex_unlock(&s_lock);

        if (spawn) {
            if (pthread_create(&tid, NULL, conn_thread,
                               (void*)(intptr_t)fd) == 0) {
                pthread_detach(tid);
            } else {
                sceNetSocketClose(fd);
                pthread_mutex_lock(&s_lock);
                --s_active_conns;
                pthread_mutex_unlock(&s_lock);
            }
        } else {
            resp_error(fd, 503, "Service Unavailable", "Too many connections");
            sceNetSocketClose(fd);
        }
    }
    return NULL;
}

bool server_start(int port)
{
    struct server_sockaddr_in addr;
    int reuse = 1;
    int ret;

    if (s_running) {
        return true;
    }
    if (netmon_mem_id() < 0) {
        LOG_WARN("server: network stack not ready");
        return false;
    }

    s_listen_fd = sceNetSocket(APP_SHORT_NAME "_http",
                               ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (s_listen_fd < 0) {
        LOG_ERROR("sceNetSocket failed: %d", (int)s_listen_fd);
        return false;
    }

    sceNetSetsockopt(s_listen_fd, SERVER_SOL_SOCKET, SERVER_SO_REUSEADDR,
                     &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = ORBIS_NET_AF_INET;
    addr.sin_port = sceNetHtons((uint16_t)port);
    addr.sin_addr = 0; /* INADDR_ANY */

    ret = sceNetBind(s_listen_fd, (OrbisNetSockaddr*)&addr, sizeof(addr));
    if (ret != 0) {
        LOG_ERROR("sceNetBind(%d) failed: %d", port, ret);
        sceNetSocketClose(s_listen_fd);
        s_listen_fd = -1;
        return false;
    }

    ret = sceNetListen(s_listen_fd, SERVER_BACKLOG);
    if (ret != 0) {
        LOG_ERROR("sceNetListen failed: %d", ret);
        sceNetSocketClose(s_listen_fd);
        s_listen_fd = -1;
        return false;
    }

    s_port = port;
    s_running = true;

    if (pthread_create(&s_thread, NULL, accept_thread, NULL) != 0) {
        s_running = false;
        sceNetSocketClose(s_listen_fd);
        s_listen_fd = -1;
        LOG_ERROR("server: pthread_create failed");
        return false;
    }

    LOG_INFO("server listening on 0.0.0.0:%d", port);
    return true;
}

void server_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    if (s_listen_fd >= 0) {
        sceNetSocketAbort(s_listen_fd, 0);
        sceNetSocketClose(s_listen_fd);
        s_listen_fd = -1;
    }
    pthread_join(s_thread, NULL);

    /* Wait briefly (bounded) for in-flight connection threads. They are
     * detached, so stragglers (e.g. a send stuck on a dead link) finish on
     * their own; blocking here forever would stall the netmon thread and
     * prevent network-up recovery. */
    {
        int waits;
        for (waits = 0; waits < 100; ++waits) { /* 100 x 50ms = 5s max */
            int active;
            pthread_mutex_lock(&s_lock);
            active = s_active_conns;
            pthread_mutex_unlock(&s_lock);
            if (active <= 0) {
                break;
            }
            usleep(50 * 1000);
        }
    }
    s_port = 0;
    LOG_INFO("server stopped");
}

bool server_is_running(void)
{
    return s_running;
}

int server_port(void)
{
    return s_port;
}
