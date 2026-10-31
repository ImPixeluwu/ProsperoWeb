add_executable(prosperobrowser_shell_tests
  main.cpp
  test_shell_controller.cpp
  test_text_input.cpp
)
target_link_libraries(prosperobrowser_shell_tests PRIVATE prosperobrowser::shell prosperobrowser_warnings)

foreach(suite IN ITEMS session navigation tabs input pointer osk osk.caret fullscreen drm menu chrome_events text_input)
  add_test(NAME shell.${suite}
           COMMAND prosperobrowser_shell_tests --test-suite=shell.${suite} --no-version)
endforeach()
