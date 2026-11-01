// SPDX-License-Identifier: GPL-3.0-or-later
#include "hwdec_stream.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>

#include "FFmpegVideoOverlayWire.h"

extern "C" void ps5_log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace prosperoweb::video {

namespace wire = WebCore::FFmpegMedia::VideoWire;

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
      .count();
}

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Stream errors in a row the decoder recovers from (reset, next keyframe)
// before the stream is handed back to the WebProcess.
constexpr int kMaxResyncs = 5;
// Good pictures after which the resync count starts again.
constexpr std::uint32_t kResyncForgiveFrames = 300;
// How often a decoder waiting for a free picture slot reconsiders
// (SlotWaitPolicy).
constexpr auto kSlotWait = std::chrono::milliseconds(500);
// A decoder may report the coded size (1088 rows for 1080p): the visible
// size the SPS declares is used when it is within this of it.
constexpr int kCodedPadding = 32;
constexpr int kStatsIntervalMs = 5000;

}  // namespace

HwStream::HwStream(std::uint32_t player_key, std::uint32_t stream_id, StreamConfig config,
                   YuvColor color, std::unique_ptr<DecoderBackend> backend,
                   HwStreamCallbacks callbacks)
    : key_(player_key),
      id_(stream_id),
      color_(color),
      backend_(std::move(backend)),
      callbacks_(std::move(callbacks)),
      slots_(backend_->slot_count()),
      pictures_(static_cast<std::size_t>(backend_->slot_count())),
      config_(std::move(config)) {
  crop_w_ = next_crop_w_ = config_.width;
  crop_h_ = next_crop_h_ = config_.height;
  stats_start_ms_ = now_ms();
  decode_thread_ = std::thread([this] { decode_loop(); });
  present_thread_ = std::thread([this] { present_loop(); });
}

HwStream::~HwStream() {
  {
    std::lock_guard<std::mutex> lock(lock_);
    stop_ = true;
  }
  decode_wake_.notify_all();
  present_wake_.notify_all();
  slot_freed_.notify_all();
  if (decode_thread_.joinable()) {
    decode_thread_.join();
  }
  if (present_thread_.joinable()) {
    present_thread_.join();
  }
}

void HwStream::packet(std::uint32_t serial, std::int64_t pts, bool keyframe,
                      const std::uint8_t* data, std::size_t size) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (serial != queue_serial_ || failed_) {
      ++consumed_;  // reported with the next "decoded"
      return;
    }
    Item item;
    item.kind = Item::Kind::Packet;
    item.serial = serial;
    item.pts = pts;
    item.keyframe = keyframe;
    item.data.assign(data, data + size);
    queue_.push_back(std::move(item));
  }
  decode_wake_.notify_one();
}

void HwStream::flush(std::uint32_t serial) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    const int held = slots_.held_count();
    std::size_t packets = 0;
    for (const Item& item : queue_) {
      packets += item.kind == Item::Kind::Packet ? 1 : 0;
    }
    char pts[32] = "none";
    if (slots_.has_position()) {
      std::snprintf(pts, sizeof(pts), "%" PRId64, slots_.current_pts());
    }
    ps5_log_printf("hwdec: stream %u flushed for seek at pts %s (dropped held %d, queue %zu; "
                   "serial %u -> %u)",
                   id_, pts, held, packets, queue_serial_, serial);
    queue_serial_ = serial;
    // Queued packets of the old position go now; they count as consumed.
    std::deque<Item> kept;
    for (Item& item : queue_) {
      if (item.kind == Item::Kind::Packet) {
        ++consumed_;
      } else if (item.kind == Item::Kind::Reconfigure) {
        kept.push_back(std::move(item));
      }
    }
    queue_.swap(kept);
    Item item;
    item.kind = Item::Kind::Flush;
    item.serial = serial;
    queue_.push_back(std::move(item));
    slots_.release_held();
    slots_.forget_position();
  }
  slot_freed_.notify_all();
  decode_wake_.notify_one();
}

void HwStream::drain(std::uint32_t serial) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    Item item;
    item.kind = Item::Kind::Drain;
    item.serial = serial;
    queue_.push_back(std::move(item));
  }
  decode_wake_.notify_one();
}

void HwStream::show(std::uint32_t serial, std::int64_t pts) {
  bool wake = false;
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (serial != queue_serial_) {
      return;
    }
    const FrameSlots::ShowResult result = slots_.show(pts);
    stats_.shows++;
    show_count_++;
    stats_.dropped += static_cast<std::uint32_t>(result.released);
    if (!result.exact) {
      stats_.late++;
    }
    if (result.slot >= 0) {
      // Also an unchanged picture: the overlay may have been cleared and
      // asks again (hole punched anew).
      show_pending_ = true;
      wake = true;
    }
  }
  slot_freed_.notify_all();
  if (wake) {
    present_wake_.notify_one();
  }
}

