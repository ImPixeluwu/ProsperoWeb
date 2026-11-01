// SPDX-License-Identifier: GPL-3.0-or-later
//
// UI side of the video overlay (src/ps5/video, protocol in
// webkit-port/media/ffmpeg/FFmpegVideoOverlayWire.h): WebProcess media
// players punch a transparent hole into the page where their video is and
// send the frames here; the compositor draws the newest frame of each
// visible player of the active page under the page, inside the content
// area, and recomposes a player's rect whenever it has a new frame (no
// WebKit repaint involved).
//
// Env (exported to the WebProcess too): PROSPEROWEB_VIDEO_PORT (default
// 17773, "off" disables the server; the players then paint into the page),
// PROSPEROWEB_VIDEO_OVERLAY=0 (players paint into the page; the server still
// runs), PROSPEROWEB_VIDEO_SCALE=nearest (default bilinear),
// PROSPEROWEB_VIDEO_DECODE=ui|webprocess (who decodes: ui, the default,
// lets players send compressed video for the hardware decoder,
// ps5_hwdec.h).
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <WebKit/WKBase.h>

#include "compose.h"
#include "damage_region.h"
#include "video_server.h"

namespace prosperoweb::frontend {

class Ps5VideoOverlay {
 public:
  // nullptr (logged) when disabled or the port cannot be bound. on_update
  // runs on the server thread whenever there is something new to compose.
  static std::unique_ptr<Ps5VideoOverlay> start(std::function<void()> on_update);
  ~Ps5VideoOverlay();

  // Whether the server is listening on its TCP port. False under the media
  // application-category sandbox, where players must arrive over a control
  // socket (add_control_fd).
  bool listening() const;
  // Adopts a WebProcess control socket (ps5_spawn_set_media_control). Takes
  // ownership of fd.
  void add_control_fd(int fd);

  // Which WebProcess and page a WKPage is, as its players report it.
  static video::PageKey page_key(WKPageRef page);

  // Window damage of the page's players since the last call (clipped to the
  // content area below content_top).
  display::Region take_damage(const video::PageKey& page, int content_top, int width, int height);
  // The page's frames, locked while the result lives (compose under it).
  video::VideoServer::Frames acquire(const video::PageKey& page) { return server_->acquire(page); }
  // Compositor layers of acquired frames.
  static std::vector<display::VideoLayer> layers(const video::VideoServer::Frames& frames,
                                                 int content_top, int width, int height);

 private:
  Ps5VideoOverlay() = default;
  std::unique_ptr<video::VideoServer> server_;
};

}  // namespace prosperoweb::frontend
