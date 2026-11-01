// SPDX-License-Identifier: GPL-3.0-or-later
//
// UI-process audio output. The PS5 mixes only the foreground application's
// audio, so the WebProcess (a separate payload process whose sceAudioOut
// port is drained but never heard) forwards its media PCM to this server,
// which plays every stream through one SDL device opened here
// (src/ps5/audio, protocol in webkit-port/media/ffmpeg/FFmpegAudioWire.h).
//
// Env: PROSPEROWEB_AUDIO_PORT (default 17772, exported to the WebProcess so
// its RemoteAudioSink finds the server; "off" disables the server and the
// forwarding),
// PROSPEROWEB_AUDIO_TEST=1 plays a 1 s 440 Hz tone at startup.
#pragma once

#include <memory>

#include <SDL.h>

#include "audio_mixer.h"
#include "audio_server.h"

namespace prosperoweb::frontend {

class Ps5AudioServer {
 public:
  // Opens the device and starts listening; nullptr (logged) on failure or
  // when disabled. Requires SDL_Init to have run.
  static std::unique_ptr<Ps5AudioServer> start();
  ~Ps5AudioServer();

  void play_test_tone();

  // Whether the server is listening on its TCP port. False under the media
  // application-category sandbox, where clients must arrive over a control
  // socket (add_control_fd).
  bool listening() const;
  // Adopts a WebProcess control socket (ps5_spawn_set_media_control). Takes
  // ownership of fd.
  void add_control_fd(int fd);

 private:
  Ps5AudioServer() = default;
  static void callback(void* user, Uint8* stream, int length);

  audio::Mixer mixer_;
  std::unique_ptr<audio::AudioServer> server_;
  SDL_AudioDeviceID device_ = 0;
};

}  // namespace prosperoweb::frontend
