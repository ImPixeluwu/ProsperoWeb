add_executable(ps5browser_shell_tests
  main.cpp
  test_shell_controller.cpp
)
target_link_libraries(ps5browser_shell_tests PRIVATE ps5browser::shell ps5browser_warnings)

foreach(suite IN ITEMS session navigation tabs input osk osk.caret fullscreen drm menu chrome_events)
  add_test(NAME shell.${suite}
           COMMAND ps5browser_shell_tests --test-suite=shell.${suite} --no-version)
endforeach()
