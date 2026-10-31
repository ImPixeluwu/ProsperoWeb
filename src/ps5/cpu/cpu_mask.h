// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU masks for cpuset_getaffinity/cpuset_setaffinity on the PS5.
//
// The payload SDK's cpuset_t comes from FreeBSD 11 headers (CPU_SETSIZE 256,
// 32 bytes), and the console's kernel answers ERANGE to that size: it wants
// its own, smaller cpuset size. Masks are therefore raw byte arrays (bit n of
// byte n/8 is CPU n, the kernel's layout on little-endian) used with the size
// probed at run time.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace prosperobrowser::cpu {

constexpr std::size_t kMaxMaskBytes = 128;  // 1024 CPUs, FreeBSD's largest CPU_MAXSIZE

// Sizes tried in order. FreeBSD rejects a buffer smaller than the kernel's
// cpuset with ERANGE, so the first size that works is the kernel's own.
inline constexpr std::size_t kProbeSizes[] = {8, 16, 32, 64, 128};

struct CpuMask {
  alignas(8) std::array<std::uint8_t, kMaxMaskBytes> bytes{};  // passed to the kernel as is

  void set(int cpu);
  bool test(int cpu) const;
  int count() const;
  bool empty() const { return count() == 0; }
  bool subset_of(const CpuMask& other) const;
  CpuMask operator&(const CpuMask& other) const;
  bool operator==(const CpuMask& other) const = default;
};

// "0-5,8", or "none" for an empty mask.
std::string to_list(const CpuMask& mask);
// "0x1fff"; "0x0" for an empty mask.
std::string to_hex(const CpuMask& mask);
// Parses a hex mask with an optional 0x prefix ("0x1fff", "FFFF"). False
// for an empty, non-hex or too long string; the mask is unchanged then.
bool parse_hex(std::string_view text, CpuMask& mask);

// "ERANGE", "EINVAL", ... for the errors the probe meets, else the number.
std::string errno_name(int error);

// Reads a mask of `size` bytes into `buffer`; returns 0 or an errno value.
using AffinityGetter = std::function<int(std::size_t size, void* buffer)>;

struct ProbeResult {
  std::size_t size = 0;  // 0: no size worked
  int error = 0;         // the last error when size is 0
  std::string tried;     // "8:ERANGE 16:ok"
  CpuMask mask;          // what the working size read
};

// Tries kProbeSizes in order; stops at the first success, or at the first
// error other than ERANGE (that one says nothing about the size).
ProbeResult probe_size(const AffinityGetter& get);

// What to set the process mask to.
struct WidenPlan {
  bool change = false;
  CpuMask target;
  std::string reason;  // why there is no change, or where the target comes from
};

// `process` is the current mask; `root` the root set (nullptr: unknown);
// `requested` an explicit mask (PROSPEROBROWSER_CPUMASK, nullptr: none).
// An explicit mask is clipped to the root set; without one the target is
// the whole root set. CPUs outside the root set are never added.
WidenPlan plan_widen(const CpuMask& process, const CpuMask* root, const CpuMask* requested);

}  // namespace prosperobrowser::cpu
