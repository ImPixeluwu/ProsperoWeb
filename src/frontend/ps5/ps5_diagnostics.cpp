// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_diagnostics.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <unistd.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include <stb/stb_image_write.h>

#include "ps5_log.h"

namespace prosperobrowser::frontend {

namespace {

// stb's deflate is slow at its default level 8; a screen dump stalls the
// frame loop while it runs, and size matters less than time here.
constexpr int kPngCompressionLevel = 2;

void append_to_string(void* context, void* data, int size) {
  static_cast<std::string*>(context)->append(static_cast<const char*>(data),
                                             static_cast<std::size_t>(size));
}

constexpr const char* kFormProbeScript = R"JS(
var out = { url: String(location.href) };
try {
  var props = ['display', 'width', 'height', 'min-width', 'min-height', 'font-size',
               'font-family', 'appearance', '-webkit-appearance', 'padding', 'border',
               'box-sizing', 'line-height', 'visibility', 'opacity', 'transform', 'zoom'];
  var media = function (q) {
    try { return window.matchMedia(q).matches; } catch (e) { return 'error: ' + e; }
  };
  var supports = function () {
    try { return CSS.supports.apply(CSS, arguments); } catch (e) { return 'error: ' + e; }
  };
  out.compatMode = document.compatMode;
  out.userAgent = navigator.userAgent;
  out.innerWidth = window.innerWidth;
  out.innerHeight = window.innerHeight;
  out.devicePixelRatio = window.devicePixelRatio;
  out.pointerFine = media('(pointer:fine)');
  out.hoverHover = media('(hover:hover)');
  out.supportsGrid = supports('display', 'grid');
  out.supportsHas = supports('selector(:has(a))');
  var all = document.querySelectorAll('input, textarea, button, [role=combobox]');
  out.controlCount = all.length;
  var list = Array.prototype.slice.call(all, 0, 10);
  out.controls = list.map(function (e) {
    var r = e.getBoundingClientRect();
    var s = window.getComputedStyle(e);
    var computed = {};
    props.forEach(function (p) { computed[p] = s.getPropertyValue(p); });
    return {
      tag: e.tagName.toLowerCase(),
      type: e.getAttribute('type'),
      id: e.id || null,
      className: typeof e.className === 'string' ? e.className : null,
      name: e.getAttribute('name'),
      role: e.getAttribute('role'),
      rect: { x: r.x, y: r.y, width: r.width, height: r.height },
      offsetWidth: e.offsetWidth,
      offsetHeight: e.offsetHeight,
      computed: computed
    };
  });
  var html = document.documentElement ? document.documentElement.outerHTML : '';
  var first = list.filter(function (e) { return e.tagName === 'INPUT'; })[0] || list[0];
  var at = first ? html.indexOf(first.outerHTML.slice(0, 80)) : -1;
  if (at < 0) at = html.search(/<(input|textarea)\b/i);
  var start = Math.max(0, at < 0 ? 0 : at - 1000);
  out.html = { length: html.length, offset: start, text: html.substr(start, 2000) };
} catch (e) {
  out.error = String(e && e.stack || e);
}
return JSON.stringify(out);
)JS";

}  // namespace

bool take_request_file(const std::string& path) {
  if (access(path.c_str(), F_OK) != 0) {
    return false;
  }
  if (unlink(path.c_str()) != 0) {
    log_printf("diagnostics: cannot delete %s: %s", path.c_str(), std::strerror(errno));
  }
  return true;
}

bool write_file_atomically(const std::string& path, const std::string& data) {
  const std::string part = path + ".part";
  std::FILE* f = std::fopen(part.c_str(), "wb");
  if (!f) {
    log_printf("diagnostics: cannot create %s: %s", part.c_str(), std::strerror(errno));
    return false;
  }
  const bool written = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  const bool closed = std::fclose(f) == 0;
  if (!written || !closed) {
    log_printf("diagnostics: writing %s failed: %s", part.c_str(), std::strerror(errno));
    unlink(part.c_str());
    return false;
  }
  if (std::rename(part.c_str(), path.c_str()) != 0) {
    log_printf("diagnostics: rename to %s failed: %s", path.c_str(), std::strerror(errno));
    unlink(part.c_str());
    return false;
  }
  return true;
}

std::string encode_png_rgbx(const std::uint8_t* pixels, int width, int height,
                            int pitch_bytes) {
  if (!pixels || width <= 0 || height <= 0) {
    return std::string();
  }
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
  for (int y = 0; y < height; y++) {
    const std::uint8_t* in = pixels + static_cast<std::size_t>(y) * pitch_bytes;
    std::uint8_t* o = &rgb[static_cast<std::size_t>(y) * width * 3];
    for (int x = 0; x < width; x++, in += 4, o += 3) {
      o[0] = in[0];
      o[1] = in[1];
      o[2] = in[2];
    }
  }
  stbi_write_png_compression_level = kPngCompressionLevel;
  std::string png;
  if (!stbi_write_png_to_func(&append_to_string, &png, width, height, 3, rgb.data(),
                              width * 3)) {
    return std::string();
  }
  return png;
}

const char* form_probe_script() { return kFormProbeScript; }

}  // namespace prosperobrowser::frontend
