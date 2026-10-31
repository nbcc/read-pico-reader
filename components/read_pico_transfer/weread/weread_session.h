/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书登录会话的定长结构与校验；不含 NVS 读写（设备侧在 .c 内）。
 *
 * English: Fixed-size WeRead login session and validation; NVS I/O lives in the .c for device builds.
 *
 * 冻结：结构定长、字段有界；任何非法字符或超长值都视为无效会话，不得猜测修复。
 * Frozen: Fixed layout with bounded fields; any illegal byte or overlong value invalidates the session.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WR_SESSION_MAGIC 0x31535257u  /* "WRS1" 小端 / little-endian */
#define WR_SESSION_VERSION 1u

#define WR_SESSION_VID_CAP 64
#define WR_SESSION_TOKEN_CAP 384
#define WR_SESSION_NAME_CAP 64

typedef struct {
    uint32_t magic;   ///< WR_SESSION_MAGIC
    uint16_t version; ///< WR_SESSION_VERSION
    uint16_t reserved;
    char vid[WR_SESSION_VID_CAP];      ///< wr_vid
    char skey[WR_SESSION_TOKEN_CAP];   ///< wr_skey
    char rt[WR_SESSION_TOKEN_CAP];     ///< wr_rt，可为空 / optional
    char name[WR_SESSION_NAME_CAP];    ///< 昵称，可为空 / optional nickname
} wr_session_t;

/// 结构合法且 vid/skey 非空。/ Valid layout with non-empty vid and skey.
bool wr_session_valid(const wr_session_t *session);

/// 清空并置为无效。/ Clear the record.
void wr_session_clear(wr_session_t *session);

/// 吸收一个同名 cookie；未知名字返回 false（不是错误，由调用方忽略）。
/// Absorb one supported cookie; unknown names return false and may be ignored by callers.
bool wr_session_set_cookie(wr_session_t *session, const char *name, size_t name_len,
                           const char *value, size_t value_len);

/// 生成 `wr_vid=..; wr_skey=..[; wr_rt=..]`；无效会话或空间不足返回 false。
/// Build the Cookie header; false for an invalid session or insufficient space.
bool wr_session_cookie_header(const wr_session_t *session, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
