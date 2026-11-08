add_executable(prosperoweb_core_tests
  main.cpp
  test_adblock.cpp
  test_addons_config.cpp
  test_sponsorblock.cpp
  test_bookmarks.cpp
  test_drm_policy.cpp
  test_fullscreen_controller.cpp
  test_gamepad_mapper.cpp
  test_history.cpp
  test_legacy_profile.cpp
  test_on_screen_keyboard.cpp
  test_pointer_model.cpp
  test_popup_policy.cpp
  test_touchpad_model.cpp
  test_repeat_timer.cpp
  test_session.cpp
  test_tab_manager.cpp
  test_url_normalizer.cpp
  test_utf16.cpp
)
target_link_libraries(prosperoweb_core_tests PRIVATE prosperoweb::core prosperoweb_warnings)
target_include_directories(prosperoweb_core_tests SYSTEM PRIVATE "${PROSPEROWEB_THIRD_PARTY_DIR}")

# One CTest entry per doctest suite so failures are reported per area.
foreach(suite IN ITEMS
    adblock addons_config sponsorblock filter_update
    bookmarks drm_policy fullscreen gamepad_mapper history legacy_profile osk pointer_model popup_policy
    repeat_timer session tab_manager touchpad_model url_normalizer utf16)
  add_test(NAME core.${suite}
           COMMAND prosperoweb_core_tests --test-suite=${suite} --no-version)
endforeach()
