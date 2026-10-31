// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_log.h"

#include <cstdarg>
#include <cstdio>

namespace ps5browser::frontend {

namespace {
FILE* g_log_file = nullptr;
}

void log_open(const std::string& path) {
  if (g_log_file) {
    return;
  }
  g_log_file = std::fopen(path.c_str(), "a");
}

void log_close() {
  if (g_log_file) {
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
}

void log_line(const std::string& message) {
  std::fprintf(stderr, "[ps5browser] %s\n", message.c_str());
  std::fflush(stderr);
  if (g_log_file) {
    std::fprintf(g_log_file, "%s\n", message.c_str());
    std::fflush(g_log_file);
  }
}

void log_printf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_line(buf);
}

}  // namespace ps5browser::frontend

// The C logging contract of the spawner (ps5browser/spawn/spawn.h).
extern "C" void ps5_log_line(const char* line) {
  ps5browser::frontend::log_line(line ? line : "");
}

extern "C" void ps5_log_printf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  ps5browser::frontend::log_line(buf);
}
