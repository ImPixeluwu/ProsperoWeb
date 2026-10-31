// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_splash_text.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace prosperoweb::frontend {

namespace {

// Code points of a UTF-8 string; invalid bytes become U+FFFD.
std::vector<std::uint32_t> decode_utf8(const std::string& s) {
  std::vector<std::uint32_t> out;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t extra = 0;
    std::uint32_t cp = c;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      cp = c & 0x07;
    } else if (c >= 0x80) {
      out.push_back(0xFFFD);
      i++;
      continue;
    }
    bool ok = i + extra < s.size();
    for (std::size_t k = 1; ok && k <= extra; k++) {
      const auto cc = static_cast<unsigned char>(s[i + k]);
      ok = (cc & 0xC0) == 0x80;
      cp = (cp << 6) | (cc & 0x3F);
    }
    out.push_back(ok ? cp : 0xFFFD);
    i += ok ? extra + 1 : 1;
  }
  return out;
}

struct Library {
  FT_Library lib = nullptr;
  FT_Face face = nullptr;
  ~Library() {
    if (face) {
      FT_Done_Face(face);
    }
    if (lib) {
      FT_Done_FreeType(lib);
    }
  }
};

}  // namespace

bool render_splash_text(const std::string& font_path, const std::string& text, int pixel_size,
                        display::GrayImage& image, std::string& error) {
  Library ft;
  if (FT_Init_FreeType(&ft.lib) != 0) {
    error = "FT_Init_FreeType failed";
    return false;
  }
  if (const FT_Error e = FT_New_Face(ft.lib, font_path.c_str(), 0, &ft.face); e != 0) {
    error = "cannot load " + font_path + " (FreeType error " + std::to_string(e) + ")";
    return false;
  }
  if (FT_Set_Pixel_Sizes(ft.face, 0, static_cast<FT_UInt>(pixel_size)) != 0) {
    error = "FT_Set_Pixel_Sizes failed";
    return false;
  }
  const int ascender = static_cast<int>(ft.face->size->metrics.ascender >> 6);
  const int descender = static_cast<int>(-(ft.face->size->metrics.descender >> 6));
  const std::vector<std::uint32_t> code_points = decode_utf8(text);

  // Two passes: measure, then draw.
  struct Placed {
    FT_UInt glyph;
    long pen_x;  // 26.6
  };
  std::vector<Placed> placed;
  long pen = 0;
  FT_UInt previous = 0;
  const bool kerning = FT_HAS_KERNING(ft.face);
  for (std::uint32_t cp : code_points) {
    const FT_UInt glyph = FT_Get_Char_Index(ft.face, cp);
    if (kerning && previous && glyph) {
      FT_Vector delta{};
      FT_Get_Kerning(ft.face, previous, glyph, FT_KERNING_DEFAULT, &delta);
      pen += delta.x;
    }
    if (FT_Load_Glyph(ft.face, glyph, FT_LOAD_DEFAULT) != 0) {
      continue;
    }
    placed.push_back({glyph, pen});
    pen += ft.face->glyph->advance.x;
    previous = glyph;
  }
  image.width = std::max(1, static_cast<int>((pen + 63) >> 6));
  image.height = std::max(1, ascender + descender);
  image.coverage.assign(static_cast<std::size_t>(image.width) * image.height, 0);

  for (const Placed& p : placed) {
    if (FT_Load_Glyph(ft.face, p.glyph, FT_LOAD_RENDER) != 0) {
      continue;
    }
    const FT_GlyphSlot slot = ft.face->glyph;
    const FT_Bitmap& bm = slot->bitmap;
    if (bm.pixel_mode != FT_PIXEL_MODE_GRAY) {
      continue;
    }
    const int x0 = static_cast<int>(p.pen_x >> 6) + slot->bitmap_left;
    const int y0 = ascender - slot->bitmap_top;
    for (unsigned row = 0; row < bm.rows; row++) {
      const int y = y0 + static_cast<int>(row);
      if (y < 0 || y >= image.height) {
        continue;
      }
      const unsigned char* src = bm.buffer + static_cast<long>(row) * bm.pitch;
      for (unsigned col = 0; col < bm.width; col++) {
        const int x = x0 + static_cast<int>(col);
        if (x < 0 || x >= image.width) {
          continue;
        }
        std::uint8_t& dst = image.coverage[static_cast<std::size_t>(y) * image.width + x];
        dst = std::max<std::uint8_t>(dst, src[col]);
      }
    }
  }
  return true;
}

}  // namespace prosperoweb::frontend
