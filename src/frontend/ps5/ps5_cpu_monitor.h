// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU allocation diagnostics: is the process (the WebProcess decoding video
// in particular) starved of cores? At start and every 30 s it logs the CPU
// affinity of the process and of its threads against the system's root set,
// the live video decoders and their threads (from the media engine), the
// process's user/system time over the interval and the busiest threads.
// The thread list comes from sysctl kern.proc when the kernel has it, else
// from the threads created through pthread_create (ps5_cpu_affinity.h). The
// masks themselves are widened by tune_process_cpus(), not here.
// PROSPEROWEB_CPU_MONITOR=0 disables it.
#pragma once

namespace prosperoweb::frontend {

// Logs the first report (and widens) synchronously, then reports from a
// detached thread every 30 s for the life of the process.
void start_cpu_monitor();

}  // namespace prosperoweb::frontend
