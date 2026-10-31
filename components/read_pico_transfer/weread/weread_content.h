/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：章节请求参数、分片校验/反混淆/Base64、psvts 提取与 TXT 转 XHTML。
 *
 * English: Chapter request parameters, shard validation/unswap/Base64, psvts extraction and
 * TXT-to-XHTML conversion.
 *
 * 冻结：全部为纯函数；不联网、不分配堆内存；输出缓冲由调用方给出。
 * Frozen: Pure functions only; no networking, no heap allocation, caller-provided buffers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WR_CONTENT_JSON_CAP 512

/// 构造 `POST /web/book/chapter/e_*` 的 JSON 体（含 s 签名）。/ Build the signed chapter request body.
bool wr_make_content_json(const char *book_id, const char *chapter_uid, const char *psvts,
                          uint32_t ct, uint32_t random_r, bool style, char *out, size_t cap);

/// 从阅读页 HTML 中取一个 JSON 字符串字段（如 psvts）。/ Extract a JSON string field from the reader HTML.
bool wr_extract_json_field(const char *text, size_t len, const char *field, char *out, size_t cap);

/// 校验分片：前 32 字节为大写 MD5，返回其后的载荷。/ Validate a shard and return the payload after its MD5.
bool wr_shard_body(const char *shard, size_t len, const char **body, size_t *body_len);

/// 解码一段已拼接的载荷：去首字符、反交换、Base64-url。解码写回同一缓冲（输出不后移）。
/// Decode a joined payload in place: drop the first byte, reverse swaps, Base64-url decode.
bool wr_decode_payload(char *data, size_t len, size_t *out_len);

/// 按魔数识别 JPEG/PNG；返回媒体类型与扩展名。/ Sniff JPEG/PNG by magic and return media type and extension.
bool wr_sniff_image(const void *data, size_t len, const char **media_type, const char **extension);

/// 纯文本转 XHTML 段落（转义 &、<、>）。/ Plain text to escaped XHTML paragraphs.
bool wr_txt_to_xhtml(const char *text, size_t len, char *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
