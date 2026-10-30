add_executable(ps5browser_ps5_tests
  main.cpp
  test_spawn_util.cpp
)
target_link_libraries(ps5browser_ps5_tests PRIVATE ps5browser::spawn_util ps5browser_warnings)
target_include_directories(ps5browser_ps5_tests SYSTEM PRIVATE "${PS5BROWSER_THIRD_PARTY_DIR}")

add_test(NAME ps5.spawn_util
         COMMAND ps5browser_ps5_tests --test-suite=spawn_self_path,spawn_fd_table --no-version)
