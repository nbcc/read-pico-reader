/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：递归下降的 JSON 扫描器；只在结构与标量边界回调，不做对象分配。
 *
 * English: Recursive-descent JSON scanner that only reports structure and scalars.
 *
 * 冻结：不复制未转义字符串；遇到超出深度的文档立即失败；错误后不再回调。
 * Frozen: Unescaped strings stay zero-copy; documents deeper than the cap fail immediately,
 * and no callbacks run after an error.
 */
#include "weread_json.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    const char *cursor;
    const char *end;
    const wr_json_sax_t *sax;
} parser_t;

static void skip_space(parser_t *parser) {
    while (parser->cursor < parser->end) {
        const char c = *parser->cursor;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        ++parser->cursor;
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t encode_utf8(uint32_t cp, char *out) {
    if (cp <= 0x7f) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7ff) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp <= 0xffff) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

// 读取 \uXXXX；代理对在此合并。/ Read \uXXXX and combine surrogate pairs here.
static bool read_unicode_escape(parser_t *parser, uint32_t *out) {
    if ((size_t)(parser->end - parser->cursor) < 4) return false;
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        const int digit = hex_value(parser->cursor[i]);
        if (digit < 0) return false;
        value = (value << 4) | (uint32_t)digit;
    }
    parser->cursor += 4;
    if (value >= 0xd800 && value <= 0xdbff) {
        if ((size_t)(parser->end - parser->cursor) < 6 || parser->cursor[0] != '\\' || parser->cursor[1] != 'u')
            return false;
        uint32_t low = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hex_value(parser->cursor[2 + i]);
            if (digit < 0) return false;
            low = (low << 4) | (uint32_t)digit;
        }
        if (low < 0xdc00 || low > 0xdfff) return false;
        parser->cursor += 6;
        value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
    } else if (value >= 0xdc00 && value <= 0xdfff) {
        return false;
    }
    *out = value;
    return true;
}

static bool decode_escaped(const char *start, size_t len, char *out, size_t cap) {
    parser_t parser = {.cursor = start, .end = start + len, .sax = NULL};
    size_t used = 0;
    while (parser.cursor < parser.end) {
        const char c = *parser.cursor++;
        if (c != '\\') {
            if (used + 1 < cap) out[used++] = c;
            continue;
        }
        if (parser.cursor >= parser.end) break;
        const char escape = *parser.cursor++;
        char decoded[4];
        size_t width = 0;
        switch (escape) {
            case '"': decoded[0] = '"'; width = 1; break;
            case '\\': decoded[0] = '\\'; width = 1; break;
            case '/': decoded[0] = '/'; width = 1; break;
            case 'b': decoded[0] = '\b'; width = 1; break;
            case 'f': decoded[0] = '\f'; width = 1; break;
            case 'n': decoded[0] = '\n'; width = 1; break;
            case 'r': decoded[0] = '\r'; width = 1; break;
            case 't': decoded[0] = '\t'; width = 1; break;
            case 'u': {
                uint32_t cp = 0;
                if (!read_unicode_escape(&parser, &cp)) return false;
                width = encode_utf8(cp, decoded);
                break;
            }
            default:
                return false;
        }
        for (size_t i = 0; i < width; ++i) {
            if (used + 1 < cap) out[used++] = decoded[i];
        }
    }
    if (cap) out[used < cap ? used : cap - 1] = 0;
    return true;
}

// 扫描字符串字面量；未转义时零拷贝返回原指针。/ Scan a string literal; zero-copy when unescaped.
static bool scan_string(parser_t *parser, char *buffer, size_t cap, const char **out, size_t *out_len) {
    if (parser->cursor >= parser->end || *parser->cursor != '"') return false;
    ++parser->cursor;
    const char *start = parser->cursor;
    bool escaped = false;
    while (parser->cursor < parser->end) {
        const unsigned char c = (unsigned char)*parser->cursor;
        if (c == '"') {
            const size_t len = (size_t)(parser->cursor - start);
            ++parser->cursor;
            if (!escaped) {
                *out = start;
                *out_len = len;
                return true;
            }
            if (!decode_escaped(start, len, buffer, cap)) return false;
            *out = buffer;
            *out_len = strlen(buffer);
            return true;
        }
        if (c == '\\') {
            escaped = true;
            ++parser->cursor;
            if (parser->cursor >= parser->end) return false;
            ++parser->cursor;
            continue;
        }
        if (c < 0x20) return false;
        ++parser->cursor;
    }
    return false;
}

static bool parse_value(parser_t *parser, int depth);

