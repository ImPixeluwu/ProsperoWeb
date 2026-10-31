# Video overlay server of the UI process: WebProcess media players send
# their frames and video boxes over loopback TCP plus shared memory
# (webkit-port/media/ffmpeg/FFmpegVideoOverlayWire.h); the compositor draws
# them under the page. Portable (POSIX, no SDL/WebKit), so the host tests
# cover it. Logs through ps5_log_printf, which the executable (or the test
# binary) defines.
find_package(Threads REQUIRED)
add_library(prosperobrowser_video_server STATIC
  video_server.cpp
)
add_library(prosperobrowser::video_server ALIAS prosperobrowser_video_server)
target_include_directories(prosperobrowser_video_server PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg")
target_compile_features(prosperobrowser_video_server PUBLIC cxx_std_20)
target_link_libraries(prosperobrowser_video_server
  PUBLIC Threads::Threads prosperobrowser::display
  PRIVATE prosperobrowser_warnings)
# shm_open() lives in librt on older glibc hosts.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(prosperobrowser_video_server PUBLIC rt)
endif()
