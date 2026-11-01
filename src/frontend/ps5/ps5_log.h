// SPDX-License-Identifier: GPL-3.0-or-later
//
// Logging for console diagnostics. Every line goes to stderr (forwarded by
// the loader), to klog, and, once opened, to /data/prosperoweb/log.txt, which
// is flushed and fsync'ed per line so a crash or a hard power-off keeps it.
// Lines are prefixed with the milliseconds since the process started and the
// role ("UI", "WebProcess", ...); all roles append to the same file.
// Thread-safe.
#pragma once

#include <string>

namespace prosperoweb::frontend {

// Sets the role tag of this process's lines (default "UI").
void log_set_role(const std::string& role);

// Opens the log file at path (appending). Safe to skip; stderr always works.
void log_open(const std::string& path);
void log_close();

// Writes one line (a trailing newline is added).
void log_line(const std::string& message);

// PROSPEROWEB_TRACE=1: per-message and per-call diagnostics are logged and
// every line is mirrored to klog and stderr (read once per process).
bool trace_enabled();

// printf-style helper.
void log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace prosperoweb::frontend
