// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU mask helpers (src/ps5/cpu): list and hex forms, hex parsing, the
// cpuset size probe against a fake kernel, and the widening policy.
#include <cerrno>
#include <cstring>
#include <vector>

#include <doctest/doctest.h>

#include "cpu_mask.h"

using namespace prosperobrowser::cpu;

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

// A FreeBSD-like kernel: ERANGE below its own cpuset size and above its
// largest one, otherwise copies `mask` into the caller's buffer.
struct FakeKernel {
  std::size_t kernel_size;
  std::size_t max_size;
  CpuMask mask;
  std::vector<std::size_t> calls;

  int operator()(std::size_t size, void* buffer) {
    calls.push_back(size);
    if (size < kernel_size || size > max_size) {
      return ERANGE;
    }
    std::memcpy(buffer, mask.bytes.data(), size);
    return 0;
  }
};

}  // namespace

TEST_SUITE("cpu_mask") {
  TEST_CASE("list form") {
    CHECK(to_list(CpuMask{}) == "none");
    CHECK(to_list(mask_of({0})) == "0");
    CHECK(to_list(range(0, 12)) == "0-12");
    CHECK(to_list(mask_of({0, 1, 2, 5, 8, 9})) == "0-2,5,8-9");
    CHECK(to_list(mask_of({15, 64, 1023})) == "15,64,1023");
  }

  TEST_CASE("hex form") {
    CHECK(to_hex(CpuMask{}) == "0x0");
    CHECK(to_hex(range(0, 12)) == "0x1fff");
    CHECK(to_hex(range(0, 15)) == "0xffff");
    CHECK(to_hex(mask_of({4})) == "0x10");
    CHECK(to_hex(mask_of({64})) == "0x10000000000000000");
  }

  TEST_CASE("hex parsing") {
    CpuMask m;
    REQUIRE(parse_hex("0x1fff", m));
    CHECK(m == range(0, 12));
    REQUIRE(parse_hex("FFFF", m));
    CHECK(m == range(0, 15));
    REQUIRE(parse_hex("0X3f", m));
    CHECK(m == range(0, 5));
    CHECK(to_hex(m) == "0x3f");
    // Rejected input leaves the mask alone.
    CHECK_FALSE(parse_hex("", m));
    CHECK_FALSE(parse_hex("0x", m));
    CHECK_FALSE(parse_hex("0x1g", m));
    CHECK_FALSE(parse_hex(std::string(kMaxMaskBytes * 2 + 1, 'f'), m));
    CHECK(m == range(0, 5));
    // Round trip of a wide mask.
    const CpuMask wide = mask_of({0, 7, 8, 100, 1000});
    CpuMask back;
    REQUIRE(parse_hex(to_hex(wide), back));
    CHECK(back == wide);
  }

  TEST_CASE("set algebra") {
    CHECK(range(0, 5).count() == 6);
    CHECK(range(0, 5).subset_of(range(0, 12)));
    CHECK_FALSE(range(0, 13).subset_of(range(0, 12)));
    CHECK((range(0, 15) & range(4, 20)) == range(4, 15));
    CHECK(CpuMask{}.empty());
    CHECK_FALSE(mask_of({-1, 5000}).test(-1));
    CHECK(mask_of({-1, 5000}).empty());
  }

  TEST_CASE("size probe finds the kernel's cpuset size") {
    // The console: the SDK's 32-byte cpuset_t is ERANGE.
    FakeKernel kernel{16, 16, range(0, 12), {}};
    const ProbeResult r = probe_size(std::ref(kernel));
    CHECK(r.size == 16);
    CHECK(r.error == 0);
    CHECK(r.tried == "8:ERANGE 16:ok");
    CHECK(r.mask == range(0, 12));
    CHECK(kernel.calls == std::vector<std::size_t>{8, 16});
  }

  TEST_CASE("size probe: smallest size, larger kernel sets") {
    FakeKernel eight{8, 128, range(0, 15), {}};
    CHECK(probe_size(std::ref(eight)).size == 8);
    FakeKernel big{64, 128, range(0, 15), {}};
    const ProbeResult r = probe_size(std::ref(big));
    CHECK(r.size == 64);
    CHECK(r.tried == "8:ERANGE 16:ERANGE 32:ERANGE 64:ok");
  }

  TEST_CASE("size probe gives up") {
    FakeKernel never{256, 256, {}, {}};
    const ProbeResult r = probe_size(std::ref(never));
    CHECK(r.size == 0);
    CHECK(r.error == ERANGE);
    CHECK(never.calls.size() == std::size(kProbeSizes));
    // Another error says nothing about the size: stop there.
    int calls = 0;
    const ProbeResult denied = probe_size([&](std::size_t, void*) {
      calls++;
      return EPERM;
    });
    CHECK(denied.size == 0);
    CHECK(denied.error == EPERM);
    CHECK(denied.tried == "8:EPERM");
    CHECK(calls == 1);
  }

  TEST_CASE("widening to the root set") {
    const CpuMask root = range(0, 12);
    WidenPlan p = plan_widen(mask_of({0}), &root, nullptr);
    CHECK(p.change);
    CHECK(p.target == root);
    CHECK(p.reason == "root set");

    p = plan_widen(root, &root, nullptr);
    CHECK_FALSE(p.change);
    CHECK(p.reason == "already root set");

    p = plan_widen(mask_of({0}), nullptr, nullptr);
    CHECK_FALSE(p.change);
    CHECK(p.reason == "root set unknown");

    const CpuMask empty;
    CHECK_FALSE(plan_widen(mask_of({0}), &empty, nullptr).change);
  }

  TEST_CASE("an explicit mask is clipped to the root set") {
    const CpuMask root = range(0, 12);
    const CpuMask all16 = range(0, 15);
    WidenPlan p = plan_widen(range(0, 5), &root, &all16);
    CHECK(p.change);
    CHECK(p.target == root);
    CHECK(p.reason == "requested");

    const CpuMask high = range(13, 15);
    p = plan_widen(range(0, 5), &root, &high);
    CHECK_FALSE(p.change);
    CHECK(p.reason == "requested mask has no cpu in the root set");

    // Without a root set the request is taken as is (and may narrow).
    const CpuMask two = mask_of({2, 3});
    p = plan_widen(range(0, 5), nullptr, &two);
    CHECK(p.change);
    CHECK(p.target == two);
  }
}
