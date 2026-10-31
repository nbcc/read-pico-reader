/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：章节协议、消毒、目录解析与 ZIP 打包的主机回归。
 * English: Host regression for chapter protocol, sanitizing, catalog parsing and ZIP packaging.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "weread_content.h"
#include "weread_protocol.h"
#include "weread_toc.h"
#include "weread_xhtml.h"
#include "weread_zip.h"

// 与 Python 参考实现独立算出的固定向量。/ Fixed vectors computed independently in Python.
static const char kExpectedJson[] =
    "{\"b\":\"c9c321c07293508bc9c79df\",\"c\":\"c81322c012c81e728d9d180\",\"r\":\"1522756\","
    "\"ct\":\"1700000123\",\"ps\":\"1700000000\",\"pc\":\"782320307a21fe8cg013826\",\"sc\":1,"
    "\"prevChapter\":false,\"st\":0,\"s\":\"22373b6e\"}";
static const char kPayload[] =
    "\x7fPWw0bGh-PGvZJHk-PHA-vYVsbG8g5Lit5paHPSG2hjwC9m9keT48LwP0bWw-";
static const char kPlain[] = "<html><body><p>Hello \xE4\xB8\xAD\xE6\x96\x87</p></body></html>";

static void content_tests(void) {
    char json[WR_CONTENT_JSON_CAP];
    assert(wr_make_content_json("43208843", "2", "1700000000", 1700000123, 1234, false, json, sizeof(json)));
    assert(!strcmp(json, kExpectedJson));

    char digest[33];
    assert(wr_md5_hex(kPayload, strlen(kPayload), digest));
    char shard[128];
    snprintf(shard, sizeof(shard), "%s%s", digest, kPayload);
    const char *body = NULL;
    size_t body_len = 0;
    assert(wr_shard_body(shard, strlen(shard), &body, &body_len));
    assert(body_len == strlen(kPayload));

    shard[0] = shard[0] == '0' ? '1' : '0';
    assert(!wr_shard_body(shard, strlen(shard), &body, &body_len));

    char decoded[256];
    snprintf(decoded, sizeof(decoded), "%s", kPayload);
    size_t decoded_len = 0;
    assert(wr_decode_payload(decoded, strlen(kPayload), &decoded_len));
    assert(decoded_len == strlen(kPlain) && !memcmp(decoded, kPlain, decoded_len));

    char psvts[64];
    const char *html = "<script>window.__INITIAL_STATE__ = {\"reader\":{\"psvts\":\"abc123\",\"token\":\"t\"}}</script>";
    assert(wr_extract_json_field(html, strlen(html), "psvts", psvts, sizeof(psvts)));
    assert(!strcmp(psvts, "abc123"));
    assert(!wr_extract_json_field(html, strlen(html), "missing", psvts, sizeof(psvts)));

    // 封面魔数识别 / Cover magic sniffing
    const char *media = NULL;
    const char *extension = NULL;
    const char jpeg[] = "\xff\xd8\xff\xe0rest";
    const char png[] = "\x89PNG\r\n\x1a\nrest";
    assert(wr_sniff_image(jpeg, sizeof(jpeg) - 1, &media, &extension));
    assert(!strcmp(media, "image/jpeg") && !strcmp(extension, "jpg"));
    assert(wr_sniff_image(png, sizeof(png) - 1, &media, &extension));
    assert(!strcmp(media, "image/png") && !strcmp(extension, "png"));
    assert(!wr_sniff_image("hello", 5, &media, &extension));

    char txt[64];
    size_t txt_len = 0;
    assert(wr_txt_to_xhtml("a&b\n\nc<d", 8, txt, sizeof(txt), &txt_len));
    assert(!strcmp(txt, "<p>a&amp;b</p>\n<p>c&lt;d</p>\n"));
}

static void sanitizer_tests(void) {
    const char *in =
        "<html><head><style>p{color:red}</style></head><body>"
        "<p class=\"x\">Hi <b>bold</b><script>bad()</script></p>"
        "<img class=\"qqreader-footnote\" alt=\"注一\"/>"
        "<img src=\"https://res.weread.qq.com/a.jpg\"/>"
        "<div style=\"text-align:center\">尾</div></body></html>";
    char out[512];
    size_t out_len = 0;
    assert(wr_sanitize_xhtml(in, strlen(in), out, sizeof(out), &out_len));
    assert(strstr(out, "<p>Hi <b>bold</b></p>"));
    assert(strstr(out, "（注：注一）"));
    assert(!strstr(out, "img") && !strstr(out, "script") && !strstr(out, "style") && !strstr(out, "class"));
    assert(strstr(out, "<div>尾</div>"));
    // 容量不足必须整体失败。/ Insufficient capacity fails the whole chapter.
    char small[8];
    size_t small_len = 0;
    assert(!wr_sanitize_xhtml(in, strlen(in), small, sizeof(small), &small_len));
}

