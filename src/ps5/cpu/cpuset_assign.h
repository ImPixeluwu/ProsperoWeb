// SPDX-License-Identifier: GPL-3.0-or-later
//
// Policy for moving a broker-spawned child (WebProcess, NetworkProcess) off
// the console's system cpuset (9,11,13-15) and onto the UI's app cores
// (0-12). The spawn broker has kernel read/write, so it raises its own
// credentials and then, as a privileged caller, either joins the child to the
// UI's cpuset (cpuset_setid) or widens the child's mask (cpuset_setaffinity);
// the unprivileged child cannot do this itself (cpuset_setaffinity returns
// EPERM at the kernel's priv_check(PRIV_SCHED_CPUSET)).
//
// This header holds the portable decision logic only (which mechanism to try,
// what mask to widen to, how to describe the outcome); the syscalls live in
// src/ps5/spawn/cpuset_assign_ps5.cpp (PS5 target only). Keeping the policy
// here lets the host tests cover it.
//
// Env PROSPEROWEB_CHILD_CPU selects the mechanism: "off"/"0" leaves the
// child alone, "setid" joins the UI set only, "mask" widens the mask only,
// anything else (the default) tries setid first and falls back to the mask.
#pragma once

#include <string>

#include "cpu_mask.h"

namespace prosperoweb::cpu {

enum class ChildCpuMode { Auto, SetId, Mask, Off };

// Parses PROSPEROWEB_CHILD_CPU (nullptr/empty -> Auto).
ChildCpuMode parse_child_cpu_mode(const char* env);
const char* child_cpu_mode_name(ChildCpuMode mode);

// What the broker should attempt for one child.
struct ChildCpuPlan {
  bool try_setid = false;    // join the UI's cpuset by id
  bool try_mask = false;     // widen the child's mask to mask_target
  CpuMask mask_target;       // the mask to set (meaningful when try_mask)
  bool mask_change = false;  // mask_target differs from the child's mask
  std::string reason;        // human-readable description for the log
};

// mode: the parsed override. have_ui_setid: a UI cpuset id was obtained.
// child: the child's current mask. ui: the UI's set mask (nullptr if unknown).
// root: the root set (nullptr if unknown). The mask target is the UI set
// clipped to root, or the whole root set when the UI mask is unknown; CPUs
// outside the root set are never added.
ChildCpuPlan plan_child_cpuset(ChildCpuMode mode, bool have_ui_setid, const CpuMask& child,
                               const CpuMask* ui, const CpuMask* root);

// The outcome of one syscall attempt: whether it ran and its errno (0 = ok).
struct AttemptLog {
  bool attempted = false;
  int error = 0;
};

// One log line summarising both attempts and the before/after child masks.
// ui_pid/ui_setid < 0 are omitted. Mirrors the "cpu: child pid N -> cpuset id
// X: ok|EPERM" shape the operator asked for, with the mask attempt and the
// effective change on the same line.
std::string format_child_cpu_log(long child_pid, long ui_pid, long ui_setid,
                                  const AttemptLog& setid, const AttemptLog& mask,
                                  const CpuMask& mask_target, const CpuMask& before,
                                  int before_error, const CpuMask& after, int after_error,
                                  const std::string& reason);

}  // namespace prosperoweb::cpu
