// SPDX-License-Identifier: GPL-3.0-or-later
//
// Video overlay server of the UI process. Each WebProcess media player in
// overlay mode connects here (protocol: webkit-port/media/ffmpeg/
// FFmpegVideoOverlayWire.h), reports where its video box is in its page's
// view, and announces every frame it presents. The server keeps, per player,
// the newest frame scaled to that box, ready for the compositor, and the
// view-coordinate damage the compositor must recompose.
//
// One thread polls the listening socket and every client: it maps the
// clients' shared-memory segments, copies announced frames out of them (or
// receives them inline), scales them (display/scale.h) and swaps the result
// in under the frames lock. The compositor holds that lock only while it
// composes (Frames below).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "damage_region.h"
#include "scale.h"

namespace ps5browser::video {

struct ServerOptions {
  int port = 17773;  // 0 = any free port (tests)
  display::ScaleFilter filter = display::ScaleFilter::Bilinear;
  // Called on the server thread whenever there is something new to compose
  // (a frame, a moved or hidden player). Must be thread-safe and quick.
  std::function<void()> on_update;
  // Log interval of the statistics line (0 = never).
  int stats_interval_ms = 5000;
};

// The identity of a page: its WebProcess and the page's WebCore identifier.
struct PageKey {
  std::uint32_t pid = 0;
  std::uint64_t page = 0;
  friend bool operator==(const PageKey&, const PageKey&) = default;
};

class VideoServer {
 public:
  explicit VideoServer(ServerOptions options);
  ~VideoServer();
  VideoServer(const VideoServer&) = delete;
  VideoServer& operator=(const VideoServer&) = delete;

  // Binds 127.0.0.1:port and starts the thread. False if it cannot listen.
  bool start();
  void stop();
  int port() const { return port_; }

  struct Layer {
    display::Rect view_rect;  // the video box in the page's view coordinates
    const std::uint32_t* pixels = nullptr;  // view_rect.w x view_rect.h ARGB32
    int stride = 0;
    std::uint32_t player_key = 0;
  };

  // The frames of the visible players of one page, locked for reading while
  // this object lives (the server thread waits to swap in newer frames).
  class Frames {
   public:
    const std::vector<Layer>& layers() const { return layers_; }
    // Frames swapped in since the previous acquire() (for the statistics).
    std::uint32_t new_frames() const { return new_frames_; }

   private:
    friend class VideoServer;
    std::unique_lock<std::mutex> lock_;
    std::vector<Layer> layers_;
    std::uint32_t new_frames_ = 0;
  };
  Frames acquire(const PageKey& page);

  // View-coordinate rects of `page` to recompose since the previous call
  // (frames, moves, hides). Damage of other pages is dropped: the
  // compositor repaints everything when the active page changes.
  std::vector<display::Rect> take_damage(const PageKey& page);

  std::size_t player_count();

 private:
  struct Connection;
  struct Player {
    std::uint32_t key = 0;
    std::uint32_t pid = 0;
    std::uint32_t player = 0;
    std::uint64_t page = 0;
    display::Rect rect;
    bool visible = false;
    bool fullscreen = false;
    std::vector<std::uint32_t> front;  // rect.w x rect.h, valid when has_frame
    int front_w = 0;
    int front_h = 0;
    bool has_frame = false;
    std::uint32_t new_frames = 0;
  };

  void run();
  void accept_clients();
  bool service(Connection& c);
  bool handle_message(Connection& c);
  bool map_segment(Connection& c, const std::string& name, bool named, std::uint32_t slots,
                   std::uint32_t slot_bytes, int& error);
  void unmap_segment(Connection& c);
  // Scales the connection's current source frame to its player's rect and
  // swaps it in. `fresh`: a new frame (not a rescale).
  void render(Connection& c, bool fresh);
  void close_connection(Connection& c, const char* why);
  void add_damage_locked(const Player& p);
  void log_stats();

  ServerOptions options_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::vector<std::unique_ptr<Connection>> connections_;  // server thread only
  std::uint32_t next_key_ = 1;

  std::mutex lock_;  // players_, damage_
  std::map<std::uint32_t, Player> players_;
  struct Damage {
    PageKey page;
    display::Rect rect;
  };
  std::vector<Damage> damage_;

  // Statistics (server thread).
  struct Stats {
    std::uint32_t received = 0;
    std::uint32_t rendered = 0;
    std::uint32_t torn = 0;
    double scale_ms = 0;
    double scale_max_ms = 0;
    std::uint64_t tcp_bytes = 0;
    int src_w = 0, src_h = 0, dst_w = 0, dst_h = 0;
  } stats_;
  std::int64_t stats_start_ms_ = 0;
};

}  // namespace ps5browser::video
