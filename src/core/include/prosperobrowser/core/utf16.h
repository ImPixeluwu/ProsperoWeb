// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace prosperobrowser::core {

// UTF-8 -> UTF-16 (code points above U+FFFF become surrogate pairs).
// Malformed or truncated sequences become U+FFFD.
std::u16string utf8_to_utf16(std::string_view utf8);

// UTF-16 -> UTF-8. Stops at the first NUL unit; unpaired surrogates become
// U+FFFD.
std::string utf16_to_utf8(std::u16string_view utf16);

// Truncates s to at most max_units UTF-16 units without splitting a
// surrogate pair.
std::u16string utf16_truncate(std::u16string s, std::size_t max_units);

}  // namespace prosperobrowser::core
