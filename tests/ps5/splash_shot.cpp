// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host harness for docs/screenshots/ps5-splash.png: renders the PS5 loading
// screen (src/ps5/display/splash.h with the frontend's FreeType text) into a
// binary PPM. Not part of the CMake build; see docs/screenshots/README.md.
//
//   splash_shot FONT.ttf OUT.ppm [progress 0..1] [width height]
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ps5_splash_text.h"
#include "splash.h"

using namespace prosperoweb;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s FONT.ttf OUT.ppm [progress] [width height]\n", argv[0]);
    return 2;
  }
  const double progress = argc > 3 ? std::atof(argv[3]) : 0.5;
  const int w = argc > 5 ? std::atoi(argv[4]) : 1920;
  const int h = argc > 5 ? std::atoi(argv[5]) : 1080;
  display::GrayImage title, subtitle;
  std::string error;
  if (!frontend::render_splash_text(argv[1], "ProsperoWeb", h * 96 / 1080, title, error) ||
      !frontend::render_splash_text(argv[1], "Loading…", h * 30 / 1080, subtitle, error)) {
    std::fprintf(stderr, "text: %s (drawing the bar only)\n", error.c_str());
    title = {};
    subtitle = {};
  }
  std::vector<std::uint32_t> px(static_cast<std::size_t>(w) * h);
  const display::SplashLayout layout = display::layout_splash(w, h, &title, &subtitle);
  display::paint_splash(display::SplashTarget{px.data(), w, h, w, false}, layout, &title,
                        &subtitle, progress);
  FILE* out = std::fopen(argv[2], "wb");
  if (!out) {
    std::perror(argv[2]);
    return 1;
  }
  std::fprintf(out, "P6\n%d %d\n255\n", w, h);
  for (std::uint32_t p : px) {
    const unsigned char rgb[3] = {static_cast<unsigned char>(p >> 16),
                                  static_cast<unsigned char>(p >> 8), static_cast<unsigned char>(p)};
    std::fwrite(rgb, 1, 3, out);
  }
  return std::fclose(out) == 0 ? 0 : 1;
}
