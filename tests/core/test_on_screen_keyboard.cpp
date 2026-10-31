// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <string_view>

#include "ps5browser/core/on_screen_keyboard.h"

using Osk = ps5browser::core::OnScreenKeyboard;

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
}
