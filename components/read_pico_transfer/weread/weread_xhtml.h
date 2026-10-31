/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信章节 XHTML 的单遍消毒：抽 body、白名单标签、去属性、脚注内联、丢图片。
 *
 * English: Single-pass sanitizer for WeRead chapter XHTML: body extraction, tag whitelist,
 * attribute stripping, inline footnotes and dropped images.
 *
 * 冻结：只输出阅读器认识的标签；`<img>` 一律不输出（脚注图片转成文字）；失败即整体失败。
 * Frozen: Only tags the reader understands; no `<img>` survives (footnote images become text),
 * and any overflow fails the whole chapter.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 输出为 body 内部片段（不含 html/head/body 包裹）。/ Output is a body fragment without wrappers.
bool wr_sanitize_xhtml(const char *in, size_t len, char *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
