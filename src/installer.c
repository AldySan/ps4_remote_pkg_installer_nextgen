/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * Install pipeline (port of the original's pkg_setup_prerequisites +
 * server install handlers, rebuilt on the NextGen core services):
 *
 *   1. fetch pkg header (0x2000) from piece 0
 *   2. fetch entry table, locate PARAM_SFO / ICON0_PNG entries
 *   3. fetch param.sfo (title for the system language) and icon
 *   4. probe remaining pieces for their sizes (Range support aware)
 *   5. write the local manifest json + icon under /data
 *   6. register a BGFT task pointing at our /static/ manifest URL
 */
#include "installer.h"
#include "bgft.h"
#include "http_client.h"
#include "log.h"
#include "netmon.h"
#include "pkg.h"
#include "sfo.h"
#include "taskman.h"
#include "util.h"
#include "version.h"

#include <orbis/SystemService.h>
#include <orbis/_types/sys_service.h>

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INSTALLER_WORK_DIR  "/data"
#define INSTALLER_PORT      APP_DEFAULT_PORT

/* Zero mini-digest placeholder, exactly like the original manifests. */
static const uint8_t s_zero_mini_digest[PKG_DIGEST_SIZE - 12] = { 0 };

static uint32_t s_tmp_counter = 0;

static int get_system_lang(void)
{
    int32_t lang = 1; /* default: English (US) */
    int ret;

    ret = sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG, &lang);
    if (ret != 0) {
        LOG_WARN("sceSystemServiceParamGetInt(LANG) failed: 0x%08X", ret);
        return 1;
    }
    return (int)lang;
}

static bool read_entry_from_table(const uint8_t* table, size_t table_size,
                                  uint32_t index, pkg_entry_t* out)
{
    const uint8_t* e;

    if ((size_t)(index + 1) * PKG_ENTRY_TABLE_SIZE > table_size) {
        return false;
    }
    e = table + (size_t)index * PKG_ENTRY_TABLE_SIZE;
    out->id = util_be32(e + 0x00);
    out->offset = util_be32(e + 0x10);
    out->size = util_be32(e + 0x14);
    return true;
}

static bool write_all_bytes(const char* path, const uint8_t* data, size_t size)
{
    FILE* f = fopen(path, "wb");
    bool ok = false;

    if (!f) {
        LOG_ERROR("fopen(%s) failed", path);
        return false;
    }
    if (fwrite(data, 1, size, f) == size) {
        ok = true;
    } else {
        LOG_ERROR("fwrite(%s) failed", path);
    }
    fclose(f);
    return ok;
}

#define INSTALL_FAIL(...)                                                     \
    do {                                                                      \
        snprintf(err, errsz, __VA_ARGS__);                                    \
        LOG_ERROR("installer: %s", err);                                      \
        goto err;                                                             \
    } while (0)

/*
 * Shared pipeline for a validated list of piece URLs.
 * On failure the job is marked FAILED and tmp files are removed.
 */
