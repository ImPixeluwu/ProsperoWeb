// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpuset_assign.h"

#include <cstring>

namespace prosperoweb::cpu {

ChildCpuMode parse_child_cpu_mode(const char* env) {
  if (!env || !*env) {
    return ChildCpuMode::Auto;
  }
  if (!std::strcmp(env, "off") || !std::strcmp(env, "0")) {
    return ChildCpuMode::Off;
  }
  if (!std::strcmp(env, "setid")) {
    return ChildCpuMode::SetId;
  }
  if (!std::strcmp(env, "mask")) {
    return ChildCpuMode::Mask;
  }
  return ChildCpuMode::Auto;
}

const char* child_cpu_mode_name(ChildCpuMode mode) {
  switch (mode) {
    case ChildCpuMode::Auto:
      return "auto";
    case ChildCpuMode::SetId:
      return "setid";
    case ChildCpuMode::Mask:
      return "mask";
    case ChildCpuMode::Off:
      return "off";
  }
  return "auto";
}

ChildCpuPlan plan_child_cpuset(ChildCpuMode mode, bool have_ui_setid, const CpuMask& child,
                               const CpuMask* ui, const CpuMask* root) {
  ChildCpuPlan plan;
  if (mode == ChildCpuMode::Off) {
    plan.reason = "disabled (PROSPEROWEB_CHILD_CPU=off)";
    return plan;
  }

  // The UI's set clipped to root is the target mask; without a UI mask, the
  // whole root set. plan_widen also tells us whether that differs from the
  // child's current mask.
  const WidenPlan widen = plan_widen(child, root, ui);
  plan.mask_target = widen.target;
  plan.mask_change = widen.change;

  plan.try_setid = have_ui_setid && (mode == ChildCpuMode::Auto || mode == ChildCpuMode::SetId);
  plan.try_mask = (mode == ChildCpuMode::Auto || mode == ChildCpuMode::Mask) && widen.change &&
                  !widen.target.empty();

  if (!plan.try_setid && !plan.try_mask) {
    std::string why = widen.reason;
    if ((mode == ChildCpuMode::Auto || mode == ChildCpuMode::SetId) && !have_ui_setid) {
      why += ", no ui setid";
    }
    plan.reason = "nothing to do (" + why + ")";
    return plan;
  }

  std::string parts;
  if (plan.try_setid) {
    parts = "join ui set";
  }
  if (plan.try_mask) {
    if (!parts.empty()) {
      parts += ", ";
    }
    parts += "widen to " + to_hex(plan.mask_target) + " (" + widen.reason + ")";
  }
  plan.reason = parts;
  return plan;
}

namespace {

std::string mask_text(int error, const CpuMask& mask) {
  if (error) {
    return "error " + errno_name(error);
  }
  return to_list(mask) + " (" + to_hex(mask) + ")";
}

}  // namespace

std::string format_child_cpu_log(long child_pid, long ui_pid, long ui_setid,
                                  const AttemptLog& setid, const AttemptLog& mask,
                                  const CpuMask& mask_target, const CpuMask& before,
                                  int before_error, const CpuMask& after, int after_error,
                                  const std::string& reason) {
  std::string out = "cpu: child pid " + std::to_string(child_pid);
  if (ui_pid >= 0) {
    out += " ui pid " + std::to_string(ui_pid);
  }

  out += " -> cpuset id ";
  if (setid.attempted) {
    out += (ui_setid >= 0 ? std::to_string(ui_setid) : std::string("?")) + ": " +
           errno_name(setid.error);
  } else {
    out += "skipped";
  }

  out += "; mask " + to_hex(mask_target) + ": ";
  out += mask.attempted ? errno_name(mask.error) : std::string("skipped");

  out += " | before " + mask_text(before_error, before);
  out += ", after " + mask_text(after_error, after);
  if (!reason.empty()) {
    out += " | " + reason;
  }
  return out;
}

}  // namespace prosperoweb::cpu
