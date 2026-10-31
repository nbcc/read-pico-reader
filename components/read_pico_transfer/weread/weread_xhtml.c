/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：章节 XHTML 消毒实现；所有输出都经过容量检查，越界立即失败。
 *
 * English: Chapter XHTML sanitizer; every write is capacity-checked and overflow fails fast.
 *
 * 冻结：不解析实体表以外的 HTML 语义，不执行脚本，不保留任何属性或外链。
 * Frozen: No HTML semantics beyond entities, no scripts, no attributes and no external links.
 */
#include "weread_xhtml.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char *out;
    size_t cap;
    size_t used;
} writer_t;

static bool emit(writer_t *writer, const char *text, size_t len) {
    if (writer->used + len >= writer->cap) return false;
    memcpy(writer->out + writer->used, text, len);
    writer->used += len;
    return true;
}

static bool emit_str(writer_t *writer, const char *text) {
    return emit(writer, text, strlen(text));
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool starts_with_ci(const char *text, size_t len, const char *prefix) {
    const size_t prefix_len = strlen(prefix);
    if (len < prefix_len) return false;
    for (size_t i = 0; i < prefix_len; ++i) {
        if (lower(text[i]) != lower(prefix[i])) return false;
    }
    return true;
}

static const char *find_ci(const char *text, size_t len, const char *needle) {
    const size_t needle_len = strlen(needle);
    if (len < needle_len) return NULL;
    for (size_t i = 0; i + needle_len <= len; ++i) {
        if (starts_with_ci(text + i, len - i, needle)) return text + i;
    }
    return NULL;
}

static bool allowed_tag(const char *name, size_t len, bool *self_closing) {
    static const char *const tags[] = {
        "p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote",
        "strong", "b", "em", "i", "u", "span", "ul", "ol", "li", "hr", "sup", "sub",
    };
    *self_closing = (len == 2 && !memcmp(name, "br", 2)) || (len == 2 && !memcmp(name, "hr", 2));
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i) {
        const size_t tag_len = strlen(tags[i]);
        if (tag_len == len && !memcmp(name, tags[i], len)) return true;
    }
    return false;
}

static bool emit_tag(writer_t *writer, const char *name, size_t len, bool closing) {
    char buffer[16];
    if (len + 4 > sizeof(buffer)) return false;
    size_t used = 0;
    buffer[used++] = '<';
    if (closing) buffer[used++] = '/';
    memcpy(buffer + used, name, len);
    used += len;
    if (!closing && len == 2 && (!memcmp(name, "br", 2) || !memcmp(name, "hr", 2))) buffer[used++] = '/';
    buffer[used++] = '>';
    return emit(writer, buffer, used);
}

static bool decode_entities(const char *src, size_t len, char *out, size_t cap) {
    size_t used = 0;
    for (size_t i = 0; i < len && used + 1 < cap; ++i) {
        if (src[i] != '&') {
            out[used++] = src[i];
            continue;
        }
        struct {
            const char *name;
            char value;
        } static const simple[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&nbsp;", ' '},
        };
        bool matched = false;
        for (size_t k = 0; k < sizeof(simple) / sizeof(simple[0]); ++k) {
            const size_t name_len = strlen(simple[k].name);
            if (i + name_len <= len && !memcmp(src + i, simple[k].name, name_len)) {
                out[used++] = simple[k].value;
                i += name_len - 1;
                matched = true;
                break;
            }
        }
        if (!matched) out[used++] = '&';
    }
    out[used] = 0;
    return true;
}

// 从标签文本里取属性值（只用于脚注 alt）。/ Read one attribute value, only for footnote alt text.
static bool attribute_value(const char *tag, size_t len, const char *name, char *out, size_t cap) {
    const size_t name_len = strlen(name);
    for (size_t i = 0; i + name_len + 2 < len; ++i) {
        if (lower(tag[i]) != lower(name[0]) || !starts_with_ci(tag + i, len - i, name)) continue;
        if (i > 0 && ((tag[i - 1] >= 'a' && tag[i - 1] <= 'z') || (tag[i - 1] >= 'A' && tag[i - 1] <= 'Z'))) continue;
        size_t at = i + name_len;
        while (at < len && (tag[at] == ' ' || tag[at] == '\t')) ++at;
        if (at >= len || tag[at] != '=') continue;
        ++at;
        while (at < len && (tag[at] == ' ' || tag[at] == '\t')) ++at;
        if (at >= len || (tag[at] != '"' && tag[at] != '\'')) continue;
        const char quote = tag[at++];
        const size_t start = at;
        while (at < len && tag[at] != quote) ++at;
        if (at >= len) return false;
        return decode_entities(tag + start, at - start, out, cap);
    }
    return false;
}

static bool contains_ci(const char *text, size_t len, const char *needle) {
    return find_ci(text, len, needle) != NULL;
}

static bool skip_element(const char *in, size_t len, size_t start, const char *name, size_t *next) {
    char closing[16];
    const int written = snprintf(closing, sizeof(closing), "</%s", name);
    if (written <= 0 || (size_t)written >= sizeof(closing)) return false;
    const char *end = find_ci(in + start, len - start, closing);
    if (!end) {
        *next = len;
        return true;
    }
    const char *close = memchr(end, '>', (size_t)(in + len - end));
    *next = close ? (size_t)(close - in) + 1 : len;
    return true;
}

