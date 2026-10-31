/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：书架解析、JSON 转义、稳定排序与落盘命名的主机回归。
 * English: Host regression for shelf parsing, JSON escapes, stable sorting and file naming.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "weread_bookpath.h"
#include "weread_shelf.h"

static wr_shelf_record_t g_items[4];

static const char kShelf[] =
    "{\"books\":["
    "{\"bookId\":\"9002\",\"title\":\"第二本\",\"author\":\"乙\",\"cover\":\"https://cdn.test/9002.jpg\",\"readUpdateTime\":200},"
    "{\"bookId\":\"9001\",\"title\":\"第一本\",\"author\":\"甲\",\"readUpdateTime\":100},"
    "{\"bookId\":\"9003\",\"title\":\"a\\\"b\\\\c\\u4e2d\\ud83d\\ude00\",\"author\":\"丙\",\"readUpdateTime\":300}"
    "],\"archive\":[{\"bookId\":\"9999\",\"title\":\"忽略\"}],\"mp\":[],\"errcode\":0}";

static void shelf_tests(void) {
    wr_shelf_t shelf = {.items = g_items, .capacity = 4};
    assert(wr_shelf_parse(kShelf, strlen(kShelf), &shelf));
    assert(shelf.count == 3 && !shelf.truncated && shelf.error_code == 0);
    assert(!strcmp(shelf.items[0].book_id, "9002"));
    assert(!strcmp(shelf.items[0].cover, "https://cdn.test/9002.jpg"));
    assert(!strcmp(shelf.items[1].book_id, "9001"));
    assert(!strcmp(shelf.items[2].book_id, "9003"));
    assert(!strcmp(shelf.items[2].title, "a\"b\\c\xE4\xB8\xAD\xF0\x9F\x98\x80"));
    assert(shelf.items[2].update_time == 300);
    assert(!strcmp(shelf.items[1].author, "甲"));

    wr_shelf_sort(&shelf);
    assert(!strcmp(shelf.items[0].book_id, "9003"));
    assert(!strcmp(shelf.items[1].book_id, "9002"));
    assert(!strcmp(shelf.items[2].book_id, "9001"));

    // 容量不足：保留前两条并置截断位。/ Capacity shortfall keeps the first two and flags truncation.
    wr_shelf_t small = {.items = g_items, .capacity = 2};
    assert(wr_shelf_parse(kShelf, strlen(kShelf), &small));
    assert(small.count == 2 && small.truncated);

    // 只有错误码：解析成功但零本书。/ Error-only body parses with zero books.
    static const char kError[] = "{\"errcode\":-2012}";
    wr_shelf_t error = {.items = g_items, .capacity = 4};
    assert(wr_shelf_parse(kError, strlen(kError), &error));
    assert(error.count == 0 && error.error_code == -2012);

    // 截断的 JSON、空表、缺 books 都要失败。/ Truncated JSON, empty table and missing books fail.
    wr_shelf_t incomplete = {.items = g_items, .capacity = 4};
    assert(!wr_shelf_parse(kShelf, strlen(kShelf) - 8, &incomplete));
    wr_shelf_t empty = {.items = g_items, .capacity = 0};
    assert(!wr_shelf_parse(kShelf, strlen(kShelf), &empty));
    static const char kNoBooks[] = "{\"archive\":[]}";
    wr_shelf_t none = {.items = g_items, .capacity = 4};
    assert(!wr_shelf_parse(kNoBooks, strlen(kNoBooks), &none));

    // 空书架可能只回 errcode；这也是合法结果。/ An empty shelf may only carry errcode; accept it.
    static const char kEmpty[] = "{\"errcode\":0}";
    wr_shelf_t empty_shelf = {.items = g_items, .capacity = 4};
    assert(wr_shelf_parse(kEmpty, strlen(kEmpty), &empty_shelf));
    assert(empty_shelf.count == 0 && empty_shelf.error_code == 0);
}

static void bookpath_tests(void) {
    char name[160];
    wr_book_file_name("书名/带:非法*字符", "abc", name, sizeof(name));
    assert(!strcmp(name, "书名_带_非法_字符-abc.epub"));

    wr_book_file_name("  ", "42", name, sizeof(name));
    assert(!strcmp(name, "book-42.epub"));

    wr_book_file_name("t", "", name, sizeof(name));
    assert(!name[0]);

    // 长中文标题按 UTF-8 截断：后缀前的最后一个字节不能是续字节。
    // Long CJK titles truncate on a boundary: the byte before the suffix must not be a continuation.
    char long_title[400];
    size_t filled = 0;
    while (filled + 3 < sizeof(long_title) - 1) {
        memcpy(long_title + filled, "\xE4\xB8\xAD", 3);
        filled += 3;
    }
    long_title[filled] = 0;
    wr_book_file_name(long_title, "7", name, sizeof(name));
    assert(strlen(name) < sizeof(name));
    char *suffix = strstr(name, "-7.epub");
    assert(suffix && suffix > name);
    // 前缀必须是完整的 UTF-8 序列，且恰好在后缀处结束。/ The prefix must be complete UTF-8 ending at the suffix.
    for (const unsigned char *p = (const unsigned char *)name; p < (const unsigned char *)suffix;) {
        unsigned width = 1;
        if (*p >= 0xf0) width = 4;
        else if (*p >= 0xe0) width = 3;
        else if (*p >= 0xc0) width = 2;
        assert(p + width <= (const unsigned char *)suffix);
        for (unsigned k = 1; k < width; ++k) assert((p[k] & 0xc0) == 0x80);
        p += width;
    }

    char root[] = "/tmp/weread-book-XXXXXX";
    assert(mkdtemp(root));
    char path[288];
    assert(wr_book_file_path(root, "书", "1", path, sizeof(path)));
    assert(strstr(path, "/WeRead/书-1.epub"));
    assert(!wr_book_file_exists(root, "书", "1"));
    char dir[288];
    snprintf(dir, sizeof(dir), "%s/%s", root, WR_BOOK_SUBDIR);
    assert(mkdir(dir, 0700) == 0);
    FILE *file = fopen(path, "wb");
    assert(file);
    fputs("x", file);
    assert(fclose(file) == 0);
    assert(wr_book_file_exists(root, "书", "1"));
    assert(!wr_book_file_exists(root, "书", "2"));
    // 目录不算命中。/ A directory is not a hit.
    char other[288];
    snprintf(other, sizeof(other), "%s/%s/dir-3.epub", root, WR_BOOK_SUBDIR);
    assert(mkdir(other, 0700) == 0);
    assert(!wr_book_file_exists(root, "dir", "3"));
}

int main(void) {
    shelf_tests();
    bookpath_tests();
    puts("weread shelf: OK");
    return 0;
}
