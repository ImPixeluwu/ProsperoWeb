// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU allocation diagnostics: is the process (the WebProcess decoding video
// in particular) starved of cores? At start and every 30 s it logs the CPU
// affinity of the process and of its threads against the system's root set,
// the live video decoders and their threads (from the media engine), the
// process's user/system time over the interval and the busiest threads.
//
// When the process may run on fewer CPUs than the root set offers, start()
// tries once to widen it to the whole root set (cpuset_setaffinity, the
// FreeBSD API the payload SDK exposes; there is no sceKernelSetCpumask
// stub) and logs the result; failure is harmless.
// PROSPEROBROWSER_CPU_WIDEN=0 skips that, PROSPEROBROWSER_CPU_MONITOR=0 disables all.
#pragma once

namespace prosperobrowser::frontend {

// Logs the first report (and widens) synchronously, then reports from a
// detached thread every 30 s for the life of the process.
void start_cpu_monitor();

}  // namespace prosperobrowser::frontend
