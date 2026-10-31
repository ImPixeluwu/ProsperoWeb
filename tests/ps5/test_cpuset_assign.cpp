// SPDX-License-Identifier: GPL-3.0-or-later
//
// Policy for moving broker-spawned children off the console's system cpuset
// and onto the UI's app cores (src/ps5/cpu/cpuset_assign): the mode parser,
// the per-child plan (which mechanism, which mask) and the log formatter.
#include <string>

#include <doctest/doctest.h>

#include "cpu_mask.h"
#include "cpuset_assign.h"

using namespace prosperoweb::cpu;

namespace {

CpuMask mask_of(std::initializer_list<int> cpus) {
  CpuMask m;
  for (int cpu : cpus) {
    m.set(cpu);
  }
  return m;
}

CpuMask range(int first, int last) {
  CpuMask m;
  for (int cpu = first; cpu <= last; cpu++) {
    m.set(cpu);
  }
  return m;
}

// The console's layout (klog run 11): children land in the system set, the UI
// owns the app cores, the root set is everything.
const CpuMask kSystem = mask_of({9, 11, 13, 14, 15});  // 0xea00
const CpuMask kApp = range(0, 12);                     // 0x1fff
const CpuMask kRoot = range(0, 15);                    // 0xffff

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_SUITE("cpuset_assign") {
  TEST_CASE("mode parsing") {
    CHECK(parse_child_cpu_mode(nullptr) == ChildCpuMode::Auto);
    CHECK(parse_child_cpu_mode("") == ChildCpuMode::Auto);
    CHECK(parse_child_cpu_mode("off") == ChildCpuMode::Off);
    CHECK(parse_child_cpu_mode("0") == ChildCpuMode::Off);
    CHECK(parse_child_cpu_mode("setid") == ChildCpuMode::SetId);
    CHECK(parse_child_cpu_mode("mask") == ChildCpuMode::Mask);
    CHECK(parse_child_cpu_mode("whatever") == ChildCpuMode::Auto);
    CHECK(std::string(child_cpu_mode_name(ChildCpuMode::Auto)) == "auto");
    CHECK(std::string(child_cpu_mode_name(ChildCpuMode::SetId)) == "setid");
    CHECK(std::string(child_cpu_mode_name(ChildCpuMode::Mask)) == "mask");
    CHECK(std::string(child_cpu_mode_name(ChildCpuMode::Off)) == "off");
  }

  TEST_CASE("auto: setid and mask when both available") {
    const ChildCpuPlan plan = plan_child_cpuset(ChildCpuMode::Auto, true, kSystem, &kApp, &kRoot);
    CHECK(plan.try_setid);
    CHECK(plan.try_mask);
    CHECK(plan.mask_change);
    CHECK(plan.mask_target == kApp);
    CHECK(contains(plan.reason, "join ui set"));
    CHECK(contains(plan.reason, "widen to 0x1fff"));
  }

  TEST_CASE("auto: no ui setid falls back to the mask") {
    const ChildCpuPlan plan = plan_child_cpuset(ChildCpuMode::Auto, false, kSystem, &kApp, &kRoot);
    CHECK_FALSE(plan.try_setid);
    CHECK(plan.try_mask);
    CHECK(plan.mask_target == kApp);
    CHECK(contains(plan.reason, "widen to 0x1fff"));
  }

  TEST_CASE("auto: no ui mask widens to the root set") {
    const ChildCpuPlan plan =
        plan_child_cpuset(ChildCpuMode::Auto, false, kSystem, nullptr, &kRoot);
    CHECK_FALSE(plan.try_setid);
    CHECK(plan.try_mask);
    CHECK(plan.mask_target == kRoot);
    CHECK(contains(plan.reason, "widen to 0xffff"));
    CHECK(contains(plan.reason, "root set"));
  }

  TEST_CASE("ui mask is clipped to the root set") {
    // UI claims 0-15 but the root set only has 0-12: never add cpus 13-15.
    const CpuMask wide_ui = range(0, 15);
    const CpuMask small_root = range(0, 12);
    const ChildCpuPlan plan =
        plan_child_cpuset(ChildCpuMode::Auto, false, kSystem, &wide_ui, &small_root);
    CHECK(plan.mask_target == range(0, 12));
    CHECK(plan.mask_change);
  }

  TEST_CASE("no root set, ui mask used verbatim") {
    const ChildCpuPlan plan =
        plan_child_cpuset(ChildCpuMode::Auto, false, kSystem, &kApp, nullptr);
    CHECK(plan.try_mask);
    CHECK(plan.mask_target == kApp);
  }

  TEST_CASE("auto: child already on the app cores still joins the set") {
    const ChildCpuPlan plan = plan_child_cpuset(ChildCpuMode::Auto, true, kApp, &kApp, &kRoot);
    CHECK(plan.try_setid);
    CHECK_FALSE(plan.try_mask);       // mask already correct
    CHECK_FALSE(plan.mask_change);
    CHECK(contains(plan.reason, "join ui set"));
  }

  TEST_CASE("auto: nothing to do when already placed and no setid") {
    const ChildCpuPlan plan = plan_child_cpuset(ChildCpuMode::Auto, false, kRoot, nullptr, &kRoot);
    CHECK_FALSE(plan.try_setid);
    CHECK_FALSE(plan.try_mask);
    CHECK(contains(plan.reason, "nothing to do"));
  }

  TEST_CASE("auto: unknown root and no ui is a no-op") {
    const ChildCpuPlan plan =
        plan_child_cpuset(ChildCpuMode::Auto, false, kSystem, nullptr, nullptr);
    CHECK_FALSE(plan.try_setid);
    CHECK_FALSE(plan.try_mask);
    CHECK(contains(plan.reason, "nothing to do"));
  }

  TEST_CASE("setid mode never touches the mask") {
    const ChildCpuPlan with = plan_child_cpuset(ChildCpuMode::SetId, true, kSystem, &kApp, &kRoot);
    CHECK(with.try_setid);
    CHECK_FALSE(with.try_mask);

    const ChildCpuPlan without =
        plan_child_cpuset(ChildCpuMode::SetId, false, kSystem, &kApp, &kRoot);
    CHECK_FALSE(without.try_setid);
    CHECK_FALSE(without.try_mask);
    CHECK(contains(without.reason, "nothing to do"));
    CHECK(contains(without.reason, "no ui setid"));
  }

  TEST_CASE("mask mode never joins the set") {
    const ChildCpuPlan plan = plan_child_cpuset(ChildCpuMode::Mask, true, kSystem, &kApp, &kRoot);
    CHECK_FALSE(plan.try_setid);
    CHECK(plan.try_mask);
    CHECK(plan.mask_target == kApp);
  }

  TEST_CASE("mask mode with no change is a no-op") {
    const ChildCpuPlan plan = plan_child_cpuset(ChildCpuMode::Mask, true, kApp, &kApp, &kRoot);
    CHECK_FALSE(plan.try_mask);
    CHECK(contains(plan.reason, "nothing to do"));
  }

  TEST_CASE("off mode disables everything") {
    const ChildCpuPlan plan = plan_child_cpuset(ChildCpuMode::Off, true, kSystem, &kApp, &kRoot);
    CHECK_FALSE(plan.try_setid);
    CHECK_FALSE(plan.try_mask);
    CHECK(contains(plan.reason, "disabled"));
  }

  TEST_CASE("log: both attempts, setid ok") {
    AttemptLog setid{true, 0};
    AttemptLog mask{false, 0};
    const std::string line = format_child_cpu_log(4321, 1000, 3, setid, mask, kApp, kSystem, 0,
                                                   kApp, 0, "join ui set");
    CHECK(contains(line, "cpu: child pid 4321"));
    CHECK(contains(line, "ui pid 1000"));
    CHECK(contains(line, "-> cpuset id 3: ok"));
    CHECK(contains(line, "mask 0x1fff: skipped"));
    CHECK(contains(line, "before 9,11,13-15 (0xea00)"));
    CHECK(contains(line, "after 0-12 (0x1fff)"));
    CHECK(contains(line, "| join ui set"));
  }

  TEST_CASE("log: setid EPERM then mask ok") {
    AttemptLog setid{true, EPERM};
    AttemptLog mask{true, 0};
    const std::string line = format_child_cpu_log(77, 1000, 3, setid, mask, kApp, kSystem, 0, kApp,
                                                   0, "join ui set, widen to 0x1fff (root set)");
    CHECK(contains(line, "cpuset id 3: EPERM"));
    CHECK(contains(line, "mask 0x1fff: ok"));
  }

  TEST_CASE("log: setid skipped, mask EPERM, after unreadable") {
    AttemptLog setid{false, 0};
    AttemptLog mask{true, EPERM};
    const std::string line =
        format_child_cpu_log(77, -1, -1, setid, mask, kApp, kSystem, 0, CpuMask{}, EPERM, "");
    CHECK(contains(line, "-> cpuset id skipped"));
    CHECK(contains(line, "mask 0x1fff: EPERM"));
    CHECK(contains(line, "after error EPERM"));
    CHECK_FALSE(contains(line, "ui pid"));  // ui_pid < 0 omitted
  }
}
