// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

#include <unistd.h>

#include <ps5/klog.h>

namespace ps5browser::frontend {

namespace {

std::mutex g_log_lock;
FILE* g_log_file = nullptr;
std::string g_role = "UI";

long long monotonic_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Taken at static initialisation, i.e. before main().
const long long g_start_ms = monotonic_ms();

void vlog(const char* fmt, va_list ap) {
  char buf[1024];
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  log_line(buf);
}

}  // namespace

void log_set_role(const std::string& role) {
  std::lock_guard<std::mutex> lock(g_log_lock);
  g_role = role;
}

void log_open(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_log_lock);
  if (g_log_file) {
    return;
  }
  g_log_file = std::fopen(path.c_str(), "a");
}

void log_close() {
  std::lock_guard<std::mutex> lock(g_log_lock);
  if (g_log_file) {
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
}

void log_line(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_log_lock);
  char prefix[64];
  std::snprintf(prefix, sizeof(prefix), "[%7lld %s]", monotonic_ms() - g_start_ms,
                g_role.c_str());
  std::fprintf(stderr, "[ps5browser] %s %s\n", prefix, message.c_str());
  std::fflush(stderr);
  klog_printf("[ps5browser] %s %s\n", prefix, message.c_str());
  if (g_log_file) {
    std::fprintf(g_log_file, "%s %s\n", prefix, message.c_str());
    std::fflush(g_log_file);
    fsync(fileno(g_log_file));
  }
}

void log_printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog(fmt, ap);
  va_end(ap);
}

}  // namespace ps5browser::frontend

// The C logging contract of the spawner (ps5browser/spawn/spawn.h).
extern "C" void ps5_log_line(const char* line) {
  ps5browser::frontend::log_line(line ? line : "");
}

extern "C" void ps5_log_printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ps5browser::frontend::vlog(fmt, ap);
  va_end(ap);
}
