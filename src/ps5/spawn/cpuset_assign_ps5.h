// SPDX-License-Identifier: GPL-3.0-or-later
//
// PS5-target syscall glue for the child cpuset policy (src/ps5/cpu:
// cpuset_assign.h). The spawn broker calls this after a child has started to
// move it off the system cpuset (9,11,13-15) and onto the UI's app cores.
#ifndef PROSPEROWEB_SPAWN_CPUSET_ASSIGN_PS5_H
#define PROSPEROWEB_SPAWN_CPUSET_ASSIGN_PS5_H

#ifdef __cplusplus
extern "C" {
#endif

// Move child_pid onto the UI's app cores. ui_pid is the UI process whose
// cpuset/mask the child should join (<= 0: unknown; the child is then only
// widened to the root set). The broker has kernel read/write, so it raises
// its own credentials for the call and restores them afterwards. Best-effort
// and non-fatal: every attempt is logged with its errno, failures included.
void ps5_cpuset_assign_child(int child_pid, int ui_pid);

#ifdef __cplusplus
}
#endif

#endif  // PROSPEROWEB_SPAWN_CPUSET_ASSIGN_PS5_H
