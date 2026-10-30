// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps5browser::core {

// Gamepad-driven on-screen keyboard. Holds the key grid, the focused key and
// the text being edited; the frontend only draws keys() and cursor() and
// forwards Osk* actions. Editing always happens at the end of the text.
class OnScreenKeyboard {
 public:
  enum class Layout : std::uint8_t { Latin, Symbols, Cyrillic };
  enum class Direction : std::uint8_t { Up, Down, Left, Right };
  enum class KeyKind : std::uint8_t { Char, Shift, Space, Backspace, LayoutSwitch, Enter };
  enum class PressResult : std::uint8_t {
    None,
    Inserted,
    Deleted,
    ShiftChanged,
    LayoutChanged,
    Committed,  // Enter pressed; text() holds the final value
  };

  struct Key {
    KeyKind kind = KeyKind::Char;
    std::string value;    // inserted text (UTF-8) for Char keys
    std::string shifted;  // value while Shift is active
  };

  struct Cursor {
    std::size_t row = 0;
    std::size_t col = 0;
    bool operator==(const Cursor&) const = default;
  };

  static constexpr std::size_t kMaxTextBytes = 2048;

  OnScreenKeyboard();

  // Opens an editing session: sets text (also remembered for cancel()),
  // resets shift, keeps the layout and moves the cursor to the first key.
  void set_text(std::string initial);
  const std::string& text() const { return text_; }

  Layout layout() const { return layout_; }
  void set_layout(Layout layout);
  bool shift() const { return shift_; }

  const std::vector<std::vector<Key>>& keys() const;
  Cursor cursor() const { return cursor_; }
  const Key& current_key() const;
  // Label to draw for a key, reflecting Shift and the next layout name.
  std::string label(const Key& key) const;

  // Moves focus; wraps around horizontally and vertically. On vertical moves
  // the column is clamped to the target row's length.
  void move(Direction dir);
  bool set_cursor(Cursor c);
  // Finds a Char key by its unshifted or shifted value in the current layout.
  std::optional<Cursor> find_key(std::string_view value) const;

  // Activates the focused key. Shift is one-shot: it is cleared after a
  // character is inserted.
  PressResult press();
  // Direct actions (bound to gamepad shortcuts).
  PressResult backspace();
  PressResult space();
  PressResult toggle_shift();
  PressResult next_layout();
  // Restores the text passed to the last set_text().
  void cancel();

 private:
  PressResult insert(std::string_view s);
  void clamp_cursor();

  std::vector<std::vector<Key>> layouts_[3];
  Layout layout_ = Layout::Latin;
  bool shift_ = false;
  Cursor cursor_;
  std::string text_;
  std::string initial_;
};

}  // namespace ps5browser::core
