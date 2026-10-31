/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书来源的 worker 与登录流程。请求串行、可取消，状态经快照发布给页面。
 *
 * English: WeRead source worker and login flow. Requests are serial and cancellable; the page reads
 * a consistent status snapshot.
 *
 * 冻结：只做登录（书架与缓存下一步接入）；不直接操作射频；不打印任何 cookie 值。
 * Frozen: Login only (shelf and caching arrive next); never touches the radio; never logs cookie values.
 */
#include "read_pico_weread.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "weread_bookpath.h"
#include "weread_content.h"
#include "weread_debug.h"
#include "weread_http.h"
#include "weread_protocol.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_xhtml.h"
#include "weread_zip.h"

static const char *TAG = "weread";

#define WR_CLOCK_MIN_EPOCH 1704067200u /* 2024-01-01T00:00:00Z */
#define WR_QR_POLL_MS 2000
#define WR_LOGIN_DEADLINE_MS 240000
#define WR_LOGIN_TIMEOUT_MS 10000
#define WR_LOGIN_MAX_FAILURES 3
#define WR_WORKER_STACK 16384
#define WR_JOIN_TIMEOUT_MS 15000
#define WR_SHELF_MAX 1024
#define WR_HTTP_BODY_MAX (2u * 1024u * 1024u)
#define WR_WORKSPACE_BASE "/sdcard/.readpico/weread"
#define WR_CHAPTER_BUF (512u * 1024u)
#define WR_XHTML_BUF (768u * 1024u)
#define WR_TOC_MAX_CHAPTERS WR_TOC_MAX
#define WR_JSON_ACCEPT "application/json, text/plain, */*"
#define WR_HTML_ACCEPT "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"

typedef enum { WR_REQ_NONE = 0, WR_REQ_RESUME, WR_REQ_LOGIN, WR_REQ_SHELF, WR_REQ_CACHE } wr_request_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static read_pico_weread_status_t s_status;
static wr_session_t s_session;
static wr_shelf_t s_shelf;
static wr_shelf_record_t *s_shelf_records;   // active table read by the UI
static wr_shelf_record_t *s_shelf_scratch;   // parse target, swapped in on success
static char *s_http_body;
static char s_root_dir[160];
static wr_toc_chapter_t *s_toc;
static size_t s_toc_count;
static char *s_chapter_buf;
static char *s_xhtml_buf;
static unsigned s_cache_index;
static esp_err_t (*s_file_changed)(const char *path);
static uint64_t (*s_free_bytes)(void *ctx);
static void *s_free_bytes_ctx;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_request_sem;
static SemaphoreHandle_t s_exit_sem;
static volatile wr_request_t s_request;
static volatile bool s_stop;
static volatile bool s_cancel;
static volatile bool s_busy;

static void run_shelf(bool allow_relogin);

/* ---- 状态发布 / Status publishing ---- */

static void publish(read_pico_weread_state_t state, read_pico_weread_error_t error, const char *message) {
    char text[sizeof(s_status.message)];
    wr_utf8_copy(text, sizeof(text), message ? message : "");
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.error = error;
    memcpy(s_status.message, text, sizeof(text));
    ++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}

static void publish_login(bool logged_in, const char *account) {
    portENTER_CRITICAL(&s_lock);
    s_status.logged_in = logged_in;
    if (account) wr_utf8_copy(s_status.account, sizeof(s_status.account), account);
    else s_status.account[0] = 0;
    ++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}

static void publish_qr(const char *url) {
    portENTER_CRITICAL(&s_lock);
    wr_utf8_copy(s_status.qr_url, sizeof(s_status.qr_url), url);
    ++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}

static void set_loading(bool loading) {
    portENTER_CRITICAL(&s_lock);
    s_status.loading = loading;
    ++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}

static void clear_qr(void) {
    portENTER_CRITICAL(&s_lock);
    s_status.qr_url[0] = 0;
    ++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}

/* ---- 请求 / Requests ---- */

static bool cancelled(void *ctx) {
    (void)ctx;
    return s_cancel;
}

static bool clock_ready(void) {
    if (time(NULL) >= (time_t)WR_CLOCK_MIN_EPOCH) return true;
    uint32_t utc = 0;
    if (read_pico_transfer_sync_time_online(&utc) != ESP_OK) return false;
    return time(NULL) >= (time_t)WR_CLOCK_MIN_EPOCH;
}

static wr_http_result_t get_ex(const char *path, const char *accept, char *out, size_t cap, size_t *len,
                               int *status, bool follow_redirects);

static wr_http_result_t get(const char *path, const char *accept, char *out, size_t cap, size_t *len, int *status) {
    return get_ex(path, accept, out, cap, len, status, false);
}

static wr_http_result_t get_ex(const char *path, const char *accept, char *out, size_t cap, size_t *len,
                               int *status, bool follow_redirects) {
    wr_http_request_t request = {
        .method = "GET",
        .path = path,
        .accept = accept,
        .timeout_ms = WR_LOGIN_TIMEOUT_MS,
        .follow_redirects = follow_redirects,
        .cancelled = cancelled,
        .out = out,
        .out_cap = cap,
        .out_len = len,
        .session = &s_session,
        .status = status,
    };
    return wr_http_perform(&request);
}

/* ---- JSON 小工具 / Small JSON helpers ---- */

