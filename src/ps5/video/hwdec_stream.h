// SPDX-License-Identifier: GPL-3.0-or-later
//
// One remotely decoded video stream in the UI process (protocol: the
// remote-decoding messages of webkit-port/media/ffmpeg/
// FFmpegVideoOverlayWire.h). The WebProcess player keeps demuxing, MSE,
// the A/V clock and the choice of the frame to show; it sends the
// compressed packets here and names the frame to show by its pts. This
// object:
//
//  - queues the packets (bounded by the client's credit: it never has more
//    than a few dozen unacknowledged packets in flight),
//  - decodes them on its own thread with a DecoderBackend (sceVideodec2 on
//    the console, src/frontend/ps5/ps5_hwdec.cpp; a fake in the tests),
//    converting AVCC to Annex-B first (hwdec_bitstream.h),
//  - keeps the decoded NV12 pictures in the backend's slots until the
//    WebProcess shows or drops them (hwdec_policy.h), and tells it about
//    every picture ("decoded"),
//  - converts the shown picture to ARGB on a second thread
//    (nv12_convert.h) and hands it to the overlay server, which scales it
//    into the player's video box like a frame the WebProcess sent.
//
// Nothing runs on the server's poll thread except queueing.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "hwdec_bitstream.h"
#include "hwdec_policy.h"
#include "nv12_convert.h"
#include "scale.h"

namespace prosperoweb::video {

// What a backend is asked to decode.
struct DecoderRequest {
  StreamConfig config;  // codec, profile/level, bit depth (hwdec_bitstream.h)
  int width = 0;        // as the container says (0: unknown)
  int height = 0;
};

// A hardware (or fake) decoder with a fixed set of picture slots. Called
// from the stream's decode thread only.
class DecoderBackend {
 public:
  virtual ~DecoderBackend() = default;

  virtual int slot_count() const = 0;

  struct Result {
    enum class Status { Ok, StreamError, Fatal };
    Status status = Status::Ok;
    std::uint32_t code = 0;          // the platform's result code, for the log
    bool target_taken = false;       // the decoder keeps `target` until it outputs it
    int frame_slot = -1;             // a picture came out in this slot
    bool corrupt = false;            // ... damaged: release it, do not show it
    std::optional<std::int64_t> pts; // the picture's pts, when the decoder says
    Nv12View picture;                // valid while the slot is not reused
  };

  // One Annex-B access unit; `target` is a free slot the decoder may write
  // a picture to.
  virtual Result decode(const std::uint8_t* au, std::size_t size, std::int64_t pts,
                        int target) = 0;
  // One picture still inside the decoder (end of stream); frame_slot -1
  // when there is none left.
  virtual Result flush(int target) = 0;
  // Forget everything (a seek): afterwards the decoder holds no slot and
  // wants a keyframe.
  virtual bool reset() = 0;
  // Largest picture the decoder was made for.
  virtual int max_width() const = 0;
  virtual int max_height() const = 0;
};

using DecoderFactory =
    std::function<std::unique_ptr<DecoderBackend>(const DecoderRequest&, std::string& reason)>;

struct HwStreamCallbacks {
  // A message for the client (complete wire message). Any thread.
  std::function<void(std::vector<std::uint8_t>&&)> send;
  // A converted picture for the player's video box (the present thread).
  std::function<void(const display::ImageView&)> deliver;
  // The player's video box size, to pick the 2x reduction for 4K.
  std::function<void(int& width, int& height)> target_size;
};

class HwStream {
 public:
  HwStream(std::uint32_t player_key, std::uint32_t stream_id, StreamConfig config,
           YuvColor color, std::unique_ptr<DecoderBackend> backend, HwStreamCallbacks callbacks);
  ~HwStream();
  HwStream(const HwStream&) = delete;
  HwStream& operator=(const HwStream&) = delete;

  std::uint32_t id() const { return id_; }
  // The stream gave up (stream-error was sent): the server destroys it.
  bool failed() const { return failed_.load(); }