void HwStream::discard(std::uint32_t serial, std::int64_t pts) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (serial != queue_serial_) {
      return;
    }
    if (slots_.discard(pts)) {
      stats_.dropped++;
    }
  }
  slot_freed_.notify_all();
}

void HwStream::reconfigure(StreamConfig config) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    Item item;
    item.kind = Item::Kind::Reconfigure;
    item.config = std::move(config);
    queue_.push_back(std::move(item));
  }
  decode_wake_.notify_one();
}

void HwStream::rerender() {
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (slots_.current() < 0) {
      return;
    }
    rerender_pending_ = true;
  }
  present_wake_.notify_one();
}

void HwStream::set_visible(bool visible) {
  bool wake = false;
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (hidden_ == !visible) {
      return;
    }
    hidden_ = !visible;
    const int held = slots_.held_count();
    const bool on_screen = slots_.current() >= 0;
    std::size_t packets = 0;
    for (const Item& item : queue_) {
      packets += item.kind == Item::Kind::Packet ? 1 : 0;
    }
    if (hidden_) {
      // The player keeps naming frames (it releases nothing meanwhile
      // otherwise); none of these will be presented.
      slots_.release_held();
    } else if (on_screen) {
      // The overlay shows the last picture again until the next show().
      rerender_pending_ = true;
      wake = true;
    }
    char pts[32] = "none";
    if (slots_.has_position()) {
      std::snprintf(pts, sizeof(pts), "%" PRId64, slots_.current_pts());
    }
    ps5_log_printf("hwdec: stream %u %s at pts %s, held %d, queue %zu", id_,
                   hidden_ ? "hidden" : "shown", pts, held, packets);
  }
  slot_freed_.notify_all();
  if (wake) {
    present_wake_.notify_one();
  }
}

bool HwStream::hidden() {
  std::lock_guard<std::mutex> lock(lock_);
  return hidden_;
}

int HwStream::held_pictures() {
  std::lock_guard<std::mutex> lock(lock_);
  return slots_.held_count();
}

std::size_t HwStream::queued_packets() {
  std::lock_guard<std::mutex> lock(lock_);
  return queue_.size();
}

void HwStream::decode_loop() {
  while (true) {
    Item item;
    {
      std::unique_lock<std::mutex> lock(lock_);
      decode_wake_.wait_for(lock, std::chrono::seconds(1),
                            [this] { return stop_ || !queue_.empty(); });
      if (stop_) {
        return;
      }
      if (queue_.empty()) {
        lock.unlock();
        maybe_log_stats();
        continue;
      }
      item = std::move(queue_.front());
      queue_.pop_front();
    }
    if (failed_) {
      if (item.kind == Item::Kind::Packet) {
        ++consumed_;
      }
      continue;
    }
    switch (item.kind) {
      case Item::Kind::Packet:
        handle_packet(item);
        break;
      case Item::Kind::Flush:
        handle_flush(item.serial);
        break;
      case Item::Kind::Drain:
        handle_drain(item.serial);
        break;
      case Item::Kind::Reconfigure:
        if (item.config.codec != config_.codec) {
          fail("the codec changed");
        } else {
          ps5_log_printf("hwdec: stream %u: new parameter sets (profile %d level %d, %dx%d, "
                         "%zu bytes): decoder reset at the next keyframe",
                         id_, item.config.profile, item.config.level, item.config.width,
                         item.config.height, item.config.parameter_sets.size());
          reset_on_keyframe_ = item.config.parameter_sets != config_.parameter_sets;
          next_crop_w_ = item.config.width;
          next_crop_h_ = item.config.height;
          if (!reset_on_keyframe_) {
            crop_w_ = next_crop_w_;
            crop_h_ = next_crop_h_;
          }
          config_ = std::move(item.config);
        }
        break;
    }
    maybe_log_stats();
  }
}

