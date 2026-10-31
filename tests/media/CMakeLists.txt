# Host tests for the engine-independent parts of the ffmpeg media backend
# (webkit-port/media/ffmpeg/FFmpeg{SupportTable,TimeRanges,AVClock,RingBuffer,
# PlaybackPolicy}.h). The backend itself only builds inside WebCore.
add_executable(ps5browser_media_tests
  main.cpp
  test_ffmpeg_media.cpp
)
target_link_libraries(ps5browser_media_tests PRIVATE ps5browser_warnings)
target_include_directories(ps5browser_media_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg")
target_include_directories(ps5browser_media_tests SYSTEM PRIVATE "${PS5BROWSER_THIRD_PARTY_DIR}")

foreach(suite IN ITEMS media_support media_ranges media_clock media_ring media_policy)
  add_test(NAME media.${suite}
           COMMAND ps5browser_media_tests --test-suite=${suite} --no-version)
endforeach()