static bool parse_object(parser_t *parser, int depth) {
    if (depth > WR_JSON_MAX_DEPTH) return false;
    ++parser->cursor; // '{'
    if (parser->sax && parser->sax->object_start) parser->sax->object_start(parser->sax->ctx);
    skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        ++parser->cursor;
        if (parser->sax && parser->sax->object_end) parser->sax->object_end(parser->sax->ctx);
        return true;
    }
    for (;;) {
        char key_buffer[96];
        const char *key = NULL;
        size_t key_len = 0;
        skip_space(parser);
        if (!scan_string(parser, key_buffer, sizeof(key_buffer), &key, &key_len)) return false;
        if (parser->sax && parser->sax->key) parser->sax->key(parser->sax->ctx, key, key_len);
        skip_space(parser);
        if (parser->cursor >= parser->end || *parser->cursor != ':') return false;
        ++parser->cursor;
        if (!parse_value(parser, depth + 1)) return false;
        skip_space(parser);
        if (parser->cursor >= parser->end) return false;
        if (*parser->cursor == ',') {
            ++parser->cursor;
            continue;
        }
        if (*parser->cursor == '}') {
            ++parser->cursor;
            if (parser->sax && parser->sax->object_end) parser->sax->object_end(parser->sax->ctx);
            return true;
        }
        return false;
    }
}

static bool parse_array(parser_t *parser, int depth) {
    if (depth > WR_JSON_MAX_DEPTH) return false;
    ++parser->cursor; // '['
    if (parser->sax && parser->sax->array_start) parser->sax->array_start(parser->sax->ctx);
    skip_space(parser);
    if (parser->cursor < parser->end && *parser->cursor == ']') {
        ++parser->cursor;
        if (parser->sax && parser->sax->array_end) parser->sax->array_end(parser->sax->ctx);
        return true;
    }
    for (;;) {
        if (!parse_value(parser, depth + 1)) return false;
        skip_space(parser);
        if (parser->cursor >= parser->end) return false;
        if (*parser->cursor == ',') {
            ++parser->cursor;
            continue;
        }
        if (*parser->cursor == ']') {
            ++parser->cursor;
            if (parser->sax && parser->sax->array_end) parser->sax->array_end(parser->sax->ctx);
            return true;
        }
        return false;
    }
}

static bool parse_number(parser_t *parser) {
    const char *start = parser->cursor;
    if (parser->cursor < parser->end && *parser->cursor == '-') ++parser->cursor;
    const char *digits = parser->cursor;
    while (parser->cursor < parser->end && *parser->cursor >= '0' && *parser->cursor <= '9') ++parser->cursor;
    if (parser->cursor == digits) return false;
    if (parser->cursor < parser->end && *parser->cursor == '.') {
        ++parser->cursor;
        const char *fraction = parser->cursor;
        while (parser->cursor < parser->end && *parser->cursor >= '0' && *parser->cursor <= '9') ++parser->cursor;
        if (parser->cursor == fraction) return false;
    }
    if (parser->cursor < parser->end && (*parser->cursor == 'e' || *parser->cursor == 'E')) {
        ++parser->cursor;
        if (parser->cursor < parser->end && (*parser->cursor == '+' || *parser->cursor == '-')) ++parser->cursor;
        const char *exponent = parser->cursor;
        while (parser->cursor < parser->end && *parser->cursor >= '0' && *parser->cursor <= '9') ++parser->cursor;
        if (parser->cursor == exponent) return false;
    }
    if (parser->sax && parser->sax->number)
        parser->sax->number(parser->sax->ctx, start, (size_t)(parser->cursor - start));
    return true;
}

static bool parse_value(parser_t *parser, int depth) {
    if (depth > WR_JSON_MAX_DEPTH) return false;
    skip_space(parser);
    if (parser->cursor >= parser->end) return false;
    const char c = *parser->cursor;
    if (c == '{') return parse_object(parser, depth);
    if (c == '[') return parse_array(parser, depth);
    if (c == '"') {
        char buffer[512];
        const char *value = NULL;
        size_t len = 0;
        if (!scan_string(parser, buffer, sizeof(buffer), &value, &len)) return false;
        if (parser->sax && parser->sax->string) parser->sax->string(parser->sax->ctx, value, len);
        return true;
    }
    if (c == 't' && (size_t)(parser->end - parser->cursor) >= 4 && !memcmp(parser->cursor, "true", 4)) {
        parser->cursor += 4;
        if (parser->sax && parser->sax->boolean) parser->sax->boolean(parser->sax->ctx, true);
        return true;
    }
    if (c == 'f' && (size_t)(parser->end - parser->cursor) >= 5 && !memcmp(parser->cursor, "false", 5)) {
        parser->cursor += 5;
        if (parser->sax && parser->sax->boolean) parser->sax->boolean(parser->sax->ctx, false);
        return true;
    }
    if (c == 'n' && (size_t)(parser->end - parser->cursor) >= 4 && !memcmp(parser->cursor, "null", 4)) {
        parser->cursor += 4;
        return true;
    }
    return parse_number(parser);
}

bool wr_json_parse(const char *text, size_t len, const wr_json_sax_t *sax) {
    if (!text || !len) return false;
    parser_t parser = {.cursor = text, .end = text + len, .sax = sax};
    if (!parse_value(&parser, 0)) return false;
    skip_space(&parser);
    return parser.cursor == parser.end;
}
