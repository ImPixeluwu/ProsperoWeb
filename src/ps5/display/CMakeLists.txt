# Display helpers of the PS5 UI process: frame-pipeline statistics, damage
# regions, the loading screen and the pixel kernels that compose the page and
# chrome caches into the SDL window surface. Portable C++ (no SDL, no
# WebKit), so the host tests cover them; the PS5 frontend links them.
add_library(prosperoweb_display STATIC
  compose.cpp
  damage_region.cpp
  frame_stats.cpp
  pointer.cpp
  scale.cpp
  splash.cpp
)
add_library(prosperoweb::display ALIAS prosperoweb_display)
target_include_directories(prosperoweb_display PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(prosperoweb_display PUBLIC cxx_std_20)
target_link_libraries(prosperoweb_display PRIVATE prosperoweb_warnings)
