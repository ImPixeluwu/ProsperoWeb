// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "prosperobrowser/core/on_screen_keyboard.h"

using Osk = prosperobrowser::core::OnScreenKeyboard;

namespace {

// Focuses the key producing ch and presses it.
Osk::PressResult tap(Osk& osk, std::string_view ch) {
  const auto pos = osk.find_key(ch);
  REQUIRE(pos.has_value());
  REQUIRE(osk.set_cursor(*pos));
  return osk.press();
}

Osk::PressResult tap_special(Osk& osk, Osk::KeyKind kind) {
  const auto& rows = osk.keys();
  for (std::size_t r = 0; r < rows.size(); ++r) {
    for (std::size_t c = 0; c < rows[r].size(); ++c) {
      if (rows[r][c].kind == kind) {
        osk.set_cursor({r, c});
        return osk.press();
      }
    }
  }
  FAIL("special key not found");
  return Osk::PressResult::None;
}

}  // namespace

TEST_SUITE("osk") {
  TEST_CASE("layouts are populated") {
    Osk osk;
    CHECK(osk.layout() == Osk::Layout::Latin);
    CHECK(osk.keys().size() == 5);
    CHECK(osk.find_key("q").has_value());
    osk.set_layout(Osk::Layout::Symbols);
    CHECK(osk.find_key(":").has_value());
    CHECK_FALSE(osk.find_key("q").has_value());
    osk.set_layout(Osk::Layout::Cyrillic);
    CHECK(osk.find_key("й").has_value());
    CHECK(osk.find_key("Ё").has_value());
  }

  TEST_CASE("typing Hello with one-shot shift") {
    Osk osk;
    osk.set_text("");
    CHECK(tap_special(osk, Osk::KeyKind::Shift) == Osk::PressResult::ShiftChanged);
    CHECK(osk.shift());
    CHECK(osk.label(osk.keys()[1][0]) == "Q");
    CHECK(tap(osk, "h") == Osk::PressResult::Inserted);
    CHECK_FALSE(osk.shift());  // one-shot
    tap(osk, "e");
    tap(osk, "l");
    tap(osk, "l");
    tap(osk, "o");
    CHECK(osk.text() == "Hello");
    CHECK(tap_special(osk, Osk::KeyKind::Space) == Osk::PressResult::Inserted);
    CHECK(osk.text() == "Hello ");
  }

  TEST_CASE("backspace removes whole UTF-8 code points") {
    Osk osk;
    osk.set_text("ab");
    CHECK(tap_special(osk, Osk::KeyKind::Backspace) == Osk::PressResult::Deleted);
    CHECK(osk.text() == "a");
    osk.set_layout(Osk::Layout::Cyrillic);
    tap(osk, "ж");
    CHECK(osk.text() == "aж");
    CHECK(osk.backspace() == Osk::PressResult::Deleted);
    CHECK(osk.text() == "a");
    osk.backspace();
    CHECK(osk.backspace() == Osk::PressResult::None);
    CHECK(osk.text().empty());
  }

  TEST_CASE("layout switch cycles and resets shift") {
    Osk osk;
    osk.toggle_shift();
    CHECK(tap_special(osk, Osk::KeyKind::LayoutSwitch) == Osk::PressResult::LayoutChanged);
    CHECK(osk.layout() == Osk::Layout::Symbols);
    CHECK_FALSE(osk.shift());
    osk.next_layout();
    CHECK(osk.layout() == Osk::Layout::Cyrillic);
    osk.toggle_shift();
    tap(osk, "п");
    CHECK(osk.text() == "П");
    osk.next_layout();
    CHECK(osk.layout() == Osk::Layout::Latin);
  }

  TEST_CASE("cursor movement wraps and clamps") {
    Osk osk;
    CHECK(osk.cursor() == Osk::Cursor{0, 0});
    osk.move(Osk::Direction::Left);
    CHECK(osk.cursor() == Osk::Cursor{0, 9});
    osk.move(Osk::Direction::Up);  // wraps to the special row (8 keys)
    CHECK(osk.cursor() == Osk::Cursor{4, 7});
    CHECK(osk.current_key().kind == Osk::KeyKind::Enter);
    osk.move(Osk::Direction::Down);
    CHECK(osk.cursor() == Osk::Cursor{0, 7});
    osk.move(Osk::Direction::Right);
    CHECK(osk.current_key().value == "9");
    CHECK_FALSE(osk.set_cursor({9, 0}));
  }

  TEST_CASE("commit and cancel") {
    Osk osk;
    osk.set_text("example.com");
    tap(osk, "x");
    CHECK(tap_special(osk, Osk::KeyKind::Enter) == Osk::PressResult::Committed);
    CHECK(osk.text() == "example.comx");
    osk.set_text("keep");
    tap(osk, "z");
    CHECK(osk.text() == "keepz");
    osk.cancel();
    CHECK(osk.text() == "keep");
  }

  TEST_CASE("set_text puts the caret at the end") {
    Osk osk;
    osk.set_text("example.com");
    CHECK(osk.caret() == 11);
    CHECK(osk.caret_byte() == 11);
    osk.set_text("");
    CHECK(osk.caret() == 0);
    osk.set_text("привет");
    CHECK(osk.caret() == 6);
    CHECK(osk.caret_byte() == 12);
  }

  TEST_CASE("typing and deleting in the middle of the text") {
    Osk osk;
    osk.set_text("exmple.com");
    CHECK(osk.move_caret(-8));
    CHECK(osk.caret() == 2);
    CHECK(tap(osk, "a") == Osk::PressResult::Inserted);
    CHECK(osk.text() == "example.com");
    CHECK(osk.caret() == 3);
    CHECK(osk.backspace() == Osk::PressResult::Deleted);
    CHECK(osk.text() == "exmple.com");
    CHECK(osk.caret() == 2);
    osk.space();
    CHECK(osk.text() == "ex mple.com");
    CHECK(osk.caret() == 3);
  }

  TEST_CASE("caret keys, home and end clamp to the text") {
    Osk osk;
    osk.set_text("ab");
    CHECK(osk.caret_right() == Osk::PressResult::None);  // already at the end
    CHECK(tap_special(osk, Osk::KeyKind::CaretLeft) == Osk::PressResult::CaretMoved);
    CHECK(osk.caret() == 1);
    CHECK(tap_special(osk, Osk::KeyKind::CaretLeft) == Osk::PressResult::CaretMoved);
    CHECK(osk.caret_left() == Osk::PressResult::None);
    CHECK(osk.caret() == 0);
    CHECK(osk.backspace() == Osk::PressResult::None);  // nothing before the caret
    CHECK(osk.text() == "ab");
    CHECK(tap_special(osk, Osk::KeyKind::CaretRight) == Osk::PressResult::CaretMoved);
    CHECK(osk.caret() == 1);
    CHECK_FALSE(osk.move_caret(0));
    osk.caret_end();
    CHECK(osk.caret() == 2);
    osk.caret_home();
    CHECK(osk.caret() == 0);
    CHECK_FALSE(osk.move_caret(-5));
    CHECK(osk.move_caret(99));
    CHECK(osk.caret() == 2);
  }

  TEST_CASE("the caret moves by UTF-8 code points in Cyrillic text") {
    Osk osk;
    osk.set_text("мир");
    osk.move_caret(-1);
    CHECK(osk.caret() == 2);
    CHECK(osk.caret_byte() == 4);  // two 2-byte letters before the caret
    osk.set_layout(Osk::Layout::Cyrillic);
    tap(osk, "е");
    CHECK(osk.text() == "миер");
    CHECK(osk.caret() == 3);
    osk.move_caret(-2);
    CHECK(osk.backspace() == Osk::PressResult::Deleted);
    CHECK(osk.text() == "иер");
    CHECK(osk.caret() == 0);
    osk.move_caret(1);
    CHECK(osk.caret_byte() == 2);
    CHECK(tap(osk, "1") == Osk::PressResult::Inserted);
    CHECK(osk.text() == "и1ер");
  }

  TEST_CASE("Clear empties the text and keeps editing possible") {
    Osk osk;
    osk.set_text("https://example.com/");
    osk.move_caret(-3);
    CHECK(tap_special(osk, Osk::KeyKind::Clear) == Osk::PressResult::Cleared);
    CHECK(osk.text().empty());
    CHECK(osk.caret() == 0);
    CHECK(osk.clear() == Osk::PressResult::None);
    tap(osk, "q");
    CHECK(osk.text() == "q");
    osk.cancel();  // restores the original text, caret at its end
    CHECK(osk.text() == "https://example.com/");
    CHECK(osk.caret() == 20);
  }

  TEST_CASE("special row labels and over-long text is cut at a code point") {
    Osk osk;
    const auto& special = osk.keys().back();
    REQUIRE(special.size() == 8);
    CHECK(osk.label(special[2]) == "Clear");
    CHECK(osk.label(special[3]) == "←");
    CHECK(osk.label(special[5]) == "→");
    std::string long_text(Osk::kMaxTextBytes - 1, 'a');
    long_text += "жж";  // the first "ж" straddles the byte limit
    osk.set_text(long_text);
    CHECK(osk.text().size() == Osk::kMaxTextBytes - 1);
    CHECK(osk.caret_byte() == osk.text().size());
  }
}
