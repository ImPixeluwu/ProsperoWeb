// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_cpu_monitor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <unistd.h>

#include "ps5_log.h"

// Defined by the media engine (FFmpegVideoDecoder.cpp) when it is linked.
extern "C" void ps5_media_video_decoder_stats(unsigned* decoders, unsigned* threads)
    __attribute__((weak));

namespace prosperobrowser::frontend {

namespace {

constexpr int kIntervalSeconds = 30;

bool env_off(const char* name) {
  const char* value = std::getenv(name);
  return value && (!std::strcmp(value, "0") || !std::strcmp(value, "off"));
}

// "0-5,8" for a mask.
std::string cpu_list(const cpuset_t& mask) {
  std::string out;
  for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
    if (!CPU_ISSET(cpu, &mask)) {
      continue;
    }
    int last = cpu;
    while (last + 1 < CPU_SETSIZE && CPU_ISSET(last + 1, &mask)) {
      last++;
    }
    if (!out.empty()) {
      out += ',';
    }
    out += std::to_string(cpu);
    if (last > cpu) {
      out += '-' + std::to_string(last);
    }
    cpu = last;
  }
  return out.empty() ? "none" : out;
}

bool get_mask(cpulevel_t level, cpuwhich_t which, id_t id, cpuset_t& mask, int& error) {
  CPU_ZERO(&mask);
  if (cpuset_getaffinity(level, which, id, sizeof(mask), &mask) != 0) {
    error = errno;
    return false;
  }
  return true;
}

double seconds_of(const timeval& tv) {
  return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
}

struct ThreadInfo {
  long tid = 0;
  std::string name;
  double cpu_seconds = 0;
  int last_cpu = -1;
};

// The process's threads (sysctl kern.proc.pid with threads). Empty when the
// kernel's kinfo_proc does not look like the SDK's.
std::vector<ThreadInfo> list_threads(std::string& why) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID | KERN_PROC_INC_THREAD,
                static_cast<int>(getpid())};
  size_t bytes = 0;
  if (sysctl(mib, 4, nullptr, &bytes, nullptr, 0) != 0 || bytes == 0) {
    why = std::string("sysctl kern.proc.pid: ") + std::strerror(errno);
    return {};
  }
  bytes += bytes / 4 + 4096;  // threads may appear meanwhile
  std::vector<char> buffer(bytes);
  if (sysctl(mib, 4, buffer.data(), &bytes, nullptr, 0) != 0) {
    why = std::string("sysctl kern.proc.pid: ") + std::strerror(errno);
    return {};
  }
  std::vector<ThreadInfo> threads;
  size_t offset = 0;
  while (offset + sizeof(kinfo_proc) <= bytes) {
    kinfo_proc info;
    std::memcpy(&info, buffer.data() + offset, sizeof(info));
    if (info.ki_structsize != static_cast<int>(sizeof(kinfo_proc))) {
      why = "kinfo_proc size " + std::to_string(info.ki_structsize) + " != " +
            std::to_string(sizeof(kinfo_proc));
      return {};
    }
    ThreadInfo t;
    t.tid = info.ki_tid;
    info.ki_tdname[sizeof(info.ki_tdname) - 1] = '\0';
    t.name = info.ki_tdname;
    t.cpu_seconds = seconds_of(info.ki_rusage.ru_utime) + seconds_of(info.ki_rusage.ru_stime);
    t.last_cpu = info.ki_lastcpu;
    threads.push_back(std::move(t));
    offset += sizeof(kinfo_proc);
  }
  return threads;
}

