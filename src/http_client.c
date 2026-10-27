/*
 * Remote PKG Installer NextGen
 * Copyright (c) 2025-present AldySan and contributors.
 * Licensed under the GNU General Public License v3.0. See LICENSE for details.
 *
 * libSceHttp wrapper. One-shot GET / range-GET requests with chunked reads,
 * plus reusable keep-alive sessions for hot loops (the segmented prefetch
 * engine issues thousands of range GETs per package; session reuse removes
 * the per-request TCP/TLS handshake).
 *
 * Notes:
 * - sceHttpAddRequestHeader mode and sceHttpsDisableOption flags are not
 *   defined by the toolchain headers; values below follow the platform
 *   HTTP SDK family (verified against VitaSDK's net/http.h).
 * - TLS certificate checks are disabled to keep downloads working against
 *   LAN hosts with self-signed certificates (same behaviour as the
 *   original application).
 * - http_client_term() is safe to call while requests or sessions are in
 *   flight: teardown is deferred until the last one finishes (the network
 *   monitor calls it when the link drops).
 */
#include "http_client.h"
#include "log.h"
#include "netmon.h"
#include "version.h"

#include <orbis/Http.h>
#include <orbis/_types/http.h>
#include <orbis/Ssl.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_CLIENT_HEAP_SIZE (1 * 1024 * 1024)
#define SSL_HEAP_SIZE (128 * 1024)
#define HTTP_CHUNK_SIZE (64 * 1024)
#define HTTP_CONNECT_TIMEOUT_US (10 * 1000 * 1000)
#define HTTP_RECV_TIMEOUT_US (30 * 1000 * 1000)

#define HTTP_HEADER_OVERWRITE 0

#define HTTPS_FLAG_SERVER_VERIFY 0x01u
#define HTTPS_FLAG_CLIENT_VERIFY 0x02u
#define HTTPS_FLAG_CN_CHECK 0x04u
#define HTTPS_FLAG_NOT_AFTER_CHECK 0x08u
#define HTTPS_FLAG_NOT_BEFORE_CHECK 0x10u
#define HTTPS_FLAG_KNOWN_CA_CHECK 0x20u

