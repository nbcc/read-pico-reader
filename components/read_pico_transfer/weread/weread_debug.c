/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书落盘日志。每行独立打开/追加/关闭，避免和阅读器长期打开的文件句柄互相影响。
 *
 * English: WeRead on-disk log. Each line opens, appends and closes independently so it never
 * holds a long-lived handle next to the reader's own SD files.
 *
 * 冻结：只追加 /sdcard/weread-debug.log；不记录 wr_vid、wr_skey、accessToken 或完整 cookie。
 * Frozen: append only to /sdcard/weread-debug.log; never log wr_vid, wr_skey, accessToken or cookies.
 */
#include "weread_debug.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WR_DEBUG_PATH "/sdcard/weread-debug.log"
#define WR_DEBUG_TAG "wereadlog"

static SemaphoreHandle_t s_log_lock;

static void lock_log(void) {
    if (!s_log_lock) s_log_lock = xSemaphoreCreateMutex();
    if (s_log_lock) (void)xSemaphoreTake(s_log_lock, portMAX_DELAY);
}

static void unlock_log(void) {
    if (s_log_lock) (void)xSemaphoreGive(s_log_lock);
}

void wr_debug_log(const char *fmt, ...) {
    char line[320];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    const int64_t uptime_ms = esp_timer_get_time() / 1000;
    const time_t now = time(NULL);
    struct tm parts;
    if (localtime_r(&now, &parts)) {
        ESP_LOGI(WR_DEBUG_TAG, "[%lldms %04d-%02d-%02d %02d:%02d:%02d] %s",
                 (long long)uptime_ms, parts.tm_year + 1900, parts.tm_mon + 1,
                 parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec, line);
    } else {
        ESP_LOGI(WR_DEBUG_TAG, "[%lldms] %s", (long long)uptime_ms, line);
    }

    lock_log();
    FILE *file = fopen(WR_DEBUG_PATH, "a");
    if (file) {
        if (localtime_r(&now, &parts)) {
            fprintf(file, "[%lldms %04d-%02d-%02d %02d:%02d:%02d] %s\n",
                    (long long)uptime_ms, parts.tm_year + 1900, parts.tm_mon + 1,
                    parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec, line);
        } else {
            fprintf(file, "[%lldms] %s\n", (long long)uptime_ms, line);
        }
        fclose(file);
    }
    unlock_log();
}
