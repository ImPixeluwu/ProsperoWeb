// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace prosperoweb::shell {

// The mouse cursor the page asks for (CSS `cursor`), as far as the engine
// reports it. None hides the pointer.
enum class PageCursor : std::uint8_t { Pointer, Hand, None };

// Callbacks from a web engine view to the shell. All calls are made on the
// shell's thread (the frontend's main loop).
class EngineViewClient {
 public:
  virtual ~EngineViewClient() = default;

  virtual void url_changed(const std::string& url) = 0;
  virtual void title_changed(const std::string& title) = 0;
  virtual void load_started() = 0;
  virtual void load_finished() = 0;
  virtual void load_failed(const std::string& url, const std::string& error_text) = 0;
  virtual void nav_state_changed(bool can_go_back, bool can_go_forward) = 0;
  // The page entered / left HTML5 element fullscreen (requestFullscreen()).
  virtual void fullscreen_entered() = 0;
  virtual void fullscreen_exited() = 0;
  // A message posted by the injected page scripts to the "prosperoweb"
  // handler, serialized as JSON (e.g. {"type":"eme","keySystem":..,"origin":..}).
  virtual void script_message(const std::string& json) = 0;
  // window.open() / target=_blank; the shell opens the URL in a new tab.
  virtual void new_window_requested(const std::string& url) = 0;
  // An editable element gained (true) or lost (false) focus.
  virtual void text_input_focused(bool focused) = 0;
  // The page changed the cursor under the pointer (engines that report it).
  virtual void cursor_changed(PageCursor) {}
};

// Mouse buttons as used by pointer_button().
enum class MouseButton : std::uint8_t { Left = 1, Middle = 2, Right = 3 };

// Keyboard modifiers bitmask for key().
enum KeyModifier : std::uint32_t {
  kModNone = 0,
  kModShift = 1u << 0,
  kModControl = 1u << 1,
  kModAlt = 1u << 2,
  kModMeta = 1u << 3,
};

// One web content view (one per tab). Implemented by each engine port: the
// WebKitGTK host frontend today, WebKit's C API (WKView) on the PS5 later.
//
// Coordinates are in view-local CSS pixels (0,0 = top-left of the view).
//
// Trusted input: pointer_move(), pointer_button(), scroll() and key() MUST be
// delivered as real engine input events (the equivalent of a user moving the
// mouse or pressing a key), so the page sees isTrusted == true, user
// activation is granted (popups, fullscreen, media autoplay with sound) and
// default actions (focus, text selection, form submission, link activation)
// run. Synthesizing DOM events from JavaScript is NOT an acceptable
// implementation on the target platform; a frontend that cannot do better
// (the GTK host) must document the limitation where it implements them.
// insert_text() is the one input path that may go through the editing
// command machinery instead of key events.
class EngineView {
 public:
  virtual ~EngineView() = default;

  // Navigation.
  virtual void load(const std::string& url) = 0;
  virtual void go_back() = 0;
  virtual void go_forward() = 0;
  virtual void reload() = 0;
  virtual void stop() = 0;

  // Geometry and visibility (only the active tab's view is visible).
  virtual void set_size(int width, int height) = 0;
  virtual void set_visible(bool visible) = 0;

  // Trusted input (see the class comment).
  virtual void pointer_move(int x, int y) = 0;
  virtual void pointer_button(int x, int y, MouseButton button, bool pressed) = 0;
  // Wheel scroll at (x, y); positive dy scrolls the content down.
  virtual void scroll(int x, int y, int dx, int dy) = 0;
  // key_code is a DOM-style virtual key code (e.g. 13 Enter, 8 Backspace);
  // text is the UTF-8 text the key produces (empty for non-printing keys).
  virtual void key(std::uint32_t key_code, std::uint32_t modifiers, bool pressed,
                   const std::string& text) = 0;

  // Commits text from the on-screen keyboard into the focused editable
  // element, replacing the selection.
  virtual void insert_text(const std::string& utf8) = 0;

  // Asks the engine to leave HTML5 element fullscreen; the engine answers
  // with EngineViewClient::fullscreen_exited().
  virtual void request_exit_fullscreen() = 0;
  // Runs a script in the main frame; the result is discarded.
  virtual void run_javascript(const std::string& js) = 0;
  // Optional: lets the engine report media/EME capabilities. No-op by default.
  virtual void evaluate_capabilities() {}
  virtual void set_user_agent(const std::string& user_agent) = 0;
  // Page zoom (1.0 = 100%): scales the whole page, layout included, like a
  // desktop browser's zoom. Coordinates stay in view pixels.
  virtual void set_zoom(double factor) = 0;
};

class EngineFactory {
 public:
  virtual ~EngineFactory() = default;
  // The client outlives the returned view.
  virtual std::unique_ptr<EngineView> create(EngineViewClient& client) = 0;
};

}  // namespace prosperoweb::shell
