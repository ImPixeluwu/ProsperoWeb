// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI process: SDL2 window + frame loop, gamepad/keyboard input, and the
// content/chrome compositor driven by the shell controller.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <SDL.h>

#include "ps5browser/core/file_storage.h"
#include "ps5browser/shell/shell_controller.h"
#include "ps5_chrome_ui.h"
#include "ps5_config.h"
#include "ps5_engine_view.h"

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
  void composite_and_present();
  std::vector<SDL_Rect> derive_chrome_rects() const;

  static void frame_trampoline(void* self);

  SDL_Window* window_ = nullptr;
  SDL_GameController* pad_ = nullptr;

  Ps5Config config_;
  std::unique_ptr<core::FileStorage> storage_;
  std::unique_ptr<Ps5EngineFactory> factory_;
  std::unique_ptr<Ps5ChromeUi> chrome_;
  std::unique_ptr<shell::ShellController> controller_;

  // ARGB32 backbuffer (what WKPagePaint writes); converted to the SDL window
  // surface (ABGR8888) on present.
  std::vector<std::uint32_t> backbuffer_;
  std::vector<std::uint32_t> chrome_buffer_;
  int width_ = 1920;
  int height_ = 1080;

  std::uint64_t last_frame_ticks_ = 0;
  bool running_ = false;
};

}  // namespace ps5browser::frontend
