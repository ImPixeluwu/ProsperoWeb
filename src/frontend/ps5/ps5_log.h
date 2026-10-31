// SPDX-License-Identifier: GPL-3.0-or-later
//
// Logging for console diagnostics. Every line goes to stderr (forwarded by
// the loader), to klog, and, once opened, to /data/ps5browser/log.txt, which
// is flushed and fsync'ed per line so a crash or a hard power-off keeps it.
// Lines are prefixed with the milliseconds since the process started and the
// role ("UI", "WebProcess", ...); all roles append to the same file.
// Thread-safe.
#pragma once

#include <string>

namespace ps5browser::frontend {

// Sets the role tag of this process's lines (default "UI").
void log_set_role(const std::string& role);

// Opens the log file at path (appending). Safe to skip; stderr always works.
void log_open(const std::string& path);
void log_close();

// Writes one line (a trailing newline is added).
void log_line(const std::string& message);

// printf-style helper.
void log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace ps5browser::frontend
