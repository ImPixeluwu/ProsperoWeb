// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/utf16.h"

#include <cstdint>

namespace prosperoweb::core {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

void append_utf16(std::u16string& out, char32_t cp) {
  if (cp < 0x10000) {
    out.push_back(static_cast<char16_t>(cp));
  } else {
    cp -= 0x10000;
    out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
    out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
  }
}

void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

std::u16string utf8_to_utf16(std::string_view utf8) {
  std::u16string out;
  out.reserve(utf8.size());
  std::size_t i = 0;
  while (i < utf8.size()) {
    const auto b = static_cast<std::uint8_t>(utf8[i]);
    std::size_t len = 0;
    char32_t cp = 0;
    char32_t min = 0;
    if (b < 0x80) {
      len = 1;
      cp = b;
    } else if ((b & 0xE0) == 0xC0) {
      len = 2;
      cp = b & 0x1F;
      min = 0x80;
    } else if ((b & 0xF0) == 0xE0) {
      len = 3;
      cp = b & 0x0F;
      min = 0x800;
    } else if ((b & 0xF8) == 0xF0) {
      len = 4;
      cp = b & 0x07;
      min = 0x10000;
    } else {
      append_utf16(out, kReplacement);
      ++i;
      continue;
    }
    std::size_t k = 1;
    for (; k < len && i + k < utf8.size(); ++k) {
      const auto c = static_cast<std::uint8_t>(utf8[i + k]);
      if ((c & 0xC0) != 0x80) {
        break;
      }
      cp = (cp << 6) | (c & 0x3F);
    }
    if (k < len) {
      append_utf16(out, kReplacement);  // truncated: resume at the bad byte
      i += k;
      continue;
    }
    i += len;
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      cp = kReplacement;
    }
    append_utf16(out, cp);
  }
  return out;
}

std::string utf16_to_utf8(std::u16string_view utf16) {
  std::string out;
  out.reserve(utf16.size());
  for (std::size_t i = 0; i < utf16.size(); ++i) {
    char32_t cp = utf16[i];
    if (cp == 0) {
      break;
    }
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i + 1 < utf16.size() && utf16[i + 1] >= 0xDC00 && utf16[i + 1] <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (utf16[i + 1] - 0xDC00);
        ++i;
      } else {
        cp = kReplacement;
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = kReplacement;
    }
    append_utf8(out, cp);
  }
  return out;
}

std::u16string utf16_truncate(std::u16string s, std::size_t max_units) {
  if (s.size() <= max_units) {
    return s;
  }
  std::size_t n = max_units;
  if (n > 0 && s[n - 1] >= 0xD800 && s[n - 1] <= 0xDBFF) {
    --n;  // do not keep half of a surrogate pair
  }
  s.resize(n);
  return s;
}

}  // namespace prosperoweb::core
