/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：用 esp_http_client 实现单请求：校验证书、吸收 Set-Cookie、按块检查取消。
 *
 * English: Single request over esp_http_client with certificate verification, Set-Cookie
 * absorption and per-chunk cancellation.
 *
 * 冻结：不跟随重定向；响应超限立即中止；任何日志都不得包含 cookie 值。
 * Frozen: No redirect following; abort on oversized responses; never log cookie values.
 */
#include "weread_http.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "weread_http";

// 只有 weread.qq.com 及其子域才允许带会话 cookie；封面走腾讯 CDN，绝不能带上 wr_skey。
// Only weread.qq.com and its subdomains may see the session cookie; covers live on a CDN.
static bool is_weread_host(const char *url) {
    const char *scheme = strstr(url, "://");
    if (!scheme) return false;
    const char *host = scheme + 3;
    const char *slash = strchr(host, '/');
    const size_t len = slash ? (size_t)(slash - host) : strlen(host);
    if (len == 0 || len >= 128) return false;
    char name[128];
    memcpy(name, host, len);
    name[len] = 0;
    char *colon = strchr(name, ':');
    if (colon) *colon = 0;
    for (char *p = name; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    if (!strcmp(name, "weread.qq.com")) return true;
    static const char suffix[] = ".weread.qq.com";
    const size_t name_len = strlen(name);
    return name_len > sizeof(suffix) - 1 && !strcmp(name + name_len - (sizeof(suffix) - 1), suffix);
}

typedef struct {
    char *out;
    size_t cap;
    size_t len;
    bool too_big;
    bool cancelled_hit;
    bool (*cancelled)(void *ctx);
    void *cancel_ctx;
    wr_session_t *session;
} request_ctx_t;

// 复用一个 HTTP 客户端以保留 TLS 连接；WeRead worker 串行调用，单事件上下文足够。
// Reuse one HTTP client to keep the TLS connection; the WeRead worker is serial, so one event context is enough.
static esp_http_client_handle_t s_client;
static bool s_client_follow_redirects;
static request_ctx_t s_ctx;

// Set-Cookie 只取第一个 name=value；已知名字才进会话，其余忽略。
// Take only the first name=value from Set-Cookie; store known names and ignore the rest.
static void absorb_set_cookie(request_ctx_t *ctx, const char *value) {
    if (!ctx->session || !value) return;
    while (*value == ' ') ++value;
    const char *equals = strchr(value, '=');
    if (!equals) return;
    const char *name_end = equals;
    while (name_end > value && name_end[-1] == ' ') --name_end;
    const size_t name_len = (size_t)(name_end - value);
    if (name_len < 3 || memcmp(value, "wr_", 3)) return;
    const char *start = equals + 1;
    while (*start == ' ') ++start;
    const char *end = strchr(start, ';');
    if (!end) end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
    (void)wr_session_set_cookie(ctx->session, value, name_len, start, (size_t)(end - start));
}

static esp_err_t event_handler(esp_http_client_event_t *event) {
    request_ctx_t *ctx = event->user_data;
    switch (event->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (event->header_key && !strcasecmp(event->header_key, "set-cookie")) {
                absorb_set_cookie(ctx, event->header_value);
            }
            return ESP_OK;
        case HTTP_EVENT_ON_DATA:
            if (!ctx->out || !event->data_len) return ESP_OK;
            if (ctx->cancelled && ctx->cancelled(ctx->cancel_ctx)) {
                ctx->cancelled_hit = true;
                return ESP_FAIL;
            }
            if (ctx->len + (size_t)event->data_len > ctx->cap) {
                ctx->too_big = true;
                return ESP_FAIL;
            }
            memcpy(ctx->out + ctx->len, event->data, (size_t)event->data_len);
            ctx->len += (size_t)event->data_len;
            return ESP_OK;
        default:
            return ESP_OK;
    }
}

static void close_client(void) {
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    s_client_follow_redirects = false;
}

void wr_http_cleanup(void) {
    close_client();
}

wr_http_result_t wr_http_perform(const wr_http_request_t *request) {
    if (!request || !request->path || request->path[0] != '/') return WR_HTTP_NET;
    char url[512];
    const bool absolute = request->path[0] == 'h' &&
                          (!strncmp(request->path, "http://", 7) || !strncmp(request->path, "https://", 8));
    const int url_len = absolute ? snprintf(url, sizeof(url), "%s", request->path)
                                 : snprintf(url, sizeof(url), "%s%s", WR_WEREAD_ORIGIN, request->path);
    if (url_len <= 0 || (size_t)url_len >= sizeof(url)) return WR_HTTP_NET;
    const bool weread = is_weread_host(url);
    if (request->out && !request->out_cap) return WR_HTTP_NET;
    if (request->out_len) *request->out_len = 0;
    if (request->status) *request->status = 0;

    const bool post = request->method && !strcasecmp(request->method, "POST");
    const bool follow = request->follow_redirects;
    const int timeout = request->timeout_ms > 0 ? request->timeout_ms : WR_HTTP_DEFAULT_TIMEOUT_MS;

    // 重定向策略不同的请求不能共用客户端；WeRead 接口默认关闭跟随。
    // Requests with a different redirect policy cannot share the client; WeRead APIs default off.
    if (s_client && s_client_follow_redirects != follow) close_client();
    if (!s_client) {
        esp_http_client_config_t config = {
            .url = url,
            .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
            .timeout_ms = timeout,
            .buffer_size = 4096,
            .disable_auto_redirect = !follow,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = event_handler,
            .user_data = &s_ctx,
            .keep_alive_enable = true,
            .keep_alive_idle = 10,
            .keep_alive_interval = 5,
            .keep_alive_count = 3,
        };
        s_client = esp_http_client_init(&config);
        if (!s_client) return WR_HTTP_NO_MEM;
        s_client_follow_redirects = follow;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.out = request->out;
    s_ctx.cap = request->out_cap;
    s_ctx.cancelled = request->cancelled;
    s_ctx.cancel_ctx = request->cancel_ctx;
    s_ctx.session = weread ? request->session : NULL;

    if (esp_http_client_set_url(s_client, url) != ESP_OK) {
        close_client();
        return WR_HTTP_NET;
    }
    esp_http_client_set_method(s_client, post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_timeout_ms(s_client, timeout);
    esp_http_client_set_header(s_client, "User-Agent", WR_WEREAD_USER_AGENT);
    esp_http_client_set_header(s_client, "Accept",
                               request->accept ? request->accept : "application/json, text/plain, */*");
    esp_http_client_set_header(s_client, "Origin", WR_WEREAD_ORIGIN);
    esp_http_client_set_header(s_client, "Referer", request->referer ? request->referer : WR_WEREAD_ORIGIN "/");
    char cookie[512];
    if (weread && request->session && wr_session_cookie_header(request->session, cookie, sizeof(cookie))) {
        esp_http_client_set_header(s_client, "Cookie", cookie);
    } else {
        esp_http_client_delete_header(s_client, "Cookie");
    }
    if (post) {
        esp_http_client_set_header(s_client, "Content-Type", "application/json;charset=UTF-8");
        esp_http_client_set_post_field(s_client, request->body, (int)request->body_len);
    } else {
        esp_http_client_delete_header(s_client, "Content-Type");
        esp_http_client_set_post_field(s_client, NULL, 0);
    }

    const esp_err_t err = esp_http_client_perform(s_client);
    const int status = esp_http_client_get_status_code(s_client);

    if (request->out_len) *request->out_len = s_ctx.len;
    if (request->status) *request->status = status;
    if (s_ctx.cancelled_hit) {
        close_client();
        return WR_HTTP_CANCELLED;
    }
    if (s_ctx.too_big) {
        close_client();
        return WR_HTTP_TOO_BIG;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request failed: path=%s err=%s", request->path, esp_err_to_name(err));
        close_client();
        return WR_HTTP_NET;
    }
    if (status < 200 || status >= 300) return WR_HTTP_STATUS;
    return WR_HTTP_OK;
}
