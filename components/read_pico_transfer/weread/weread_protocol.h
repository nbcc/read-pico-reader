/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书 Web 协议的纯函数层——MD5、ID 混淆、请求签名、URL 编码与 UTF-8 拷贝。
 *
 * English: Pure-function layer of the WeRead web protocol: MD5, id obfuscation, request signing,
 * URL encoding and UTF-8 safe copying.
 *
 * 冻结：不联网、不读文件、不分配堆内存；所有输出由调用方给出定长缓冲。
 * Frozen: No networking, no file access, no heap allocation; callers provide fixed output buffers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 输出缓冲最小长度（含结尾零）。/ Minimum output capacity including the terminating NUL.
#define WR_ENCODE_ID_CAP 64
#define WR_SIGN_CAP 16
#define WR_MD5_HEX_CAP 33

/// 计算小写十六进制 MD5；输出必须为 33 字节。/ Lowercase hex MD5; out must hold 33 bytes.
bool wr_md5_hex(const void *data, size_t len, char out[WR_MD5_HEX_CAP]);

/// `_e(value)`：微信前端用于隐藏 bookId / chapterUid 的混淆串。/ `_e()` id obfuscation.
bool wr_encode_id(const char *value, char *out, size_t cap);

/// `_s(query)`：按键名排序、URL 编码后用 & 连接，跳过 s 自身的签名。/ Request signature.
bool wr_sign_query(const char *query, char *out, size_t cap);

/// JS `encodeURIComponent` 的等价实现：仅保留 A-Za-z0-9-_.~。/ JS encodeURIComponent equivalent.
bool wr_url_encode(const char *value, char *out, size_t cap);

/// 复制 UTF-8 且不切断多字节序列；空间不足时截断。/ Copy UTF-8 without splitting a sequence.
void wr_utf8_copy(char *dst, size_t cap, const char *src);

#ifdef __cplusplus
}
#endif
