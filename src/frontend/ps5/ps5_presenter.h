// SPDX-License-Identifier: GPL-3.0-or-later
//
// Presents the SDL window surface and paces the UI frame loop.
//
// Under the PS5 SDL driver SDL_UpdateWindowSurfaceRects() de-tiles the
// bounding box of the given rects (plus what the other scanout buffer still
// lacks) into the GPU-tiled buffer, submits the flip and then blocks until
// the flip completes, i.e. up to a whole vblank. On the main thread that
// wait would stall WebKit's run loop, and with it the WebProcess updates
// (DrawingArea::Update is answered only when the UI process handles it).
// The presenter thread takes that call off the main thread:
//
//   main:      compose the damage into the surface -> submit(rects) -> run loop
//   presenter: SDL_UpdateWindowSurfaceRects(rects) (de-tile, flip, wait)
//              -> request_frame()
//
// While a present is in flight (busy()) the main thread must not write the
// surface; it keeps painting its caches and composes the accumulated damage
// once the presenter is idle. The thread also drives the frame loop: it
// requests a frame right after each completed flip (so composing starts at
// vblank and at most one present happens per vblank) and every kTickMs while
// idle, which replaces the SDL_Delay of the old loop. A setViewNeedsDisplay
// requests a frame too (WkWebView::set_damage_hook), so a WebProcess update
// is painted without waiting for the next tick.
//
// PS5BROWSER_PRESENT_THREAD=0 presents synchronously on the caller's thread
// (the old behaviour); pacing still comes from the thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL.h>

#include "frame_stats.h"

namespace ps5browser::frontend {

class Ps5Presenter {
 public:
  static constexpr std::uint32_t kTickMs = 16;

  // request_frame is called from the presenter thread; it must be
  // thread-safe and cheap (it posts the frame to the main run loop).
  Ps5Presenter(SDL_Window* window, bool threaded, std::function<void()> request_frame);
  ~Ps5Presenter();

  Ps5Presenter(const Ps5Presenter&) = delete;
  Ps5Presenter& operator=(const Ps5Presenter&) = delete;

  bool threaded() const { return threaded_; }
  // A present is in flight: the window surface must not be written.
  bool busy() const { return busy_.load(std::memory_order_acquire); }
  // Presents rects of the window surface. Threaded, returns at once and
  // busy() stays true until the flip completed. Must not be called while busy.
  void submit(std::vector<SDL_Rect> rects);
  // Stops and joins the thread (waits for an in-flight present).
  void stop();

  // Present timings and areas since the last call.
  display::TimingStat take_stats();

 private:
  void run();
  void present(const std::vector<SDL_Rect>& rects);

  SDL_Window* window_;
  const bool threaded_;
  std::function<void()> request_frame_;

  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool job_ready_ = false;
  std::vector<SDL_Rect> job_;
  std::atomic<bool> busy_{false};
  display::TimingStat stats_;  // guarded by mutex_
  bool first_logged_ = false;
  bool error_logged_ = false;
  std::thread thread_;
};

}  // namespace ps5browser::frontend
