add_executable(prosperobrowser_ps5_tests
  main.cpp
  test_spawn_util.cpp
  test_spawn_broker.cpp
  test_audio_server.cpp
  test_display.cpp
  test_video_overlay.cpp
  test_cpu_mask.cpp
)
target_link_libraries(prosperobrowser_ps5_tests PRIVATE prosperobrowser::spawn_util
  prosperobrowser::spawn_broker prosperobrowser::audio_server prosperobrowser::display prosperobrowser::video_server prosperobrowser::cpu prosperobrowser_warnings)
target_include_directories(prosperobrowser_ps5_tests SYSTEM PRIVATE "${PROSPEROBROWSER_THIRD_PARTY_DIR}")

add_test(NAME ps5.spawn_util
         COMMAND prosperobrowser_ps5_tests --test-suite=spawn_self_path,spawn_fd_table --no-version)
add_test(NAME ps5.spawn_broker
         COMMAND prosperobrowser_ps5_tests
                 --test-suite=broker_proto,broker_endpoints,broker_fd_passing,broker_e2e,broker_tcp,broker_websrv --no-version)
add_test(NAME ps5.audio_server
         COMMAND prosperobrowser_ps5_tests
                 --test-suite=audio_wire,audio_clock,audio_mixer,audio_ring,audio_e2e --no-version)
add_test(NAME ps5.display
         COMMAND prosperobrowser_ps5_tests
                 --test-suite=display_region,display_pixels,display_stats,display_splash --no-version)
add_test(NAME ps5.video_overlay
         COMMAND prosperobrowser_ps5_tests
                 --test-suite=video_wire,video_geometry,video_scale,video_compose,video_e2e --no-version)
add_test(NAME ps5.cpu_mask
         COMMAND prosperobrowser_ps5_tests --test-suite=cpu_mask --no-version)
