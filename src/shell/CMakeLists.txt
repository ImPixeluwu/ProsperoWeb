add_library(prosperoweb_shell STATIC
  shell_controller.cpp
  text_input.cpp
)
add_library(prosperoweb::shell ALIAS prosperoweb_shell)

target_include_directories(prosperoweb_shell PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(prosperoweb_shell PUBLIC cxx_std_20)
target_link_libraries(prosperoweb_shell
  PUBLIC prosperoweb::core
  PRIVATE prosperoweb_warnings)
