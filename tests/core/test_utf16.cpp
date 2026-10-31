// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "ps5browser/core/utf16.h"

using namespace ps5browser::core;

TEST_SUITE("utf16") {
  TEST_CASE("ASCII, Cyrillic and astral code points round-trip") {
    const std::string s = "https://ya.ru/?q=\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x98\x80";
    const std::u16string w = utf8_to_utf16(s);
    CHECK(w.size() == 17 + 6 + 1 + 2);
    CHECK(w[17] == u'п');
    CHECK(w[24] == 0xD83D);
    CHECK(w[25] == 0xDE00);
    CHECK(utf16_to_utf8(w) == s);
  }

  TEST_CASE("malformed input becomes U+FFFD") {
    CHECK(utf8_to_utf16("a\xFF" "b") == u"a�b");
    CHECK(utf8_to_utf16("\xD0") == u"�");
    CHECK(utf8_to_utf16("\xC0\x80") == u"�");  // overlong NUL
    const char16_t lone[] = {u'x', 0xD800, u'y', 0};
    CHECK(utf16_to_utf8(lone) == "x\xEF\xBF\xBDy");
  }

  TEST_CASE("conversion stops at NUL and truncation keeps pairs whole") {
    const char16_t buf[] = {u'o', u'k', 0, u'z'};
    CHECK(utf16_to_utf8(std::u16string_view(buf, 4)) == "ok");
    const std::u16string w = u"ab\U0001F600";
    CHECK(utf16_truncate(w, 3) == u"ab");
    CHECK(utf16_truncate(w, 4) == w);
    CHECK(utf16_truncate(w, 1) == u"a");
  }
}