static bool install_pieces(const char* const* urls, size_t url_count,
                           install_result_t* out, char* err, size_t errsz)
{
    int job_id = -1;
    char tmp_name[32];
    char manifest_path[64];
    char icon_path[64];
    char icon_url_path[64];
    char content_url[256 + 64];
    uint8_t* hdr_buf = NULL;
    uint8_t* table_buf = NULL;
    uint8_t* sfo_buf = NULL;
    uint8_t* icon_buf = NULL;
    pkg_header_info_t hdr;
    pkg_entry_t entry;
    uint64_t* piece_sizes = NULL;
    uint64_t total_sum = 0;
    char digest_str[PKG_DIGEST_SIZE * 2 + 1];
    char mini_digest_str[(PKG_DIGEST_SIZE - 12) * 2 + 1];
    char ip[16];
    char title[256];
    const char* package_type;
    int task_id = -1;
    int bgft_error = 0;
    bool has_icon = false;
    bool status = false;
    int lang;
    size_t i;

    if (!urls || url_count == 0 || !out || !err || errsz == 0) {
        return false;
    }
    err[0] = '\0';
    memset(out, 0, sizeof(*out));

    job_id = taskman_create(urls[0]);
    if (job_id < 0) {
        INSTALL_FAIL("task registry full");
    }
    out->job_id = job_id;
    taskman_set_state(job_id, TASK_STATE_PREPARING, 0);

    if (!netmon_is_up() || !netmon_get_ip(ip, sizeof(ip))) {
        INSTALL_FAIL("network is down");
    }
    lang = get_system_lang();

    snprintf(tmp_name, sizeof(tmp_name), "tmp_%06x%04x",
             s_tmp_counter++, (unsigned)job_id & 0xFFFF);
    snprintf(manifest_path, sizeof(manifest_path),
             INSTALLER_WORK_DIR "/%s.json", tmp_name);
    snprintf(icon_path, sizeof(icon_path),
             INSTALLER_WORK_DIR "/%s.png", tmp_name);
    snprintf(icon_url_path, sizeof(icon_url_path),
             "/user" INSTALLER_WORK_DIR "/%s.png", tmp_name);

    /* 1. pkg header from piece 0. */
    hdr_buf = (uint8_t*)malloc(PKG_HEADER_SIZE);
    if (!hdr_buf) {
        INSTALL_FAIL("out of memory");
    }
    {
        http_result_t r;
        if (!http_client_get(urls[0], 0, PKG_HEADER_SIZE, hdr_buf, PKG_HEADER_SIZE, &r)) {
            INSTALL_FAIL("unable to download pkg header from '%s'", urls[0]);
        }
        if (!HTTP_IS_OK(r.status)) {
            INSTALL_FAIL("pkg header request failed: HTTP %d", r.status);
        }
        if (r.body_len != PKG_HEADER_SIZE) {
            INSTALL_FAIL("pkg header size mismatch for '%s'", urls[0]);
        }
    }
    if (!pkg_parse_header(hdr_buf, PKG_HEADER_SIZE, &hdr)) {
        INSTALL_FAIL("invalid pkg format for '%s'", urls[0]);
    }
    if (!pkg_is_supported_content_type(hdr.content_type)) {
        INSTALL_FAIL("unsupported content type 0x%02X for '%s'",
                     hdr.content_type, urls[0]);
    }

    /* 2. entry table. */
    {
        http_result_t r;
        size_t table_size = (size_t)hdr.entry_count * PKG_ENTRY_TABLE_SIZE;
        table_buf = (uint8_t*)malloc(table_size);
        if (!table_buf) {
            INSTALL_FAIL("out of memory");
        }
        if (!http_client_get(urls[0], hdr.entry_table_offset, table_size,
                             table_buf, table_size, &r)) {
            INSTALL_FAIL("unable to download entry table for '%s'", urls[0]);
        }
        if (!HTTP_IS_OK(r.status) || r.body_len != table_size) {
            INSTALL_FAIL("entry table size mismatch for '%s'", urls[0]);
        }
    }

    /* 3. locate + fetch PARAM_SFO and ICON0_PNG. */
    sfo_buf = NULL;
    icon_buf = NULL;
    {
        size_t sfo_size = 0;
        size_t icon_size = 0;
        for (i = 0; i < hdr.entry_count; ++i) {
            if (!read_entry_from_table(table_buf,
                                       (size_t)hdr.entry_count * PKG_ENTRY_TABLE_SIZE,
                                       (uint32_t)i, &entry)) {
                break;
            }
            if (entry.id == PKG_ENTRY_ID_PARAM_SFO && !sfo_size && entry.size > 0 &&
                entry.size < 0x100000) {
                http_result_t r;
                sfo_size = entry.size;
                sfo_buf = (uint8_t*)malloc(sfo_size);
                if (!sfo_buf) {
                    INSTALL_FAIL("out of memory");
                }
                if (!http_client_get(urls[0], entry.offset, sfo_size,
                                     sfo_buf, sfo_size, &r) ||
                    !HTTP_IS_OK(r.status) || r.body_len != sfo_size) {
                    INSTALL_FAIL("unable to download param.sfo for '%s'", urls[0]);
                }
            } else if (entry.id == PKG_ENTRY_ID_ICON0_PNG && !icon_size &&
                       entry.size > 0 && entry.size < 0x1000000) {
                http_result_t r;
                icon_size = entry.size;
                icon_buf = (uint8_t*)malloc(icon_size);
                if (!icon_buf) {
                    INSTALL_FAIL("out of memory");
                }
                if (!http_client_get(urls[0], entry.offset, icon_size,
                                     icon_buf, icon_size, &r) ||
                    !HTTP_IS_OK(r.status) || r.body_len != icon_size) {
                    INSTALL_FAIL("unable to download icon0.png for '%s'", urls[0]);
                }
            }
        }
        if (!sfo_buf || sfo_size == 0) {
            INSTALL_FAIL("no param.sfo entry for '%s'", urls[0]);
        }
        if (!sfo_get_title(sfo_buf, sfo_size, lang, title, sizeof(title)) ||
            title[0] == '\0') {
            INSTALL_FAIL("unable to get title for '%s'", urls[0]);
        }
        if (icon_buf && icon_size > 0) {
            if (!write_all_bytes(icon_path, icon_buf, icon_size)) {
                INSTALL_FAIL("unable to write icon0.png for '%s'", urls[0]);
            }
            has_icon = true;
        }
    }

    /* 4. piece sizes (piece 0 known from the header request). */
    piece_sizes = (uint64_t*)calloc(url_count, sizeof(uint64_t));
    if (!piece_sizes) {
        INSTALL_FAIL("out of memory");
    }
    for (i = 0; i < url_count; ++i) {
        if (!http_client_probe(urls[i], &piece_sizes[i], NULL)) {
            INSTALL_FAIL("unable to probe '%s'", urls[i]);
        }
        if (piece_sizes[i] == 0) {
            INSTALL_FAIL("piece %zu has unknown size", i);
        }
        total_sum += piece_sizes[i];
    }
    if (hdr.package_size > 0 && total_sum != hdr.package_size) {
        INSTALL_FAIL("unexpected total file size (%llu != %llu) for '%s'",
                     (unsigned long long)total_sum,
                     (unsigned long long)hdr.package_size, urls[0]);
    }

    /* 5. write local manifest + icon. */
    util_hex_encode(hdr.digest, PKG_DIGEST_SIZE, digest_str, sizeof(digest_str));
    util_hex_encode(s_zero_mini_digest, sizeof(s_zero_mini_digest),
                    mini_digest_str, sizeof(mini_digest_str));

    {
        FILE* f = fopen(manifest_path, "wb");
        if (!f) {
            INSTALL_FAIL("fopen(%s) failed", manifest_path);
        }
        fprintf(f,
                "{\"originalFileSize\":%llu,\"packageDigest\":\"%s\","
                "\"numberOfSplitFiles\":%zu,\"pieces\":[",
                (unsigned long long)hdr.package_size, digest_str, url_count);
        for (i = 0, total_sum = 0; i < url_count; ++i) {
            fprintf(f,
                    "{\"url\":\"%s\",\"fileOffset\":%llu,\"fileSize\":%llu,"
                    "\"hashValue\":\"%s\"}",
                    urls[i], (unsigned long long)total_sum,
                    (unsigned long long)piece_sizes[i], mini_digest_str);
            if (i + 1 < url_count) {
                fputs(",", f);
            }
            total_sum += piece_sizes[i];
        }
        fputs("]}", f);
        fclose(f);
    }

    /* 6. package type + BGFT registration. */
    switch (hdr.content_type) {
        case PKG_CONTENT_TYPE_GD: package_type = "PS4GD"; break;
        case PKG_CONTENT_TYPE_AC: package_type = "PS4AC"; break;
        case PKG_CONTENT_TYPE_AL: package_type = "PS4AL"; break;
        case PKG_CONTENT_TYPE_DP: package_type = "PS4DP"; break;
        default:
            INSTALL_FAIL("unsupported content type 0x%02X", hdr.content_type);
            return false;
    }

    snprintf(content_url, sizeof(content_url), "http://%s:%d/static/%s.json",
             ip, INSTALLER_PORT, tmp_name);

    if (!bgft_register(hdr.content_id, content_url, title,
                       has_icon ? icon_url_path : NULL, package_type, NULL,
                       hdr.package_size, pkg_is_patch(hdr.content_flags),
                       &task_id, &bgft_error)) {
        taskman_set_state(job_id, TASK_STATE_FAILED, bgft_error);
        INSTALL_FAIL("unable to register download task: 0x%08X", bgft_error);
    }

    taskman_set_registered(job_id, task_id, hdr.content_id, title,
                           hdr.package_size, pkg_is_patch(hdr.content_flags),
                           has_icon, manifest_path,
                           has_icon ? icon_url_path : NULL);

    out->task_id = task_id;
    snprintf(out->title, sizeof(out->title), "%s", title);
    snprintf(out->content_id, sizeof(out->content_id), "%s", hdr.content_id);
    out->package_size = hdr.package_size;
    out->is_patch = pkg_is_patch(hdr.content_flags);

    LOG_INFO("install registered: job=%d bgft=%d title='%s' size=%llu",
             job_id, task_id, title, (unsigned long long)hdr.package_size);

    status = true;

err:
    if (!status) {
        if (job_id >= 0 && task_id < 0) {
            taskman_set_state(job_id, TASK_STATE_FAILED, bgft_error);
        }
        unlink(manifest_path);
        unlink(icon_path);
    }
    free(hdr_buf);
    free(table_buf);
    free(sfo_buf);
    free(icon_buf);
    free(piece_sizes);
    return status;
}