void HwStream::handle_packet(Item& item) {
  if (item.serial != serial_ || (wait_keyframe_ && !item.keyframe)) {
    ++consumed_;
    send_decoded(0, 0, 0, 0);
    return;
  }
  if (!to_annexb(config_, item.data.data(), item.data.size(), item.keyframe, annexb_)) {
    ++consumed_;
    ps5_log_printf("hwdec: stream %u: malformed packet (%zu bytes, pts %" PRId64 "), dropped",
                   id_, item.data.size(), item.pts);
    send_decoded(0, 0, 0, 0);
    return;
  }
  if (item.keyframe && reset_on_keyframe_) {
    // The new parameter sets start here: the old session's pictures come
    // out first, then the decoder starts over with the new size.
    reset_on_keyframe_ = false;
    if (!drain_decoder("parameter switch")) {
      ++consumed_;
      return;
    }
    crop_w_ = next_crop_w_;
    crop_h_ = next_crop_h_;
    wait_keyframe_ = false;
    key_pts_ = item.pts;
    ps5_log_printf("hwdec: stream %u: decoder reset for the new parameter sets at pts %" PRId64
                   " (visible %dx%d)",
                   id_, item.pts, crop_w_, crop_h_);
  }
  const int target = wait_for_free_slot();
  if (target < 0) {
    ++consumed_;
    send_decoded(0, 0, 0, 0);
    return;
  }
  if (wait_keyframe_) {
    wait_keyframe_ = false;
    key_pts_ = item.pts;
  }
  pts_queue_.add(item.pts);
  const auto start = Clock::now();
  const DecoderBackend::Result result = backend_->decode(annexb_.data(), annexb_.size(), item.pts,
                                                         target);
  const double ms = ms_since(start);
  {
    std::lock_guard<std::mutex> lock(lock_);
    stats_.decode_ms += ms;
  }
  ++consumed_;
  take_result(result, target, "decode");
}

bool HwStream::take_result(const DecoderBackend::Result& result, int target, const char* what) {
  if (result.target_taken) {
    std::lock_guard<std::mutex> lock(lock_);
    slots_.set_decoder(target, true);
  }
  if (result.status != DecoderBackend::Result::Status::Ok) {
    if (result.status == DecoderBackend::Result::Status::StreamError && resyncs_ < kMaxResyncs) {
      ++resyncs_;
      const bool reset = backend_->reset();
      {
        std::lock_guard<std::mutex> lock(lock_);
        slots_.reset_decoder();
      }
      slot_freed_.notify_all();
      pts_queue_.clear();
      wait_keyframe_ = true;
      key_pts_.reset();
      ps5_log_printf("hwdec: stream %u: %s error %#x: reset%s, waiting for a keyframe (%d of %d)",
                     id_, what, result.code, reset ? "" : " FAILED", resyncs_, kMaxResyncs);
      send_decoded(0, 0, 0, 0);
      return true;
    }
    char reason[96];
    std::snprintf(reason, sizeof(reason), "%s error %#x%s", what, result.code,
                  result.status == DecoderBackend::Result::Status::StreamError
                      ? " (stream errors keep coming)"
                      : "");
    fail(reason);
    return false;
  }
  if (result.frame_slot < 0) {
    send_decoded(0, 0, 0, 0);
    return true;
  }
  const std::optional<std::int64_t> pts = pts_queue_.take(result.pts);
  // Pictures from before the keyframe decoding started at (an open GOP's
  // leading pictures) refer to pictures the decoder never got.
  const bool leading = pts && key_pts_ && *pts < *key_pts_;
  if (result.corrupt || !pts || leading) {
    {
      std::lock_guard<std::mutex> lock(lock_);
      slots_.set_decoder(result.frame_slot, false);
      stats_.dropped++;
    }
    slot_freed_.notify_all();
    send_decoded(0, 0, 0, 0);
    return true;
  }
  const Nv12View picture = visible_picture(result.picture);
  {
    std::lock_guard<std::mutex> lock(lock_);
    slots_.hold(result.frame_slot, *pts);
    pictures_[static_cast<std::size_t>(result.frame_slot)] = picture;
    stats_.decoded++;
    stats_.width = picture.width;
    stats_.height = picture.height;
  }
  if (!logged_first_picture_) {
    logged_first_picture_ = true;
    ps5_log_printf("hwdec: stream %u: first picture %dx%d (pitch %d), pts %" PRId64, id_,
                   picture.width, picture.height, picture.y_pitch, *pts);
  }
  if (++good_pictures_ % kResyncForgiveFrames == 0) {
    resyncs_ = 0;
  }
  send_decoded(wire::Decoded::HasFrame, picture.width, picture.height, *pts);
  return true;
}

