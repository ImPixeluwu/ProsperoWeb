// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU placement of the PS5 processes (UI, broker, WebProcess,
// NetworkProcess).
//
// cpuset_getaffinity/cpuset_setaffinity with the SDK's cpuset_t (FreeBSD 11,
// 32 bytes) fail with ERANGE on the console: the kernel's cpuset is smaller.
// The size is probed once (src/ps5/cpu) and used for every call.
//
// tune_process_cpus() runs in main() before the role starts any thread: it
// widens the process mask to the root set (or to PROSPEROBROWSER_CPUMASK, hex,
// clipped to the root set) for every thread, so the threads WebKit and
// libavcodec create later inherit it. The executable is linked with
// --wrap=pthread_create/pthread_exit: every new thread is registered (tid,
// CPU clock) for the CPU monitor and re-widened at start if the C library
// handed it a different mask. PROSPEROBROWSER_CPU_WIDEN=0 leaves masks alone.
//
// raise_main_thread_priority() puts the calling thread (the WebProcess main
// thread, which also runs the media presentation tick) above the timeshare
// decoder threads: rtprio_thread realtime at the lowest realtime level, then
// SCHED_RR through pthread_setschedparam. PROSPEROBROWSER_THREAD_PRIO=0 skips it.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <sys/types.h>

#include "cpu_mask.h"

namespace prosperobrowser::frontend {

// The kernel's cpuset size in bytes (probed on first use); 0 when no size
// works. `tried` gets the probe's "8:ERANGE 16:ok" record.
std::size_t kernel_cpuset_size(std::string* tried = nullptr);

// cpuset_getaffinity/cpuset_setaffinity with the probed size; 0 or errno.
int get_cpu_mask(int level, int which, id_t id, cpu::CpuMask& mask);
int set_cpu_mask(int level, int which, id_t id, const cpu::CpuMask& mask);

// Logs and widens the process mask. Call once, before the role's threads.
void tune_process_cpus();

void raise_main_thread_priority();

// Threads created since start through pthread_create.
struct ThreadSample {
  long tid = 0;
  std::string name;
  double cpu_seconds = -1;  // < 0: unknown
  std::string mask;         // cpu list, or "error <name>"
};
struct ThreadCounters {
  unsigned started = 0;
  unsigned widened = 0;        // re-widened at start
  unsigned widen_failed = 0;
};
std::vector<ThreadSample> sample_registered_threads();
ThreadCounters registered_thread_counters();

}  // namespace prosperobrowser::frontend
