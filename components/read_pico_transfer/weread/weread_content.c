/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：章节内容协议的纯实现：签名参数、分片校验、反交换、Base64 与纯文本转段落。
 *
 * English: Pure chapter-content protocol: signed params, shard validation, unswap, Base64 and
 * plain-text paragraphs.
 *
 * 冻结：解码在原地完成且输出不越过读指针；任何校验失败都返回 false，不猜测修复。
 * Frozen: Decoding stays in place without writing past the read cursor; validation failures
 * return false and are never guessed around.
 */
#include "weread_content.h"

#include <stdio.h>
#include <string.h>

#include "weread_protocol.h"

bool wr_make_content_json(const char *book_id, const char *chapter_uid, const char *psvts,
                          uint32_t ct, uint32_t random_r, bool style, char *out, size_t cap) {
    if (!book_id || !chapter_uid || !psvts || !out || !cap) return false;
    char b[WR_ENCODE_ID_CAP];
    char c[WR_ENCODE_ID_CAP];
    char pc[WR_ENCODE_ID_CAP];
    char ct_text[16];
    char ps_encoded[256];
    if (!wr_encode_id(book_id, b, sizeof(b))) return false;
    if (!wr_encode_id(chapter_uid, c, sizeof(c))) return false;
    snprintf(ct_text, sizeof(ct_text), "%u", (unsigned)ct);
    if (!wr_encode_id(ct_text, pc, sizeof(pc))) return false;
    if (!wr_url_encode(psvts, ps_encoded, sizeof(ps_encoded))) return false;

    const uint64_t r_value = (uint64_t)random_r * random_r;
    char query[512];
    const int query_len = snprintf(query, sizeof(query),
                                   "b=%s&c=%s&ct=%u&pc=%s&prevChapter=false&ps=%s&r=%llu&sc=1&st=%d",
                                   b, c, (unsigned)ct, pc, ps_encoded, (unsigned long long)r_value, style ? 1 : 0);
    if (query_len <= 0 || (size_t)query_len >= sizeof(query)) return false;
    char signature[WR_SIGN_CAP];
    if (!wr_sign_query(query, signature, sizeof(signature))) return false;

    const int json_len = snprintf(out, cap,
                                  "{\"b\":\"%s\",\"c\":\"%s\",\"r\":\"%llu\",\"ct\":\"%u\",\"ps\":\"%s\","
                                  "\"pc\":\"%s\",\"sc\":1,\"prevChapter\":false,\"st\":%d,\"s\":\"%s\"}",
                                  b, c, (unsigned long long)r_value, (unsigned)ct, ps_encoded, pc, style ? 1 : 0,
                                  signature);
    return json_len > 0 && (size_t)json_len < cap;
}

static const char *find_sub(const char *haystack, size_t haystack_len, const char *needle, size_t needle_len) {
    if (!needle_len || haystack_len < needle_len) return NULL;
    for (size_t i = 0; i + needle_len <= haystack_len; ++i) {
        if (haystack[i] == needle[0] && !memcmp(haystack + i, needle, needle_len)) return haystack + i;
    }
    return NULL;
}