Nv12View HwStream::visible_picture(const Nv12View& picture) {
  Nv12View v = picture;
  if (crop_w_ <= 0 || crop_h_ <= 0) {
    return v;
  }
  const bool fits = crop_w_ <= v.width && crop_h_ <= v.height &&
                    v.width - crop_w_ < kCodedPadding && v.height - crop_h_ < kCodedPadding;
  if (!fits) {
    if (!logged_crop_mismatch_) {
      logged_crop_mismatch_ = true;
      ps5_log_printf("hwdec: stream %u: picture %dx%d does not match the parameter sets' %dx%d;"
                     " shown as decoded",
                     id_, v.width, v.height, crop_w_, crop_h_);
    }
    return v;
  }
  v.width = crop_w_ & ~1;
  v.height = crop_h_ & ~1;
  return v;
}

void HwStream::handle_flush(std::uint32_t serial) {
  serial_ = serial;
  const bool reset = backend_->reset();
  {
    std::lock_guard<std::mutex> lock(lock_);
    slots_.reset_decoder();
    slots_.release_held();
    slots_.forget_position();
  }
  slot_freed_.notify_all();
  if (!reset) {
    ps5_log_printf("hwdec: stream %u: reset failed on flush", id_);
  }
  pts_queue_.clear();
  wait_keyframe_ = true;
  key_pts_.reset();
  // The client may be out of credit: tell it what was consumed.
  send_decoded(0, 0, 0, 0);
}

void HwStream::handle_drain(std::uint32_t serial) {
  if (serial != serial_) {
    return;
  }
  if (!drain_decoder("flush")) {
    return;
  }
  send_decoded(wire::Decoded::Drained, 0, 0, 0);
}

bool HwStream::drain_decoder(const char* what) {
  for (int i = 0; i < 64; ++i) {
    const int target = wait_for_free_slot();
    if (target < 0) {
      break;
    }
    const DecoderBackend::Result result = backend_->flush(target);
    if (result.target_taken) {
      std::lock_guard<std::mutex> lock(lock_);
      slots_.set_decoder(target, true);
    }
    if (result.status != DecoderBackend::Result::Status::Ok || result.frame_slot < 0) {
      break;
    }
    // A picture: announce it (the "decoded" message per picture).
    DecoderBackend::Result picture = result;
    picture.target_taken = false;
    if (!take_result(picture, target, what)) {
      return false;
    }
  }
  // A drained decoder wants a reset before it takes more (a loop, a seek).
  backend_->reset();
  {
    std::lock_guard<std::mutex> lock(lock_);
    slots_.reset_decoder();
  }
  slot_freed_.notify_all();
  pts_queue_.clear();
  wait_keyframe_ = true;
  key_pts_.reset();
  return true;
}

int HwStream::wait_for_free_slot() {
  std::unique_lock<std::mutex> lock(lock_);
  SlotWaitPolicy policy;
  policy.start(now_ms(), show_count_);
  bool logged = false;
  while (true) {
    if (stop_ || queue_serial_ != serial_) {
      return -1;
    }
    const int slot = slots_.find_free();
    if (slot >= 0) {
      return slot;
    }
    if (slot_freed_.wait_for(lock, kSlotWait) != std::cv_status::timeout) {
      continue;
    }
    if (slots_.find_free() >= 0) {
      continue;
    }
    if (slots_.held_count() == 0) {
      break;  // the decoder (and the picture on screen) has them all
    }
    switch (policy.on_timeout(now_ms(), show_count_, hidden_, slots_.stale_count())) {
      case SlotWaitPolicy::Action::Wait:
        if (!logged) {
          logged = true;
          ps5_log_printf("hwdec: stream %u: every picture slot taken (held %d, decoder %d): "
                         "waiting for the player to show them",
                         id_, slots_.held_count(), slots_.decoder_count());
        }
        continue;
      case SlotWaitPolicy::Action::ReleaseStale:
        stats_.dropped += static_cast<std::uint32_t>(slots_.release_stale());
        continue;
      case SlotWaitPolicy::Action::ReleaseHeld:
        stats_.dropped += static_cast<std::uint32_t>(slots_.release_held());
        continue;
      case SlotWaitPolicy::Action::EvictNewest:
        if (slots_.evict_newest()) {
          stats_.evicted++;
          ps5_log_printf("hwdec: stream %u: no show for %lld ms with every slot held (%d): "
                         "freeing the newest picture",
                         id_, static_cast<long long>(SlotWaitPolicy::kNoShowMs),
                         slots_.held_count() + 1);
        }
        policy.start(now_ms(), show_count_);
        continue;
      case SlotWaitPolicy::Action::EvictOldest:
        if (slots_.evict_oldest()) {
          stats_.evicted++;
          ps5_log_printf("hwdec: stream %u: the player shows frames but none of the %d held "
                         "for %lld ms: evicting the oldest",
                         id_, slots_.held_count(),
                         static_cast<long long>(SlotWaitPolicy::kLeakMs));
          policy.start(now_ms(), show_count_);
          continue;
        }
        continue;
    }
  }
  lock.unlock();
  fail("out of picture slots (the decoder holds them all)");
  return -1;
}

