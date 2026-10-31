// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_stats.h"

#include <cstdio>

namespace ps5browser::display {

namespace {

double per_second(std::uint32_t n, double seconds) {
  return seconds > 0 ? n / seconds : 0.0;
}

}  // namespace

std::string FrameStats::format(double seconds) const {
  char buf[512];
  const double rects_per_present =
      present.count ? static_cast<double>(damage_rects) / present.count : 0.0;
  std::snprintf(
      buf, sizeof(buf),
      "perf %.1fs: ui %.1f fps | ndisp page %.1f/s %.0fkpx chrome %.1f/s %.0fkpx"
      " | paint page %.1f/s %.2f/%.2fms %.0fkpx chrome %.1f/s %.2f/%.2fms %.0fkpx"
      " | compose %.1f/s %.2f/%.2fms %.0fkpx"
      " | present %.1f/s %.2f/%.2fms %.1f rects %.0fkpx waits %u"
      " | video %.1f/s %.0fkpx",
      seconds, per_second(frames, seconds), per_second(page_needs_display.count, seconds),
      page_needs_display.avg_kpx(), per_second(chrome_needs_display.count, seconds),
      chrome_needs_display.avg_kpx(), per_second(page_paint.count, seconds),
      page_paint.avg_ms(), page_paint.max_ms, page_paint.avg_kpx(),
      per_second(chrome_paint.count, seconds), chrome_paint.avg_ms(), chrome_paint.max_ms,
      chrome_paint.avg_kpx(), per_second(compose.count, seconds), compose.avg_ms(),
      compose.max_ms, compose.avg_kpx(), per_second(present.count, seconds),
      present.avg_ms(), present.max_ms, rects_per_present, present.avg_kpx(),
      present_waits, per_second(video_frames.count, seconds), video_frames.avg_kpx());
  return buf;
}

}  // namespace ps5browser::display
