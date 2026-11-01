// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI process: SDL2 window + frame loop, gamepad/keyboard input, and the
// content/chrome compositor driven by the shell controller.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <SDL.h>

#include "prosperoweb/core/file_storage.h"
#include "damage_region.h"
#include "frame_stats.h"
#include "pointer.h"
#include "splash.h"
#include "prosperoweb/shell/shell_controller.h"
#include "ps5_audio_server.h"
#include "ps5_chrome_ui.h"
#include "ps5_config.h"
#include "ps5_engine_view.h"
#include "ps5_ime.h"
#include "ps5_presenter.h"
#include "ps5_touchpad.h"
#include "ps5_video_overlay.h"

namespace prosperoweb::frontend {

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
  void handle_touchpad(std::uint64_t now);
  void trigger_edge(core::Button button, bool down, bool& was_down);
  void paint_content();
  void paint_chrome();
  void composite_and_present();
  // Takes the accumulated page damage plus the chrome changes visible in
  // chrome_rects, in window pixels.
  display::Region take_frame_damage(const std::vector<display::Rect>& chrome_rects);
  // Loading screen (splash.h): drawn once the window exists, its progress
  // bar advanced per startup stage (kSplash* bits) until the first WebKit
  // frame replaces it.
  void draw_splash();
  void mark_splash_stage(unsigned stage);
  void update_splash_progress(SDL_Surface* surface);
  static void splash_spawn_progress(const char* event, void* self);
  // Frame-pipeline statistics: one log line every kStatsWindowMs while
  // anything is painted or presented.
  void collect_and_log_stats(std::uint64_t now);
  std::vector<display::Rect> derive_chrome_rects();
  // Queues frame() on the main run loop unless one is queued already.
  // Thread-safe (the presenter calls it).
  void request_frame();
  // Remote diagnostics (ps5_diagnostics.h).
  void poll_diagnostic_requests(std::uint64_t now);
  // Writes the screen as a PNG. With `chrome_rects`, the whole frame is
  // composed anew from the caches with the video overlay frames of
  // `video_page` (the window surface may hold only the damaged parts, and
  // under the PS5 driver no video where nothing changed lately); without,
  // the surface as it is (the splash).
  void dump_screen(SDL_Surface* surface, const std::vector<display::Rect>* chrome_rects = nullptr,
                   int top = 0, const video::PageKey& video_page = {});
  void run_form_probe();

  static void frame_trampoline(void* self);

  SDL_Window* window_ = nullptr;
  SDL_GameController* pad_ = nullptr;
  // Touchpad fingers, fed to the shell every frame.
  Ps5Touchpad touchpad_;
  std::vector<core::TouchContact> touch_contacts_;
  float touchpad_aspect_ = 0.0f;  // last aspect given to the shell
  std::unique_ptr<Ps5AudioServer> audio_server_;
  std::unique_ptr<Ps5VideoOverlay> video_overlay_;
  std::unique_ptr<Ps5Presenter> presenter_;
  std::atomic<bool> frame_queued_{false};

  Ps5Config config_;
  std::unique_ptr<core::FileStorage> storage_;
  std::unique_ptr<Ps5EngineFactory> factory_;
  std::unique_ptr<Ps5ChromeUi> chrome_;
  // The system keyboard dialog (nullptr with PROSPEROWEB_OSK=builtin); the
  // controller holds a pointer to it, so it is declared (and outlives) first.
  std::unique_ptr<Ps5ImeTextInput> ime_input_;
  std::unique_ptr<shell::ShellController> controller_;

  // Content cache: ARGB32 (what WKPagePaint writes), the active view painted
  // at y = content_top(); only dirty areas are repainted. With the video
  // overlay, pixels a video element left transparent (its hole) keep alpha
  // 0 here, and the compositor puts the video under them. The composed frame
  // goes straight to the SDL window surface (ABGR8888), never back here, and
  // only the damaged rects are composed and presented.
  std::vector<std::uint32_t> page_buffer_;
  std::uint64_t painted_tab_ = ~std::uint64_t{0};  // tab id in page_buffer_
  int painted_top_ = -1;
  // Chrome cache: premultiplied ARGB32 over a transparent background.
  std::vector<std::uint32_t> chrome_buffer_;
  bool chrome_painted_ = false;
  // Chrome rects of the last frames (newest first), see derive_chrome_rects().
  static constexpr std::size_t kChromeRectHistory = 4;
  std::deque<std::vector<display::Rect>> recent_chrome_rects_;
  // Damage not yet composed into the window surface (window pixels): page
  // repaints, and chrome repaints (shown only inside the chrome rects).
  display::Region pending_damage_;
  display::Region pending_chrome_damage_;
  // The chrome rects the surface was last composed with.
  std::vector<display::Rect> composed_chrome_rects_;
  // Where the pointer was last drawn into the surface (none: hidden).
  std::optional<display::Rect> composed_pointer_;
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
  static constexpr std::uint64_t kStatsWindowMs = 5000;
  display::FrameStats stats_;
  std::uint64_t stats_start_ticks_ = 0;
  bool running_ = false;
  // One-shot startup diagnostics (log.txt).
  std::uint64_t frames_ = 0;
  bool surface_logged_ = false;
  // The startup splash stays on screen until WebKit reports its first paint
  // (page or chrome); until then the frame loop does not flip.
  bool webkit_painted_ = false;
  bool splash_replaced_ = false;
  static constexpr unsigned kSplashWindow = 1u << 0;
  static constexpr unsigned kSplashWebKit = 1u << 1;
  static constexpr unsigned kSplashBroker = 1u << 2;
  static constexpr unsigned kSplashNetworkProcess = 1u << 3;
  static constexpr unsigned kSplashWebProcess = 1u << 4;
  static constexpr int kSplashStages = 6;  // the five above, then the first WebKit frame
  display::GrayImage splash_title_;
  display::GrayImage splash_subtitle_;
  display::SplashLayout splash_layout_;
  bool splash_drawn_ = false;
  std::atomic<unsigned> splash_stages_{0};  // set from the spawning thread too
  unsigned splash_shown_stages_ = 0;
  std::atomic<bool> frame_loop_ready_{false};
  std::thread::id main_thread_;
};

}  // namespace prosperoweb::frontend