bool wr_extract_json_field(const char *text, size_t len, const char *field, char *out, size_t cap) {
    if (!text || !field || !out || cap < 2) return false;
    out[0] = 0;
    char needle[64];
    const int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", field);
    if (needle_len <= 0 || (size_t)needle_len >= sizeof(needle)) return false;

    size_t at = 0;
    while (at + (size_t)needle_len <= len) {
        const char *found = find_sub(text + at, len - at, needle, (size_t)needle_len);
        if (!found) return false;
        at = (size_t)(found - text) + (size_t)needle_len;
        while (at < len && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
        if (at >= len || text[at] != ':') continue;
        ++at;
        while (at < len && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
        if (at >= len || text[at] != '"') continue;
        ++at;
        size_t used = 0;
        while (at < len && text[at] != '"') {
            char c = text[at];
            if (c == '\\' && at + 1 < len) {
                ++at;
                c = text[at];
            }
            if (used + 1 < cap) out[used++] = c;
            ++at;
        }
        out[used] = 0;
        return used > 0;
    }
    return false;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool wr_shard_body(const char *shard, size_t len, const char **body, size_t *body_len) {
    if (!shard || !body || !body_len || len <= 32) return false;
    char digest[WR_MD5_HEX_CAP];
    if (!wr_md5_hex(shard + 32, len - 32, digest)) return false;
    for (size_t i = 0; i < 32; ++i) {
        const int expected = hex_value(shard[i]);
        const int actual = hex_value(digest[i]);
        if (expected < 0 || expected != actual) return false;
    }
    *body = shard + 32;
    *body_len = len - 32;
    return true;
}

// 二进制串按 4 进制解析，与前端混淆位算法一致。/ Parse the binary string as base 4.
static unsigned binary_as_base4(unsigned char byte) {
    char bits[9];
    int count = 0;
    if (!byte) bits[count++] = '0';
    while (byte) {
        bits[count++] = (char)('0' + (byte & 1u));
        byte >>= 1;
    }
    unsigned value = 0;
    for (int i = count - 1; i >= 0; --i) value = value * 4u + (unsigned)(bits[i] - '0');
    return value;
}

static size_t decimal_mod(const char *text, size_t len, size_t mod) {
    size_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') break;
        value = (value * 10 + (size_t)(c - '0')) % mod;
    }
    return value;
}

static bool swap_positions(const char *encoded, size_t len, size_t *positions, size_t *count) {
    *count = 0;
    if (len < 4) return true;
    if (len < 11) {
        positions[0] = 0;
        positions[1] = 2;
        *count = 2;
        return true;
    }
    size_t tail = (len + 9) / 10;
    if (tail > 4) tail = 4;
    char digits[32];
    size_t used = 0;
    for (size_t i = len; i > len - tail; --i) {
        char number[12];
        const int written = snprintf(number, sizeof(number), "%u", binary_as_base4((unsigned char)encoded[i - 1]));
        if (written <= 0 || used + (size_t)written >= sizeof(digits)) return false;
        memcpy(digits + used, number, (size_t)written);
        used += (size_t)written;
    }
    const size_t mod = len - tail - 2;
    if (!mod) return false;
    size_t step = 1;
    for (size_t value = mod; value >= 10; value /= 10) ++step;

    size_t cursor = 0;
    while (*count < 10 && cursor + step < used) {
        positions[(*count)++] = decimal_mod(digits + cursor, step, mod);
        if (cursor + 1 < used) positions[(*count)++] = decimal_mod(digits + cursor + 1, step, mod);
        cursor += step;
    }
    return true;
}

static void reverse_swaps(char *encoded, size_t len, const size_t *positions, size_t count) {
    for (size_t i = count; i >= 2; i -= 2) {
        for (int delta = 1; delta >= 0; --delta) {
            const size_t left = positions[i - 1] + (size_t)delta;
            const size_t right = positions[i - 2] + (size_t)delta;
            if (left >= len || right >= len) continue;
            const char swap = encoded[left];
            encoded[left] = encoded[right];
            encoded[right] = swap;
        }
    }
}

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

bool wr_decode_payload(char *data, size_t len, size_t *out_len) {
    if (!data || !out_len || len < 2) return false;
    char *encoded = data + 1;
    const size_t encoded_len = len - 1;
    size_t positions[16];
    size_t count = 0;
    if (!swap_positions(encoded, encoded_len, positions, &count)) return false;
    reverse_swaps(encoded, encoded_len, positions, count);

    size_t used = 0;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < encoded_len; ++i) {
        const char c = encoded[i];
        if (c == '=') break;
        const int value = base64_value(c);
        if (value < 0) continue;
        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            data[used++] = (char)((accumulator >> bits) & 0xffu);
        }
    }
    *out_len = used;
    return true;
}

static bool append_text(char *out, size_t cap, size_t *used, const char *text, size_t len) {
    if (*used + len >= cap) return false;
    memcpy(out + *used, text, len);
    *used += len;
    return true;
}

bool wr_sniff_image(const void *data, size_t len, const char **media_type, const char **extension) {
    if (!data || !media_type || !extension) return false;
    const unsigned char *bytes = data;
    if (len >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) {
        *media_type = "image/jpeg";
        *extension = "jpg";
        return true;
    }
    if (len >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) {
        *media_type = "image/png";
        *extension = "png";
        return true;
    }
    return false;
}

bool wr_txt_to_xhtml(const char *text, size_t len, char *out, size_t cap, size_t *out_len) {
    if (!text || !out || !cap || !out_len) return false;
    size_t used = 0;
    size_t line_start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i != len && text[i] != '\n' && text[i] != '\r') continue;
        size_t line_end = i;
        while (line_end > line_start && (text[line_end - 1] == ' ' || text[line_end - 1] == '\t')) --line_end;
        if (line_end > line_start) {
            if (!append_text(out, cap, &used, "<p>", 3)) return false;
            for (size_t k = line_start; k < line_end; ++k) {
                const char c = text[k];
                const char *escape = NULL;
                size_t escape_len = 0;
                if (c == '&') { escape = "&amp;"; escape_len = 5; }
                else if (c == '<') { escape = "&lt;"; escape_len = 4; }
                else if (c == '>') { escape = "&gt;"; escape_len = 4; }
                if (escape) {
                    if (!append_text(out, cap, &used, escape, escape_len)) return false;
                } else {
                    if (!append_text(out, cap, &used, &c, 1)) return false;
                }
            }
            if (!append_text(out, cap, &used, "</p>\n", 5)) return false;
        }
        if (i != len && text[i] == '\r' && i + 1 < len && text[i + 1] == '\n') ++i;
        line_start = i + 1;
    }
    out[used] = 0;
    *out_len = used;
    return true;
}
