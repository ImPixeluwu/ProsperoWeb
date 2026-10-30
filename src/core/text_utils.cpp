// SPDX-License-Identifier: GPL-3.0-or-later
#include "text_utils.h"

namespace ps5browser::core::detail {

namespace {

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

char lower_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_space(s.front())) {
    s.remove_prefix(1);
  }
  while (!s.empty() && is_space(s.back())) {
    s.remove_suffix(1);
  }
  return s;
}

std::string to_lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = lower_ascii(c);
  }
  return out;
}

std::string fold_case(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
      out.push_back(lower_ascii(static_cast<char>(b0)));
      continue;
    }
    if ((b0 == 0xD0 || b0 == 0xD1) && i + 1 < s.size()) {
      const auto b1 = static_cast<unsigned char>(s[i + 1]);
      unsigned cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
      if (cp >= 0x0410 && cp <= 0x042F) {
        cp += 0x20;  // А..Я -> а..я
      } else if (cp >= 0x0400 && cp <= 0x040F) {
        cp += 0x50;  // Ѐ..Џ (incl. Ё) -> ѐ..џ
      }
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      ++i;
      continue;
    }
    out.push_back(static_cast<char>(b0));
  }
  return out;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (lower_ascii(s[i]) != lower_ascii(prefix[i])) {
      return false;
    }
  }
  return true;
}

void pop_utf8(std::string& s) {
  while (!s.empty()) {
    const auto c = static_cast<unsigned char>(s.back());
    s.pop_back();
    if ((c & 0xC0) != 0x80) {
      break;  // removed the lead byte (or an ASCII byte)
    }
  }
}

}  // namespace ps5browser::core::detail
