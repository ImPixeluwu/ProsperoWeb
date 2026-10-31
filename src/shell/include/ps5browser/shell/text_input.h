// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace ps5browser::shell {

// What a text request is for; providers pick a keyboard layout and an enter
// label from it (the PS5 system dialog: URL / mail / number types, "Go" or
// "Search" labels, masked input for passwords).
enum class TextInputKind : std::uint8_t { Text, Url, Search, Email, Number, Password };

const char* text_input_kind_name(TextInputKind kind);
// Maps an <input type> (or "textarea" / "contenteditable") to a kind; unknown
// types are Text.
TextInputKind text_input_kind_from_type(const std::string& input_type);

struct TextInputRequest {
  TextInputKind kind = TextInputKind::Text;
  std::string title;    // UTF-8, shown above the input line
  std::string initial;  // UTF-8, pre-filled text the user edits
  std::size_t max_length = 2048;  // in UTF-16 code units
};

struct TextInputResult {
  enum class Outcome : std::uint8_t { Done, Cancelled, Error };
  Outcome outcome = Outcome::Cancelled;
  std::string text;         // UTF-8, the whole edited text (Done only)
  std::int64_t error = 0;   // provider error code (Error only)
};

// A text entry front end other than the shell's own HTML on-screen keyboard,
// e.g. the PS5 system keyboard dialog. The shell uses its built-in OSK when
// no provider is configured (ShellController::Config::text_input) and as the
// fallback when a provider fails.
//
// While a request is active the shell stays in its current focus, keeps
// rendering and ignores gamepad input (the provider owns it). All calls are
// made on the shell's thread.
class TextInputProvider {
 public:
  using Completion = std::function<void(const TextInputResult&)>;

  virtual ~TextInputProvider() = default;

  // Starts a request. Returns false when it cannot start at all; done is then
  // never called. Otherwise done is called exactly once, from request_text()
  // itself or from a later poll().
  virtual bool request_text(const TextInputRequest& request, Completion done) = 0;
  // Called from ShellController::tick(); asynchronous providers complete here.
  virtual void poll() = 0;
  // True between a successful request_text() and its completion.
  virtual bool active() const = 0;
};

}  // namespace ps5browser::shell
