// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI process: SDL2 window + frame loop, gamepad/keyboard input, and the
// content/chrome compositor driven by the shell controller.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include <SDL.h>

#include "ps5browser/core/file_storage.h"
#include "ps5browser/shell/shell_controller.h"
#include "ps5_chrome_ui.h"
#include "ps5_config.h"
#include "ps5_engine_view.h"
#include "ps5_ime.h"

namespace ps5browser::frontend {

class Ps5App {
 public:
  Ps5App();
  ~Ps5App();

  // Runs the UI process until the shell requests quit. Returns a process exit
  // code. Must be called on the main thread.
  int run();

 private:
  void frame();
  void pump_sdl_events();
  void handle_controller_button(const SDL_ControllerButtonEvent& e, bool pressed);
  void handle_axis_motion();
  void trigger_edge(core::Button button, bool down, bool& was_down);
  void paint_content();
  void paint_chrome();
  void composite_and_present();
  void draw_splash();
  std::vector<SDL_Rect> derive_chrome_rects();
  // Remote diagnostics (ps5_diagnostics.h).
  void poll_diagnostic_requests(std::uint64_t now);
  void dump_screen(SDL_Surface* surface);
  void run_form_probe();

  static void frame_trampoline(void* self);

  SDL_Window* window_ = nullptr;
  SDL_GameController* pad_ = nullptr;

  Ps5Config config_;
  std::unique_ptr<core::FileStorage> storage_;
  std::unique_ptr<Ps5EngineFactory> factory_;
  std::unique_ptr<Ps5ChromeUi> chrome_;
  // The system keyboard dialog (nullptr with PS5BROWSER_OSK=builtin); the
  // controller holds a pointer to it, so it is declared (and outlives) first.
  std::unique_ptr<Ps5ImeTextInput> ime_input_;
  std::unique_ptr<shell::ShellController> controller_;

  // Content cache: ARGB32 (what WKPagePaint writes), the active view painted
  // at y = content_top(); only dirty areas are repainted. The composed frame
  // goes straight to the SDL window surface (ABGR8888), never back here.
  std::vector<std::uint32_t> page_buffer_;
  std::uint64_t painted_tab_ = ~std::uint64_t{0};  // tab id in page_buffer_
  int painted_top_ = -1;
  // Chrome cache: premultiplied ARGB32 over a transparent background.
  std::vector<std::uint32_t> chrome_buffer_;
  bool chrome_painted_ = false;
  // Chrome rects of the last frames (newest first), see derive_chrome_rects().
  static constexpr std::size_t kChromeRectHistory = 4;
  std::deque<std::vector<SDL_Rect>> recent_chrome_rects_;
  int width_ = 1920;
  int height_ = 1080;

  // SDL's own dialog pump sends Return when it consumes our dialog's end;
  // the matching auto-release must not reach the page either.
  bool swallow_return_up_ = false;
  // Share: a short press goes to the shell on release, a press held for
  // kShareLongPressMs dumps the screen instead.
  static constexpr std::uint64_t kShareLongPressMs = 1000;
  bool share_down_ = false;
  bool share_long_fired_ = false;
  std::uint64_t share_down_ticks_ = 0;
  // Diagnostics requests: files polled once per second; a pending screen dump
  // is taken from the next composed frame.
  static constexpr std::uint64_t kRequestPollMs = 1000;
  std::uint64_t last_request_poll_ticks_ = 0;
  bool screenshot_pending_ = false;
  bool l2_down_ = false;
  bool r2_down_ = false;
  std::uint64_t last_frame_ticks_ = 0;
  bool running_ = false;
  // One-shot startup diagnostics (log.txt).
  std::uint64_t frames_ = 0;
  bool surface_logged_ = false;
  bool first_flip_logged_ = false;
  bool flip_error_logged_ = false;
  // The startup splash stays on screen until WebKit reports its first paint
  // (page or chrome); until then the frame loop does not flip.
  bool webkit_painted_ = false;
  bool splash_replaced_ = false;
};

}  // namespace ps5browser::frontend
