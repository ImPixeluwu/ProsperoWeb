// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frame-pipeline statistics of the UI process, summed over a window (5 s)
// and logged as one line. Counts per second, durations as avg/max in ms and
// areas as the average pixels per event (kpx = 1000 pixels).
#pragma once

#include <cstdint>
#include <string>

namespace prosperoweb::display {

// A timed, sized event (a paint, a compose, a present).
struct TimingStat {
  std::uint32_t count = 0;
  double total_ms = 0;
  double max_ms = 0;
  std::uint64_t pixels = 0;

  void add(double ms, std::uint64_t px) {
    count++;
    total_ms += ms;
    if (ms > max_ms) {
      max_ms = ms;
    }
    pixels += px;
  }
  void merge(const TimingStat& o) {
    count += o.count;
    total_ms += o.total_ms;
    if (o.max_ms > max_ms) {
      max_ms = o.max_ms;
    }
    pixels += o.pixels;
  }
  double avg_ms() const { return count ? total_ms / count : 0.0; }
  double avg_kpx() const { return count ? static_cast<double>(pixels) / count / 1000.0 : 0.0; }
};

// An untimed, sized event (setViewNeedsDisplay).
struct AreaStat {
  std::uint32_t count = 0;
  std::uint64_t pixels = 0;

  void add(std::uint64_t px) {
    count++;
    pixels += px;
  }
  void merge(const AreaStat& o) {
    count += o.count;
    pixels += o.pixels;
  }
  double avg_kpx() const { return count ? static_cast<double>(pixels) / count / 1000.0 : 0.0; }
};

struct FrameStats {
  std::uint32_t frames = 0;        // UI frame-loop iterations
  AreaStat page_needs_display;     // setViewNeedsDisplay of the active page
  AreaStat chrome_needs_display;   // setViewNeedsDisplay of the chrome view
  TimingStat page_paint;           // WKPagePaint into the page cache
  TimingStat chrome_paint;         // WKPagePaint into the chrome cache
  TimingStat compose;              // caches -> window surface (swap + blend)
  TimingStat present;              // SDL_UpdateWindowSurfaceRects (incl. vblank wait)
  std::uint32_t damage_rects = 0;  // rects handed to the presents
  std::uint32_t present_waits = 0; // frames whose damage waited for a busy presenter
  AreaStat video_frames;           // new video overlay pictures composed (their visible area)

  // Whether anything moved through the pipeline (an idle UI is not logged).
  bool active() const {
    return page_needs_display.count || chrome_needs_display.count || page_paint.count ||
           chrome_paint.count || present.count || video_frames.count;
  }

  // One log line for a window of `seconds`.
  std::string format(double seconds) const;
};

}  // namespace prosperoweb::display
