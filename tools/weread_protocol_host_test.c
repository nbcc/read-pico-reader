/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：微信读书协议纯函数的主机回归：MD5、_e、_s、URL 编码与 UTF-8 截断。
 * English: Host regression for the WeRead protocol helpers: MD5, _e, _s, URL encoding and UTF-8 truncation.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "weread_protocol.h"

static void expect_id(const char *value, const char *expected) {
    char out[WR_ENCODE_ID_CAP];
    assert(wr_encode_id(value, out, sizeof(out)));
    if (strcmp(out, expected)) {
        fprintf(stderr, "encode_id(%s) = %s, expected %s\n", value, out, expected);
        assert(0);
    }
}

int main(void) {
    char hex[WR_MD5_HEX_CAP];
    assert(wr_md5_hex("", 0, hex) && !strcmp(hex, "d41d8cd98f00b204e9800998ecf8427e"));
    assert(wr_md5_hex("abc", 3, hex) && !strcmp(hex, "900150983cd24fb0d6963f7d28e17f72"));
    assert(wr_md5_hex("43208843", 8, hex) && !strcmp(hex, "c9c7f86b2173164939da0b09dd6de21c"));

    expect_id("43208843", "c9c321c07293508bc9c79df");
    expect_id("2", "c81322c012c81e728d9d180");

    char sign[WR_SIGN_CAP];
    assert(wr_sign_query("a=1&b=2", sign, sizeof(sign)) && !strcmp(sign, "2a0a2922"));

    char encoded[64];
    assert(wr_url_encode("a b/\xE4\xB8\xAD", encoded, sizeof(encoded)));
    assert(!strcmp(encoded, "a%20b%2F%E4%B8%AD"));
    assert(wr_url_encode("aA0-_.~", encoded, sizeof(encoded)));
    assert(!strcmp(encoded, "aA0-_.~"));

    char small[4] = "xxx";
    char tiny[3];
    assert(!wr_url_encode("abc", tiny, sizeof(tiny)));
    assert(!wr_encode_id("43208843", small, sizeof(small)));

    char utf8[8];
    wr_utf8_copy(utf8, 5, "\xE4\xB8\xAD\xE6\x96\x87" "abc");
    assert(!strcmp(utf8, "\xE4\xB8\xAD"));
    wr_utf8_copy(utf8, sizeof(utf8), "abcdef");
    assert(!strcmp(utf8, "abcdef"));
    wr_utf8_copy(utf8, sizeof(utf8), NULL);
    assert(utf8[0] == 0);

    puts("weread protocol: OK");
    return 0;
}
