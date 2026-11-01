// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_audio_server.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <unistd.h>

#include "FFmpegAudioWire.h"
#include "ps5_log.h"

namespace prosperoweb::frontend {

namespace {

namespace wire = WebCore::FFmpegMedia::AudioWire;

// -1 = disabled.
int configured_port() {
  const char* value = std::getenv("PROSPEROWEB_AUDIO_PORT");
  if (!value || !*value) {
    return wire::defaultPort;
  }
  if (!std::strcmp(value, "off") || !std::strcmp(value, "0")) {
    return -1;
  }
  char* end = nullptr;
  const long port = std::strtol(value, &end, 10);
  if (*end || port < 0 || port > 65535) {
    log_printf("audio server: PROSPEROWEB_AUDIO_PORT=%s is not a port; disabled", value);
    return -1;
  }
  return static_cast<int>(port);
}

}  // namespace

std::unique_ptr<Ps5AudioServer> Ps5AudioServer::start() {
  const int port = configured_port();
  if (port < 0) {
    log_line("audio server: disabled (PROSPEROWEB_AUDIO_PORT)");
    return nullptr;
  }
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    log_printf("audio server: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
    return nullptr;
  }
  std::unique_ptr<Ps5AudioServer> self(new Ps5AudioServer());
  SDL_AudioSpec desired{};
  desired.freq = audio::Mixer::kRate;
  desired.format = AUDIO_F32SYS;
  desired.channels = audio::Mixer::kChannels;
  desired.samples = 1024;
  desired.callback = callback;
  desired.userdata = self.get();
  SDL_AudioSpec obtained{};
  // No allowed changes: SDL converts to the device format itself.
  self->device_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
  const char* driver = SDL_GetCurrentAudioDriver();
  if (!self->device_) {
    log_printf("audio server: SDL_OpenAudioDevice (driver %s) failed: %s",
               driver ? driver : "(none)", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return nullptr;
  }
  log_printf("audio server: device opened: SDL driver %s, device %u, %d Hz format 0x%x %d ch "
             "%d samples",
             driver ? driver : "(none)", static_cast<unsigned>(self->device_), obtained.freq,
             static_cast<unsigned>(obtained.format), obtained.channels, obtained.samples);

  audio::ServerOptions options;
  options.port = port;
  // The callback fills one buffer while the device plays the previous one.
  options.output_latency_frames = 2u * obtained.samples;
  // Keep serving even if listen() is refused (media category): clients then
  // arrive over control sockets (ps5_spawn_set_media_control).
  options.listen_optional = true;
  self->server_ = std::make_unique<audio::AudioServer>(self->mixer_, options);
  if (!self->server_->start()) {
    return nullptr;  // the destructor closes the device
  }
  SDL_PauseAudioDevice(self->device_, 0);
  return self;
}

bool Ps5AudioServer::listening() const { return server_ && server_->listening(); }

void Ps5AudioServer::add_control_fd(int fd) {
  if (server_) {
    server_->add_control_fd(fd);
  } else {
    ::close(fd);
  }
}

Ps5AudioServer::~Ps5AudioServer() {
  if (server_) {
    server_->stop();
  }
  if (device_) {
    // Returns once no callback is running.
    SDL_CloseAudioDevice(device_);
  }
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void Ps5AudioServer::callback(void* user, Uint8* stream, int length) {
  auto* self = static_cast<Ps5AudioServer*>(user);
  const std::size_t frames =
      static_cast<std::size_t>(length) / (sizeof(float) * audio::Mixer::kChannels);
  self->mixer_.mix(reinterpret_cast<float*>(stream), frames);
}

void Ps5AudioServer::play_test_tone() {
  constexpr int kRate = audio::Mixer::kRate;
  constexpr double kPi = 3.14159265358979323846;
  std::vector<float> samples(static_cast<std::size_t>(kRate) * audio::Mixer::kChannels);
  for (int i = 0; i < kRate; ++i) {
    // 440 Hz at -12 dBFS with 10 ms fades, so it does not click.
    const double fade = std::min({1.0, i / 480.0, (kRate - i) / 480.0});
    const float v = static_cast<float>(0.25 * fade * std::sin(2 * kPi * 440.0 * i / kRate));
    samples[2 * i] = v;
    samples[2 * i + 1] = v;
  }
  log_line("audio server: playing the test tone (1 s, 440 Hz)");
  server_->play_local(samples, "audio server: test tone played");
}

}  // namespace prosperoweb::frontend