bool installer_install_direct(const char* const* urls, size_t url_count,
                              install_result_t* out, char* err, size_t errsz)
{
    size_t i;

    if (!urls || url_count == 0 || !out || !err || errsz == 0) {
        return false;
    }
    for (i = 0; i < url_count; ++i) {
        if (!urls[i] || urls[i][0] == '\0') {
            snprintf(err, errsz, "empty package url at index %zu", i);
            return false;
        }
        if (!util_starts_with(urls[i], "http://") &&
            !util_starts_with(urls[i], "https://")) {
            snprintf(err, errsz, "invalid package url at index %zu", i);
            return false;
        }
    }
    return install_pieces(urls, url_count, out, err, errsz);
}

bool installer_install_ref_pkg_url(const char* url, install_result_t* out,
                                   char* err, size_t errsz)
{
    http_result_t r;
    uint8_t* body = NULL;
    cJSON* root = NULL;
    const cJSON* pieces = NULL;
    const cJSON* item = NULL;
    char** urls = NULL;
    size_t count = 0;
    bool status = false;
    size_t i;

    if (!url || !out || !err || errsz == 0) {
        return false;
    }
    err[0] = '\0';
    memset(out, 0, sizeof(*out));

    if (!util_starts_with(url, "http://") && !util_starts_with(url, "https://")) {
        snprintf(err, errsz, "invalid reference url");
        return false;
    }

    /* Fetch the manifest (bounded; ref manifests are small). */
    body = (uint8_t*)malloc(1024 * 1024);
    if (!body) {
        snprintf(err, errsz, "out of memory");
        goto err;
    }
    if (!http_client_get(url, 0, 0, body, 1024 * 1024, &r)) {
        snprintf(err, errsz, "unable to download reference manifest");
        goto err;
    }
    if (!HTTP_IS_OK(r.status)) {
        snprintf(err, errsz, "reference manifest request failed: HTTP %d", r.status);
        goto err;
    }
    if (r.body_len == 0) {
        snprintf(err, errsz, "empty reference manifest");
        goto err;
    }
    body[r.body_len < (1024 * 1024) ? r.body_len : (1024 * 1024 - 1)] = '\0';

    root = cJSON_Parse((const char*)body);
    if (!root) {
        snprintf(err, errsz, "invalid reference manifest json");
        goto err;
    }

    pieces = cJSON_GetObjectItemCaseSensitive(root, "pieces");
    if (!cJSON_IsArray(pieces)) {
        snprintf(err, errsz, "reference manifest has no pieces");
        goto err;
    }
    count = (size_t)cJSON_GetArraySize(pieces);
    if (count == 0) {
        snprintf(err, errsz, "reference manifest has no pieces");
        goto err;
    }

    urls = (char**)calloc(count, sizeof(char*));
    if (!urls) {
        snprintf(err, errsz, "out of memory");
        goto err;
    }

    {
        size_t k = 0;
        cJSON_ArrayForEach(item, pieces) {
            const cJSON* url_item = cJSON_GetObjectItemCaseSensitive(item, "url");
            if (!cJSON_IsString(url_item) || url_item->valuestring[0] == '\0') {
                snprintf(err, errsz, "piece %zu has no url", k);
                goto err;
            }
            urls[k] = strdup(url_item->valuestring);
            if (!urls[k]) {
                snprintf(err, errsz, "out of memory");
                goto err;
            }
            ++k;
        }
    }

    status = install_pieces((const char* const*)urls, count, out, err, errsz);

err:
    if (urls) {
        for (i = 0; i < count; ++i) {
            free(urls[i]);
        }
        free(urls);
    }
    cJSON_Delete(root);
    free(body);
    return status;
}
