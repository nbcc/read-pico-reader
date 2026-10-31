/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书书籍的落盘命名与存在性检查，供书架标记与后续下载共用。
 *
 * English: On-disk naming and existence checks for WeRead books, shared by the shelf marker
 * and the upcoming downloader.
 *
 * 冻结：文件名固定为 `<书名>-<bookId>.epub`；非法字符换下划线，截断不切断 UTF-8。
 * Frozen: Names are `<title>-<bookId>.epub`; illegal bytes become underscores and truncation
 * never splits a UTF-8 sequence.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 书库子目录名。/ Library subdirectory owned by this source.
#define WR_BOOK_SUBDIR "WeRead"

/// 生成文件名（不含目录）。/ Build the file name without the directory.
void wr_book_file_name(const char *title, const char *book_id, char *out, size_t cap);

/// 拼接完整路径；空间不足返回 false。/ Build the full path; false when it does not fit.
bool wr_book_file_path(const char *root_dir, const char *title, const char *book_id, char *out, size_t cap);

/// 已在书库中则为真（普通文件）。/ True when the book already exists as a regular file.
bool wr_book_file_exists(const char *root_dir, const char *title, const char *book_id);

/// 递归创建目录。/ Create a directory and its parents.
bool wr_ensure_dir(const char *path);

/// 原子提交：先备份旧文件再改名，FatFs 不能直接覆盖时回滚。
/// Atomic commit: back up the old file before renaming and roll back when FatFs refuses to replace.
bool wr_book_commit(const char *part, const char *path);

/// 递归删除目录（不跟随链接）。/ Recursively remove a directory tree without following links.
bool wr_remove_tree(const char *path);

#ifdef __cplusplus
}
#endif