struct http_session {
    int tpl;
    int conn;
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_ready = false;
static int s_ssl_ctx = -1;
static int s_http_ctx = -1;
static int s_busy = 0;         /* in-flight requests + open sessions */
static bool s_term_pending = false;

/* Caller must hold s_lock. */
static bool ensure_init_locked(void)
{
    int ret;

    if (s_ready) {
        return true;
    }
    if (!netmon_is_up() || netmon_mem_id() < 0) {
        return false;
    }

    ret = sceSslInit(SSL_HEAP_SIZE);
    if (ret < 0) {
        LOG_ERROR("sceSslInit failed: 0x%08X", ret);
        return false;
    }
    s_ssl_ctx = ret;

    ret = sceHttpInit(netmon_mem_id(), s_ssl_ctx, HTTP_CLIENT_HEAP_SIZE);
    if (ret < 0) {
        LOG_ERROR("sceHttpInit failed: 0x%08X", ret);
        sceSslTerm();
        s_ssl_ctx = -1;
        return false;
    }
    s_http_ctx = ret;

    s_ready = true;
    s_term_pending = false;
    return true;
}

/* Caller must hold s_lock (and s_busy == 0). */
static void force_term_locked(void)
{
    s_term_pending = false;
    if (!s_ready) {
        return;
    }
    s_ready = false;
    sceHttpTerm(s_http_ctx);
    s_http_ctx = -1;
    sceSslTerm();
    s_ssl_ctx = -1;
}

/* Caller must hold s_lock. Releases one busy reference; runs a deferred
 * teardown when the last one leaves. */
static void busy_release_locked(void)
{
    if (s_busy > 0) {
        --s_busy;
    }
    if (s_busy == 0 && s_term_pending) {
        force_term_locked();
    }
}

/* Creates a configured request template. The caller must hold a busy
 * reference so s_http_ctx stays valid. */
static int create_template(void)
{
    unsigned int ssl_flags;
    int tpl;
    int ret;

    tpl = sceHttpCreateTemplate(s_http_ctx, APP_SHORT_NAME "/" APP_VERSION,
                                ORBIS_HTTP_VERSION_1_1, 1 /* auto redirect */);
    if (tpl < 0) {
        LOG_ERROR("sceHttpCreateTemplate failed: 0x%08X", tpl);
        return -1;
    }

    ret = sceHttpSetConnectTimeOut(tpl, HTTP_CONNECT_TIMEOUT_US);
    if (ret < 0) {
        LOG_WARN("sceHttpSetConnectTimeOut failed: 0x%08X", ret);
    }
    sceHttpSetRecvTimeOut(tpl, HTTP_RECV_TIMEOUT_US);

    ssl_flags = HTTPS_FLAG_SERVER_VERIFY | HTTPS_FLAG_CLIENT_VERIFY |
                HTTPS_FLAG_CN_CHECK | HTTPS_FLAG_KNOWN_CA_CHECK |
                HTTPS_FLAG_NOT_AFTER_CHECK | HTTPS_FLAG_NOT_BEFORE_CHECK;
    ret = sceHttpsDisableOption(tpl, ssl_flags);
    if (ret != 0) {
        /* Non-fatal: some firmwares refuse the full flag set. */
        LOG_DEBUG("sceHttpsDisableOption: 0x%08X", ret);
    }
    return tpl;
}

/* Parses "Content-Range: bytes 0-0/123456" total part. */
static bool parse_content_range_total(const char* headers, uint64_t* total)
{
    const char* p;
    char* end;
    unsigned long long v;

    if (!headers || !total) {
        return false;
    }
    p = strstr(headers, "content-range:");
    if (!p) {
        p = strstr(headers, "Content-Range:");
    }
    if (!p) {
        return false;
    }
    p = strstr(p, "/");
    if (!p) {
        return false;
    }
    v = strtoull(p + 1, &end, 10);
    if (end == p + 1 || v == 0) {
        return false;
    }
    *total = (uint64_t)v;
    return true;
}

/*
 * Core request over an existing template/connection pair: GET url
 * (optionally with a Range header value), reads up to buf capacity bytes
 * into buf (buf may be NULL to discard). resp_headers (optional) receives
 * the raw response headers. The caller must hold a busy reference (directly
 * or via a session) so the http context stays alive.
 */
static bool request_with(int tpl, int conn, const char* url, const char* range_value,
                         uint8_t* buf, size_t bufsz,
                         http_result_t* result,
                         char* resp_headers, size_t resp_headers_sz)
{
    int req = -1;
    uint64_t stored = 0;
    uint8_t* chunk = NULL;
    bool status = false;
    int ret;

    if (!url || !result) {
        return false;
    }
    memset(result, 0, sizeof(*result));

    chunk = (uint8_t*)malloc(HTTP_CHUNK_SIZE);
    if (!chunk) {
        return false;
    }

    do {
        req = sceHttpCreateRequestWithURL2(conn, "GET", url, (uint64_t)0);
        if (req < 0) {
            LOG_ERROR("sceHttpCreateRequestWithURL2 failed: 0x%08X", req);
            break;
        }

        ret = sceHttpAddRequestHeader(req, "Accept-Encoding", "identity",
                                      HTTP_HEADER_OVERWRITE);
        if (ret != 0) {
            LOG_ERROR("sceHttpAddRequestHeader failed: 0x%08X", ret);
            break;
        }
        if (range_value) {
            ret = sceHttpAddRequestHeader(req, "Range", range_value,
                                          HTTP_HEADER_OVERWRITE);
            if (ret != 0) {
                LOG_ERROR("sceHttpAddRequestHeader(Range) failed: 0x%08X", ret);
                break;
            }
        }

        ret = sceHttpSendRequest(req, NULL, 0);
        if (ret < 0) {
            LOG_ERROR("sceHttpSendRequest failed: 0x%08X", ret);
            break;
        }

        ret = sceHttpGetStatusCode(req, &result->status);
        if (ret < 0) {
            LOG_ERROR("sceHttpGetStatusCode failed: 0x%08X", ret);
            break;
        }

        if (HTTP_IS_OK(result->status)) {
            int clen_type = 0;
            size_t clen = 0;
            ret = sceHttpGetResponseContentLength(req, &clen_type, &clen);
            if (ret == 0) {
                result->total_len = (uint64_t)clen;
            }
            result->partial = (result->status == 206);

            if (resp_headers && resp_headers_sz > 0) {
                char* hdr = NULL;
                size_t hdr_sz = 0;
                if (sceHttpGetAllResponseHeaders(req, &hdr, &hdr_sz) == 0 && hdr) {
                    size_t n = hdr_sz;
                    if (n >= resp_headers_sz) {
                        n = resp_headers_sz - 1;
                    }
                    memcpy(resp_headers, hdr, n);
                    resp_headers[n] = '\0';
                }
            }

            /* Read the body in chunks; store up to bufsz bytes.
             * Mirrors the original: stop once the requested amount is
             * buffered (Range responses are exact; deleting the request
             * closes anything the server still tries to send). */
            for (;;) {
                size_t want = HTTP_CHUNK_SIZE;
                int n = sceHttpReadData(req, chunk, want);
                if (n < 0) {
                    LOG_ERROR("sceHttpReadData failed: 0x%08X", n);
                    break;
                }
                if (n == 0) {
                    break; /* EOF */
                }
                if (buf && stored < bufsz) {
                    size_t room = bufsz - (size_t)stored;
                    size_t cp = ((size_t)n < room) ? (size_t)n : room;
                    memcpy(buf + stored, chunk, cp);
                    stored += cp;
                }
                if (!buf || stored >= bufsz) {
                    break;
                }
            }
            result->body_len = stored;
        }

        status = true;
    } while (false);

    if (req >= 0) {
        sceHttpDeleteRequest(req);
    }
    free(chunk);
    return status;
}

/* One-shot request: creates (and tears down) a template + connection. */
static bool do_request(const char* url, const char* range_value,
                       uint8_t* buf, size_t bufsz,
                       http_result_t* result,
                       char* resp_headers, size_t resp_headers_sz)
{
    int tpl = -1;
    int conn = -1;
    bool ok = false;
    bool inited;

    if (!url || !result) {
        return false;
    }
    memset(result, 0, sizeof(*result));

    pthread_mutex_lock(&s_lock);
    inited = ensure_init_locked();
    if (inited) {
        ++s_busy;
    }
    pthread_mutex_unlock(&s_lock);
    if (!inited) {
        return false;
    }

    tpl = create_template();
    if (tpl >= 0) {
        conn = sceHttpCreateConnectionWithURL(tpl, url, 1 /* keepalive */);
        if (conn < 0) {
            LOG_ERROR("sceHttpCreateConnectionWithURL failed: 0x%08X", conn);
        }
    }
    if (tpl >= 0 && conn >= 0) {
        ok = request_with(tpl, conn, url, range_value, buf, bufsz,
                          result, resp_headers, resp_headers_sz);
    }

    if (conn >= 0) {
        sceHttpDeleteConnection(conn);
    }
    if (tpl >= 0) {
        sceHttpDeleteTemplate(tpl);
    }
    pthread_mutex_lock(&s_lock);
    busy_release_locked();
    pthread_mutex_unlock(&s_lock);
    return ok;
}

http_session_t* http_session_open(const char* url)
{
    http_session_t* s;
    bool inited;

    if (!url) {
        return NULL;
    }

    pthread_mutex_lock(&s_lock);
    inited = ensure_init_locked();
    if (inited) {
        ++s_busy;
    }
    pthread_mutex_unlock(&s_lock);
    if (!inited) {
        return NULL;
    }

    s = (http_session_t*)calloc(1, sizeof(*s));
    if (!s) {
        goto err;
    }
    s->tpl = create_template();
    if (s->tpl < 0) {
        goto err;
    }
    s->conn = sceHttpCreateConnectionWithURL(s->tpl, url, 1 /* keepalive */);
    if (s->conn < 0) {
        LOG_ERROR("sceHttpCreateConnectionWithURL failed: 0x%08X", s->conn);
        goto err;
    }
    return s;

err:
    if (s) {
        if (s->tpl >= 0) {
            sceHttpDeleteTemplate(s->tpl);
        }
        free(s);
    }
    pthread_mutex_lock(&s_lock);
    busy_release_locked();
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

void http_session_close(http_session_t* session)
{
    if (!session) {
        return;
    }
    /* Delete while the busy reference still pins the http context. */
    sceHttpDeleteConnection(session->conn);
    sceHttpDeleteTemplate(session->tpl);
    free(session);
    pthread_mutex_lock(&s_lock);
    busy_release_locked();
    pthread_mutex_unlock(&s_lock);
}

void http_client_term(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_busy > 0) {
        /* Requests/sessions still in flight; tear down when the last
         * one finishes (see busy_release_locked). */
        s_term_pending = true;
    } else {
        force_term_locked();
    }
    pthread_mutex_unlock(&s_lock);
}

bool http_client_get(const char* url, uint64_t offset, uint64_t length,
                     uint8_t* buf, size_t bufsz, http_result_t* result)
{
    char range[64];

    if (!url || !result) {
        return false;
    }
    if (length > 0) {
        snprintf(range, sizeof(range), "bytes=%llu-%llu",
                 (unsigned long long)offset,
                 (unsigned long long)(offset + length - 1));
        return do_request(url, range, buf, bufsz, result, NULL, 0);
    }
    return do_request(url, NULL, buf, bufsz, result, NULL, 0);
}

bool http_client_get_session(http_session_t** psess, const char* url,
                             uint64_t offset, uint64_t length,
                             uint8_t* buf, size_t bufsz, http_result_t* result)
{
    char range[64];
    const char* range_value = NULL;

    if (!psess || !url || !result) {
        return false;
    }
    if (length > 0) {
        snprintf(range, sizeof(range), "bytes=%llu-%llu",
                 (unsigned long long)offset,
                 (unsigned long long)(offset + length - 1));
        range_value = range;
    }

    if (!*psess) {
        *psess = http_session_open(url);
    }
    if (*psess) {
        if (request_with((*psess)->tpl, (*psess)->conn, url, range_value,
                         buf, bufsz, result, NULL, 0)) {
            return true;
        }
        /* Transport failure — likely a stale keep-alive connection:
         * retry once with a fresh session. */
        http_session_close(*psess);
        *psess = http_session_open(url);
        if (*psess) {
            return request_with((*psess)->tpl, (*psess)->conn, url, range_value,
                                buf, bufsz, result, NULL, 0);
        }
    }
    return false;
}

bool http_client_probe(const char* url, uint64_t* total_size, bool* accepts_ranges)
{
    http_result_t result;
    char headers[2048];
    uint64_t total = 0;
    bool ranged = false;

    if (!url) {
        return false;
    }

    memset(headers, 0, sizeof(headers));
    if (!do_request(url, "bytes=0-0", NULL, 0, &result, headers, sizeof(headers))) {
        return false;
    }

    if (result.partial) {
        ranged = true;
        if (!parse_content_range_total(headers, &total)) {
            /* 206 without a parseable Content-Range; fall back below. */
            ranged = false;
        }
    }
    if (total == 0) {
        total = result.total_len;
    }

    if (total_size) {
        *total_size = total;
    }
    if (accepts_ranges) {
        *accepts_ranges = ranged;
    }
    return HTTP_IS_OK(result.status);
}
