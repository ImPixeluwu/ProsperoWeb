add_library(ps5browser_shell STATIC
  shell_controller.cpp
  text_input.cpp
)
add_library(ps5browser::shell ALIAS ps5browser_shell)

target_include_directories(ps5browser_shell PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ps5browser_shell PUBLIC cxx_std_20)
target_link_libraries(ps5browser_shell
  PUBLIC ps5browser::core
  PRIVATE ps5browser_warnings)
