/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：章节目录的字段提取；复用 JSON 扫描器，不建解析树。
 *
 * English: Field extraction for the chapter catalog, reusing the JSON scanner with no tree.
 *
 * 冻结：章节 UID 兼容数字与字符串两种形态；错误码只取根层 errcode/errCode。
 * Frozen: Chapter UIDs accept both numbers and strings; error codes come from the root only.
 */
#include "weread_toc.h"

#include <string.h>

#include "weread_json.h"

typedef enum {
    TOC_FIELD_NONE = 0,
    TOC_FIELD_ARRAY,
    TOC_FIELD_UID,
    TOC_FIELD_IDX,
    TOC_FIELD_TITLE,
    TOC_FIELD_WORDS,
    TOC_FIELD_LEVEL,
    TOC_FIELD_PAID,
    TOC_FIELD_ERROR,
} toc_field_t;

typedef struct {
    wr_toc_chapter_t *items;
    size_t capacity;
    size_t count;
    int error_code;
    int depth;
    int array_depth;
    int chapter_depth;
    bool in_array;
    bool pending_array;
    bool in_chapter;
    bool saw_array;
    bool saw_error;
    toc_field_t field;
    wr_toc_chapter_t current;
} toc_parser_t;

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
        if (text[i] < '0' || text[i] > '9') break;
        value = value * 10 + (uint64_t)(text[i] - '0');
        if (value > UINT32_MAX) return UINT32_MAX;
    }
    return (uint32_t)value;
}

static bool array_key(const char *key, size_t len) {
    return (len == 7 && !memcmp(key, "updated", 7)) || (len == 12 && !memcmp(key, "chapterInfos", 12)) ||
           (len == 8 && !memcmp(key, "chapters", 8));
}

static void on_key(void *raw, const char *key, size_t len) {
    toc_parser_t *parser = raw;
    parser->field = TOC_FIELD_NONE;
    if (array_key(key, len)) {
        parser->pending_array = true;
        parser->field = TOC_FIELD_ARRAY;
    } else if (len == 10 && !memcmp(key, "chapterUid", 10)) {
        parser->field = TOC_FIELD_UID;
    } else if (len == 10 && !memcmp(key, "chapterIdx", 10)) {
        parser->field = TOC_FIELD_IDX;
    } else if (len == 5 && !memcmp(key, "title", 5)) {
        parser->field = TOC_FIELD_TITLE;
    } else if (len == 9 && !memcmp(key, "wordCount", 9)) {
        parser->field = TOC_FIELD_WORDS;
    } else if (len == 5 && !memcmp(key, "level", 5)) {
        parser->field = TOC_FIELD_LEVEL;
    } else if (len == 4 && !memcmp(key, "paid", 4)) {
        parser->field = TOC_FIELD_PAID;
    } else if ((len == 7 && !memcmp(key, "errcode", 7)) || (len == 7 && !memcmp(key, "errCode", 7))) {
        parser->field = TOC_FIELD_ERROR;
    }
}

static void take_string(void *raw, const char *value, size_t len) {
    toc_parser_t *parser = raw;
    if (parser->in_chapter && parser->depth == parser->chapter_depth) {
        switch (parser->field) {
            case TOC_FIELD_UID: copy_span(parser->current.uid, sizeof(parser->current.uid), value, len); break;
            case TOC_FIELD_TITLE: copy_span(parser->current.title, sizeof(parser->current.title), value, len); break;
            default: break;
        }
    } else if (parser->field == TOC_FIELD_ERROR && parser->depth == 1) {
        parser->error_code = (int)parse_uint32(value, len);
        parser->saw_error = true;
    }
    parser->field = TOC_FIELD_NONE;
    parser->pending_array = false;
}

static void take_number(void *raw, const char *value, size_t len) {
    toc_parser_t *parser = raw;
    if (parser->in_chapter && parser->depth == parser->chapter_depth) {
        switch (parser->field) {
            case TOC_FIELD_UID: {
                char number[WR_TOC_UID_CAP];
                copy_span(number, sizeof(number), value, len);
                memcpy(parser->current.uid, number, sizeof(parser->current.uid));
                break;
            }
            case TOC_FIELD_IDX: parser->current.idx = (uint16_t)parse_uint32(value, len); break;
            case TOC_FIELD_WORDS: parser->current.words = parse_uint32(value, len); break;
            case TOC_FIELD_LEVEL: parser->current.level = (uint8_t)parse_uint32(value, len); break;
            case TOC_FIELD_PAID: parser->current.paid = parse_uint32(value, len) ? 1 : 0; break;
            default: break;
        }
    } else if (parser->field == TOC_FIELD_ERROR && parser->depth == 1) {
        const bool negative = len && value[0] == '-';
        const int code = (int)parse_uint32(value + (negative ? 1 : 0), len - (negative ? 1 : 0));
        parser->error_code = negative ? -code : code;
        parser->saw_error = true;
    }
    parser->field = TOC_FIELD_NONE;
    parser->pending_array = false;
}

static void on_boolean(void *raw, bool value) {
    toc_parser_t *parser = raw;
    if (parser->in_chapter && parser->depth == parser->chapter_depth && parser->field == TOC_FIELD_PAID)
        parser->current.paid = value ? 1 : 0;
    parser->field = TOC_FIELD_NONE;
    parser->pending_array = false;
}

static void on_object_start(void *raw) {
    toc_parser_t *parser = raw;
    ++parser->depth;
    parser->pending_array = false;
    if (parser->in_array && parser->depth == parser->array_depth + 1) {
        memset(&parser->current, 0, sizeof(parser->current));
        parser->chapter_depth = parser->depth;
        parser->in_chapter = true;
    }
    parser->field = TOC_FIELD_NONE;
}

static void on_object_end(void *raw) {
    toc_parser_t *parser = raw;
    if (parser->in_chapter && parser->depth == parser->chapter_depth) {
        if (parser->current.uid[0]) {
            if (parser->count < parser->capacity) parser->items[parser->count++] = parser->current;
        }
        parser->in_chapter = false;
        parser->chapter_depth = -1;
    }
    if (parser->depth > 0) --parser->depth;
    parser->field = TOC_FIELD_NONE;
}

static void on_array_start(void *raw) {
    toc_parser_t *parser = raw;
    ++parser->depth;
    if (parser->pending_array && !parser->in_array) {
        parser->in_array = true;
        parser->array_depth = parser->depth;
        parser->saw_array = true;
        parser->pending_array = false;
    }
    parser->field = TOC_FIELD_NONE;
}

static void on_array_end(void *raw) {
    toc_parser_t *parser = raw;
    if (parser->in_array && parser->depth == parser->array_depth) {
        parser->in_array = false;
        parser->array_depth = -1;
    }
    if (parser->depth > 0) --parser->depth;
    parser->field = TOC_FIELD_NONE;
}

bool wr_toc_parse(const char *text, size_t len, wr_toc_chapter_t *items, size_t capacity,
                  size_t *count, int *error_code) {
    if (!text || !len || !items || !capacity || !count || !error_code) return false;
    toc_parser_t parser = {
        .items = items,
        .capacity = capacity,
        .array_depth = -1,
        .chapter_depth = -1,
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
        .boolean = on_boolean,
    };
    if (!wr_json_parse(text, len, &sax)) return false;
    *count = parser.count;
    *error_code = parser.error_code;
    return parser.saw_array || parser.saw_error;
}

bool wr_toc_readable(const wr_toc_chapter_t *chapter) {
    if (!chapter || !chapter->uid[0]) return false;
    if (!chapter->words) return false;
    return strcmp(chapter->title, "封面") != 0;
}
