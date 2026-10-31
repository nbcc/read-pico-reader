/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：有界的流式 JSON 扫描器，只回调结构事件，不建解析树、不分配堆内存。
 *
 * English: Bounded streaming JSON scanner emitting structural events only; no tree, no heap.
 *
 * 冻结：深度上限 32；字符串值优先零拷贝，含转义时解码到调用方缓冲并截断。
 * Frozen: Depth is capped at 32; string values are zero-copy unless escaped, and escaped
 * values decode into the caller buffer with safe truncation.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WR_JSON_MAX_DEPTH 32

typedef struct {
    void *ctx;
    void (*object_start)(void *ctx);
    void (*object_end)(void *ctx);
    void (*array_start)(void *ctx);
    void (*array_end)(void *ctx);
    void (*key)(void *ctx, const char *key, size_t len);
    void (*string)(void *ctx, const char *value, size_t len);
    void (*number)(void *ctx, const char *value, size_t len);
    void (*boolean)(void *ctx, bool value);
} wr_json_sax_t;

/// 扫描一段完整 JSON；失败返回 false（语法错误、截断或超深）。
/// Scan one complete JSON document; false on syntax error, truncation or over-deep nesting.
bool wr_json_parse(const char *text, size_t len, const wr_json_sax_t *sax);

#ifdef __cplusplus
}
#endif
