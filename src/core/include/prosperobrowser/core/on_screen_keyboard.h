// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prosperobrowser::core {

// Gamepad-driven on-screen keyboard. Holds the key grid, the focused key
// (cursor) and the text being edited with a caret; the frontend only draws
// keys(), cursor() and caret() and forwards Osk* actions. Characters are
// inserted at the caret and Backspace deletes the code point before it.
// The caret moves by whole UTF-8 code points.
class OnScreenKeyboard {
 public:
  enum class Layout : std::uint8_t { Latin, Symbols, Cyrillic };
  enum class Direction : std::uint8_t { Up, Down, Left, Right };
  enum class KeyKind : std::uint8_t {
    Char,
    Shift,
    Space,
    Backspace,
    LayoutSwitch,
    Enter,
    Clear,       // empties the text
    CaretLeft,   // moves the caret one code point left
    CaretRight,  // moves the caret one code point right
  };
  enum class PressResult : std::uint8_t {
    None,
    Inserted,
    Deleted,
    ShiftChanged,
    LayoutChanged,
    CaretMoved,
    Cleared,
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
  // puts the caret at its end, resets shift, keeps the layout and moves the
  // cursor to the first key. Text over kMaxTextBytes is cut at a code point.
  void set_text(std::string initial);
  const std::string& text() const { return text_; }

  // Caret position in code points from the start of text() (0..length).
  std::size_t caret() const;
  // Caret position in bytes into text(); always on a code point boundary.
  std::size_t caret_byte() const { return caret_; }
  // Moves the caret by delta code points, clamped to the text. Returns
  // whether it moved.
  bool move_caret(int delta);
  void caret_home();
  void caret_end();

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
  // Empties the text (the caret goes to 0). None if already empty.
  PressResult clear();
  PressResult caret_left();
  PressResult caret_right();
  PressResult toggle_shift();
  PressResult next_layout();
  // Restores the text passed to the last set_text(), caret at its end.
  void cancel();

 private:
  PressResult insert(std::string_view s);
  void clamp_cursor();

  std::vector<std::vector<Key>> layouts_[3];
  Layout layout_ = Layout::Latin;
  bool shift_ = false;
  Cursor cursor_;
  std::string text_;
  std::size_t caret_ = 0;  // byte offset into text_
  std::string initial_;
};

}  // namespace prosperobrowser::core
