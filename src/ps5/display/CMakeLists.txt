# Display helpers of the PS5 UI process: frame-pipeline statistics, damage
# regions and the pixel kernels that compose the page and chrome caches into
# the SDL window surface. Portable C++ (no SDL, no WebKit), so the host tests
# cover them; the PS5 frontend links them.
add_library(ps5browser_display STATIC
  compose.cpp
  damage_region.cpp
  frame_stats.cpp
)
add_library(ps5browser::display ALIAS ps5browser_display)
target_include_directories(ps5browser_display PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ps5browser_display PUBLIC cxx_std_20)
target_link_libraries(ps5browser_display PRIVATE ps5browser_warnings)