static const char *json_string(const cJSON *root, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static bool json_bool(const cJSON *root, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valueint != 0;
    if (cJSON_IsString(item) && item->valuestring) {
        return !strcmp(item->valuestring, "1") || !strcmp(item->valuestring, "true");
    }
    return false;
}

/* ---- 登录 / Login ---- */

static bool fetch_login_uid(char *uid, size_t cap) {
    // 阅读页先建立会话 cookie；失败不致命，uid 请求会给出真正结论。
    // The skills page seeds session cookies; a failure here is not fatal, getLoginUid decides.
    size_t ignored = 0;
    int status = 0;
    const wr_http_result_t page = get_ex("/r/weread-skills", WR_HTML_ACCEPT, NULL, 0, &ignored, &status, true);
    if (page != WR_HTTP_OK) ESP_LOGW(TAG, "skills page: result=%d status=%d", (int)page, status);

    char body[1024];
    size_t len = 0;
    const wr_http_result_t result = get("/api/auth/getLoginUid", WR_JSON_ACCEPT, body, sizeof(body), &len, &status);
    if (result == WR_HTTP_CANCELLED) return false;
    if (result != WR_HTTP_OK || !len) {
        ESP_LOGW(TAG, "getLoginUid: result=%d status=%d", (int)result, status);
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(body, len);
    if (!root) return false;
    const char *value = json_string(root, "uid");
    const bool ok = value && value[0] && strlen(value) + 1 <= cap;
    if (ok) snprintf(uid, cap, "%s", value);
    cJSON_Delete(root);
    return ok;
}

static void save_session_after_login(const char *vid, const char *token) {
    s_session.magic = WR_SESSION_MAGIC;
    s_session.version = WR_SESSION_VERSION;
    if (!wr_session_set_cookie(&s_session, "wr_vid", 6, vid, strlen(vid)) ||
        !wr_session_set_cookie(&s_session, "wr_skey", 7, token, strlen(token)) ||
        !wr_session_valid(&s_session)) {
        wr_debug_log("login response lacks credentials");
        publish(RW_ERROR, RW_ERR_LOGIN_FAILED, "登录响应缺少凭据");
        return;
    }
    const esp_err_t saved = wr_session_save(&s_session);
    publish_login(true, NULL);
    if (saved != ESP_OK) {
        ESP_LOGW(TAG, "session save failed: %s", esp_err_to_name(saved));
        wr_debug_log("session save failed err=%s", esp_err_to_name(saved));
        publish(RW_ERROR, RW_ERR_SD, "登录成功，但会话未能保存");
        return;
    }
    wr_debug_log("session saved; load shelf");
    publish(RW_IDLE, RW_ERR_NONE, "登录成功");
    run_shelf(false);
}

static void run_login(void) {
    wr_debug_log("login begin clock=%d", clock_ready());
    publish(RW_IDLE, RW_ERR_NONE, "正在获取二维码…");
    if (!clock_ready()) {
        wr_debug_log("login rejected: clock not ready");
        publish(RW_ERROR, RW_ERR_CLOCK, "设备时间未校准，请先联网对时");
        return;
    }
    if (s_cancel) {
        publish(RW_IDLE, RW_ERR_CANCELLED, "已取消");
        return;
    }

    char uid[64];
    if (!fetch_login_uid(uid, sizeof(uid))) {
        wr_debug_log("login getLoginUid failed cancel=%d", s_cancel);
        if (s_cancel) publish(RW_IDLE, RW_ERR_CANCELLED, "已取消");
        else publish(RW_ERROR, RW_ERR_LOGIN_FAILED, "获取登录二维码失败");
        return;
    }
    wr_debug_log("login uid_len=%u", (unsigned)strlen(uid));

    char encoded[192];
    if (!wr_url_encode(uid, encoded, sizeof(encoded))) {
        publish(RW_ERROR, RW_ERR_PROTOCOL, "登录参数异常");
        return;
    }
    char qr_url[192];
    const int qr_len = snprintf(qr_url, sizeof(qr_url), "%s/web/confirm?uid=%s", WR_WEREAD_ORIGIN, encoded);
    if (qr_len <= 0 || (size_t)qr_len >= sizeof(qr_url)) {
        publish(RW_ERROR, RW_ERR_PROTOCOL, "登录参数异常");
        return;
    }
    publish_qr(qr_url);
    publish(RW_QR, RW_ERR_NONE, "请用微信扫码");
    wr_debug_log("login qr published url_len=%d", qr_len);

    char path[256];
    snprintf(path, sizeof(path), "/api/auth/getLoginInfo?uid=%s&otp=", encoded);
    const int64_t deadline = esp_timer_get_time() / 1000 + WR_LOGIN_DEADLINE_MS;
    unsigned failures = 0;
    bool waiting = false;

    for (;;) {
        if (s_cancel) {
            clear_qr();
            publish(RW_IDLE, RW_ERR_CANCELLED, "已取消");
            return;
        }
        if (esp_timer_get_time() / 1000 >= deadline) {
            clear_qr();
            wr_debug_log("login timeout");
            publish(RW_ERROR, RW_ERR_LOGIN_TIMEOUT, "二维码已过期，请重新获取");
            return;
        }

        char body[2048];
        size_t len = 0;
        int status = 0;
        const wr_http_result_t result = get(path, WR_JSON_ACCEPT, body, sizeof(body), &len, &status);
        if (result == WR_HTTP_CANCELLED) continue;
        if (result != WR_HTTP_OK || !len) {
            ++failures;
            wr_debug_log("login poll http result=%d status=%d failures=%u",
                         (int)result, status, failures);
            if (failures >= WR_LOGIN_MAX_FAILURES) {
                clear_qr();
                publish(RW_ERROR, RW_ERR_PROTOCOL, "网络不稳定，请重试");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(WR_QR_POLL_MS));
            continue;
        }
        failures = 0;

        cJSON *root = cJSON_ParseWithLength(body, len);
        if (!root) {
            clear_qr();
            wr_debug_log("login poll parse failed len=%u", (unsigned)len);
            publish(RW_ERROR, RW_ERR_PROTOCOL, "登录响应无法解析");
            return;
        }
        const bool succeed = json_bool(root, "succeed");
        const char *logic = json_string(root, "logicCode");
        // Some deployments wrap the payload in data; accept both shapes and common key names.
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (!cJSON_IsObject(payload)) payload = root;
        const char *vid = json_string(payload, "webLoginVid");
        if (!vid) vid = json_string(payload, "vid");
        if (!vid) vid = json_string(payload, "wr_vid");
        const char *token = json_string(payload, "accessToken");
        if (!token) token = json_string(payload, "skey");
        if (!token) token = json_string(payload, "wr_skey");
        const bool has_credentials = vid && vid[0] && token && token[0];
        char logic_copy[48];
        wr_utf8_copy(logic_copy, sizeof(logic_copy), logic ? logic : "");
        char vid_copy[WR_SESSION_VID_CAP];
        char token_copy[WR_SESSION_TOKEN_CAP];
        wr_utf8_copy(vid_copy, sizeof(vid_copy), vid ? vid : "");
        wr_utf8_copy(token_copy, sizeof(token_copy), token ? token : "");
        const bool logic_success = !strcmp(logic_copy, "LOGIN_SUCCESS");
        if (logic_success) {
            char keys[192] = {0};
            const cJSON *child = NULL;
            cJSON_ArrayForEach(child, root) {
                if (!child->string) continue;
                size_t used = strlen(keys);
                if (used + strlen(child->string) + 2 >= sizeof(keys)) break;
                if (used) keys[used++] = ',';
                strcpy(keys + used, child->string);
            }
            wr_debug_log("login LOGIN_SUCCESS succeed=%d vid_len=%u token_len=%u keys=%s",
                         succeed, (unsigned)strlen(vid_copy), (unsigned)strlen(token_copy), keys);
        }
        cJSON_Delete(root);

        if ((succeed || logic_success) && has_credentials) {
            clear_qr();
            wr_debug_log("login success vid_len=%u token_len=%u",
                         (unsigned)strlen(vid_copy), (unsigned)strlen(token_copy));
            save_session_after_login(vid_copy, token_copy);
            return;
        }
        if (!strcmp(logic_copy, "NEED_OTP")) {
            clear_qr();
            wr_debug_log("login requires OTP");
            publish(RW_ERROR, RW_ERR_OTP_REQUIRED, "该账号需要验证码（本版暂不支持）");
            return;
        }
        if (!strcmp(logic_copy, "LOGIN_TIMEOUT")) {
            clear_qr();
            wr_debug_log("login logic timeout");
            publish(RW_ERROR, RW_ERR_LOGIN_TIMEOUT, "二维码已过期，请重新获取");
            return;
        }
        if (logic_copy[0] && strcmp(logic_copy, "WAITING")) {
            clear_qr();
            wr_debug_log("login unexpected logic=%s", logic_copy);
            publish(RW_ERROR, RW_ERR_LOGIN_FAILED, "登录未完成");
            return;
        }
        if (!waiting) {
            waiting = true;
            wr_debug_log("login waiting for scan");
            publish(RW_WAIT_SCAN, RW_ERR_NONE, "等待手机确认");
        }
        vTaskDelay(pdMS_TO_TICKS(WR_QR_POLL_MS));
    }
}

// 会话失效：清掉本地凭据并重新扫码，只用于自动续期路径。
// Expired session: drop credentials and start a new QR flow; auto-resume path only.
static void relogin_after_expiry(void) {
    (void)wr_session_erase();
    wr_session_clear(&s_session);
    publish_login(false, NULL);
    run_login();
}

// 已有书架时保留列表，只在底部提示本次失败；没有可用数据才进错误页。
// Keep the list on a failed refresh; only go to the error page when nothing is available.
static void shelf_fail(read_pico_weread_error_t error, const char *message) {
    portENTER_CRITICAL(&s_lock);
    const bool has_data = s_shelf.count > 0;
    portEXIT_CRITICAL(&s_lock);
    set_loading(false);
    if (has_data) publish(RW_SHELF, error, message);
    else publish(RW_ERROR, error, message);
}

static bool shelf_ensure_buffers(void) {
    if (!s_http_body) {
        s_http_body = heap_caps_malloc(WR_HTTP_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_http_body != NULL && s_shelf_records != NULL && s_shelf_scratch != NULL;
}

static void run_shelf(bool allow_relogin) {
    wr_debug_log("shelf begin session=%d clock=%d allow_relogin=%d",
                 wr_session_valid(&s_session), clock_ready(), allow_relogin);
    set_loading(true);
    publish(s_status.state == RW_SHELF ? RW_SHELF : RW_IDLE, RW_ERR_NONE, "正在载入书架…");
    if (!wr_session_valid(&s_session)) {
        wr_debug_log("shelf rejected: no valid session");
        shelf_fail(RW_ERR_SESSION_EXPIRED, "请先扫码登录");
        return;
    }
    if (!clock_ready()) {
        wr_debug_log("shelf rejected: clock not ready");
        shelf_fail(RW_ERR_CLOCK, "设备时间未校准，请先联网对时");
        return;
    }
    if (!shelf_ensure_buffers()) {
        wr_debug_log("shelf rejected: no memory");
        shelf_fail(RW_ERR_NO_MEM, "内存不足，请重启后重试");
        return;
    }

    size_t len = 0;
    int status = 0;
    wr_http_result_t result = wr_http_perform(&(wr_http_request_t){
        .method = "GET",
        .path = "/web/shelf/sync",
        .accept = WR_JSON_ACCEPT,
        .timeout_ms = 15000,
        .cancelled = cancelled,
        .out = s_http_body,
        .out_cap = WR_HTTP_BODY_MAX,
        .out_len = &len,
        .session = &s_session,
        .status = &status,
    });
    wr_debug_log("shelf http result=%d status=%d len=%u",
                 (int)result, status, (unsigned)len);
    if (result == WR_HTTP_CANCELLED || s_cancel) {
        set_loading(false);
        publish(RW_IDLE, RW_ERR_CANCELLED, "已取消");
        return;
    }
    if (result != WR_HTTP_OK) {
        if (status == 401 || status == 403) {
            wr_debug_log("shelf session rejected status=%d", status);
            (void)wr_session_erase();
            wr_session_clear(&s_session);
            publish_login(false, NULL);
            if (allow_relogin) {
                set_loading(false);
                relogin_after_expiry();
            } else {
                shelf_fail(RW_ERR_SESSION_EXPIRED, "登录已失效，请重新扫码");
            }
            return;
        }
        wr_debug_log("shelf http failed result=%d status=%d", (int)result, status);
        shelf_fail(RW_ERR_PROTOCOL, result == WR_HTTP_TOO_BIG ? "书架数据过大" : "网络异常，请重试");
        return;
    }

    // 解析写进备用表，成功后整表切换；UI 不会读到半新半旧的书架。
    // Parse into the spare table and swap on success so the UI never sees a partial shelf.
    wr_shelf_t parsed = {.items = s_shelf_scratch, .capacity = WR_SHELF_MAX};
    if (!wr_shelf_parse(s_http_body, len, &parsed)) {
        wr_debug_log("shelf parse failed len=%u", (unsigned)len);
        shelf_fail(RW_ERR_PROTOCOL, "书架数据无法解析");
        return;
    }
    if (parsed.error_code == -2012) {
        wr_debug_log("shelf error_code=-2012; clear session");
        (void)wr_session_erase();
        wr_session_clear(&s_session);
        publish_login(false, NULL);
        if (allow_relogin) {
            set_loading(false);
            relogin_after_expiry();
        } else {
            shelf_fail(RW_ERR_SESSION_EXPIRED, "登录已失效，请重新扫码");
        }
        return;
    }
    if (parsed.error_code != 0) {
        wr_debug_log("shelf error_code=%d", (int)parsed.error_code);
        shelf_fail(RW_ERR_PROTOCOL, "微信读书返回了错误");
        return;
    }
    wr_debug_log("shelf parsed count=%u truncated=%d",
                 (unsigned)parsed.count, parsed.truncated);
    wr_shelf_sort(&parsed);
    portENTER_CRITICAL(&s_lock);
    wr_shelf_record_t *previous = s_shelf_records;
    s_shelf_records = s_shelf_scratch;
    s_shelf_scratch = previous;
    s_shelf.items = s_shelf_records;
    s_shelf.capacity = WR_SHELF_MAX;
    s_shelf.count = parsed.count;
    s_shelf.truncated = parsed.truncated;
    s_shelf.error_code = 0;
    s_status.shelf_count = (unsigned)parsed.count;
    ++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
    set_loading(false);
    (void)wr_session_save(&s_session);
    publish(RW_SHELF, RW_ERR_NONE, parsed.truncated ? "书架条目过多，仅显示前 1024 本" : "");
}


/* ---- 缓存一本书 / Cache one book ---- */

static void publish_cache(read_pico_weread_state_t state, read_pico_weread_error_t error,
                          const char *message, uint8_t stage, uint8_t percent,
                          unsigned done, unsigned total) {
    char text[sizeof(s_status.message)];
    wr_utf8_copy(text, sizeof(text), message ? message : "");
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.error = error;
    memcpy(s_status.message, text, sizeof(text));
    s_status.stage = stage;
    s_status.percent = percent;
    s_status.chapter_done = done;
    s_status.chapter_total = total;
    ++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}

static void publish_cache_book(const char *title, const char *book_id) {
    portENTER_CRITICAL(&s_lock);
    wr_utf8_copy(s_status.title, sizeof(s_status.title), title ? title : "");
    wr_utf8_copy(s_status.book_id, sizeof(s_status.book_id), book_id ? book_id : "");
    ++s_status.revision;
    portEXIT_CRITICAL(&s_lock);
}

static void cache_fail(read_pico_weread_error_t error, const char *message) {
    portENTER_CRITICAL(&s_lock);
    const bool has_data = s_shelf.count > 0;
    portEXIT_CRITICAL(&s_lock);
    if (has_data) publish_cache(RW_SHELF, error, message, 0, 0, 0, 0);
    else publish_cache(RW_ERROR, error, message, 0, 0, 0, 0);
}

static bool cache_ensure_buffers(void) {
    if (!s_toc) s_toc = heap_caps_malloc(WR_TOC_MAX_CHAPTERS * sizeof(*s_toc), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_chapter_buf) s_chapter_buf = heap_caps_malloc(WR_CHAPTER_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_xhtml_buf) s_xhtml_buf = heap_caps_malloc(WR_XHTML_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_toc && s_chapter_buf && s_xhtml_buf && shelf_ensure_buffers();
}

static bool append_fmt(char *buffer, size_t cap, size_t *used, const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer + *used, cap - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= cap - *used) return false;
    *used += (size_t)written;
    return true;
}


static bool http_call(const char *method, const char *path, const char *body, size_t body_len,
                      const char *accept, const char *referer, char *out, size_t cap,
                      size_t *out_len, int *status) {
    wr_http_request_t request = {
        .method = method,
        .path = path,
        .body = body,
        .body_len = body_len,
        .accept = accept,
        .referer = referer,
        .timeout_ms = 15000,
        .cancelled = cancelled,
        .out = out,
        .out_cap = cap,
        .out_len = out_len,
        .session = &s_session,
        .status = status,
    };
    return wr_http_perform(&request) == WR_HTTP_OK;
}

static bool fetch_reader_psvts(const char *book_id, char *psvts, size_t cap, const char **message) {
    char encoded[WR_ENCODE_ID_CAP];
    if (!wr_encode_id(book_id, encoded, sizeof(encoded))) {
        *message = "书籍参数异常";
        return false;
    }
    char path[160];
    snprintf(path, sizeof(path), "/web/reader/%s", encoded);
    size_t len = 0;
    int status = 0;
    if (!http_call("GET", path, NULL, 0, WR_HTML_ACCEPT, WR_WEREAD_ORIGIN "/", s_http_body,
                   WR_HTTP_BODY_MAX, &len, &status)) {
        *message = "打开微信读书阅读页失败";
        return false;
    }
    if (!wr_extract_json_field(s_http_body, len, "psvts", psvts, cap)) {
        *message = "阅读页缺少 psvts";
        return false;
    }
    return true;
}

static bool fetch_catalog(const char *book_id, const char *reader_referer, size_t *readable,
                          const char **message, read_pico_weread_error_t *error) {
    char body[96];
    const int written = snprintf(body, sizeof(body), "{\"bookIds\":[\"%s\"]}", book_id);
    if (written <= 0 || (size_t)written >= sizeof(body)) {
        *message = "书籍参数异常";
        return false;
    }
    size_t len = 0;
    int status = 0;
    if (!http_call("POST", "/web/book/chapterInfos", body, (size_t)written, WR_JSON_ACCEPT, reader_referer,
                   s_http_body, WR_HTTP_BODY_MAX, &len, &status)) {
        *message = "获取章节目录失败";
        return false;
    }
    size_t count = 0;
    int error_code = 0;
    memset(s_toc, 0, WR_TOC_MAX_CHAPTERS * sizeof(*s_toc));
    if (!wr_toc_parse(s_http_body, len, s_toc, WR_TOC_MAX_CHAPTERS, &count, &error_code)) {
        *message = "章节目录无法解析";
        return false;
    }
    if (error_code == -2012) {
        *error = RW_ERR_SESSION_EXPIRED;
        *message = "登录已失效，请重新扫码";
        return false;
    }
    if (error_code != 0) {
        *message = "微信读书返回了错误";
        return false;
    }
    if (count >= WR_TOC_MAX_CHAPTERS) {
        *message = "章节数量超出上限";
        return false;
    }
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (wr_toc_readable(&s_toc[i])) ++total;
    }
    if (!total) {
        *message = "这本书没有可缓存的章节";
        return false;
    }
    *readable = total;
    s_toc_count = count;
    return true;
}

static bool fetch_shard(const char *book_id, const char *chapter_uid, const char *psvts,
                        const char *referer, const char *endpoint, char *out, size_t cap,
                        size_t *out_len, const char **message) {
    char body[WR_CONTENT_JSON_CAP];
    if (!wr_make_content_json(book_id, chapter_uid, psvts, (uint32_t)time(NULL),
                              (uint32_t)(esp_timer_get_time() & 0xffff), false, body, sizeof(body))) {
        *message = "章节请求参数异常";
        return false;
    }
    int status = 0;
    if (!http_call("POST", endpoint, body, strlen(body), WR_JSON_ACCEPT, referer, out, cap, out_len, &status)) {
        *message = "下载章节内容失败";
        return false;
    }
    return true;
}

static bool contains_bytes(const char *haystack, size_t haystack_len, const char *needle, size_t needle_len) {
    if (!needle_len || haystack_len < needle_len) return false;
    for (size_t i = 0; i + needle_len <= haystack_len; ++i) {
        if (!memcmp(haystack + i, needle, needle_len)) return true;
    }
    return false;
}

static bool append_shard(const char *shard, size_t len, char *target, size_t cap, size_t *used, const char **message) {
    const char *body = NULL;
    size_t body_len = 0;
    if (!wr_shard_body(shard, len, &body, &body_len)) {
        *message = "章节内容校验失败";
        return false;
    }
    if (*used + body_len >= cap) {
        *message = "章节内容过大";
        return false;
    }
    memcpy(target + *used, body, body_len);
    *used += body_len;
    return true;
}

static bool cache_read_chapter(const wr_shelf_record_t *book, const char *chapter_uid, const char *psvts,
                               const char *referer, bool *txt_mode, size_t *chapter_len, const char **message) {
    size_t len = 0;
    int status = 0;

    if (!*txt_mode) {
        char body[WR_CONTENT_JSON_CAP];
        if (!wr_make_content_json(book->book_id, chapter_uid, psvts, (uint32_t)time(NULL),
                                  (uint32_t)(esp_timer_get_time() & 0xffff), false, body, sizeof(body))) {
            *message = "章节请求参数异常";
            return false;
        }
        if (!http_call("POST", "/web/book/chapter/e_0", body, strlen(body), WR_JSON_ACCEPT, referer,
                       s_http_body, WR_HTTP_BODY_MAX, &len, &status)) {
            *message = "下载章节内容失败";
            return false;
        }
        if (len >= 2 && s_http_body[0] == '{' && contains_bytes(s_http_body, len, "\"bookId\"", 8)) {
            *txt_mode = true;
        }
    }

    if (*txt_mode) {
        size_t used = 0;
        if (!fetch_shard(book->book_id, chapter_uid, psvts, referer, "/web/book/chapter/t_0",
                         s_http_body, WR_HTTP_BODY_MAX, &len, message)) return false;
        if (!append_shard(s_http_body, len, s_chapter_buf, WR_CHAPTER_BUF, &used, message)) return false;
        // t_1 可能为空或缺席；取不到就只用 t_0，但取消仍然中止。
        // t_1 may be empty or absent; fall back to t_0 while still honouring cancellation.
        if (!fetch_shard(book->book_id, chapter_uid, psvts, referer, "/web/book/chapter/t_1",
                         s_http_body, WR_HTTP_BODY_MAX, &len, message)) {
            if (s_cancel) return false;
            len = 0;
        }
        if (len && !append_shard(s_http_body, len, s_chapter_buf, WR_CHAPTER_BUF, &used, message)) return false;
        if (used) {
            size_t decoded_len = 0;
            if (!wr_decode_payload(s_chapter_buf, used, &decoded_len)) {
                *message = "章节内容解码失败";
                return false;
            }
            size_t xhtml_len = 0;
            if (!wr_txt_to_xhtml(s_chapter_buf, decoded_len, s_xhtml_buf, WR_XHTML_BUF, &xhtml_len)) {
                *message = "章节排版失败";
                return false;
            }
            *chapter_len = xhtml_len;
        } else {
            *chapter_len = 0;
        }
        return true;
    }

    size_t used = 0;
    if (!append_shard(s_http_body, len, s_chapter_buf, WR_CHAPTER_BUF, &used, message)) return false;
    if (!fetch_shard(book->book_id, chapter_uid, psvts, referer, "/web/book/chapter/e_1",
                     s_http_body, WR_HTTP_BODY_MAX, &len, message)) return false;
    if (!append_shard(s_http_body, len, s_chapter_buf, WR_CHAPTER_BUF, &used, message)) return false;
    if (!fetch_shard(book->book_id, chapter_uid, psvts, referer, "/web/book/chapter/e_3",
                     s_http_body, WR_HTTP_BODY_MAX, &len, message)) return false;
    if (!append_shard(s_http_body, len, s_chapter_buf, WR_CHAPTER_BUF, &used, message)) return false;

    size_t decoded_len = 0;
    if (!wr_decode_payload(s_chapter_buf, used, &decoded_len)) {
        *message = "章节内容解码失败";
        return false;
    }
    size_t xhtml_len = 0;
    if (!wr_sanitize_xhtml(s_chapter_buf, decoded_len, s_xhtml_buf, WR_XHTML_BUF, &xhtml_len)) {
        *message = "章节排版失败";
        return false;
    }
    *chapter_len = xhtml_len;
    return true;
}

static bool write_chapter_file(const char *directory, size_t index, const char *uid, const char *title,
                               const char *xhtml, size_t xhtml_len) {
    char path[288];
    if (snprintf(path, sizeof(path), "%s/%04u.xhtml", directory, (unsigned)index) >= (int)sizeof(path)) return false;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fputs("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n"
                    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>", file) >= 0;
    if (ok) {
        for (const char *p = title; *p && ok; ++p) {
            if (*p == '&') ok = fputs("&amp;", file) >= 0;
            else if (*p == '<') ok = fputs("&lt;", file) >= 0;
            else if (*p == '>') ok = fputs("&gt;", file) >= 0;
            else ok = fputc(*p, file) != EOF;
        }
    }
    if (ok) ok = fprintf(file, "</title><!--weread-uid:%s--><link rel=\"stylesheet\" type=\"text/css\" "
                               "href=\"../style.css\"/></head><body>\n", uid) >= 0;
    if (ok && xhtml_len) ok = fwrite(xhtml, 1, xhtml_len, file) == xhtml_len;
    if (ok) ok = fputs("\n</body></html>\n", file) >= 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

// 封面在腾讯 CDN 上，不带 cookie；失败只跳过封面，不影响整本。
// Covers live on a CDN and are fetched without cookies; failures only skip the cover.
static bool fetch_cover(const wr_shelf_record_t *book, const char *referer, size_t *cover_len,
                        const char **media_type, const char **extension) {
    if (!book->cover[0]) return false;
    char url[WR_SHELF_COVER_CAP + 8];
    if (!strncmp(book->cover, "//", 2)) {
        snprintf(url, sizeof(url), "https:%s", book->cover);
    } else if (!strncmp(book->cover, "http://", 7) || !strncmp(book->cover, "https://", 8)) {
        snprintf(url, sizeof(url), "%s", book->cover);
    } else {
        return false;
    }
    size_t len = 0;
    int status = 0;
    if (!http_call("GET", url, NULL, 0, "image/jpeg,image/png,image/*,*/*", referer, s_http_body,
                   WR_HTTP_BODY_MAX, &len, &status) || !len) {
        return false;
    }
    if (!wr_sniff_image(s_http_body, len, media_type, extension)) return false;
    *cover_len = len;
    return true;
}

static bool append_escaped(char *buffer, size_t cap, size_t *used, const char *text) {
    for (const char *p = text; p && *p; ++p) {
        const char *replacement = NULL;
        switch (*p) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&apos;"; break;
            default: break;
        }
        if (replacement) {
            if (!append_fmt(buffer, cap, used, "%s", replacement)) return false;
        } else if (!append_fmt(buffer, cap, used, "%c", *p)) {
            return false;
        }
    }
    return true;
}

static bool build_manifest(char *buffer, size_t cap, size_t *used, const char *book_title,
                           const char *author, const char *book_id, unsigned chapters,
                           const char *cover_href, const char *cover_media) {
    *used = 0;
    bool ok = append_fmt(buffer, cap, used,
                         "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                         "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" version=\"3.0\">\n"
                         "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                         "<dc:identifier id=\"bookid\">weread-%s</dc:identifier>\n<dc:title>",
                         book_id);
    if (ok) ok = append_escaped(buffer, cap, used, book_title);
    if (ok) ok = append_fmt(buffer, cap, used, "</dc:title>\n<dc:creator>");
    if (ok) ok = append_escaped(buffer, cap, used, author);
    if (ok) ok = append_fmt(buffer, cap, used,
                            "</dc:creator>\n<dc:language>zh-CN</dc:language>\n"
                            "<meta property=\"dcterms:modified\">1970-01-01T00:00:00Z</meta>\n");
    if (ok && cover_href) {
        ok = append_fmt(buffer, cap, used, "<meta name=\"cover\" content=\"cover-image\"/>\n");
    }
    if (ok) ok = append_fmt(buffer, cap, used,
                            "</metadata>\n<manifest>\n"
                            "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
                            "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
                            "<item id=\"style\" href=\"style.css\" media-type=\"text/css\"/>\n");
    if (ok && cover_href) {
        ok = append_fmt(buffer, cap, used,
                        "<item id=\"cover-image\" href=\"%s\" media-type=\"%s\" properties=\"cover-image\"/>\n",
                        cover_href, cover_media);
    }
    for (unsigned i = 1; ok && i <= chapters; ++i) {
        ok = append_fmt(buffer, cap, used,
                        "<item id=\"c%04u\" href=\"text/chapter-%04u.xhtml\" media-type=\"application/xhtml+xml\"/>\n",
                        i, i);
    }
    if (ok) ok = append_fmt(buffer, cap, used, "</manifest>\n<spine toc=\"ncx\">\n");
    for (unsigned i = 1; ok && i <= chapters; ++i) {
        ok = append_fmt(buffer, cap, used, "<itemref idref=\"c%04u\"/>\n", i);
    }
    if (ok) ok = append_fmt(buffer, cap, used, "</spine>\n</package>\n");
    return ok;
}

static bool build_nav(char *buffer, size_t cap, size_t *used, const char *book_title, unsigned chapters) {
    *used = 0;
    bool ok = append_fmt(buffer, cap, used,
                         "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                         "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
                         "<head><title>");
    if (ok) ok = append_escaped(buffer, cap, used, book_title);
    if (ok) ok = append_fmt(buffer, cap, used, "</title></head><body><nav epub:type=\"toc\"><ol>\n");
    unsigned emitted = 0;
    for (size_t i = 0; ok && i < s_toc_count && emitted < chapters; ++i) {
        if (!wr_toc_readable(&s_toc[i])) continue;
        ++emitted;
        ok = append_fmt(buffer, cap, used, "<li><a href=\"text/chapter-%04u.xhtml\">", emitted) &&
             append_escaped(buffer, cap, used, s_toc[i].title) && append_fmt(buffer, cap, used, "</a></li>\n");
    }
    if (ok) ok = append_fmt(buffer, cap, used, "</ol></nav></body></html>\n");
    return ok;
}

static bool build_ncx(char *buffer, size_t cap, size_t *used, const char *book_title, unsigned chapters) {
    *used = 0;
    bool ok = append_fmt(buffer, cap, used,
                         "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                         "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
                         "<head><meta name=\"dtb:uid\" content=\"weread\"/><meta name=\"dtb:depth\" content=\"1\"/>\n"
                         "<meta name=\"dtb:totalPageCount\" content=\"0\"/><meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n"
                         "</head><docTitle><text>");
    if (ok) ok = append_escaped(buffer, cap, used, book_title);
    if (ok) ok = append_fmt(buffer, cap, used, "</text></docTitle><navMap>\n");
    unsigned emitted = 0;
    for (size_t i = 0; ok && i < s_toc_count && emitted < chapters; ++i) {
        if (!wr_toc_readable(&s_toc[i])) continue;
        ++emitted;
        ok = append_fmt(buffer, cap, used,
                        "<navPoint id=\"p%04u\" playOrder=\"%u\"><navLabel><text>", emitted, emitted) &&
             append_escaped(buffer, cap, used, s_toc[i].title) &&
             append_fmt(buffer, cap, used, "</text></navLabel><content src=\"text/chapter-%04u.xhtml\"/></navPoint>\n",
                        emitted);
    }
    if (ok) ok = append_fmt(buffer, cap, used, "</navMap></ncx>\n");
    return ok;
}

static bool cache_book_inner(const wr_shelf_record_t *book, read_pico_weread_error_t *error, char *message,
                             size_t message_cap) {
    *error = RW_ERR_PROTOCOL;
    snprintf(message, message_cap, "缓存失败");
    char safe_id[64];
    size_t id_len = 0;
    for (const char *p = book->book_id; *p && id_len + 1 < sizeof(safe_id); ++p) {
        const unsigned char c = (unsigned char)*p;
        const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        safe_id[id_len++] = plain ? (char)c : '_';
    }
    safe_id[id_len] = 0;
    if (!id_len) return false;

    char workspace[128], chapters[160], work[160];
    if (snprintf(workspace, sizeof(workspace), "%s/%s", WR_WORKSPACE_BASE, safe_id) >= (int)sizeof(workspace) ||
        snprintf(chapters, sizeof(chapters), "%s/chapters", workspace) >= (int)sizeof(chapters) ||
        snprintf(work, sizeof(work), "%s/work", workspace) >= (int)sizeof(work)) {
        *error = RW_ERR_SD;
        snprintf(message, message_cap, "工作目录路径过长");
        return false;
    }
    (void)wr_remove_tree(workspace);
    if (!wr_ensure_dir(chapters) || !wr_ensure_dir(work)) {
        *error = RW_ERR_SD;
        snprintf(message, message_cap, "无法创建缓存目录");
        return false;
    }

    char reader_referer[160];
    char encoded_book[WR_ENCODE_ID_CAP];
    if (!wr_encode_id(book->book_id, encoded_book, sizeof(encoded_book))) {
        snprintf(message, message_cap, "书籍参数异常");
        return false;
    }
    snprintf(reader_referer, sizeof(reader_referer), "%s/web/reader/%s", WR_WEREAD_ORIGIN, encoded_book);

    publish_cache(RW_CACHING, RW_ERR_NONE, "正在获取目录", 0, 2, 0, 0);
    char psvts[128];
    const char *reason = NULL;
    if (!fetch_reader_psvts(book->book_id, psvts, sizeof(psvts), &reason)) {
        snprintf(message, message_cap, "%s", reason);
        return false;
    }
    size_t total = 0;
    if (!fetch_catalog(book->book_id, reader_referer, &total, &reason, error)) {
        snprintf(message, message_cap, "%s", reason);
        return false;
    }

    uint64_t estimate = 10ull * 1024 * 1024;
    for (size_t i = 0; i < s_toc_count; ++i) {
        if (wr_toc_readable(&s_toc[i])) estimate += (uint64_t)s_toc[i].words * 6ull;
    }
    if (s_free_bytes && s_free_bytes(s_free_bytes_ctx) < estimate) {
        *error = RW_ERR_NO_SPACE;
        snprintf(message, message_cap, "存储空间不足，至少需要 %u MB", (unsigned)(estimate >> 20));
        return false;
    }

    unsigned done = 0;
    unsigned written = 0;
    bool txt_mode = false;
    const int64_t deadline = esp_timer_get_time() / 1000 + 15ll * 60 * 1000;
    for (size_t i = 0; i < s_toc_count; ++i) {
        wr_toc_chapter_t *chapter = &s_toc[i];
        if (!wr_toc_readable(chapter)) continue;
        if (s_cancel) {
            *error = RW_ERR_CANCELLED;
            snprintf(message, message_cap, "已取消");
            return false;
        }
        if (esp_timer_get_time() / 1000 > deadline) {
            snprintf(message, message_cap, "缓存超时，请重试");
            return false;
        }
        size_t chapter_len = 0;
        if (!cache_read_chapter(book, chapter->uid, psvts, reader_referer, &txt_mode, &chapter_len, &reason)) {
            snprintf(message, message_cap, "%s", reason);
            return false;
        }
        ++written;
        if (!write_chapter_file(chapters, written, chapter->uid, chapter->title, s_xhtml_buf, chapter_len)) {
            *error = RW_ERR_SD;
            snprintf(message, message_cap, "写入章节失败");
            return false;
        }
        ++done;
        const uint8_t percent = (uint8_t)(5 + (total ? (80ull * done / total) : 0));
        publish_cache(RW_CACHING, RW_ERR_NONE, "", 1, percent, done, (unsigned)total);
    }

    size_t cover_len = 0;
    const char *cover_media = NULL;
    const char *cover_ext = NULL;
    char cover_href[16] = "";
    char cover_entry[32] = "";
    const bool has_cover = fetch_cover(book, reader_referer, &cover_len, &cover_media, &cover_ext);
    if (has_cover) {
        snprintf(cover_href, sizeof(cover_href), "cover.%s", cover_ext);
        snprintf(cover_entry, sizeof(cover_entry), "OEBPS/cover.%s", cover_ext);
    }

    publish_cache(RW_CACHING, RW_ERR_NONE, "正在打包", 2, 88, done, (unsigned)total);
    char part[288], central[288];
    if (snprintf(part, sizeof(part), "%s/book.epub.part", work) >= (int)sizeof(part) ||
        snprintf(central, sizeof(central), "%s/central.tmp", work) >= (int)sizeof(central)) {
        *error = RW_ERR_SD;
        snprintf(message, message_cap, "工作目录路径过长");
        return false;
    }
    remove(part);
    if (!wr_zip_begin(part, central)) {
        *error = RW_ERR_SD;
        snprintf(message, message_cap, "无法创建 EPUB 文件");
        return false;
    }
    static const char kContainer[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
        "</rootfiles></container>";
    static const char kStyle[] = "body{line-height:1.7;margin:5%;}img{max-width:100%;}p{margin:0 0 0.8em 0;}";
    size_t used = 0;
    bool packaged = wr_zip_add_buffer("mimetype", "application/epub+zip", 20) &&
                    wr_zip_add_buffer("META-INF/container.xml", kContainer, strlen(kContainer)) &&
                    build_manifest(s_chapter_buf, WR_CHAPTER_BUF, &used, book->title, book->author,
                                   book->book_id, written, has_cover ? cover_href : NULL,
                                   has_cover ? cover_media : NULL) &&
                    wr_zip_add_buffer("OEBPS/content.opf", s_chapter_buf, used) &&
                    build_nav(s_chapter_buf, WR_CHAPTER_BUF, &used, book->title, written) &&
                    wr_zip_add_buffer("OEBPS/nav.xhtml", s_chapter_buf, used) &&
                    build_ncx(s_chapter_buf, WR_CHAPTER_BUF, &used, book->title, written) &&
                    wr_zip_add_buffer("OEBPS/toc.ncx", s_chapter_buf, used) &&
                    wr_zip_add_buffer("OEBPS/style.css", kStyle, strlen(kStyle)) &&
                    (!has_cover || wr_zip_add_buffer(cover_entry, s_http_body, cover_len));
    for (unsigned i = 1; packaged && i <= written; ++i) {
        char source[288], name[64];
        snprintf(source, sizeof(source), "%s/%04u.xhtml", chapters, i);
        snprintf(name, sizeof(name), "OEBPS/text/chapter-%04u.xhtml", i);
        packaged = wr_zip_add_file(name, source);
    }
    if (!packaged || !wr_zip_finish()) {
        wr_zip_abort();
        *error = RW_ERR_SD;
        snprintf(message, message_cap, "打包 EPUB 失败");
        return false;
    }

    publish_cache(RW_CACHING, RW_ERR_NONE, "正在保存", 3, 96, done, (unsigned)total);
    char final[288], directory[288];
    if (!wr_book_file_path(s_root_dir, book->title, book->book_id, final, sizeof(final))) {
        *error = RW_ERR_SD;
        snprintf(message, message_cap, "书库路径异常");
        return false;
    }
    snprintf(directory, sizeof(directory), "%s/%s", s_root_dir, WR_BOOK_SUBDIR);
    if (!wr_ensure_dir(directory) || !wr_book_commit(part, final)) {
        *error = RW_ERR_SD;
        snprintf(message, message_cap, "保存 EPUB 失败");
        return false;
    }
    if (s_file_changed) (void)s_file_changed(final);
    (void)wr_remove_tree(workspace);
    return true;
}

static void workspace_path(const char *book_id, char *out, size_t cap) {
    char safe_id[64];
    size_t used = 0;
    for (const char *p = book_id; p && *p && used + 1 < sizeof(safe_id); ++p) {
        const unsigned char c = (unsigned char)*p;
        const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           c == '_' || c == '-';
        safe_id[used++] = plain ? (char)c : '_';
    }
    safe_id[used] = 0;
    snprintf(out, cap, "%s/%s", WR_WORKSPACE_BASE, safe_id);
}

// 成功与失败都清工作区；失败路径由包装器统一收尾。/ The wrapper removes the workspace on every failure.
static bool cache_book(const wr_shelf_record_t *book, read_pico_weread_error_t *error, char *message,
                       size_t message_cap) {
    const bool ok = cache_book_inner(book, error, message, message_cap);
    if (!ok) {
        char workspace[128];
        workspace_path(book->book_id, workspace, sizeof(workspace));
        (void)wr_remove_tree(workspace);
    }
    return ok;
}

static void run_cache(unsigned index) {
    wr_shelf_record_t book;
    portENTER_CRITICAL(&s_lock);
    const bool valid = index < s_shelf.count;
    if (valid) book = s_shelf.items[index];
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        cache_fail(RW_ERR_PROTOCOL, "书架条目已变化，请重试");
        return;
    }
    publish_cache_book(book.title, book.book_id);
    if (!wr_session_valid(&s_session)) {
        cache_fail(RW_ERR_SESSION_EXPIRED, "请先扫码登录");
        return;
    }
    if (!clock_ready()) {
        cache_fail(RW_ERR_CLOCK, "设备时间未校准，请先联网对时");
        return;
    }
    if (!cache_ensure_buffers()) {
        cache_fail(RW_ERR_NO_MEM, "内存不足，请重启后重试");
        return;
    }
    read_pico_weread_error_t error = RW_ERR_PROTOCOL;
    char message[96];
    if (cache_book(&book, &error, message, sizeof(message))) {
        char notice[96];
        snprintf(notice, sizeof(notice), "已缓存《%.60s》", book.title);
        publish_cache(RW_SHELF, RW_ERR_NONE, notice, 4, 100, 0, 0);
    } else {
        if (s_cancel) {
            error = RW_ERR_CANCELLED;
            snprintf(message, sizeof(message), "已取消");
        }
        cache_fail(error, message);
    }
}

// 进入来源时决定"复用会话"还是"直接扫码"；NVS 读取放在 worker 上，不占事件任务。
// Decide between reusing a session and starting the QR login; NVS reads stay on the worker.
static void run_resume(void) {
    wr_session_t stored;
    wr_debug_log("resume begin");
    if (wr_session_load(&stored) == ESP_OK) {
        s_session = stored;
        wr_session_clear(&stored);
        wr_debug_log("resume session found; load shelf");
        publish_login(true, NULL);
        publish(RW_IDLE, RW_ERR_NONE, "已登录");
        run_shelf(true);
        return;
    }
    wr_session_clear(&s_session);
    wr_debug_log("resume no session; start qr login");
    publish_login(false, NULL);
    run_login();
}

/* ---- worker ---- */

static void worker(void *argument) {
    (void)argument;
    for (;;) {
        xSemaphoreTake(s_request_sem, portMAX_DELAY);
        if (s_stop) break;
        const wr_request_t request = s_request;
        s_request = WR_REQ_NONE;
        if (request == WR_REQ_LOGIN) {
            run_login();
        } else if (request == WR_REQ_RESUME) {
            run_resume();
        } else if (request == WR_REQ_SHELF) {
            run_shelf(false);
        } else if (request == WR_REQ_CACHE) {
            run_cache(s_cache_index);
        }
        s_busy = false;
    }
    xSemaphoreGive(s_exit_sem);
    vTaskDelete(NULL);
}

/* ---- 生命周期 / Lifecycle ---- */

esp_err_t wr_service_prepare(void) {
    if (s_task) return ESP_OK;
    s_request_sem = xSemaphoreCreateBinary();
    s_exit_sem = xSemaphoreCreateBinary();
    if (!s_request_sem || !s_exit_sem) {
        if (s_request_sem) vSemaphoreDelete(s_request_sem);
        if (s_exit_sem) vSemaphoreDelete(s_exit_sem);
        s_request_sem = s_exit_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_stop = s_cancel = s_busy = false;
    s_request = WR_REQ_NONE;
    if (!s_shelf_records) {
        s_shelf_records = heap_caps_malloc(WR_SHELF_MAX * sizeof(*s_shelf_records),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_shelf_scratch) {
        s_shelf_scratch = heap_caps_malloc(WR_SHELF_MAX * sizeof(*s_shelf_scratch),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_shelf_records || !s_shelf_scratch) {
        free(s_shelf_records);
        free(s_shelf_scratch);
        s_shelf_records = s_shelf_scratch = NULL;
        s_shelf = (wr_shelf_t){0};
        vSemaphoreDelete(s_request_sem);
        vSemaphoreDelete(s_exit_sem);
        s_request_sem = s_exit_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_shelf.items = s_shelf_records;
    s_shelf.capacity = WR_SHELF_MAX;
    s_shelf.count = 0;
    s_shelf.truncated = false;
    s_shelf.error_code = 0;
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = RW_IDLE;
    s_status.revision = 1;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(worker, "weread", WR_WORKER_STACK, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        vSemaphoreDelete(s_request_sem);
        vSemaphoreDelete(s_exit_sem);
        s_request_sem = s_exit_sem = NULL;
        wr_debug_log("service prepare failed: no task");
        return ESP_ERR_NO_MEM;
    }
    wr_debug_log("service prepared");
    return ESP_OK;
}

void wr_service_on_network_ready(void) {
    if (!s_task) return;
    wr_debug_log("network ready");
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_busy || s_request != WR_REQ_NONE;
    if (!busy) {
        s_request = WR_REQ_RESUME;
        s_busy = true;
        s_cancel = false;
    }
    portEXIT_CRITICAL(&s_lock);
    wr_debug_log("network ready busy=%d request=%d", busy, (int)s_request);
    if (!busy) xSemaphoreGive(s_request_sem);
}

void wr_service_shutdown(void) {
    if (!s_task) return;
    s_cancel = true;
    s_stop = true;
    s_busy = false;
    xSemaphoreGive(s_request_sem);
    if (xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(WR_JOIN_TIMEOUT_MS)) != pdTRUE) {
        // 取消只在分块之间生效，阻塞读要等自身超时；宁可多等，也不能拆掉射频后在用。
        // Cancellation lands between chunks, so a blocked read waits for its timeout; wait it out
        // rather than tearing down the radio underneath a running request.
        ESP_LOGW(TAG, "worker join exceeded %d ms", WR_JOIN_TIMEOUT_MS);
        xSemaphoreTake(s_exit_sem, portMAX_DELAY);
    }
    s_task = NULL;
    vSemaphoreDelete(s_request_sem);
    vSemaphoreDelete(s_exit_sem);
    s_request_sem = s_exit_sem = NULL;
    free(s_http_body);
    s_http_body = NULL;
    free(s_shelf_records);
    s_shelf_records = NULL;
    free(s_shelf_scratch);
    s_shelf_scratch = NULL;
    free(s_toc);
    s_toc = NULL;
    s_toc_count = 0;
    free(s_chapter_buf);
    s_chapter_buf = NULL;
    free(s_xhtml_buf);
    s_xhtml_buf = NULL;
    s_shelf = (wr_shelf_t){0};
    s_root_dir[0] = 0;
    s_stop = s_cancel = false;
    s_request = WR_REQ_NONE;
}

esp_err_t wr_service_login(void) {
    if (!s_task) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_busy || s_request != WR_REQ_NONE;
    if (!busy) {
        s_request = WR_REQ_LOGIN;
        s_busy = true;
        s_cancel = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (busy) return ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_request_sem);
    return ESP_OK;
}

void wr_service_cancel(void) {
    s_cancel = true;
}

bool wr_service_busy(void) {
    return s_busy;
}

void wr_service_get_status(read_pico_weread_status_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t wr_service_logout(void) {
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_cancel = true;
    const esp_err_t err = wr_session_erase();
    wr_session_clear(&s_session);
    clear_qr();
    publish_login(false, NULL);
    publish(RW_IDLE, RW_ERR_NONE, "已退出登录");
    return err;
}

/* ---- 公共包装 / Public wrappers ---- */

esp_err_t read_pico_transfer_weread_login(void) {
    return wr_service_login();
}

esp_err_t read_pico_transfer_weread_shelf(void) {
    if (!s_task) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_busy || s_request != WR_REQ_NONE;
    if (!busy) {
        s_request = WR_REQ_SHELF;
        s_busy = true;
        s_cancel = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (busy) return ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_request_sem);
    return ESP_OK;
}

unsigned read_pico_transfer_weread_shelf_count(void) {
    portENTER_CRITICAL(&s_lock);
    const unsigned count = (unsigned)s_shelf.count;
    portEXIT_CRITICAL(&s_lock);
    return count;
}

bool read_pico_transfer_weread_shelf_item(unsigned index, read_pico_weread_shelf_item_t *out) {
    if (!out) return false;
    char root[sizeof(s_root_dir)];
    portENTER_CRITICAL(&s_lock);
    if (index >= s_shelf.count) {
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    const wr_shelf_record_t *record = &s_shelf.items[index];
    memset(out, 0, sizeof(*out));
    memcpy(out->book_id, record->book_id, sizeof(out->book_id));
    memcpy(out->title, record->title, sizeof(out->title));
    memcpy(out->author, record->author, sizeof(out->author));
    out->update_time = record->update_time;
    memcpy(root, s_root_dir, sizeof(root));
    portEXIT_CRITICAL(&s_lock);
    out->cached = root[0] && wr_book_file_exists(root, out->title, out->book_id);
    return true;
}

void wr_service_set_file_changed(esp_err_t (*callback)(const char *path)) {
    s_file_changed = callback;
}

void wr_service_set_free_bytes(uint64_t (*callback)(void *ctx), void *ctx) {
    s_free_bytes = callback;
    s_free_bytes_ctx = ctx;
}

void wr_service_set_root(const char *root_dir) {
    snprintf(s_root_dir, sizeof(s_root_dir), "%s", root_dir ? root_dir : "");
}

esp_err_t read_pico_transfer_weread_cache(unsigned index) {
    if (!s_task) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_busy || s_request != WR_REQ_NONE;
    const bool valid = index < s_shelf.count;
    if (!busy && valid) {
        s_cache_index = index;
        s_request = WR_REQ_CACHE;
        s_busy = true;
        s_cancel = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (busy) return ESP_ERR_INVALID_STATE;
    if (!valid) return ESP_ERR_INVALID_ARG;
    xSemaphoreGive(s_request_sem);
    return ESP_OK;
}

esp_err_t read_pico_transfer_weread_cancel(void) {
    wr_service_cancel();
    return ESP_OK;
}

void read_pico_transfer_weread_get_status(read_pico_weread_status_t *out) {
    wr_service_get_status(out);
}

bool read_pico_transfer_weread_busy(void) {
    return wr_service_busy();
}

esp_err_t read_pico_transfer_weread_logout(void) {
    return wr_service_logout();
}

bool read_pico_transfer_weread_disclaimer_accepted(void) {
    return wr_disclaimer_accepted();
}

esp_err_t read_pico_transfer_weread_accept_disclaimer(void) {
    return wr_disclaimer_accept();
}
