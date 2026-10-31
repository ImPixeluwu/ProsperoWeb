// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_presenter.h"

#include <chrono>
#include <utility>

#include "ps5_log.h"

namespace prosperobrowser::frontend {

Ps5Presenter::Ps5Presenter(SDL_Window* window, bool threaded,
                           std::function<void()> request_frame)
    : window_(window), threaded_(threaded), request_frame_(std::move(request_frame)) {
  thread_ = std::thread([this] { run(); });
}

Ps5Presenter::~Ps5Presenter() { stop(); }

void Ps5Presenter::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Ps5Presenter::submit(std::vector<SDL_Rect> rects) {
  if (!threaded_) {
    present(rects);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = std::move(rects);
    job_ready_ = true;
    busy_.store(true, std::memory_order_release);
  }
  cv_.notify_all();
}

display::TimingStat Ps5Presenter::take_stats() {
  std::lock_guard<std::mutex> lock(mutex_);
  display::TimingStat s = stats_;
  stats_ = display::TimingStat{};
  return s;
}

void Ps5Presenter::present(const std::vector<SDL_Rect>& rects) {
  std::uint64_t area = 0;
  for (const SDL_Rect& r : rects) {
    area += static_cast<std::uint64_t>(r.w) * static_cast<std::uint64_t>(r.h);
  }
  const std::uint64_t t0 = SDL_GetPerformanceCounter();
  const int rc = SDL_UpdateWindowSurfaceRects(window_, rects.data(), static_cast<int>(rects.size()));
  const double ms = static_cast<double>(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                    static_cast<double>(SDL_GetPerformanceFrequency());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.add(ms, area);
  }
  if (!first_logged_) {
    first_logged_ = true;
    log_printf("present: first SDL_UpdateWindowSurfaceRects (%zu rects, %s): %s", rects.size(),
               threaded_ ? "presenter thread" : "main thread",
               rc == 0 ? "ok" : SDL_GetError());
  } else if (rc != 0 && !error_logged_) {
    error_logged_ = true;
    log_printf("present: SDL_UpdateWindowSurfaceRects failed: %s", SDL_GetError());
  }
}

void Ps5Presenter::run() {
  using Clock = std::chrono::steady_clock;
  const auto tick = std::chrono::milliseconds(kTickMs);
  auto next_tick = Clock::now() + tick;
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    cv_.wait_until(lock, next_tick, [this] { return stop_ || job_ready_; });
    if (stop_) {
      break;
    }
    if (job_ready_) {
      std::vector<SDL_Rect> rects = std::move(job_);
      job_ready_ = false;
      lock.unlock();
      present(rects);  // returns after the flip completed
      busy_.store(false, std::memory_order_release);
      request_frame_();
      next_tick = Clock::now() + tick;
      lock.lock();
      continue;
    }
    if (Clock::now() >= next_tick) {
      lock.unlock();
      request_frame_();
      lock.lock();
      next_tick += tick;
      // After a stall, do not fire a burst of catch-up ticks.
      if (next_tick < Clock::now()) {
        next_tick = Clock::now() + tick;
      }
    }
  }
}

}  // namespace prosperobrowser::frontend
