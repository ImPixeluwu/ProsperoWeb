// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/on_screen_keyboard.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

#include "text_utils.h"

namespace prosperoweb::core {

namespace {

using Key = OnScreenKeyboard::Key;
using KeyKind = OnScreenKeyboard::KeyKind;
using Row = std::vector<Key>;
using Pair = std::pair<const char*, const char*>;

Row char_row(std::initializer_list<Pair> keys) {
  Row row;
  for (const auto& [value, shifted] : keys) {
    row.push_back(Key{KeyKind::Char, value, shifted});
  }
  return row;
}

// Clear sits away from Backspace so a sideways slip does not wipe the text.
Row special_row() {
  return {Key{KeyKind::Shift, "", ""},     Key{KeyKind::LayoutSwitch, "", ""},
          Key{KeyKind::Clear, "", ""},     Key{KeyKind::CaretLeft, "", ""},
          Key{KeyKind::Space, " ", " "},   Key{KeyKind::CaretRight, "", ""},
          Key{KeyKind::Backspace, "", ""}, Key{KeyKind::Enter, "", ""}};
}

std::vector<Row> latin() {
  return {
      char_row({{"1", "!"}, {"2", "@"}, {"3", "#"}, {"4", "$"}, {"5", "%"},
                {"6", "^"}, {"7", "&"}, {"8", "*"}, {"9", "("}, {"0", ")"}}),
      char_row({{"q", "Q"}, {"w", "W"}, {"e", "E"}, {"r", "R"}, {"t", "T"},
                {"y", "Y"}, {"u", "U"}, {"i", "I"}, {"o", "O"}, {"p", "P"}}),
      char_row({{"a", "A"}, {"s", "S"}, {"d", "D"}, {"f", "F"}, {"g", "G"},
                {"h", "H"}, {"j", "J"}, {"k", "K"}, {"l", "L"}, {"-", "_"}}),
      char_row({{"z", "Z"}, {"x", "X"}, {"c", "C"}, {"v", "V"}, {"b", "B"},
                {"n", "N"}, {"m", "M"}, {",", "<"}, {".", ">"}, {"/", "?"}}),
      special_row(),
  };
}

std::vector<Row> symbols() {
  return {
      char_row({{"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"},
                {"6", "6"}, {"7", "7"}, {"8", "8"}, {"9", "9"}, {"0", "0"}}),
      char_row({{"!", "!"}, {"@", "@"}, {"#", "#"}, {"$", "$"}, {"%", "%"},
                {"^", "^"}, {"&", "&"}, {"*", "*"}, {"(", "("}, {")", ")"}}),
      char_row({{"-", "-"}, {"_", "_"}, {"=", "="}, {"+", "+"}, {"[", "["},
                {"]", "]"}, {"{", "{"}, {"}", "}"}, {";", ";"}, {":", ":"}}),
      char_row({{"'", "'"}, {"\"", "\""}, {",", ","}, {".", "."}, {"/", "/"}, {"?", "?"},
                {"\\", "\\"}, {"|", "|"}, {"<", "<"}, {">", ">"}, {"~", "~"}, {"`", "`"}}),
      special_row(),
  };
}

std::vector<Row> cyrillic() {
  return {
      char_row({{"1", "!"}, {"2", "\""}, {"3", "№"}, {"4", ";"}, {"5", "%"},
                {"6", ":"}, {"7", "?"}, {"8", "*"}, {"9", "("}, {"0", ")"}}),
      char_row({{"й", "Й"}, {"ц", "Ц"}, {"у", "У"}, {"к", "К"}, {"е", "Е"}, {"н", "Н"},
                {"г", "Г"}, {"ш", "Ш"}, {"щ", "Щ"}, {"з", "З"}, {"х", "Х"}, {"ъ", "Ъ"}}),
      char_row({{"ф", "Ф"}, {"ы", "Ы"}, {"в", "В"}, {"а", "А"}, {"п", "П"}, {"р", "Р"},
                {"о", "О"}, {"л", "Л"}, {"д", "Д"}, {"ж", "Ж"}, {"э", "Э"}}),
      char_row({{"я", "Я"}, {"ч", "Ч"}, {"с", "С"}, {"м", "М"}, {"и", "И"}, {"т", "Т"},
                {"ь", "Ь"}, {"б", "Б"}, {"ю", "Ю"}, {"ё", "Ё"}, {".", ","}}),
      special_row(),
  };
}

std::size_t index(OnScreenKeyboard::Layout layout) { return static_cast<std::size_t>(layout); }

OnScreenKeyboard::Layout following(OnScreenKeyboard::Layout layout) {
  switch (layout) {
    case OnScreenKeyboard::Layout::Latin: return OnScreenKeyboard::Layout::Symbols;
    case OnScreenKeyboard::Layout::Symbols: return OnScreenKeyboard::Layout::Cyrillic;
    case OnScreenKeyboard::Layout::Cyrillic: return OnScreenKeyboard::Layout::Latin;
  }
  return OnScreenKeyboard::Layout::Latin;
}

const char* layout_name(OnScreenKeyboard::Layout layout) {
  switch (layout) {
    case OnScreenKeyboard::Layout::Latin: return "ABC";
    case OnScreenKeyboard::Layout::Symbols: return "?123";
    case OnScreenKeyboard::Layout::Cyrillic: return "АБВ";
  }
  return "";
}

}  // namespace

OnScreenKeyboard::OnScreenKeyboard()
    : layouts_{latin(), symbols(), cyrillic()} {}

void OnScreenKeyboard::set_text(std::string initial) {
  if (initial.size() > kMaxTextBytes) {
    initial.resize(detail::utf8_prev(initial, kMaxTextBytes + 1));
  }
  text_ = initial;
  caret_ = text_.size();
  initial_ = std::move(initial);
  shift_ = false;
  cursor_ = {};
}

std::size_t OnScreenKeyboard::caret() const {
  return detail::utf8_length(std::string_view(text_).substr(0, caret_));
}

bool OnScreenKeyboard::move_caret(int delta) {
  const std::size_t before = caret_;
  for (; delta < 0; ++delta) {
    caret_ = detail::utf8_prev(text_, caret_);
  }
  for (; delta > 0; --delta) {
    caret_ = detail::utf8_next(text_, caret_);
  }
  return caret_ != before;
}

void OnScreenKeyboard::caret_home() { caret_ = 0; }

void OnScreenKeyboard::caret_end() { caret_ = text_.size(); }

void OnScreenKeyboard::set_layout(Layout layout) {
  layout_ = layout;
  shift_ = false;
  clamp_cursor();
}

const std::vector<std::vector<Key>>& OnScreenKeyboard::keys() const {
  return layouts_[index(layout_)];
}

const Key& OnScreenKeyboard::current_key() const { return keys()[cursor_.row][cursor_.col]; }

std::string OnScreenKeyboard::label(const Key& key) const {
  switch (key.kind) {
    case KeyKind::Char: return shift_ ? key.shifted : key.value;
    case KeyKind::Shift: return "Shift";
    case KeyKind::Space: return "Space";
    case KeyKind::Backspace: return "Backspace";
    case KeyKind::LayoutSwitch: return layout_name(following(layout_));
    case KeyKind::Enter: return "Enter";
    case KeyKind::Clear: return "Clear";
    case KeyKind::CaretLeft: return "\u2190";
    case KeyKind::CaretRight: return "\u2192";
  }
  return {};
}

void OnScreenKeyboard::clamp_cursor() {
  const auto& rows = keys();
  cursor_.row = std::min(cursor_.row, rows.size() - 1);
  cursor_.col = std::min(cursor_.col, rows[cursor_.row].size() - 1);
}

void OnScreenKeyboard::move(Direction dir) {
  const auto& rows = keys();
  const std::size_t nrows = rows.size();
  switch (dir) {
    case Direction::Up:
      cursor_.row = (cursor_.row + nrows - 1) % nrows;
      break;
    case Direction::Down:
      cursor_.row = (cursor_.row + 1) % nrows;
      break;
    case Direction::Left: {
      const std::size_t n = rows[cursor_.row].size();
      cursor_.col = (cursor_.col + n - 1) % n;
      break;
    }
    case Direction::Right: {
      const std::size_t n = rows[cursor_.row].size();
      cursor_.col = (cursor_.col + 1) % n;
      break;
    }
  }
  clamp_cursor();
}

bool OnScreenKeyboard::set_cursor(Cursor c) {
  const auto& rows = keys();
  if (c.row >= rows.size() || c.col >= rows[c.row].size()) {
    return false;
  }
  cursor_ = c;
  return true;
}

std::optional<OnScreenKeyboard::Cursor> OnScreenKeyboard::find_key(std::string_view value) const {
  const auto& rows = keys();
  for (std::size_t r = 0; r < rows.size(); ++r) {
    for (std::size_t c = 0; c < rows[r].size(); ++c) {
      const Key& k = rows[r][c];
      if (k.kind == KeyKind::Char && (k.value == value || k.shifted == value)) {
        return Cursor{r, c};
      }
    }
  }
  return std::nullopt;
}

OnScreenKeyboard::PressResult OnScreenKeyboard::insert(std::string_view s) {
  if (text_.size() + s.size() > kMaxTextBytes) {
    return PressResult::None;
  }
  text_.insert(caret_, s);
  caret_ += s.size();
  return PressResult::Inserted;
}

OnScreenKeyboard::PressResult OnScreenKeyboard::press() {
  const Key& key = current_key();
  switch (key.kind) {
    case KeyKind::Char: {
      const PressResult r = insert(shift_ ? key.shifted : key.value);
      shift_ = false;
      return r;
    }
    case KeyKind::Shift: return toggle_shift();
    case KeyKind::Space: return space();
    case KeyKind::Backspace: return backspace();
    case KeyKind::LayoutSwitch: return next_layout();
    case KeyKind::Enter: return PressResult::Committed;
    case KeyKind::Clear: return clear();
    case KeyKind::CaretLeft: return caret_left();
    case KeyKind::CaretRight: return caret_right();
  }
  return PressResult::None;
}

OnScreenKeyboard::PressResult OnScreenKeyboard::backspace() {
  if (caret_ == 0) {
    return PressResult::None;
  }
  const std::size_t start = detail::utf8_prev(text_, caret_);
  text_.erase(start, caret_ - start);
  caret_ = start;
  return PressResult::Deleted;
}

OnScreenKeyboard::PressResult OnScreenKeyboard::clear() {
  if (text_.empty()) {
    return PressResult::None;
  }
  text_.clear();
  caret_ = 0;
  return PressResult::Cleared;
}

OnScreenKeyboard::PressResult OnScreenKeyboard::caret_left() {
  return move_caret(-1) ? PressResult::CaretMoved : PressResult::None;
}

OnScreenKeyboard::PressResult OnScreenKeyboard::caret_right() {
  return move_caret(1) ? PressResult::CaretMoved : PressResult::None;
}

OnScreenKeyboard::PressResult OnScreenKeyboard::space() { return insert(" "); }

OnScreenKeyboard::PressResult OnScreenKeyboard::toggle_shift() {
  shift_ = !shift_;
  return PressResult::ShiftChanged;
}

OnScreenKeyboard::PressResult OnScreenKeyboard::next_layout() {
  set_layout(following(layout_));
  return PressResult::LayoutChanged;
}

void OnScreenKeyboard::cancel() {
  text_ = initial_;
  caret_ = text_.size();
  shift_ = false;
}

}  // namespace prosperoweb::core
