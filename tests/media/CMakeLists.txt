# Host tests for the engine-independent parts of the ffmpeg media backend
# (webkit-port/media/ffmpeg/FFmpeg{SupportTable,TimeRanges,AVClock,RingBuffer,
# PlaybackPolicy,PresenterPolicy}.h and mse/FFmpegMSE{Parsing,Policy}.h). The backend itself
# only builds inside WebCore.
add_executable(prosperoweb_media_tests
  main.cpp
  test_ffmpeg_media.cpp
  test_ffmpeg_mse.cpp
  test_ffmpeg_presenter.cpp
)
target_link_libraries(prosperoweb_media_tests PRIVATE prosperoweb_warnings)
target_include_directories(prosperoweb_media_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg"
  "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg/mse")
target_include_directories(prosperoweb_media_tests SYSTEM PRIVATE "${PROSPEROWEB_THIRD_PARTY_DIR}")

foreach(suite IN ITEMS media_support media_ranges media_clock media_ring media_policy
                       media_mse_support media_mse_bytes media_mse_time media_mse_policy
                       media_presenter media_network)
  add_test(NAME media.${suite}
           COMMAND prosperoweb_media_tests --test-suite=${suite} --no-version)
endforeach()

# FFmpegAppendParser against the host libavformat: real fragmented MP4 and
# WebM fed in random small appends (tests/media/data). Skipped when the
# libavformat/libavcodec development files are missing.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PROSPEROWEB_HOST_FFMPEG QUIET IMPORTED_TARGET libavformat libavcodec libavutil)
endif()
if(PROSPEROWEB_HOST_FFMPEG_FOUND)
  add_executable(prosperoweb_media_stream_tests
    main.cpp
    test_ffmpeg_mse_stream.cpp
    "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg/mse/FFmpegAppendParser.cpp"
  )
  target_link_libraries(prosperoweb_media_stream_tests PRIVATE prosperoweb_warnings PkgConfig::PROSPEROWEB_HOST_FFMPEG)
  target_include_directories(prosperoweb_media_stream_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/host-config"
    "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg"
    "${CMAKE_SOURCE_DIR}/webkit-port/media/ffmpeg/mse")
  target_include_directories(prosperoweb_media_stream_tests SYSTEM PRIVATE "${PROSPEROWEB_THIRD_PARTY_DIR}")
  target_compile_definitions(prosperoweb_media_stream_tests PRIVATE
    PROSPEROWEB_MEDIA_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
  # config.h's ENABLE()/USE() macros expand to defined(), as in WebKit.
  target_compile_options(prosperoweb_media_stream_tests PRIVATE -Wno-expansion-to-defined)
  add_test(NAME media.media_mse_stream
           COMMAND prosperoweb_media_stream_tests --test-suite=media_mse_stream --no-version)
else()
  message(STATUS "media tests: libavformat development files not found, skipping media.media_mse_stream")
endif()
