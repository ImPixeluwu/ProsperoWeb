add_library(prosperobrowser_shell STATIC
  shell_controller.cpp
  text_input.cpp
)
add_library(prosperobrowser::shell ALIAS prosperobrowser_shell)

target_include_directories(prosperobrowser_shell PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(prosperobrowser_shell PUBLIC cxx_std_20)
target_link_libraries(prosperobrowser_shell
  PUBLIC prosperobrowser::core
  PRIVATE prosperobrowser_warnings)
