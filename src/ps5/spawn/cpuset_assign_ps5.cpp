// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpuset_assign_ps5.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <sys/param.h>
#include <sys/cpuset.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "cpu_mask.h"
#include "cpuset_assign.h"
#include "prosperobrowser/spawn/spawn.h"

namespace {

using prosperobrowser::cpu::AttemptLog;
using prosperobrowser::cpu::ChildCpuMode;
using prosperobrowser::cpu::ChildCpuPlan;
using prosperobrowser::cpu::CpuMask;
using prosperobrowser::cpu::ProbeResult;

// The broker already holds kernel read/write (elfldr.c). Raising its own
// credentials to uid 0 with full capabilities makes the kernel's
// priv_check(PRIV_SCHED_CPUSET) pass for the cpuset calls below; the child,
// being unprivileged, cannot do this itself (hence the EPERM it sees). The
// kernel_* helpers go through the read/write primitive, not the caller's
// credentials, so raising our own creds this way is always possible.
struct SavedCreds {
  bool raised = false;
  uid_t uid = 0;
  uint8_t caps[16] = {};
};

SavedCreds raise_self() {
  SavedCreds saved;
  static const uint8_t full[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  const pid_t self = getpid();
  saved.uid = kernel_get_ucred_uid(self);
  if (kernel_get_ucred_caps(self, saved.caps) != 0) {
    return saved;
  }
  if (kernel_set_ucred_uid(self, 0) != 0) {
    return saved;
  }
  if (kernel_set_ucred_caps(self, full) != 0) {
    kernel_set_ucred_uid(self, saved.uid);
    return saved;
  }
  saved.raised = true;
  return saved;
}

void restore_self(const SavedCreds& saved) {
  if (!saved.raised) {
    return;
  }
  const pid_t self = getpid();
  kernel_set_ucred_caps(self, saved.caps);
  kernel_set_ucred_uid(self, saved.uid);
}

int raw_get(int level, int which, id_t id, std::size_t size, CpuMask& mask) {
  mask = {};
  if (cpuset_getaffinity(static_cast<cpulevel_t>(level), static_cast<cpuwhich_t>(which), id, size,
                         reinterpret_cast<cpuset_t*>(mask.bytes.data())) != 0) {
    return errno;
  }
  return 0;
}

}  // namespace

extern "C" void ps5_cpuset_assign_child(int child_pid, int ui_pid) {
  using namespace prosperobrowser::cpu;
  if (child_pid <= 0) {
    return;
  }
  const ChildCpuMode mode = parse_child_cpu_mode(std::getenv("PROSPEROBROWSER_CHILD_CPU"));
  if (mode == ChildCpuMode::Off) {
    ps5_log_printf("cpu: child pid %d left alone (PROSPEROBROWSER_CHILD_CPU=off)", child_pid);
    return;
  }

  const SavedCreds creds = raise_self();

  // Probe the kernel's cpuset size against the child (FreeBSD answers ERANGE
  // below its own size; the console's is 8 bytes).
  const ProbeResult probe = probe_size([&](std::size_t size, void* buffer) {
    if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, static_cast<id_t>(child_pid), size,
                           static_cast<cpuset_t*>(buffer)) != 0) {
      return errno;
    }
    return 0;
  });
  if (!probe.size) {
    ps5_log_printf("cpu: child pid %d: no cpuset size works (tried %s), left alone", child_pid,
                   probe.tried.c_str());
    if (!creds.raised) {
      ps5_log_printf("cpu: child pid %d: could not raise broker credentials", child_pid);
    }
    restore_self(creds);
    return;
  }
  const std::size_t size = probe.size;
  const CpuMask child_before = probe.mask;

  CpuMask root;
  const int root_error = raw_get(CPU_LEVEL_ROOT, CPU_WHICH_PID, static_cast<id_t>(child_pid), size,
                                 root);

  CpuMask ui_mask;
  bool have_ui_mask = false;
  long ui_setid = -1;
  if (ui_pid > 0) {
    have_ui_mask =
        raw_get(CPU_LEVEL_CPUSET, CPU_WHICH_PID, static_cast<id_t>(ui_pid), size, ui_mask) == 0;
    cpusetid_t setid = 0;
    if (cpuset_getid(CPU_LEVEL_CPUSET, CPU_WHICH_PID, static_cast<id_t>(ui_pid), &setid) == 0) {
      ui_setid = setid;
    }
  }

  const ChildCpuPlan plan = plan_child_cpuset(mode, ui_setid >= 0, child_before,
                                              have_ui_mask ? &ui_mask : nullptr,
                                              root_error ? nullptr : &root);

  AttemptLog setid_log;
  AttemptLog mask_log;
  if (plan.try_setid) {
    setid_log.attempted = true;
    if (cpuset_setid(CPU_WHICH_PID, static_cast<id_t>(child_pid),
                     static_cast<cpusetid_t>(ui_setid)) != 0) {
      setid_log.error = errno;
    }
  }
  const bool setid_ok = setid_log.attempted && setid_log.error == 0;
  if (plan.try_mask && !setid_ok) {
    mask_log.attempted = true;
    CpuMask target = plan.mask_target;
    if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, static_cast<id_t>(child_pid), size,
                           reinterpret_cast<const cpuset_t*>(target.bytes.data())) != 0) {
      mask_log.error = errno;
    }
  }

  CpuMask after;
  const int after_error = raw_get(CPU_LEVEL_WHICH, CPU_WHICH_PID, static_cast<id_t>(child_pid), size,
                                  after);

  std::string reason = plan.reason;
  if (!creds.raised) {
    reason += " [broker creds not raised]";
  }
  ps5_log_printf("%s",
                 format_child_cpu_log(child_pid, ui_pid > 0 ? ui_pid : -1, ui_setid, setid_log,
                                       mask_log, plan.mask_target, child_before, 0, after,
                                       after_error, reason)
                     .c_str());

  restore_self(creds);
}
