// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bookkeeping of the UI's hardware decoding (hwdec_stream.h), kept free of
// threads and platform calls so the host tests drive it directly:
//
//  - PtsQueue: the hardware decoder returns pictures in display order but
//    does not hand back the pts of each one reliably, so a picture takes
//    the smallest pts still pending (as VLC's and Kodi's PS5 decoders do),
//    or the pts the decoder did report when it is pending.
//  - FrameSlots: the picture buffers. A slot is free unless the decoder
//    holds it (it "accepted" the buffer as an output target and has not
//    returned it yet), the WebProcess may still show it (decoded and
//    announced, not yet shown past, discarded or flushed), it is the
//    picture on screen (kept for re-scaling when the video box changes), or
//    the converter is reading it.
//
// Show policy: show(pts) picks the newest held picture at or before pts,
// which becomes current; every older one is released (the WebProcess
// presenter moves forward only, and a frame it skipped is never wanted
// again).
#pragma once

#include <cstdint>
#include <limits>
#include <iterator>
#include <optional>
#include <set>
#include <vector>

namespace prosperoweb::video {

class PtsQueue {
 public:
  // Entries left behind by packets that never produced a picture are
  // dropped beyond this many.
  static constexpr std::size_t kMaxPending = 64;

  void add(std::int64_t pts) {
    pending_.insert(pts);
    while (pending_.size() > kMaxPending) {
      pending_.erase(pending_.begin());
    }
  }

  // The pts of the next picture out. `reported`: what the decoder says, used
  // when it is pending (older entries are then stale and dropped).
  std::optional<std::int64_t> take(std::optional<std::int64_t> reported = std::nullopt) {
    if (pending_.empty()) {
      return reported;
    }
    if (reported) {
      auto it = pending_.find(*reported);
      if (it != pending_.end()) {
        pending_.erase(pending_.begin(), std::next(it));
        return reported;
      }
    }
    const std::int64_t pts = *pending_.begin();
    pending_.erase(pending_.begin());
    return pts;
  }

  void clear() { pending_.clear(); }
  std::size_t size() const { return pending_.size(); }

 private:
  std::multiset<std::int64_t> pending_;
};

class FrameSlots {
 public:
  explicit FrameSlots(int count) : slots_(static_cast<std::size_t>(count > 0 ? count : 0)) {}

  int count() const { return static_cast<int>(slots_.size()); }

  // A free slot, the lowest index first; -1 when there is none.
  int find_free() const {
    for (int i = 0; i < count(); ++i) {
      if (is_free(i)) {
        return i;
      }
    }
    return -1;
  }

  bool is_free(int slot) const {
    const Slot& s = slots_[static_cast<std::size_t>(slot)];
    return !s.decoder && !s.held && !s.pins && slot != current_;
  }

  void set_decoder(int slot, bool owned) {
    if (valid(slot)) {
      slots_[static_cast<std::size_t>(slot)].decoder = owned;
    }
  }

  // After a decoder reset: it holds nothing any more.
  void reset_decoder() {
    for (Slot& s : slots_) {
      s.decoder = false;
    }
  }

  // A decoded picture the WebProcess was told about.
  void hold(int slot, std::int64_t pts) {
    if (!valid(slot)) {
      return;
    }
    Slot& s = slots_[static_cast<std::size_t>(slot)];
    s.decoder = false;
    s.held = true;
    s.pts = pts;
  }

  struct ShowResult {
    int slot = -1;         // the picture to show (-1: none at or before pts)
    bool exact = false;    // it has exactly the requested pts
    bool changed = false;  // it differs from the previous current picture
    int released = 0;      // held pictures released by this call
  };

  ShowResult show(std::int64_t pts) {
    ShowResult result;
    if (current_ >= 0 && pts < current_pts_) {
      // The player went back without a flush (a discontinuity): the held
      // pictures are of the old position and out of its reach.
      result.released = release_held();
    }
    int best = -1;
    for (int i = 0; i < count(); ++i) {
      const Slot& s = slots_[static_cast<std::size_t>(i)];
      if (s.held && s.pts <= pts &&
          (best < 0 || s.pts > slots_[static_cast<std::size_t>(best)].pts)) {
        best = i;
      }
    }
    if (best < 0) {
      // Nothing new: the current picture stays if it is the one asked for.
      if (current_ >= 0 && current_pts_ <= pts) {
        result.slot = current_;
        result.exact = current_pts_ == pts;
      }
      return result;
    }
    const std::int64_t best_pts = slots_[static_cast<std::size_t>(best)].pts;
    for (int i = 0; i < count(); ++i) {
      Slot& s = slots_[static_cast<std::size_t>(i)];
      if (s.held && s.pts <= best_pts) {
        s.held = false;
        if (i != best) {
          ++result.released;
        }
      }
    }
    result.changed = best != current_;
    current_ = best;
    current_pts_ = best_pts;
    result.slot = best;
    result.exact = best_pts == pts;
    return result;
  }

  // The WebProcess dropped the picture with this pts.
  bool discard(std::int64_t pts) {
    for (Slot& s : slots_) {
      if (s.held && s.pts == pts) {
        s.held = false;
        return true;
      }
    }
    return false;
  }

  // A seek: every held picture goes; the one on screen stays until the next
  // show (it is what the overlay shows meanwhile).
  int release_held() {
    int released = 0;
    for (Slot& s : slots_) {
      released += s.held ? 1 : 0;
      s.held = false;
    }
    return released;
  }

