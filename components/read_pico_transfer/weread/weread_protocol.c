/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书 Web 协议的纯函数实现。算法参数来自公开协议研究（crossmux，MIT），
 *      本文件为独立实现，不复制其源码。
 *
 * English: Pure implementations for the WeRead web protocol. Parameters follow the public
 * protocol research (crossmux, MIT); this file is an independent implementation.
 *
 * 冻结：不联网、不读文件、不分配堆内存；不得在此文件记录任何凭据。
 * Frozen: No networking, no file access, no heap allocation; never log credentials here.
 */
#include "weread_protocol.h"

#include <stdio.h>
#include <string.h>

/* ---- MD5 / RFC 1321 ---- */

static const uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942,
    0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6,
    0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
    0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f,
    0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t kMd5S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

typedef struct {
    uint32_t h[4];
    uint64_t bytes;
    uint8_t buf[64];
    size_t fill;
} wr_md5_ctx;

static uint32_t rotl32(uint32_t value, unsigned shift) {
    return (value << shift) | (value >> (32 - shift));
}

static void md5_block(wr_md5_ctx *ctx, const uint8_t *block) {
    uint32_t w[16];
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
    }
    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t f;
        unsigned g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const uint32_t next_d = c;
        const uint32_t next_c = b;
        const uint32_t next_b = b + rotl32(a + f + kMd5K[i] + w[g], kMd5S[i]);
        a = d;
        d = next_d;
        c = next_c;
        b = next_b;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
}

static void md5_init(wr_md5_ctx *ctx) {
    ctx->h[0] = 0x67452301;
    ctx->h[1] = 0xefcdab89;
    ctx->h[2] = 0x98badcfe;
    ctx->h[3] = 0x10325476;
    ctx->bytes = 0;
    ctx->fill = 0;
}

static void md5_update(wr_md5_ctx *ctx, const uint8_t *data, size_t len) {
    ctx->bytes += len;
    while (len) {
        size_t room = sizeof(ctx->buf) - ctx->fill;
        size_t take = len < room ? len : room;
        memcpy(ctx->buf + ctx->fill, data, take);
        ctx->fill += take;
        data += take;
        len -= take;
        if (ctx->fill == sizeof(ctx->buf)) {
            md5_block(ctx, ctx->buf);
            ctx->fill = 0;
        }
    }
}

static void md5_final(wr_md5_ctx *ctx, uint8_t digest[16]) {
    const uint64_t bits = ctx->bytes * 8;
    uint8_t pad[72] = {0x80};
    size_t pad_len = (ctx->fill < 56) ? (56 - ctx->fill) : (120 - ctx->fill);
    md5_update(ctx, pad, pad_len);
    uint8_t length_bytes[8];
    for (unsigned i = 0; i < 8; ++i) length_bytes[i] = (uint8_t)(bits >> (8 * i));
    md5_update(ctx, length_bytes, sizeof(length_bytes));
    for (unsigned i = 0; i < 4; ++i) {
        digest[i * 4] = (uint8_t)ctx->h[i];
        digest[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 8);
        digest[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 16);
        digest[i * 4 + 3] = (uint8_t)(ctx->h[i] >> 24);
    }
}

bool wr_md5_hex(const void *data, size_t len, char out[WR_MD5_HEX_CAP]) {
    if (!out || (!data && len)) return false;
    wr_md5_ctx ctx;
    md5_init(&ctx);
    if (len) md5_update(&ctx, (const uint8_t *)data, len);
    uint8_t digest[16];
    md5_final(&ctx, digest);
    static const char kHex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 15];
    }
    out[32] = 0;
    return true;
}

/* ---- 缓冲区拼接 / Bounded appends ---- */

static bool append(char *out, size_t cap, size_t *pos, const char *text, size_t len) {
    if (*pos + len >= cap) return false;
    memcpy(out + *pos, text, len);
    *pos += len;
    out[*pos] = 0;
    return true;
}

/* ---- `_e()` 混淆 / id obfuscation ---- */

