/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：文件名清洗与路径拼接；不创建目录、不写文件。
 *
 * English: File-name sanitizing and path joining; creates and writes nothing.
 *
 * 冻结：只接受 root_dir 下的 WeRead 子目录；拒绝空 id 与路径分隔符注入。
 * Frozen: Only the WeRead subdirectory under root_dir; empty ids and separator injection fail.
 */
#include "weread_bookpath.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "weread_protocol.h"
#include "weread_shelf.h"

#define WR_BOOK_NAME_CAP 160

static bool illegal_byte(unsigned char c) {
    if (c < 0x20 || c == 0x7f) return true;
    return strchr("/\\:*?\"<>|", c) != NULL;
}

static void sanitize_id(const char *book_id, char *out, size_t cap) {
    if (!cap) return;
    size_t used = 0;
    for (const char *p = book_id; p && *p && used + 1 < cap; ++p) {
        const unsigned char c = (unsigned char)*p;
        const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           c == '_' || c == '-';
        out[used++] = plain ? (char)c : '_';
    }
    out[used] = 0;
}

void wr_book_file_name(const char *title, const char *book_id, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    char id[WR_SHELF_ID_CAP];
    sanitize_id(book_id ? book_id : "", id, sizeof(id));
    if (!id[0]) return;

    char sanitized[WR_BOOK_NAME_CAP];
    size_t used = 0;
    for (const char *p = title; p && *p && used + 1 < sizeof(sanitized); ++p) {
        const unsigned char c = (unsigned char)*p;
        sanitized[used++] = illegal_byte(c) ? '_' : (char)c;
    }
    sanitized[used] = 0;

    // 去掉首尾空白，再按 UTF-8 边界截断到剩余空间。/ Trim, then truncate on a UTF-8 boundary.
    char *start = sanitized;
    while (*start == ' ') ++start;
    size_t len = strlen(start);
    while (len && start[len - 1] == ' ') start[--len] = 0;
    if (!len) strcpy(start, "book");

    const size_t suffix = strlen(id) + 6; // '-' + id + ".epub"
    if (suffix + 1 >= cap) return;
    const size_t title_cap = cap - suffix;
    char base[WR_BOOK_NAME_CAP];
    wr_utf8_copy(base, title_cap < sizeof(base) ? title_cap : sizeof(base), start);
    if (!base[0]) snprintf(base, sizeof(base), "book");
    snprintf(out, cap, "%s-%s.epub", base, id);
}

bool wr_book_file_path(const char *root_dir, const char *title, const char *book_id, char *out, size_t cap) {
    if (!root_dir || !root_dir[0] || !out || !cap) return false;
    char name[WR_BOOK_NAME_CAP];
    wr_book_file_name(title, book_id, name, sizeof(name));
    if (!name[0]) return false;
    const int written = snprintf(out, cap, "%s/%s/%s", root_dir, WR_BOOK_SUBDIR, name);
    return written > 0 && (size_t)written < cap;
}

bool wr_book_file_exists(const char *root_dir, const char *title, const char *book_id) {
    char path[288];
    if (!wr_book_file_path(root_dir, title, book_id, path, sizeof(path))) return false;
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

/* ---- 文件系统辅助 / Filesystem helpers ---- */

bool wr_ensure_dir(const char *path) {
    if (!path || !path[0]) return false;
    char partial[288];
    const size_t len = strlen(path);
    if (len >= sizeof(partial)) return false;
    memcpy(partial, path, len + 1);
    for (size_t i = 1; i <= len; ++i) {
        if (partial[i] != '/' && partial[i] != 0) continue;
        const char saved = partial[i];
        partial[i] = 0;
        if (partial[0]) {
            struct stat info;
            if (stat(partial, &info) != 0) {
                if (mkdir(partial, 0777) != 0) return false;
            } else if (!S_ISDIR(info.st_mode)) {
                return false;
            }
        }
        partial[i] = saved;
    }
    return true;
}

bool wr_book_commit(const char *part, const char *path) {
    if (!part || !path) return false;
    if (rename(part, path) == 0) return true;
    // FatFs 不能直接覆盖；保留旧书直到新文件就位。/ FatFs cannot replace; keep the old book until the new one lands.
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return false;
    char backup[320];
    const int written = snprintf(backup, sizeof(backup), "%s.weread-backup", path);
    if (written <= 0 || (size_t)written >= sizeof(backup)) return false;
    if (rename(path, backup) != 0) return false;
    if (rename(part, path) != 0) {
        (void)rename(backup, path);
        return false;
    }
    (void)remove(backup);
    return true;
}

bool wr_remove_tree(const char *path) {
    if (!path || !path[0]) return false;
    struct stat info;
    if (stat(path, &info) != 0) return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) return remove(path) == 0;
    DIR *dir = opendir(path);
    if (!dir) return false;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[288];
        const int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(child) || !wr_remove_tree(child)) ok = false;
    }
    if (closedir(dir) != 0) ok = false;
    return ok && rmdir(path) == 0;
}
