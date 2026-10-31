/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：`chapterInfos` 响应的定长章节表与解析。
 *
 * English: Fixed-size chapter table and parser for the `chapterInfos` response.
 *
 * 冻结：只读 chapterUid/chapterIdx/title/wordCount/level/paid；表满即截断。
 * Frozen: Only chapterUid/chapterIdx/title/wordCount/level/paid; a full table truncates.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WR_TOC_MAX 2048
#define WR_TOC_UID_CAP 32
#define WR_TOC_TITLE_CAP 128

typedef struct {
    char uid[WR_TOC_UID_CAP];
    char title[WR_TOC_TITLE_CAP];
    uint32_t words;
    uint16_t idx;
    uint8_t level;
    uint8_t paid;
} wr_toc_chapter_t;

/// 解析目录响应；`updated` / `chapterInfos` / `chapters` 任一数组均可。
/// Parse the catalog; any of `updated`, `chapterInfos` or `chapters` arrays is accepted.
bool wr_toc_parse(const char *text, size_t len, wr_toc_chapter_t *items, size_t capacity,
                  size_t *count, int *error_code);

/// 可读章节：有字数且不是「封面」。/ Readable chapters: has words and is not the cover.
bool wr_toc_readable(const wr_toc_chapter_t *chapter);

#ifdef __cplusplus
}
#endif
