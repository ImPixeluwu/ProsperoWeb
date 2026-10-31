// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_video_overlay.h"

#include <cstdlib>
#include <cstring>

#include <WebKit/WKPagePrivate.h>
#include <WebKit/WKPagePrivatePlayStation.h>

#include "FFmpegVideoOverlayWire.h"
#include "ps5_hwdec.h"
#include "ps5_log.h"
#include "video_geometry.h"

namespace prosperoweb::frontend {

namespace {

// -1 = disabled.
int configured_port() {
  const char* value = std::getenv("PROSPEROWEB_VIDEO_PORT");
  if (!value || !*value) {
    return WebCore::FFmpegMedia::VideoWire::defaultPort;
  }
  if (!std::strcmp(value, "off") || !std::strcmp(value, "0")) {
    return -1;
  }
  char* end = nullptr;
  const long port = std::strtol(value, &end, 10);
  if (*end || port <= 0 || port > 65535) {
    log_printf("video overlay: PROSPEROWEB_VIDEO_PORT=%s is not a port; disabled", value);
    return -1;
  }
  return static_cast<int>(port);
}

}  // namespace

std::unique_ptr<Ps5VideoOverlay> Ps5VideoOverlay::start(std::function<void()> on_update) {
  const int port = configured_port();
  if (port < 0) {
    log_line(
        "video overlay: server disabled (PROSPEROWEB_VIDEO_PORT); video is painted into pages");
    return nullptr;
  }
  if (const char* mode = std::getenv("PROSPEROWEB_VIDEO_OVERLAY");
      mode && (!std::strcmp(mode, "0") || !std::strcmp(mode, "off"))) {
    log_line("video overlay: players paint into pages (PROSPEROWEB_VIDEO_OVERLAY=0)");
  }
  video::ServerOptions options;
  options.port = port;
  const char* scale = std::getenv("PROSPEROWEB_VIDEO_SCALE");
  options.filter = scale && !std::strcmp(scale, "nearest") ? display::ScaleFilter::Nearest
                                                            : display::ScaleFilter::Bilinear;
  options.on_update = std::move(on_update);
  // Remote decoding: players send compressed video when the UI can decode
  // it (ps5_hwdec.h); without a factory every stream is refused.
  options.decoder_factory = hwdec_factory();
  log_line(options.decoder_factory
               ? "video overlay: remote decoding on (sceVideodec2: h264, hevc; probed on demand)"
               : "video overlay: remote decoding off (no hardware decoder); players decode");
  std::unique_ptr<Ps5VideoOverlay> self(new Ps5VideoOverlay());
  self->server_ = std::make_unique<video::VideoServer>(std::move(options));
  if (!self->server_->start()) {
    log_line("video overlay: server not running; video is painted into pages");
    return nullptr;
  }
  return self;
}

Ps5VideoOverlay::~Ps5VideoOverlay() {
  if (server_) {
    server_->stop();
  }
}

video::PageKey Ps5VideoOverlay::page_key(WKPageRef page) {
  if (!page) {
    return {};
  }
  return video::PageKey{static_cast<std::uint32_t>(WKPageGetProcessIdentifier(page)),
                        WKPageGetWebPageIdentifierPlayStation(page)};
}

display::Region Ps5VideoOverlay::take_damage(const video::PageKey& page, int content_top,
                                             int width, int height) {
  return video::damage_to_window(server_->take_damage(page), content_top, width, height);
}

std::vector<display::VideoLayer> Ps5VideoOverlay::layers(const video::VideoServer::Frames& frames,
                                                         int content_top, int width, int height) {
  std::vector<display::VideoLayer> out;
  const display::Rect area = video::content_area(content_top, width, height);
  for (const auto& layer : frames.layers()) {
    out.push_back(display::VideoLayer{layer.pixels, layer.stride,
                                      video::view_to_window(layer.view_rect, content_top), area});
  }
  return out;
}

}  // namespace prosperoweb::frontend
