/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书调试日志的公共入口；仅记录状态和错误，不记录 cookie 或 token。
 *
 * English: Public entry for WeRead debug logging; states and errors only, never cookies or tokens.
 *
 * 冻结：日志只写 /sdcard/weread-debug.log，不参与协议或状态判断。
 * Frozen: logs go only to /sdcard/weread-debug.log and never affect protocol or state decisions.
 */
#pragma once

#include <stdarg.h>

#ifdef ESP_PLATFORM
/// 追加一行带时间戳的调试日志；同时打印到串口。/ Append one timestamped line and mirror to serial.
void wr_debug_log(const char *fmt, ...);
#else
// 宿主测试没有 /sdcard，也不链接 ESP-IDF；保持调用点可编译且无副作用。
// Host tests have no /sdcard and do not link ESP-IDF; keep call sites compilable and inert.
static inline void wr_debug_log(const char *fmt, ...) { (void)fmt; }
#endif
