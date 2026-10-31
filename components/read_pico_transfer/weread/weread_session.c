/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：登录会话与免责标记的校验；设备构建内含 NVS 单 blob 读写。
 *
 * English: Validation of the login session and disclaimer marker; device builds add single-blob NVS I/O.
 *
 * 冻结：只保存 wr_vid / wr_skey / wr_rt / 昵称；不得写入响应体、完整 Cookie 头或任何令牌日志。
 * Frozen: Persist only wr_vid/wr_skey/wr_rt/nickname; never response bodies, raw Cookie headers or tokens in logs.
 */
#include "weread_session.h"

#include <stdio.h>
#include <string.h>

#ifndef WEREAD_HOST_TEST
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#endif

#define WR_DISCLAIMER_MAGIC 0x31445257u /* "WRD1" */
#define WR_DISCLAIMER_VERSION 1u
#define WR_NVS_NAMESPACE "rp_weread"

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t accepted;
} wr_disclaimer_t;

/* ---- 校验 / Validation ---- */

// cookie 值只允许可见 ASCII 且不含分隔符，避免拼头时注入。
// Cookie values allow visible ASCII only and no separators, preventing header injection.
static bool token_chars_ok(const char *text, size_t len) {
    if (!len) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 0x21 || c > 0x7e || c == ';' || c == ',' || c == '"' || c == '\\') return false;
    }
    return true;
}

static size_t bounded_len(const char *text, size_t cap) {
    size_t len = 0;
    while (len < cap && text[len]) ++len;
    return len;
}

static bool bounded_string(const char *text, size_t cap) {
    if (!text || !text[0]) return false;
    const size_t len = bounded_len(text, cap);
    return len < cap && token_chars_ok(text, len);
}

bool wr_session_valid(const wr_session_t *session) {
    if (!session) return false;
    if (session->magic != WR_SESSION_MAGIC || session->version != WR_SESSION_VERSION) return false;
    if (!bounded_string(session->vid, sizeof(session->vid))) return false;
    if (!bounded_string(session->skey, sizeof(session->skey))) return false;
    if (session->rt[0] && !bounded_string(session->rt, sizeof(session->rt))) return false;
    if (session->name[0]) {
        if (bounded_len(session->name, sizeof(session->name)) >= sizeof(session->name)) return false;
    }
    return true;
}

void wr_session_clear(wr_session_t *session) {
    if (!session) return;
    volatile unsigned char *p = (volatile unsigned char *)session;
    for (size_t i = 0; i < sizeof(*session); ++i) p[i] = 0;
}

bool wr_session_set_cookie(wr_session_t *session, const char *name, size_t name_len,
                           const char *value, size_t value_len) {
    if (!session || !name || !value || !token_chars_ok(value, value_len)) return false;
    char *target = NULL;
    size_t cap = 0;
    if (name_len == 6 && !memcmp(name, "wr_vid", 6)) {
        target = session->vid;
        cap = sizeof(session->vid);
    } else if (name_len == 7 && !memcmp(name, "wr_skey", 7)) {
        target = session->skey;
        cap = sizeof(session->skey);
    } else if (name_len == 5 && !memcmp(name, "wr_rt", 5)) {
        target = session->rt;
        cap = sizeof(session->rt);
    } else {
        return false;
    }
    if (value_len + 1 > cap) return false;
    memcpy(target, value, value_len);
    target[value_len] = 0;
    return true;
}

bool wr_session_cookie_header(const wr_session_t *session, char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    if (!wr_session_valid(session)) return false;
    const int written = session->rt[0]
        ? snprintf(out, cap, "wr_vid=%s; wr_skey=%s; wr_rt=%s", session->vid, session->skey, session->rt)
        : snprintf(out, cap, "wr_vid=%s; wr_skey=%s", session->vid, session->skey);
    return written > 0 && (size_t)written < cap;
}

/* ---- NVS / device only ---- */

#ifndef WEREAD_HOST_TEST

static bool disclaimer_storage_valid(const wr_disclaimer_t *record) {
    return record->magic == WR_DISCLAIMER_MAGIC && record->version == WR_DISCLAIMER_VERSION &&
           record->accepted == 1;
}

esp_err_t wr_session_load(wr_session_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    wr_session_clear(out);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WR_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    wr_session_t stored;
    size_t size = sizeof(stored);
    err = nvs_get_blob(handle, "session", &stored, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    if (size != sizeof(stored) || !wr_session_valid(&stored)) {
        wr_session_clear(&stored);
        return ESP_ERR_INVALID_STATE;
    }
    *out = stored;
    wr_session_clear(&stored);
    return ESP_OK;
}

esp_err_t wr_session_save(const wr_session_t *session) {
    if (!wr_session_valid(session)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WR_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "session", session, sizeof(*session));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t wr_session_erase(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WR_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, "session");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool wr_disclaimer_accepted(void) {
    nvs_handle_t handle;
    if (nvs_open(WR_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    wr_disclaimer_t record;
    size_t size = sizeof(record);
    const esp_err_t err = nvs_get_blob(handle, "disclaimer", &record, &size);
    nvs_close(handle);
    return err == ESP_OK && size == sizeof(record) && disclaimer_storage_valid(&record);
}

esp_err_t wr_disclaimer_accept(void) {
    const wr_disclaimer_t record = {
        .magic = WR_DISCLAIMER_MAGIC,
        .version = WR_DISCLAIMER_VERSION,
        .accepted = 1,
    };
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WR_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "disclaimer", &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

#endif /* WEREAD_HOST_TEST */
