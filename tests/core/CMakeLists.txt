add_executable(ps5browser_core_tests
  main.cpp
  test_bookmarks.cpp
  test_drm_policy.cpp
  test_fullscreen_controller.cpp
  test_gamepad_mapper.cpp
  test_history.cpp
  test_on_screen_keyboard.cpp
  test_pointer_model.cpp
  test_repeat_timer.cpp
  test_session.cpp
  test_tab_manager.cpp
  test_url_normalizer.cpp
  test_utf16.cpp
)
target_link_libraries(ps5browser_core_tests PRIVATE ps5browser::core ps5browser_warnings)
target_include_directories(ps5browser_core_tests SYSTEM PRIVATE "${PS5BROWSER_THIRD_PARTY_DIR}")

# One CTest entry per doctest suite so failures are reported per area.
foreach(suite IN ITEMS
    bookmarks drm_policy fullscreen gamepad_mapper history osk pointer_model
    repeat_timer session tab_manager url_normalizer utf16)
  add_test(NAME core.${suite}
           COMMAND ps5browser_core_tests --test-suite=${suite} --no-version)
endforeach()
