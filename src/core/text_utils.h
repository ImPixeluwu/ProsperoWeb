// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace ps5browser::core::detail {

// Strips leading/trailing ASCII whitespace.
std::string_view trim(std::string_view s);

// Lower-cases ASCII letters only; other bytes are copied verbatim.
std::string to_lower_ascii(std::string_view s);

// Lower-cases ASCII and basic Cyrillic (U+0400..U+042F) in a UTF-8 string.
// Good enough for case-insensitive search without locale support.
std::string fold_case(std::string_view s);

bool starts_with_ci(std::string_view s, std::string_view prefix);

// Removes the last UTF-8 code point from s (no-op on empty string).
void pop_utf8(std::string& s);

}  // namespace ps5browser::core::detail
