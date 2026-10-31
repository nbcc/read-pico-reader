/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书来源的内部接口：worker 生命周期、登录任务与状态发布。
 *
 * English: Internal WeRead source interface: worker lifecycle, login job and status publishing.
 *
 * 冻结：只有本文件能创建/销毁 WeRead worker；网络由 transfer 会话提供，不得调用 esp_wifi_*。
 * Frozen: Only this file owns the WeRead worker; networking comes from the transfer session and
 * this code must never call esp_wifi_*.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "read_pico_transfer.h"
#include "weread_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 射频启动前预留 worker 栈；失败则整个会话启动失败。/ Reserve the worker stack before the radio starts.
esp_err_t wr_service_prepare(void);
/// 网络就绪：有会话直接可用，否则自动开始扫码。/ On network ready: reuse a session or start the QR login.
void wr_service_on_network_ready(void);
/// 取消并汇合 worker；可重复调用。/ Cancel and join the worker; safe to repeat.
void wr_service_shutdown(void);
/// 异步开始扫码登录；正忙返回 ESP_ERR_INVALID_STATE。/ Start QR login asynchronously.
esp_err_t wr_service_login(void);
/// 记录书库根目录，用于"已缓存"判断；在会话启动时调用。/ Record the library root for the cached marker.
void wr_service_set_root(const char *root_dir);
/// 注入提交回调与可用空间查询。/ Inject the commit callback and the free-space query.
void wr_service_set_file_changed(esp_err_t (*callback)(const char *path));
void wr_service_set_free_bytes(uint64_t (*callback)(void *ctx), void *ctx);
/// 置取消标志，立即返回。/ Request cancellation and return immediately.
void wr_service_cancel(void);
/// worker 正在处理任务。/ A job is running on the worker.
bool wr_service_busy(void);
/// 一致状态快照。/ Consistent status snapshot.
void wr_service_get_status(read_pico_weread_status_t *out);
/// 退出登录；正忙时返回 ESP_ERR_INVALID_STATE。/ Log out; rejected while busy.
esp_err_t wr_service_logout(void);

/// 会话与免责标记的 NVS 读写（实现在 weread_session.c，仅设备构建）。/ NVS I/O implemented in weread_session.c.
esp_err_t wr_session_load(wr_session_t *out);
esp_err_t wr_session_save(const wr_session_t *session);
esp_err_t wr_session_erase(void);
bool wr_disclaimer_accepted(void);
esp_err_t wr_disclaimer_accept(void);

#ifdef __cplusplus
}
#endif
