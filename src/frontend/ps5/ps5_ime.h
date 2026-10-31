// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS5 system on-screen keyboard (libSceImeDialog) as the shell's text
// input provider. The built-in HTML OSK stays the fallback: a dialog that
// cannot open or ends with an error hands the request back to the shell.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "prosperoweb/shell/text_input.h"

namespace prosperoweb::frontend {

// One sceImeDialog session at a time: open(), poll() every frame until it
// reports Done / Cancelled / Error (the dialog is terminated then), or term().
class Ps5SystemIme {
 public:
  enum class State : std::uint8_t { Idle, Running, Done, Cancelled, Error };
  struct Status {
    State state = State::Idle;
    std::string text;        // UTF-8, Done only
    std::int32_t error = 0;  // SCE error code, Error only
  };

  // Limits of the dialog (in UTF-16 units, without the terminating NUL).
  static constexpr std::size_t kMaxTextLength = 2048;
  static constexpr std::size_t kMaxTitleLength = 127;

  Ps5SystemIme() = default;
  ~Ps5SystemIme();
  Ps5SystemIme(const Ps5SystemIme&) = delete;
  Ps5SystemIme& operator=(const Ps5SystemIme&) = delete;

  // Shows the dialog pre-filled with initial_utf8. Returns 0 or the SCE
  // error code of the failing call (the dialog is not open then).
  std::int32_t open(const std::string& title_utf8, const std::string& initial_utf8,
                    std::size_t max_length, shell::TextInputKind kind);
  // Running while the dialog is up; Done / Cancelled / Error exactly once
  // when it ends; Idle when no dialog is open.
  Status poll();
  // Closes the dialog without a result (no-op when none is open).
  void term();
  bool is_open() const { return open_; }

  // SDL's PS5 video driver watches the same dialog from SDL_PollEvent()
  // (its SDL_StartTextInput() support): when it sees the dialog finish
  // first, it fetches the result, terminates the dialog and, on OK only,
  // sends a Return key. Ps5App reports that key here so poll() can still
  // tell OK from cancel; the text is in our buffer either way.
  void note_sdl_return() { sdl_return_ = true; }

 private:
  std::int32_t ensure_user();

  int user_id_ = -1;
  bool user_ready_ = false;
  bool open_ = false;
  bool sdl_return_ = false;
  bool seen_running_ = false;
  bool password_ = false;  // never log the result
  int last_status_ = -1;
  int start_polls_ = 0;
  std::vector<char16_t> buffer_;  // inputTextBuffer, kMaxTextLength + 1 units
  std::u16string title_;
};

// shell::TextInputProvider backed by Ps5SystemIme.
class Ps5ImeTextInput final : public shell::TextInputProvider {
 public:
  bool request_text(const shell::TextInputRequest& request, Completion done) override;
  void poll() override;
  bool active() const override { return static_cast<bool>(done_); }

  Ps5SystemIme& ime() { return ime_; }

 private:
  Ps5SystemIme ime_;
  Completion done_;
};

// "system" (the default) or "builtin": the PROSPEROWEB_OSK environment
// variable when set, else config_value (config.json "osk"). Unknown values
// select "system".
bool use_system_ime(const std::string& config_value);

}  // namespace prosperoweb::frontend
