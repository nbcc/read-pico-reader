/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书 HTTPS 传输层：单请求、Cookie 吸收、取消与响应上限。
 *
 * English: WeRead HTTPS transport: one request, cookie absorption, cancellation and response cap.
 *
 * 冻结：只允许 weread.qq.com；校验证书（crt bundle）；禁止 setInsecure、重定向跟随与凭据日志。
 * Frozen: Only weread.qq.com; verify certificates through the crt bundle; no setInsecure, no redirect
 * following, no credential logging.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "weread_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WR_WEREAD_ORIGIN "https://weread.qq.com"
#define WR_WEREAD_USER_AGENT                                                              \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like "    \
    "Gecko) Chrome/135.0.0.0 Safari/537.36 Edg/135.0.0.0"
#define WR_HTTP_DEFAULT_TIMEOUT_MS 20000

typedef enum {
    WR_HTTP_OK = 0,
    WR_HTTP_NET,       ///< 连接/读取失败或超时 / transport failure or timeout
    WR_HTTP_STATUS,    ///< 非 2xx / non-2xx status
    WR_HTTP_TOO_BIG,   ///< 响应超过 out_cap / response exceeds out_cap
    WR_HTTP_CANCELLED, ///< 调用方取消 / cancelled by the caller
    WR_HTTP_NO_MEM,    ///< 客户端初始化失败 / client allocation failed
} wr_http_result_t;

typedef struct {
    const char *method;  ///< "GET" 或 "POST"；空串按 GET / empty means GET
    const char *path;    ///< 以 '/' 开头，不含主机 / starts with '/', no host
    const char *body;    ///< POST 体，可为 NULL / POST body, may be NULL
    size_t body_len;
    const char *accept;  ///< 覆盖默认 Accept / optional Accept override
    const char *referer; ///< 覆盖默认 Referer / optional Referer override
    int timeout_ms;      ///< 0 表示默认 / zero means default
    bool follow_redirects; ///< 仅公开页面使用；接口请求必须为 false / public pages only
    bool (*cancelled)(void *ctx);
    void *cancel_ctx;
    char *out;           ///< 响应体缓冲；可为 NULL / response buffer, may be NULL
    size_t out_cap;
    size_t *out_len;     ///< 实际长度 / actual length
    wr_session_t *session; ///< 吸收 Set-Cookie；可为 NULL / absorbs Set-Cookie, may be NULL
    int *status;         ///< 输出 HTTP 状态 / output status
} wr_http_request_t;

/// 执行一次请求；默认不跟随重定向。/ Perform one request; redirects are off by default.
wr_http_result_t wr_http_perform(const wr_http_request_t *request);
/// 释放复用的 HTTP 客户端；worker 退出时调用。/ Release the reused HTTP client at worker exit.
void wr_http_cleanup(void);

#ifdef __cplusplus
}
#endif
