/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：ZIP store 写入实现；CRC32 逐块计算，中央目录写入临时文件后拼接。
 *
 * English: Store-only ZIP writer; CRC32 is computed per chunk and the central directory is
 * staged in a temporary file before being appended.
 *
 * 冻结：单个文件失败立即 abort；条目名上限 200 字节；不覆盖调用方的文件系统语义。
 * Frozen: Any single failure aborts the whole archive; names are capped at 200 bytes.
 */
#include "weread_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WR_ZIP_COPY_BUF (16u * 1024u)
#define WR_ZIP_LOCAL_MIN 30u
#define WR_ZIP_CENTRAL_MIN 46u
#define WR_ZIP_EOCD 22u

static FILE *s_zip;
static FILE *s_central;
static uint8_t *s_copy;
static uint32_t s_offset;
static uint16_t s_count;
static bool s_active;
static char s_zip_path[288];
static char s_central_path[288];

static uint32_t s_crc_table[256];
static bool s_crc_ready;

static void crc_init(void) {
    if (s_crc_ready) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) value = (value & 1) ? (0xedb88320u ^ (value >> 1)) : (value >> 1);
        s_crc_table[i] = value;
    }
    s_crc_ready = true;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) crc = s_crc_table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return crc;
}

static bool write_u16(FILE *file, uint16_t value) {
    const uint8_t bytes[2] = {(uint8_t)(value & 0xff), (uint8_t)(value >> 8)};
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static bool write_u32(FILE *file, uint32_t value) {
    const uint8_t bytes[4] = {(uint8_t)(value & 0xff), (uint8_t)((value >> 8) & 0xff),
                              (uint8_t)((value >> 16) & 0xff), (uint8_t)((value >> 24) & 0xff)};
    return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static bool write_name(FILE *file, const char *name, size_t len) {
    return fwrite(name, 1, len, file) == len;
}

static bool add_entry(const char *name, const void *data, size_t len, const char *source_path) {
    if (!s_active || !name) return false;
    const size_t name_len = strlen(name);
    if (!name_len || name_len > 200 || s_count == UINT16_MAX) return false;

    uint32_t crc = 0xffffffffu;
    size_t size = len;
    if (source_path) {
        FILE *input = fopen(source_path, "rb");
        if (!input) return false;
        crc_init();
        size = 0;
        for (;;) {
            const size_t got = fread(s_copy, 1, WR_ZIP_COPY_BUF, input);
            if (!got) break;
            crc = crc_update(crc, s_copy, got);
            size += got;
        }
        const bool failed = ferror(input) != 0;
        fclose(input);
        if (failed) return false;
    } else {
        crc_init();
        if (len) crc = crc_update(crc, data, len);
    }
    crc ^= 0xffffffffu;
    if (size > UINT32_MAX) return false;

    const uint32_t local_offset = s_offset;
    if (!write_u32(s_zip, 0x04034b50u) || !write_u16(s_zip, 20) || !write_u16(s_zip, 0) ||
        !write_u16(s_zip, 0) || !write_u16(s_zip, 0) || !write_u16(s_zip, 0x21) ||
        !write_u32(s_zip, crc) || !write_u32(s_zip, (uint32_t)size) || !write_u32(s_zip, (uint32_t)size) ||
        !write_u16(s_zip, (uint16_t)name_len) || !write_u16(s_zip, 0) || !write_name(s_zip, name, name_len)) {
        return false;
    }
    s_offset += WR_ZIP_LOCAL_MIN + (uint32_t)name_len;

    if (source_path) {
        FILE *input = fopen(source_path, "rb");
        if (!input) return false;
        size_t copied = 0;
        while (copied < size) {
            size_t want = size - copied;
            if (want > WR_ZIP_COPY_BUF) want = WR_ZIP_COPY_BUF;
            const size_t got = fread(s_copy, 1, want, input);
            if (!got || fwrite(s_copy, 1, got, s_zip) != got) {
                fclose(input);
                return false;
            }
            copied += got;
        }
        fclose(input);
    } else if (len) {
        if (fwrite(data, 1, len, s_zip) != len) return false;
    }
    s_offset += (uint32_t)size;

    if (!write_u32(s_central, 0x02014b50u) || !write_u16(s_central, 20) || !write_u16(s_central, 20) ||
        !write_u16(s_central, 0) || !write_u16(s_central, 0) || !write_u16(s_central, 0) ||
        !write_u16(s_central, 0x21) || !write_u32(s_central, crc) || !write_u32(s_central, (uint32_t)size) ||
        !write_u32(s_central, (uint32_t)size) || !write_u16(s_central, (uint16_t)name_len) ||
        !write_u16(s_central, 0) || !write_u16(s_central, 0) || !write_u16(s_central, 0) ||
        !write_u16(s_central, 0) || !write_u32(s_central, 0) || !write_u32(s_central, local_offset) ||
        !write_name(s_central, name, name_len)) {
        return false;
    }
    ++s_count;
    return true;
}

bool wr_zip_begin(const char *path, const char *central_path) {
    if (!path || !central_path || s_active) return false;
    s_zip = fopen(path, "wb");
    if (!s_zip) return false;
    s_central = fopen(central_path, "w+b");
    if (!s_central) {
        fclose(s_zip);
        s_zip = NULL;
        unlink(path);
        return false;
    }
    s_copy = malloc(WR_ZIP_COPY_BUF);
    if (!s_copy) {
        fclose(s_zip);
        fclose(s_central);
        s_zip = s_central = NULL;
        unlink(path);
        unlink(central_path);
        return false;
    }
    snprintf(s_zip_path, sizeof(s_zip_path), "%s", path);
    snprintf(s_central_path, sizeof(s_central_path), "%s", central_path);
    s_offset = 0;
    s_count = 0;
    s_active = true;
    return true;
}

bool wr_zip_add_buffer(const char *name, const void *data, size_t len) {
    if (!data && len) return false;
    return add_entry(name, data, len, NULL);
}

bool wr_zip_add_file(const char *name, const char *path) {
    if (!path) return false;
    return add_entry(name, NULL, 0, path);
}

bool wr_zip_finish(void) {
    if (!s_active) return false;
    bool ok = fflush(s_zip) == 0 && fflush(s_central) == 0;
    const uint32_t central_offset = s_offset;
    uint32_t central_size = 0;
    if (ok) {
        rewind(s_central);
        for (;;) {
            const size_t got = fread(s_copy, 1, WR_ZIP_COPY_BUF, s_central);
            if (!got) break;
            if (fwrite(s_copy, 1, got, s_zip) != got) {
                ok = false;
                break;
            }
            central_size += (uint32_t)got;
        }
        if (ok && ferror(s_central)) ok = false;
    }
    if (ok) {
        ok = write_u32(s_zip, 0x06054b50u) && write_u16(s_zip, 0) && write_u16(s_zip, 0) &&
             write_u16(s_zip, s_count) && write_u16(s_zip, s_count) && write_u32(s_zip, central_size) &&
             write_u32(s_zip, central_offset) && write_u16(s_zip, 0);
    }
    if (fflush(s_zip) != 0) ok = false;
    fclose(s_zip);
    fclose(s_central);
    s_zip = s_central = NULL;
    free(s_copy);
    s_copy = NULL;
    s_active = false;
    if (ok) unlink(s_central_path);
    return ok;
}

void wr_zip_abort(void) {
    if (s_zip) fclose(s_zip);
    if (s_central) fclose(s_central);
    s_zip = s_central = NULL;
    free(s_copy);
    s_copy = NULL;
    if (s_active) {
        unlink(s_zip_path);
        unlink(s_central_path);
    }
    s_active = false;
}