void HwStream::send_decoded(std::uint8_t flags, int width, int height, std::int64_t pts) {
  if (!callbacks_.send) {
    return;
  }
  wire::Decoded decoded;
  decoded.stream = id_;
  decoded.serial = serial_;
  decoded.consumed = consumed_.load();
  decoded.flags = flags;
  decoded.width = static_cast<std::uint32_t>(width);
  decoded.height = static_cast<std::uint32_t>(height);
  decoded.pts = pts;
  std::vector<std::uint8_t> out;
  wire::appendDecoded(out, decoded);
  callbacks_.send(std::move(out));
}

void HwStream::fail(const std::string& reason) {
  if (failed_.exchange(true)) {
    return;
  }
  ps5_log_printf("hwdec: stream %u (player key %u): %s; the WebProcess decodes in software", id_,
                 key_, reason.c_str());
  if (callbacks_.send) {
    std::vector<std::uint8_t> out;
    wire::appendStreamFailure(out, wire::Type::StreamError, wire::StreamFailure{id_, reason});
    callbacks_.send(std::move(out));
  }
}

void HwStream::present_loop() {
  std::vector<std::uint32_t> argb;
  std::vector<std::int32_t> scratch;
  while (true) {
    int slot = -1;
    Nv12View picture;
    {
      std::unique_lock<std::mutex> lock(lock_);
      present_wake_.wait(lock, [this] { return stop_ || show_pending_ || rerender_pending_; });
      if (stop_) {
        return;
      }
      show_pending_ = rerender_pending_ = false;
      slot = slots_.current();
      if (slot < 0) {
        continue;
      }
      slots_.pin(slot);
      picture = pictures_[static_cast<std::size_t>(slot)];
    }
    int box_w = 0;
    int box_h = 0;
    if (callbacks_.target_size) {
      callbacks_.target_size(box_w, box_h);
    }
    const int step =
        box_w > 0 && box_h > 0 && box_w * 2 <= picture.width && box_h * 2 <= picture.height ? 2
                                                                                            : 1;
    const int out_w = picture.width / step;
    const int out_h = picture.height / step;
    argb.resize(static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h));
    const auto start = Clock::now();
    nv12_to_argb(picture, color_, step, argb.data(), out_w, scratch);
    const double ms = ms_since(start);
    {
      std::lock_guard<std::mutex> lock(lock_);
      slots_.unpin(slot);
      stats_.presented++;
      stats_.convert_ms += ms;
      stats_.convert_max_ms = std::max(stats_.convert_max_ms, ms);
      stats_.out_w = out_w;
      stats_.out_h = out_h;
    }
    slot_freed_.notify_all();
    if (callbacks_.deliver && out_w > 0 && out_h > 0) {
      callbacks_.deliver(display::ImageView{argb.data(), out_w, out_h, out_w});
    }
  }
}

void HwStream::maybe_log_stats() {
  const std::int64_t now = now_ms();
  Stats stats;
  std::size_t queued = 0;
  int held = 0;
  int in_decoder = 0;
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (now - stats_start_ms_ < kStatsIntervalMs) {
      return;
    }
    stats = stats_;
    stats_ = Stats{};
    queued = queue_.size();
    held = slots_.held_count();
    in_decoder = slots_.decoder_count();
  }
  const double seconds = static_cast<double>(now - stats_start_ms_) / 1000.0;
  stats_start_ms_ = now;
  if (!stats.decoded && !stats.shows) {
    return;
  }
  ps5_log_printf(
      "hwdec: stats %.1fs: decoded %.1f fps (%dx%d, decode call %.2f ms), convert %.2f/%.2f ms "
      "-> %dx%d, queue %zu, held %d, in decoder %d, presented %u, shows %u, late %u, dropped %u, "
      "evicted %u",
      seconds, stats.decoded / seconds, stats.width, stats.height,
      stats.decoded ? stats.decode_ms / stats.decoded : 0.0,
      stats.presented ? stats.convert_ms / stats.presented : 0.0, stats.convert_max_ms,
      stats.out_w, stats.out_h, queued, held, in_decoder, stats.presented, stats.shows,
      stats.late, stats.dropped, stats.evicted);
}

}  // namespace prosperoweb::video