class Monitor {
 public:
  void report(bool first) {
    const auto now = std::chrono::steady_clock::now();
    cpuset_t root, process;
    int error = 0;
    std::string root_text = "?";
    std::string process_text = "?";
    int root_count = 0;
    int process_count = 0;
    if (get_mask(CPU_LEVEL_ROOT, CPU_WHICH_PID, -1, root, error)) {
      root_text = cpu_list(root);
      root_count = CPU_COUNT(&root);
    } else {
      root_text = std::string("error ") + std::strerror(error);
    }
    if (get_mask(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, process, error)) {
      process_text = cpu_list(process);
      process_count = CPU_COUNT(&process);
    } else {
      process_text = std::string("error ") + std::strerror(error);
    }
    if (first) {
      const long online = sysconf(_SC_NPROCESSORS_ONLN);
      log_printf("cpu: %ld cpus online, root set %s (%d), process may run on %s (%d)", online,
                 root_text.c_str(), root_count, process_text.c_str(), process_count);
      if (process_count > 0 && root_count > process_count &&
          !env_off("PROSPEROBROWSER_CPU_WIDEN")) {
        if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(root), &root) == 0) {
          cpuset_t after;
          get_mask(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, after, error);
          log_printf("cpu: widened the process affinity to %s (%d cpus)", cpu_list(after).c_str(),
                     CPU_COUNT(&after));
        } else {
          log_printf("cpu: widening the affinity to %s failed: %s (kept %s)", root_text.c_str(),
                     std::strerror(errno), process_text.c_str());
        }
      }
    }

    // Threads: affinity groups, and the busiest ones over the interval.
    std::string why;
    std::vector<ThreadInfo> threads = list_threads(why);
    std::map<std::string, int> masks;  // cpu list -> threads
    for (const ThreadInfo& t : threads) {
      cpuset_t mask;
      masks[get_mask(CPU_LEVEL_WHICH, CPU_WHICH_TID, static_cast<id_t>(t.tid), mask, error)
                ? cpu_list(mask)
                : std::string("error")]++;
    }
    std::string mask_text;
    for (const auto& [list, count] : masks) {
      mask_text += (mask_text.empty() ? "" : ", ") + list + " x" + std::to_string(count);
    }
    if (threads.empty()) {
      mask_text = "unavailable (" + why + ")";
    }

    unsigned decoders = 0;
    unsigned decoder_threads = 0;
    if (ps5_media_video_decoder_stats) {
      ps5_media_video_decoder_stats(&decoders, &decoder_threads);
    }

    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const double user = seconds_of(usage.ru_utime);
    const double sys = seconds_of(usage.ru_stime);
    const double wall =
        first ? 0.0 : std::chrono::duration<double>(now - last_time_).count();
    const double busy = wall > 0 ? (user - last_user_ + sys - last_sys_) / wall : 0.0;

    // Busiest threads since the previous report.
    std::vector<std::pair<double, const ThreadInfo*>> deltas;
    for (const ThreadInfo& t : threads) {
      auto it = last_thread_cpu_.find(t.tid);
      const double before = it == last_thread_cpu_.end() ? 0.0 : it->second;
      deltas.emplace_back(t.cpu_seconds - before, &t);
    }
    std::sort(deltas.begin(), deltas.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string top;
    for (std::size_t i = 0; i < deltas.size() && i < 6; i++) {
      if (deltas[i].first < 0.05) {
        break;
      }
      char item[96];
      std::snprintf(item, sizeof(item), "%s%s %.1fs@cpu%d", top.empty() ? "" : ", ",
                    deltas[i].second->name.c_str(), deltas[i].first, deltas[i].second->last_cpu);
      top += item;
    }

    log_printf("cpu: affinity %s (%d of %d) | threads %zu: %s | video decoders %u, %u codec threads "
               "| user %.1fs sys %.1fs over %.0fs = %.2f cpus busy | top %s",
               process_text.c_str(), process_count, root_count, threads.size(), mask_text.c_str(),
               decoders, decoder_threads, user - last_user_, sys - last_sys_, wall, busy,
               top.empty() ? "-" : top.c_str());

    last_time_ = now;
    last_user_ = user;
    last_sys_ = sys;
    last_thread_cpu_.clear();
    for (const ThreadInfo& t : threads) {
      last_thread_cpu_[t.tid] = t.cpu_seconds;
    }
  }

 private:
  std::chrono::steady_clock::time_point last_time_;
  double last_user_ = 0;
  double last_sys_ = 0;
  std::map<long, double> last_thread_cpu_;
};

}  // namespace

void start_cpu_monitor() {
  if (env_off("PROSPEROBROWSER_CPU_MONITOR")) {
    return;
  }
  auto* monitor = new Monitor();  // lives as long as the process
  monitor->report(/*first=*/true);
  std::thread([monitor] {
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(kIntervalSeconds));
      monitor->report(/*first=*/false);
    }
  }).detach();
}

}  // namespace prosperobrowser::frontend
