# Video overlay server of the UI process: WebProcess media players send
# their frames and video boxes over loopback TCP plus shared memory
# (webkit-port/media/ffmpeg/FFmpegVideoOverlayWire.h), or their compressed
# video for the UI to decode (hwdec_*: the platform decoder itself is
# plugged in by the frontend, src/frontend/ps5/ps5_hwdec.cpp); the compositor
# draws them under the page. Portable (POSIX, no SDL/WebKit), so the host
# tests cover it. Logs through ps5_log_printf, which the executable (or the
# test binary) defines.
find_package(Threads REQUIRED)
add_library(prosperoweb_video_server STATIC
  video_server.cpp
  hwdec_bitstream.cpp
  hwdec_stream.cpp
  nv12_convert.cpp
)
# The console's CPU is Zen 2: let the converter's loops use AVX2.
if(PS5)
  set_source_files_properties(nv12_convert.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
endif()
add_library(prosperoweb::video_server ALIAS prosperoweb_video_server)
target_include_directories(prosperoweb_video_server PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg")
target_compile_features(prosperoweb_video_server PUBLIC cxx_std_20)
target_link_libraries(prosperoweb_video_server
  PUBLIC Threads::Threads prosperoweb::display
  PRIVATE prosperoweb_warnings)
# shm_open() lives in librt on older glibc hosts.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(prosperoweb_video_server PUBLIC rt)
endif()
