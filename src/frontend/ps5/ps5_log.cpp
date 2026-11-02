// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_log.h"

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    g_log_fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0666);
    // The spawn broker runs as root and opens this same log; a stale run can
    // leave log.txt root-owned 0644, so the uid-1 UI's open() fails with
    // EACCES and the whole UI log silently vanishes. The profile directory is
    // world-writable (the broker's open_profile), so replace the stale file.
    if (g_log_fd < 0 && (errno == EACCES || errno == EPERM)) {
      unlink(path.c_str());
      g_log_fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0666);
    }
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

// Lines mirrored to klog (synchronous, read over the network by klogsrv)
// and made durable with fsync: the first ones of every process (start-up)
// and failures; every line with PROSPEROWEB_TRACE=1. Console run 32: per-
// message diagnostics written this way stalled the children's run loops.
namespace {

constexpr int kStartupLines = 120;
int g_lines = 0;

bool important(const std::string& message) {
  for (const char* word : {"FAIL", "fail", "error", "ERROR", "cannot", "refused", "MISSING",
                           "DROPS", "TIMED OUT", "crash", "abort"}) {
    if (message.find(word) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool trace_enabled() {
  // Read from the environment while the process starts (config.json's env
  // is applied after the first log lines), then fixed.
  static std::atomic<int> cached{-1};
  const int known = cached.load(std::memory_order_relaxed);
  if (known >= 0) {
    return known != 0;
  }
  const char* value = std::getenv("PROSPEROWEB_TRACE");
  const bool enabled = value && !std::strcmp(value, "1");
  if (monotonic_ms() - g_start_ms > 5000) {
    cached.store(enabled ? 1 : 0, std::memory_order_relaxed);
  }
  return enabled;
}

namespace {

bool mirror_line(const std::string& message) {
  return trace_enabled() || g_lines <= kStartupLines || important(message);
}

}  // namespace

void log_line(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_log_lock);
  g_lines++;
  const bool mirror = mirror_line(message);
  char prefix[64];
  std::snprintf(prefix, sizeof(prefix), "[%7lld %s]", monotonic_ms() - g_start_ms,
                g_role.c_str());
#if PROSPEROWEB_NATIVE_TITLE
  if (mirror) {
    klog_printf("[prosperoweb] %s %s\n", prefix, message.c_str());
  }
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
  if (mirror) {
    std::fprintf(stderr, "[prosperoweb] %s %s\n", prefix, message.c_str());
    std::fflush(stderr);
    klog_printf("[prosperoweb] %s %s\n", prefix, message.c_str());
  }
  if (g_log_file) {
    std::fprintf(g_log_file, "%s %s\n", prefix, message.c_str());
    std::fflush(g_log_file);
    if (mirror && !trace_enabled()) {
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

// WebKit's diagnostics hook (webkit-port patch 0018): routine traces only
// with PROSPEROWEB_TRACE=1.
extern "C" bool ps5_trace_enabled(void) { return prosperoweb::frontend::trace_enabled(); }

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