bool wr_encode_id(const char *value, char *out, size_t cap) {
    if (!value || !value[0] || !out || cap < 24) return false;
    const size_t value_len = strlen(value);
    char first_hash[WR_MD5_HEX_CAP];
    if (!wr_md5_hex(value, value_len, first_hash)) return false;

    size_t pos = 0;
    out[0] = 0;
    if (!append(out, cap, &pos, first_hash, 3)) return false;

    bool numeric = true;
    for (size_t i = 0; i < value_len; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            numeric = false;
            break;
        }
    }
    if (!append(out, cap, &pos, numeric ? "3" : "4", 1)) return false;
    if (!append(out, cap, &pos, "2", 1)) return false;
    if (!append(out, cap, &pos, first_hash + 30, 2)) return false;

    if (numeric) {
        for (size_t offset = 0; offset < value_len; offset += 9) {
            if (offset && !append(out, cap, &pos, "g", 1)) return false;
            const size_t count = (value_len - offset) < 9 ? (value_len - offset) : 9;
            uint32_t number = 0;
            for (size_t i = 0; i < count; ++i) number = number * 10u + (uint32_t)(value[offset + i] - '0');
            char chunk[16];
            int chunk_len = snprintf(chunk, sizeof(chunk), "%x", (unsigned)number);
            if (chunk_len <= 0 || chunk_len >= (int)sizeof(chunk)) return false;
            char prefix[3];
            snprintf(prefix, sizeof(prefix), "%02x", (unsigned)chunk_len);
            if (!append(out, cap, &pos, prefix, 2)) return false;
            if (!append(out, cap, &pos, chunk, (size_t)chunk_len)) return false;
        }
    } else {
        char encoded[261];
        size_t encoded_len = 0;
        for (size_t i = 0; i < value_len; ++i) {
            char byte_hex[3];
            snprintf(byte_hex, sizeof(byte_hex), "%x", (unsigned char)value[i]);
            const size_t piece = strlen(byte_hex);
            if (encoded_len + piece >= sizeof(encoded)) return false;
            memcpy(encoded + encoded_len, byte_hex, piece);
            encoded_len += piece;
        }
        if (encoded_len > 0xff) return false;
        char prefix[3];
        snprintf(prefix, sizeof(prefix), "%02x", (unsigned)encoded_len);
        if (!append(out, cap, &pos, prefix, 2)) return false;
        if (!append(out, cap, &pos, encoded, encoded_len)) return false;
    }

    while (pos < 20) {
        const size_t room = 20 - pos;
        const size_t take = room < 32 ? room : 32;
        if (!append(out, cap, &pos, first_hash, take)) return false;
    }

    char final_hash[WR_MD5_HEX_CAP];
    if (!wr_md5_hex(out, pos, final_hash)) return false;
    return append(out, cap, &pos, final_hash, 3);
}

/* ---- `_s()` 签名 / request signature ---- */

bool wr_sign_query(const char *query, char *out, size_t cap) {
    if (!query || !out || cap < 9) return false;
    const size_t length = strlen(query);
    uint32_t a = 0x15051505u;
    uint32_t b = a;
    size_t i = length;
    while (i > 1) {
        const uint32_t current = (uint8_t)query[i - 1];
        const uint32_t previous = (uint8_t)query[i - 2];
        a = (a ^ (current << ((length - i + 1) % 30))) & 0x7fffffffu;
        b = (b ^ (previous << ((i - 1) % 30))) & 0x7fffffffu;
        i -= 2;
    }
    const uint64_t sum = (uint64_t)a + (uint64_t)b;
    const int written = snprintf(out, cap, "%llx", (unsigned long long)sum);
    return written > 0 && (size_t)written < cap;
}

/* ---- URL 编码 / URL encoding ---- */

bool wr_url_encode(const char *value, char *out, size_t cap) {
    if (!value || !out || cap == 0) return false;
    static const char kHex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        const unsigned char c = *p;
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            if (pos + 1 >= cap) return false;
            out[pos++] = (char)c;
        } else {
            if (pos + 3 >= cap) return false;
            out[pos++] = '%';
            out[pos++] = kHex[c >> 4];
            out[pos++] = kHex[c & 15];
        }
    }
    out[pos] = 0;
    return true;
}

/* ---- UTF-8 安全拷贝 / UTF-8 safe copy ---- */

void wr_utf8_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!src) return;
    const size_t limit = cap - 1;
    size_t last_boundary = 0;
    size_t i = 0;
    while (src[i] && i <= limit) {
        const unsigned char c = (unsigned char)src[i];
        size_t width = 1;
        if (c >= 0xF0) width = 4;
        else if (c >= 0xE0) width = 3;
        else if (c >= 0xC0) width = 2;
        if (i + width > limit) break;
        bool valid = true;
        for (size_t k = 1; k < width; ++k) {
            if (((unsigned char)src[i + k] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
        i += width;
        last_boundary = i;
    }
    memcpy(dst, src, last_boundary);
    dst[last_boundary] = 0;
}