// 处理一个 body 片段；返回 false 表示输出越界或结构异常。/ Sanitize one body fragment.
static bool sanitize_region(const char *in, size_t len, writer_t *writer) {
    size_t i = 0;
    while (i < len) {
        if (in[i] != '<') {
            const char *next = memchr(in + i, '<', len - i);
            const size_t end = next ? (size_t)(next - in) : len;
            for (size_t k = i; k < end; ++k) {
                const unsigned char c = (unsigned char)in[k];
                if (c == '\r') continue;
                if (c < 0x20 && c != '\n' && c != '\t') continue;
                if (!emit(writer, &in[k], 1)) return false;
            }
            i = end;
            continue;
        }
        if (starts_with_ci(in + i, len - i, "<!--")) {
            const char *end = find_ci(in + i + 4, len - i - 4, "-->");
            i = end ? (size_t)(end - in) + 3 : len;
            continue;
        }
        if (starts_with_ci(in + i, len - i, "<![CDATA[")) {
            const char *end = find_ci(in + i + 9, len - i - 9, "]]>");
            const size_t stop = end ? (size_t)(end - in) : len;
            if (!emit(writer, in + i + 9, stop - (i + 9))) return false;
            i = end ? stop + 3 : len;
            continue;
        }
        if (starts_with_ci(in + i, len - i, "<!") || starts_with_ci(in + i, len - i, "<?")) {
            const char *end = memchr(in + i, '>', len - i);
            i = end ? (size_t)(end - in) + 1 : len;
            continue;
        }

        const char *close = NULL;
        for (size_t k = i + 1; k < len; ++k) {
            if (in[k] == '>' && (k == i + 1 || in[k - 1] != '\\')) {
                close = in + k;
                break;
            }
        }
        if (!close) break;
        const size_t tag_len = (size_t)(close - in) - i - 1;
        const char *tag = in + i + 1;
        bool closing = false;
        size_t name_start = 0;
        while (name_start < tag_len && (tag[name_start] == ' ' || tag[name_start] == '\t' || tag[name_start] == '\n')) ++name_start;
        if (name_start < tag_len && tag[name_start] == '/') {
            closing = true;
            ++name_start;
        }
        size_t name_end = name_start;
        while (name_end < tag_len && tag[name_end] != ' ' && tag[name_end] != '\t' && tag[name_end] != '\n' &&
               tag[name_end] != '/' && tag[name_end] != '>') ++name_end;
        const char *name = tag + name_start;
        const size_t name_len = name_end - name_start;
        i = (size_t)(close - in) + 1;
        if (!name_len) continue;

        if (!closing && starts_with_ci(name, name_len, "script")) {
            if (!skip_element(in, len, i, "script", &i)) return false;
            continue;
        }
        if (!closing && starts_with_ci(name, name_len, "style")) {
            if (!skip_element(in, len, i, "style", &i)) return false;
            continue;
        }
        if (!closing && starts_with_ci(name, name_len, "head")) {
            if (!skip_element(in, len, i, "head", &i)) return false;
            continue;
        }
        if (starts_with_ci(name, name_len, "img") || starts_with_ci(name, name_len, "image")) {
            char alt[192];
            alt[0] = 0;
            const bool footnote = contains_ci(tag, tag_len, "footnote");
            if (footnote && attribute_value(tag, tag_len, "alt", alt, sizeof(alt)) && alt[0]) {
                if (!emit_str(writer, "（注：") || !emit_str(writer, alt) || !emit_str(writer, "）")) return false;
            }
            continue;
        }
        bool self_closing = false;
        if (allowed_tag(name, name_len, &self_closing)) {
            if (!emit_tag(writer, name, name_len, closing)) return false;
        }
    }
    return true;
}

bool wr_sanitize_xhtml(const char *in, size_t len, char *out, size_t cap, size_t *out_len) {
    if (!in || !out || !cap || !out_len) return false;
    writer_t writer = {.out = out, .cap = cap};
    size_t pos = 0;
    bool any_body = false;
    while (pos < len) {
        const char *open = find_ci(in + pos, len - pos, "<body");
        if (!open) break;
        const char *open_end = memchr(open, '>', (size_t)(in + len - open));
        if (!open_end) break;
        const char *close = find_ci(open_end + 1, (size_t)(in + len - open_end - 1), "</body");
        const size_t region_start = (size_t)(open_end - in) + 1;
        const size_t region_end = close ? (size_t)(close - in) : len;
        if (any_body && !emit_str(&writer, "\n")) return false;
        if (!sanitize_region(in + region_start, region_end - region_start, &writer)) return false;
        any_body = true;
        if (!close) break;
        const char *close_end = memchr(close, '>', (size_t)(in + len - close));
        pos = close_end ? (size_t)(close_end - in) + 1 : len;
    }
    if (!any_body) {
        if (!sanitize_region(in, len, &writer)) return false;
    }
    out[writer.used] = 0;
    *out_len = writer.used;
    return true;
}