  int stale_count() const {
    int n = 0;
    for (const Slot& s : slots_) {
      n += current_ >= 0 && s.held && s.pts < current_pts_ ? 1 : 0;
    }
    return n;
  }

  // Frees the held pictures older than the one on screen: the WebProcess
  // went past them (shows release older pictures, so these came late).
  int release_stale() {
    if (current_ < 0) {
      return 0;
    }
    int released = 0;
    for (Slot& s : slots_) {
      if (s.held && s.pts < current_pts_) {
        s.held = false;
        ++released;
      }
    }
    return released;
  }

  // Frees the newest held picture (the one the player needs last).
  bool evict_newest() {
    int newest = -1;
    for (int i = 0; i < count(); ++i) {
      const Slot& s = slots_[static_cast<std::size_t>(i)];
      if (s.held && (newest < 0 || s.pts > slots_[static_cast<std::size_t>(newest)].pts)) {
        newest = i;
      }
    }
    if (newest < 0) {
      return false;
    }
    slots_[static_cast<std::size_t>(newest)].held = false;
    return true;
  }

  // Frees the oldest held picture (the WebProcess never answered for it).
  bool evict_oldest() {
    int oldest = -1;
    for (int i = 0; i < count(); ++i) {
      const Slot& s = slots_[static_cast<std::size_t>(i)];
      if (s.held && (oldest < 0 || s.pts < slots_[static_cast<std::size_t>(oldest)].pts)) {
        oldest = i;
      }
    }
    if (oldest < 0) {
      return false;
    }
    slots_[static_cast<std::size_t>(oldest)].held = false;
    return true;
  }

  // The converter reads `slot`: it stays allocated until unpin().
  void pin(int slot) {
    if (valid(slot)) {
      ++slots_[static_cast<std::size_t>(slot)].pins;
    }
  }
  void unpin(int slot) {
    if (valid(slot) && slots_[static_cast<std::size_t>(slot)].pins > 0) {
      --slots_[static_cast<std::size_t>(slot)].pins;
    }
  }

  // A seek: the picture on screen stays shown, but its pts says nothing
  // about the new position (no "went back" or "stale" judgement against it).
  void forget_position() { current_pts_ = std::numeric_limits<std::int64_t>::min(); }
  // A picture is on screen and its pts is the player's position.
  bool has_position() const {
    return current_ >= 0 && current_pts_ != std::numeric_limits<std::int64_t>::min();
  }

  // Forget the picture on screen (the overlay was cleared, the stream ends).
  void drop_current() { current_ = -1; }

  int current() const { return current_; }
  // The pts of the picture on screen (meaningful when current() >= 0).
  std::int64_t current_pts() const { return current_pts_; }
  int held_count() const {
    int n = 0;
    for (const Slot& s : slots_) {
      n += s.held ? 1 : 0;
    }
    return n;
  }
  int decoder_count() const {
    int n = 0;
    for (const Slot& s : slots_) {
      n += s.decoder ? 1 : 0;
    }
    return n;
  }

 private:
  struct Slot {
    bool decoder = false;
    bool held = false;
    int pins = 0;
    std::int64_t pts = 0;
  };

  bool valid(int slot) const { return slot >= 0 && slot < count(); }

  std::vector<Slot> slots_;
  int current_ = -1;
  std::int64_t current_pts_ = 0;
};

// What the decode thread does when every picture slot is taken and none was
// freed for a while (HwStream::wait_for_free_slot). Held pictures ahead of
// the one on screen are frames the WebProcess will show: a paused player or
// one whose clock trails the decoder (resuming a hidden tab, a quality
// switch) frees them later, so the decoder waits (the client's credit stops
// the packets meanwhile). Evicting the oldest instead loses exactly the
// frame shown next, and the picture freezes while the audio goes on.
//
// Waiting is bounded all the same: with no show at all for kNoShowMs (a
// seek whose flush never came: the player waits for a first frame of the new
// position the decoder cannot make) the newest held picture goes, so the
// decoder takes one more packet and the client's credit flows again. A
// paused player loses one far-ahead frame per kNoShowMs that way.
//
// Pure, with the time passed in (tests drive it with a fake clock).
class SlotWaitPolicy {
 public:
  enum class Action {
    Wait,          // the player frees them in time
    ReleaseStale,  // free the held pictures older than the one on screen
    ReleaseHeld,   // hidden: nothing is presented, free every held picture
    EvictOldest,   // the player shows frames but never these: a leak
    EvictNewest,   // no show at all for kNoShowMs: unblock the decoder
  };
  // Shows arriving all along without freeing a slot for this long mean the
  // held pictures will never be shown.
  static constexpr std::int64_t kLeakMs = 10000;
  // No show at all for this long: free one slot.
  static constexpr std::int64_t kNoShowMs = 3000;

  // A wait for a slot begins, or starts over after an eviction (`shows`:
  // the stream's show counter).
  void start(std::int64_t now_ms, std::uint32_t shows) {
    start_ms_ = now_ms;
    shows_ = shows;
  }

  Action on_timeout(std::int64_t now_ms, std::uint32_t shows, bool hidden, int stale) const {
    if (hidden) {
      return Action::ReleaseHeld;
    }
    if (stale > 0) {
      return Action::ReleaseStale;
    }
    const std::int64_t waited = now_ms - start_ms_;
    if (shows == shows_) {
      return waited >= kNoShowMs ? Action::EvictNewest : Action::Wait;
    }
    return waited >= kLeakMs ? Action::EvictOldest : Action::Wait;
  }

 private:
  std::int64_t start_ms_ = 0;
  std::uint32_t shows_ = 0;
};

}  // namespace prosperoweb::video
