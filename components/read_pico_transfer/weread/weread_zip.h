/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：最小 ZIP 写入器（store-only）：本地头 + 数据 + 中央目录，条目数上限 65535。
 *
 * English: Minimal store-only ZIP writer: local headers, data, central directory, at most
 * 65535 entries.
 *
 * 冻结：不压缩、不写 extra 字段、不支持 ZIP64；中央目录先落临时文件，结束时拼接。
 * Frozen: No compression, no extra fields, no ZIP64; the central directory is staged in a
 * temporary file and appended at finish.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool wr_zip_begin(const char *path, const char *central_path);
bool wr_zip_add_buffer(const char *name, const void *data, size_t len);
/// 多个连续片段作为一个条目写入，不拼接、不额外分配。
/// Write multiple contiguous chunks as one entry without concatenating or allocating.
typedef struct {
    const void *data;
    size_t len;
} wr_zip_part_t;
bool wr_zip_add_parts(const char *name, const wr_zip_part_t *parts, size_t count);
bool wr_zip_add_file(const char *name, const char *path);
bool wr_zip_finish(void);
void wr_zip_abort(void);

#ifdef __cplusplus
}
#endif
