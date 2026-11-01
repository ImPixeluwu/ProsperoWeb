// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

#include <fcntl.h>
#include <unistd.h>

#include <ps5/klog.h>

namespace prosperoweb::frontend {

namespace {

std::mutex g_log_lock;
FILE* g_log_file = nullptr;
// The native title writes the log at the descriptor level and never through
// stdio (run 21: the first log line crashed in fprintf(stderr) with a NULL
// stderr).
int g_log_fd = -1;
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
#if PROSPEROWEB_NATIVE_TITLE
  if (g_log_fd < 0) {
    g_log_fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  }
#else
  if (g_log_file) {
    return;
  }
  g_log_file = std::fopen(path.c_str(), "a");
#endif
}

void log_close() {
  std::lock_guard<std::mutex> lock(g_log_lock);
  if (g_log_file) {
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
  if (g_log_fd >= 0) {
    close(g_log_fd);
    g_log_fd = -1;
  }
}

void log_line(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_log_lock);
  char prefix[64];
  std::snprintf(prefix, sizeof(prefix), "[%7lld %s]", monotonic_ms() - g_start_ms,
                g_role.c_str());
#if PROSPEROWEB_NATIVE_TITLE
  klog_printf("[prosperoweb] %s %s\n", prefix, message.c_str());
  if (g_log_fd >= 0) {
    std::string line = std::string(prefix) + " " + message + "\n";
    const char* p = line.data();
    size_t left = line.size();
    while (left > 0) {
      const ssize_t n = write(g_log_fd, p, left);
      if (n <= 0) {
        break;
      }
      p += n;
      left -= static_cast<size_t>(n);
    }
  }
  return;
#endif
  std::fprintf(stderr, "[prosperoweb] %s %s\n", prefix, message.c_str());
  std::fflush(stderr);
  klog_printf("[prosperoweb] %s %s\n", prefix, message.c_str());
  if (g_log_file) {
    std::fprintf(g_log_file, "%s %s\n", prefix, message.c_str());
    std::fflush(g_log_file);
    // Diagnostic traces (WebKit IPC timings, one line per message) are not
    // made durable one by one: an fsync per line would slow down what they
    // measure.
    if (message.compare(0, 7, "webkit:") != 0 && message.compare(0, 6, "trace:") != 0) {
      fsync(fileno(g_log_file));
    }
  }
}

void log_printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog(fmt, ap);
  va_end(ap);
}

}  // namespace prosperoweb::frontend

// The C logging contract of the spawner (prosperoweb/spawn/spawn.h).
extern "C" void ps5_log_line(const char* line) {
  prosperoweb::frontend::log_line(line ? line : "");
}

extern "C" void ps5_log_printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  prosperoweb::frontend::vlog(fmt, ap);
  va_end(ap);
}
