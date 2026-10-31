// SPDX-License-Identifier: GPL-3.0-or-later
//
// Renders one line of text into a coverage image with FreeType, for the
// loading screen (splash.h), before WebKit and fontconfig are up. Simple
// left-to-right layout with kerning; enough for "ProsperoWeb" and
// "Loading…". Depends on FreeType only.
#pragma once

#include <string>

#include "splash.h"

namespace prosperoweb::frontend {

// UTF-8 `text` in the font at `font_path`, `pixel_size` px em. The image is
// as tall as the font's ascender + descender and as wide as the advances.
// False (with `error`) when FreeType cannot load the font.
bool render_splash_text(const std::string& font_path, const std::string& text, int pixel_size,
                        display::GrayImage& image, std::string& error);

}  // namespace prosperoweb::frontend
