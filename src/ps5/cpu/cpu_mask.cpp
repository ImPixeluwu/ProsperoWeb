// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu_mask.h"

#include <bit>
#include <cerrno>
#include <cstring>

namespace prosperoweb::cpu {

namespace {

constexpr int kMaxCpus = static_cast<int>(kMaxMaskBytes * 8);

int hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

void CpuMask::set(int cpu) {
  if (cpu >= 0 && cpu < kMaxCpus) {
    bytes[static_cast<std::size_t>(cpu) / 8] |= static_cast<std::uint8_t>(1u << (cpu % 8));
  }
}

bool CpuMask::test(int cpu) const {
  return cpu >= 0 && cpu < kMaxCpus &&
         (bytes[static_cast<std::size_t>(cpu) / 8] >> (cpu % 8)) & 1u;
}

int CpuMask::count() const {
  int n = 0;
  for (std::uint8_t b : bytes) {
    n += std::popcount(b);
  }
  return n;
}

bool CpuMask::subset_of(const CpuMask& other) const {
  for (std::size_t i = 0; i < bytes.size(); i++) {
    if (bytes[i] & ~other.bytes[i]) {
      return false;
    }
  }
  return true;
}

CpuMask CpuMask::operator&(const CpuMask& other) const {
  CpuMask out;
  for (std::size_t i = 0; i < bytes.size(); i++) {
    out.bytes[i] = bytes[i] & other.bytes[i];
  }
  return out;
}

std::string to_list(const CpuMask& mask) {
  std::string out;
  for (int cpu = 0; cpu < kMaxCpus; cpu++) {
    if (!mask.test(cpu)) {
      continue;
    }
    int last = cpu;
    while (last + 1 < kMaxCpus && mask.test(last + 1)) {
      last++;
    }
    if (!out.empty()) {
      out += ',';
    }
    out += std::to_string(cpu);
    if (last > cpu) {
      out += '-' + std::to_string(last);
    }
    cpu = last;
  }
  return out.empty() ? "none" : out;
}

std::string to_hex(const CpuMask& mask) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = mask.bytes.size(); i-- > 0;) {
    const std::uint8_t b = mask.bytes[i];
    if (out.empty() && b == 0) {
      continue;
    }
    if (out.empty() && b < 0x10) {
      out += kDigits[b];
    } else {
      out += kDigits[b >> 4];
      out += kDigits[b & 0xf];
    }
  }
  return "0x" + (out.empty() ? std::string("0") : out);
}

bool parse_hex(std::string_view text, CpuMask& mask) {
  if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() > kMaxMaskBytes * 2) {
    return false;
  }
  CpuMask parsed;
  int nibble = 0;
  for (std::size_t i = text.size(); i-- > 0; nibble++) {
    const int digit = hex_digit(text[i]);
    if (digit < 0) {
      return false;
    }
    parsed.bytes[static_cast<std::size_t>(nibble) / 2] |=
        static_cast<std::uint8_t>(digit << ((nibble % 2) * 4));
  }
  mask = parsed;
  return true;
}

std::string errno_name(int error) {
  switch (error) {
    case 0: return "ok";
    case ERANGE: return "ERANGE";
    case EINVAL: return "EINVAL";
    case EPERM: return "EPERM";
    case ESRCH: return "ESRCH";
    case EFAULT: return "EFAULT";
    case EDEADLK: return "EDEADLK";
    case ENOSYS: return "ENOSYS";
    default: return "errno " + std::to_string(error);
  }
}

ProbeResult probe_size(const AffinityGetter& get) {
  ProbeResult result;
  for (std::size_t size : kProbeSizes) {
    CpuMask mask;
    const int error = get(size, mask.bytes.data());
    if (!result.tried.empty()) {
      result.tried += ' ';
    }
    result.tried += std::to_string(size) + ':' + errno_name(error);
    if (error == 0) {
      result.size = size;
      result.error = 0;
      result.mask = mask;
      return result;
    }
    result.error = error;
    if (error != ERANGE) {
      break;
    }
  }
  return result;
}

WidenPlan plan_widen(const CpuMask& process, const CpuMask* root, const CpuMask* requested) {
  WidenPlan plan;
  if (requested) {
    plan.target = root ? (*requested & *root) : *requested;
    if (plan.target.empty()) {
      plan.reason = "requested mask has no cpu in the root set";
      return plan;
    }
    plan.reason = "requested";
  } else if (root) {
    if (root->empty()) {
      plan.reason = "root set empty";
      return plan;
    }
    plan.target = *root;
    plan.reason = "root set";
  } else {
    plan.reason = "root set unknown";
    return plan;
  }
  if (plan.target == process) {
    plan.reason = "already " + plan.reason;
    return plan;
  }
  plan.change = true;
  return plan;
}

}  // namespace prosperoweb::cpu
