add_executable(prosperoweb_shell_tests
  main.cpp
  test_addons.cpp
  test_shell_controller.cpp
  test_text_input.cpp
)
target_link_libraries(prosperoweb_shell_tests PRIVATE prosperoweb::shell prosperoweb_warnings)

foreach(suite IN ITEMS session navigation tabs input pointer touchpad osk osk.caret fullscreen drm menu chrome_events text_input cursor addons popups i18n dpad zoom)
  add_test(NAME shell.${suite}
           COMMAND prosperoweb_shell_tests --test-suite=shell.${suite} --no-version)
endforeach()
