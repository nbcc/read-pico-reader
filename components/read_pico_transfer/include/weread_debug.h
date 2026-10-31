/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：微信读书调试日志的公共入口；仅记录状态和错误，不记录 cookie 或 token。
 *
 * English: Public entry for WeRead debug logging; states and errors only, never cookies or tokens.
 *
 * 冻结：仅显式调试构建启用；日志只写 /sdcard/weread-debug.log，不参与协议或状态判断。
 * Frozen: enabled only in explicit debug builds; logs go only to /sdcard/weread-debug.log and never affect protocol or state decisions.
 */
#pragma once

#include <stdarg.h>

// 调试日志是显式构建选项；正式固件不编译日志实现，也不保留格式字符串。
// Debug logging is an explicit build option; release firmware compiles out the implementation and format strings.
#if defined(KIIKO_WEREAD_DEBUG_LOG) && KIIKO_WEREAD_DEBUG_LOG && defined(ESP_PLATFORM)
#define WR_DEBUG_ENABLED 1
/// 追加一行带时间戳的调试日志；同时打印到串口。/ Append one timestamped line and mirror to serial.
void wr_debug_log(const char *fmt, ...);
#else
#define WR_DEBUG_ENABLED 0
#define wr_debug_log(...) ((void)0)
#endif
