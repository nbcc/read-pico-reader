/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书书架的定长记录与解析；记录表由调用方提供，不在解析中分配。
 *
 * English: Fixed-size WeRead shelf records and parsing; the caller owns the record table.
 *
 * 冻结：只取 books[] 里的 bookId/title/author/readUpdateTime；忽略 archive、mp 与其他字段。
 * Frozen: Only bookId/title/author/readUpdateTime inside books[]; ignore archive, mp and the rest.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WR_SHELF_ID_CAP 48
#define WR_SHELF_TITLE_CAP 128
#define WR_SHELF_AUTHOR_CAP 64
#define WR_SHELF_COVER_CAP 192

typedef struct {
    char book_id[WR_SHELF_ID_CAP];
    char title[WR_SHELF_TITLE_CAP];
    char author[WR_SHELF_AUTHOR_CAP];
    char cover[WR_SHELF_COVER_CAP];
    uint32_t update_time;
} wr_shelf_record_t;

typedef struct {
    wr_shelf_record_t *items; ///< 调用方提供的定长表 / caller-provided fixed table
    size_t capacity;
    size_t count;
    bool truncated;           ///< 表满后仍有条目 / more records than capacity
    int error_code;           ///< 响应里的 errcode/errCode / errcode from the response
} wr_shelf_t;

/// 解析 `/web/shelf/sync` 响应；失败表示 JSON 非法或没有可用的 books 数组。
/// Parse a `/web/shelf/sync` response; false means invalid JSON or no usable books array.
bool wr_shelf_parse(const char *text, size_t len, wr_shelf_t *shelf);

/// 按最近阅读时间降序稳定排序；时间相同保持服务端顺序。
/// Stable descending sort by readUpdateTime; ties keep the server order.
void wr_shelf_sort(wr_shelf_t *shelf);

#ifdef __cplusplus
}
#endif
