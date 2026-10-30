// SPDX-License-Identifier: GPL-3.0-or-later
//
// Logging to stderr (elfldr forwards it to the launcher) and, when opened, to
// /data/ps5browser/log.txt.
#pragma once

#include <string>

namespace ps5browser::frontend {

// Opens the log file at path (appending). Safe to skip; stderr always works.
void log_open(const std::string& path);
void log_close();

// Writes one line (a trailing newline is added) to stderr and the log file.
void log_line(const std::string& message);

// printf-style helper.
void log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace ps5browser::frontend
