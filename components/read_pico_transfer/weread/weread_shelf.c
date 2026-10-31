/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：书架响应的字段提取与稳定排序，基于有界 JSON 扫描器，不建树。
 *
 * English: Field extraction and stable sorting for the shelf response, built on the bounded
 * JSON scanner with no parse tree.
 *
 * 冻结：字段只在 books 数组的对象层生效；表满即截断并置位，不丢弃已有记录。
 * Frozen: Fields apply only to objects directly inside books[]; a full table truncates and
 * sets the flag without dropping earlier records.
 */
#include "weread_shelf.h"

#include <string.h>

#include "weread_json.h"

typedef enum {
    FIELD_NONE = 0,
    FIELD_BOOKS,
    FIELD_BOOK_ID,
    FIELD_TITLE,
    FIELD_AUTHOR,
    FIELD_COVER,
    FIELD_UPDATE_TIME,
    FIELD_ERROR_CODE,
} shelf_field_t;

typedef struct {
    wr_shelf_t *shelf;
    int depth;
    int books_depth;
    int book_depth;
    bool in_books;
    bool pending_books;
    bool in_book;
    bool saw_books;
    bool saw_error_code;
    shelf_field_t field;
    wr_shelf_record_t current;
} shelf_parser_t;

// 按 UTF-8 边界复制并补零；超长截断。/ Copy on a UTF-8 boundary with NUL; truncate when long.
static void copy_span(char *dst, size_t cap, const char *src, size_t len) {
    if (!cap) return;
    size_t n = len < cap - 1 ? len : cap - 1;
    while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) --n;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static uint32_t parse_uint32(const char *text, size_t len) {
    uint64_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') break;
        value = value * 10 + (uint64_t)(c - '0');
        if (value > UINT32_MAX) return UINT32_MAX;
    }
    return (uint32_t)value;
}

static void on_key(void *raw, const char *key, size_t len) {
    shelf_parser_t *parser = raw;
    parser->field = FIELD_NONE;
    if (len == 5 && !memcmp(key, "books", 5)) {
        parser->field = FIELD_BOOKS;
        parser->pending_books = true;
    } else if (len == 6 && !memcmp(key, "bookId", 6)) {
        parser->field = FIELD_BOOK_ID;
    } else if (len == 5 && !memcmp(key, "title", 5)) {
        parser->field = FIELD_TITLE;
    } else if (len == 6 && !memcmp(key, "author", 6)) {
        parser->field = FIELD_AUTHOR;
    } else if (len == 5 && !memcmp(key, "cover", 5)) {
        parser->field = FIELD_COVER;
    } else if (len == 14 && !memcmp(key, "readUpdateTime", 14)) {
        parser->field = FIELD_UPDATE_TIME;
    } else if ((len == 7 && !memcmp(key, "errcode", 7)) || (len == 7 && !memcmp(key, "errCode", 7))) {
        parser->field = FIELD_ERROR_CODE;
    }
}

static void take_string(void *raw, const char *value, size_t len) {
    shelf_parser_t *parser = raw;
    if (parser->in_book && parser->depth == parser->book_depth) {
        switch (parser->field) {
            case FIELD_BOOK_ID: copy_span(parser->current.book_id, sizeof(parser->current.book_id), value, len); break;
            case FIELD_TITLE: copy_span(parser->current.title, sizeof(parser->current.title), value, len); break;
            case FIELD_AUTHOR: copy_span(parser->current.author, sizeof(parser->current.author), value, len); break;
            case FIELD_COVER: copy_span(parser->current.cover, sizeof(parser->current.cover), value, len); break;
            default: break;
        }
    } else if (parser->field == FIELD_ERROR_CODE && parser->depth == 1) {
        parser->shelf->error_code = (int)parse_uint32(value, len);
        parser->saw_error_code = true;
    }
    parser->field = FIELD_NONE;
    parser->pending_books = false;
}

static void take_number(void *raw, const char *value, size_t len) {
    shelf_parser_t *parser = raw;
    if (parser->in_book && parser->depth == parser->book_depth) {
        if (parser->field == FIELD_UPDATE_TIME) parser->current.update_time = parse_uint32(value, len);
    } else if (parser->field == FIELD_ERROR_CODE && parser->depth == 1) {
        const bool negative = len && value[0] == '-';
        const int code = (int)parse_uint32(value + (negative ? 1 : 0), len - (negative ? 1 : 0));
        parser->shelf->error_code = negative ? -code : code;
        parser->saw_error_code = true;
    }
    parser->field = FIELD_NONE;
    parser->pending_books = false;
}

static void on_object_start(void *raw) {
    shelf_parser_t *parser = raw;
    ++parser->depth;
    parser->pending_books = false;
    if (parser->in_books && parser->depth == parser->books_depth + 1) {
        memset(&parser->current, 0, sizeof(parser->current));
        parser->book_depth = parser->depth;
        parser->in_book = true;
    }
    parser->field = FIELD_NONE;
}

static void on_object_end(void *raw) {
    shelf_parser_t *parser = raw;
    if (parser->in_book && parser->depth == parser->book_depth) {
        if (parser->current.book_id[0]) {
            if (parser->shelf->count < parser->shelf->capacity) {
                parser->shelf->items[parser->shelf->count++] = parser->current;
            } else {
                parser->shelf->truncated = true;
            }
        }
        parser->in_book = false;
        parser->book_depth = -1;
    }
    if (parser->depth > 0) --parser->depth;
    parser->field = FIELD_NONE;
}

static void on_array_start(void *raw) {
    shelf_parser_t *parser = raw;
    ++parser->depth;
    if (parser->pending_books) {
        parser->in_books = true;
        parser->books_depth = parser->depth;
        parser->saw_books = true;
        parser->pending_books = false;
    }
    parser->field = FIELD_NONE;
}

static void on_array_end(void *raw) {
    shelf_parser_t *parser = raw;
    if (parser->in_books && parser->depth == parser->books_depth) {
        parser->in_books = false;
        parser->books_depth = -1;
    }
    if (parser->depth > 0) --parser->depth;
    parser->field = FIELD_NONE;
}

bool wr_shelf_parse(const char *text, size_t len, wr_shelf_t *shelf) {
    if (!text || !len || !shelf || (!shelf->items && shelf->capacity) || !shelf->capacity) return false;
    shelf->count = 0;
    shelf->truncated = false;
    shelf->error_code = 0;
    shelf_parser_t parser = {
        .shelf = shelf,
        .depth = 0,
        .books_depth = -1,
        .book_depth = -1,
    };
    const wr_json_sax_t sax = {
        .ctx = &parser,
        .object_start = on_object_start,
        .object_end = on_object_end,
        .array_start = on_array_start,
        .array_end = on_array_end,
        .key = on_key,
        .string = take_string,
        .number = take_number,
    };
    if (!wr_json_parse(text, len, &sax)) return false;
    return parser.saw_books || parser.saw_error_code;
}

void wr_shelf_sort(wr_shelf_t *shelf) {
    if (!shelf || shelf->count < 2) return;
    for (size_t i = 1; i < shelf->count; ++i) {
        const wr_shelf_record_t current = shelf->items[i];
        size_t j = i;
        while (j > 0 && shelf->items[j - 1].update_time < current.update_time) {
            shelf->items[j] = shelf->items[j - 1];
            --j;
        }
        shelf->items[j] = current;
    }
}
