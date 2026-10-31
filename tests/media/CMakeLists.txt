# Host tests for the engine-independent parts of the ffmpeg media backend
# (webkit-port/media/ffmpeg/FFmpeg{SupportTable,TimeRanges,AVClock,RingBuffer,
# PlaybackPolicy}.h and mse/FFmpegMSE{Parsing,Policy}.h). The backend itself
# only builds inside WebCore.
add_executable(prosperobrowser_media_tests
  main.cpp
  test_ffmpeg_media.cpp
  test_ffmpeg_mse.cpp
)
target_link_libraries(prosperobrowser_media_tests PRIVATE prosperobrowser_warnings)
target_include_directories(prosperobrowser_media_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg"
  "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg/mse")
target_include_directories(prosperobrowser_media_tests SYSTEM PRIVATE "${PROSPEROBROWSER_THIRD_PARTY_DIR}")

foreach(suite IN ITEMS media_support media_ranges media_clock media_ring media_policy
                       media_mse_support media_mse_bytes media_mse_time media_mse_policy)
  add_test(NAME media.${suite}
           COMMAND prosperobrowser_media_tests --test-suite=${suite} --no-version)
endforeach()