  // From the server's poll thread; all non-blocking.
  void packet(std::uint32_t serial, std::int64_t pts, bool keyframe, const std::uint8_t* data,
              std::size_t size);
  void flush(std::uint32_t serial);
  void drain(std::uint32_t serial);
  void show(std::uint32_t serial, std::int64_t pts);
  void discard(std::uint32_t serial, std::int64_t pts);
  void reconfigure(StreamConfig config);
  // Converts the picture on screen again (the video box changed size).
  void rerender();
  // The player's video box was hidden (page hidden, another tab) or shown
  // again. Hidden: nothing is presented and the held pictures are freed so
  // the decoder never blocks on them; shown: the picture on screen is
  // presented again and the next show() continues from the player's clock.
  void set_visible(bool visible);

  // For tests and logs.
  bool hidden();
  int held_pictures();

  // Pending packets, for tests.
  std::size_t queued_packets();

 private:
  struct Item {
    enum class Kind { Packet, Flush, Drain, Reconfigure } kind = Kind::Packet;
    std::uint32_t serial = 0;
    std::int64_t pts = 0;
    bool keyframe = false;
    std::vector<std::uint8_t> data;
    StreamConfig config;
  };

  void decode_loop();
  void present_loop();
  void handle_packet(Item& item);
  void handle_flush(std::uint32_t serial);
  void handle_drain(std::uint32_t serial);
  // Records what a backend call returned. False when the stream failed.
  bool take_result(const DecoderBackend::Result& result, int target, const char* what);
  int wait_for_free_slot();
  // Outputs what the decoder still holds (announcing the pictures), then
  // resets it. False when the stream failed.
  bool drain_decoder(const char* what);
  // The picture as shown: cropped to the SPS's visible size.
  Nv12View visible_picture(const Nv12View& picture);
  void send_decoded(std::uint8_t flags, int width, int height, std::int64_t pts);
  void fail(const std::string& reason);
  void maybe_log_stats();

  const std::uint32_t key_;
  const std::uint32_t id_;
  const YuvColor color_;
  std::unique_ptr<DecoderBackend> backend_;
  HwStreamCallbacks callbacks_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false};
  std::atomic<std::uint32_t> consumed_{0};

  std::mutex lock_;
  std::condition_variable decode_wake_;
  std::condition_variable present_wake_;
  std::condition_variable slot_freed_;
  std::deque<Item> queue_;              // lock_
  std::uint32_t queue_serial_ = 0;      // lock_: newest flush seen by the poll thread
  FrameSlots slots_;                    // lock_
  std::vector<Nv12View> pictures_;      // lock_: per slot
  bool show_pending_ = false;           // lock_
  bool rerender_pending_ = false;       // lock_
  bool hidden_ = false;                 // lock_
  std::uint32_t show_count_ = 0;        // lock_: show() calls, for SlotWaitPolicy

  // Decode thread only.
  StreamConfig config_;
  PtsQueue pts_queue_;
  std::uint32_t serial_ = 0;
  bool wait_keyframe_ = true;
  std::optional<std::int64_t> key_pts_;
  int resyncs_ = 0;
  std::uint32_t good_pictures_ = 0;
  std::vector<std::uint8_t> annexb_;
  bool logged_first_picture_ = false;
  // New parameter sets: the decoder is drained and reset at the next
  // keyframe (a resolution switch inside one decoder session fails).
  bool reset_on_keyframe_ = false;
  // The visible size pictures are cropped to (the SPS's), for the pictures
  // of the current decoder session; the new one applies after the reset.
  int crop_w_ = 0, crop_h_ = 0;
  int next_crop_w_ = 0, next_crop_h_ = 0;
  bool logged_crop_mismatch_ = false;

  // Statistics (5 s windows), lock_.
  struct Stats {
    std::uint32_t decoded = 0;
    std::uint32_t shows = 0;
    std::uint32_t presented = 0;
    std::uint32_t late = 0;
    std::uint32_t dropped = 0;  // corrupt, leading, discarded, evicted
    std::uint32_t evicted = 0;
    double decode_ms = 0;
    double convert_ms = 0;
    double convert_max_ms = 0;
    int width = 0, height = 0, out_w = 0, out_h = 0;
  } stats_;
  std::int64_t stats_start_ms_ = 0;

  std::thread decode_thread_;
  std::thread present_thread_;
};

}  // namespace prosperoweb::video
