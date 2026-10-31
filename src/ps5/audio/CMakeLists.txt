# Audio server of the UI process: WebProcess media streams arrive over
# loopback TCP (webkit-port/media/ffmpeg/FFmpegAudioWire.h) and are mixed
# into one output. Portable (POSIX sockets, no SDL): the PS5 frontend feeds
# Mixer::mix() from its SDL audio callback, the host tests from a fake one.
# Logs through ps5_log_printf, which the executable (or the test binary)
# defines.
find_package(Threads REQUIRED)
add_library(ps5browser_audio_server STATIC
  audio_mixer.cpp
  audio_server.cpp
)
add_library(ps5browser::audio_server ALIAS ps5browser_audio_server)
target_include_directories(ps5browser_audio_server PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg")
target_compile_features(ps5browser_audio_server PUBLIC cxx_std_20)
target_link_libraries(ps5browser_audio_server
  PUBLIC Threads::Threads
  PRIVATE ps5browser_warnings)