static void toc_tests(void) {
    static const char kCatalog[] =
        "{\"bookId\":\"1\",\"format\":\"epub\",\"updated\":["
        "{\"chapterUid\":1,\"chapterIdx\":1,\"title\":\"封面\",\"wordCount\":0,\"level\":1,\"paid\":0},"
        "{\"chapterUid\":\"2\",\"chapterIdx\":2,\"title\":\"a\\\"b\",\"wordCount\":1200,\"level\":1,\"paid\":1}"
        "],\"errcode\":0}";
    wr_toc_chapter_t items[8];
    size_t count = 0;
    int error = -1;
    assert(wr_toc_parse(kCatalog, strlen(kCatalog), items, 8, &count, &error));
    assert(count == 2 && error == 0);
    assert(!strcmp(items[0].uid, "1") && !strcmp(items[0].title, "封面"));
    assert(!strcmp(items[1].uid, "2") && !strcmp(items[1].title, "a\"b"));
    assert(items[1].words == 1200 && items[1].paid == 1 && items[1].idx == 2);
    assert(wr_toc_readable(&items[1]) && !wr_toc_readable(&items[0]));

    static const char kError[] = "{\"errcode\":-2012}";
    assert(wr_toc_parse(kError, strlen(kError), items, 8, &count, &error));
    assert(count == 0 && error == -2012);
}

static uint32_t crc32_of(const uint8_t *data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

static uint16_t read_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void zip_tests(void) {
    char dir[] = "/tmp/weread-zip-XXXXXX";
    assert(mkdtemp(dir));
    char zip_path[288], central_path[288], source_path[288];
    snprintf(zip_path, sizeof(zip_path), "%s/book.zip", dir);
    snprintf(central_path, sizeof(central_path), "%s/central.tmp", dir);
    snprintf(source_path, sizeof(source_path), "%s/chapter", dir);
    FILE *source = fopen(source_path, "wb");
    assert(source && fputs("<p>hello</p>", source) >= 0 && fclose(source) == 0);

    assert(wr_zip_begin(zip_path, central_path));
    assert(wr_zip_add_buffer("mimetype", "application/epub+zip", 20));
    assert(wr_zip_add_file("OEBPS/text/ch1.xhtml", source_path));
    assert(wr_zip_finish());

    FILE *zip = fopen(zip_path, "rb");
    assert(zip);
    assert(fseek(zip, 0, SEEK_END) == 0);
    const long size = ftell(zip);
    assert(size > 22);
    uint8_t *data = malloc((size_t)size);
    assert(data);
    assert(fseek(zip, 0, SEEK_SET) == 0 && fread(data, 1, (size_t)size, zip) == (size_t)size);
    fclose(zip);

    const uint8_t *eocd = data + size - 22;
    assert(read_u32(eocd) == 0x06054b50u);
    const uint16_t count = read_u16(eocd + 10);
    const uint32_t central_offset = read_u32(eocd + 16);
    assert(count == 2 && central_offset < (uint32_t)size);

    const char *names[2] = {"mimetype", "OEBPS/text/ch1.xhtml"};
    const char *contents[2] = {"application/epub+zip", "<p>hello</p>"};
    size_t at = central_offset;
    for (uint16_t i = 0; i < count; ++i) {
        assert(read_u32(data + at) == 0x02014b50u);
        const uint32_t crc = read_u32(data + at + 16);
        const uint32_t packed = read_u32(data + at + 20);
        const uint16_t name_len = read_u16(data + at + 28);
        const uint16_t extra_len = read_u16(data + at + 30);
        const uint16_t comment_len = read_u16(data + at + 32);
        const uint32_t local = read_u32(data + at + 42);
        const char *name = (const char *)(data + at + 46);
        assert(strlen(names[i]) == name_len && !memcmp(name, names[i], name_len));
        const uint32_t local_name_len = read_u16(data + local + 26);
        const uint32_t local_extra_len = read_u16(data + local + 28);
        const uint8_t *body = data + local + 30 + local_name_len + local_extra_len;
        assert(packed == strlen(contents[i]));
        assert(crc == crc32_of(body, packed));
        assert(!memcmp(body, contents[i], packed));
        at += 46 + name_len + extra_len + comment_len;
    }
    assert(at == central_offset + read_u32(eocd + 12));
    free(data);
}

int main(void) {
    content_tests();
    sanitizer_tests();
    toc_tests();
    zip_tests();
    puts("weread content: OK");
    return 0;
}
