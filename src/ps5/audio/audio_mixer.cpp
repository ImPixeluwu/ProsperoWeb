// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_mixer.h"

#include <algorithm>

namespace prosperoweb::audio {

Mixer::Mixer(std::size_t capacity_frames)
    : capacity_frames_(std::max<std::size_t>(capacity_frames, 1)),
      scratch_(4096 * kChannels) {}

std::uint32_t Mixer::add_stream() {
  std::lock_guard<std::mutex> lock(lock_);
  const std::uint32_t id = next_id_++;
  streams_[id] = std::make_unique<Stream>(capacity_frames_ * kChannels);
  return id;
}

void Mixer::remove_stream(std::uint32_t id) {
  std::unique_ptr<Stream> doomed;
  std::lock_guard<std::mutex> lock(lock_);
  auto it = streams_.find(id);
  if (it != streams_.end()) {
    doomed = std::move(it->second);
    streams_.erase(it);
  }
}

std::size_t Mixer::write(std::uint32_t id, const float* samples, std::size_t frames) {
  std::lock_guard<std::mutex> lock(lock_);
  auto it = streams_.find(id);
  if (it == streams_.end()) {
    return 0;
  }
  Stream& s = *it->second;
  const std::size_t room = s.ring.freeSpace() / kChannels;
  const std::size_t accepted = std::min(room, frames);
  s.ring.write(samples, accepted * kChannels);
  if (accepted < frames) {
    s.status.overflowed += frames - accepted;
    s.status.retired += frames - accepted;
  }
  if (accepted) {
    s.primed = true;
  }
  return accepted;
}

void Mixer::flush(std::uint32_t id) {
  std::lock_guard<std::mutex> lock(lock_);
  auto it = streams_.find(id);
  if (it == streams_.end()) {
    return;
  }
  Stream& s = *it->second;
  s.status.retired += s.ring.size() / kChannels;
  s.ring.clear();
  s.primed = false;
}

void Mixer::set_paused(std::uint32_t id, bool paused) {
  std::lock_guard<std::mutex> lock(lock_);
  auto it = streams_.find(id);
  if (it != streams_.end()) {
    it->second->paused = paused;
  }
}

void Mixer::set_volume(std::uint32_t id, float gain) {
  std::lock_guard<std::mutex> lock(lock_);
  auto it = streams_.find(id);
  if (it != streams_.end()) {
    it->second->volume = std::clamp(gain, 0.0f, 1.0f);
  }
}

bool Mixer::status(std::uint32_t id, Status& out) const {
  std::lock_guard<std::mutex> lock(lock_);
  auto it = streams_.find(id);
  if (it == streams_.end()) {
    return false;
  }
  const Stream& s = *it->second;
  out = s.status;
  out.queued = static_cast<std::uint32_t>(s.ring.size() / kChannels);
  out.paused = s.paused;
  return true;
}

std::size_t Mixer::stream_count() const {
  std::lock_guard<std::mutex> lock(lock_);
  return streams_.size();
}

void Mixer::mix(float* out, std::size_t frames) {
  const std::size_t samples = frames * kChannels;
  std::fill(out, out + samples, 0.0f);
  std::lock_guard<std::mutex> lock(lock_);
  if (scratch_.size() < samples) {
    scratch_.resize(samples);  // only if SDL asks for more than 4096 frames
  }
  for (auto& [id, stream] : streams_) {
    Stream& s = *stream;
    if (s.paused) {
      continue;
    }
    const std::size_t read = s.ring.read(scratch_.data(), samples);
    const float gain = s.volume;
    for (std::size_t i = 0; i < read; ++i) {
      out[i] += scratch_[i] * gain;
    }
    s.status.retired += read / kChannels;
    if (read < samples && s.primed) {
      ++s.status.underruns;
      s.primed = false;
    }
  }
  for (std::size_t i = 0; i < samples; ++i) {
    out[i] = std::clamp(out[i], -1.0f, 1.0f);
  }
}

}  // namespace prosperoweb::audio
