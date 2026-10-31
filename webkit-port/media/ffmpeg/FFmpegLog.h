// SPDX-License-Identifier: GPL-3.0-or-later
//
// Console diagnostics of the ffmpeg media engine. On the PS5 the shell
// executable defines ps5_log_line() (src/frontend/ps5/ps5_log.cpp), which
// prefixes "[ms role]" and appends to /data/prosperobrowser/log.txt; WebCore only
// sees a weak declaration, so builds without that symbol (the smoke test,
// other embedders) still link and log nothing. Lines start with "media:".
// Thread-safe: ps5_log_line is.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include <cstdarg>
#include <cstdio>

extern "C" void ps5_log_line(const char*) __attribute__((weak));

namespace WebCore {

inline bool ffmpegMediaLogEnabled()
{
    return !!ps5_log_line;
}

inline void ffmpegMediaLog(const char* format, ...) __attribute__((format(printf, 1, 2)));

inline void ffmpegMediaLog(const char* format, ...)
{
    if (!ps5_log_line)
        return;
    char line[512];
    int prefix = snprintf(line, sizeof(line), "media: ");
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line + prefix, sizeof(line) - prefix, format, arguments);
    va_end(arguments);
    ps5_log_line(line);
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
