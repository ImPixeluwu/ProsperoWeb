// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mixer of the UI-process audio server: one ring buffer per stream
// (interleaved float32 stereo, 48 kHz), summed with per-stream gain and
// clipped to [-1, 1]. mix() runs on the output thread (SDL's audio callback);
// everything else on the server thread. One mutex, held only for copies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "FFmpegRingBuffer.h"

namespace prosperoweb::audio {

class Mixer {
 public:
  static constexpr int kChannels = 2;
  static constexpr int kRate = 48000;

  explicit Mixer(std::size_t capacity_frames = kRate);

  struct Status {
    std::uint64_t retired = 0;     // mixed + flushed + overflowed frames
    std::uint32_t queued = 0;      // frames in the ring
    std::uint32_t underruns = 0;   // times the ring ran dry while playing
    std::uint64_t overflowed = 0;  // frames dropped because the ring was full
    bool paused = false;
  };

  std::uint32_t add_stream();
  void remove_stream(std::uint32_t id);
  // Queues whole frames; frames that do not fit are dropped and counted as
  // retired (the client then sees them as played). Returns frames queued.
  std::size_t write(std::uint32_t id, const float* samples, std::size_t frames);
  void flush(std::uint32_t id);
  void set_paused(std::uint32_t id, bool paused);
  void set_volume(std::uint32_t id, float gain);
  bool status(std::uint32_t id, Status& out) const;
  std::size_t stream_count() const;
  std::size_t capacity_frames() const { return capacity_frames_; }

  // Output thread: fills `frames` interleaved stereo frames.
  void mix(float* out, std::size_t frames);

 private:
  struct Stream {
    explicit Stream(std::size_t capacity) : ring(capacity) {}
    WebCore::FFmpegMedia::RingBuffer<float> ring;
    float volume = 1.0f;
    bool paused = false;
    bool primed = false;  // has data since the last underrun or flush
    Status status;
  };

  std::size_t capacity_frames_;
  mutable std::mutex lock_;
  std::map<std::uint32_t, std::unique_ptr<Stream>> streams_;
  std::uint32_t next_id_ = 1;
  std::vector<float> scratch_;
};

}  // namespace prosperoweb::audio
