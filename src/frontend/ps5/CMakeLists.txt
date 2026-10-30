# PS5 frontend (ps5browser executable). Activated by -DPS5BROWSER_FRONTEND=ps5.
#
# The WebKit static library is built separately; before it exists the frontend
# can still be compile-checked (target ps5browser_frontend_ps5_objs) against a
# generated forwarding-header directory that symlinks WebKit/<name>.h to the
# real public C API headers in third_party/webkit.

if(NOT PS5)
  message(FATAL_ERROR "The ps5 frontend requires the PS5 toolchain "
                      "(-DCMAKE_TOOLCHAIN_FILE=.../prospero.cmake)")
endif()

set(PS5_WEBKIT_BUILD_DIR "build-wk-ps5" CACHE STRING
    "WebKit build directory (for generated forwarding headers)")
set(PS5_WEBKIT_PREFIX "${CMAKE_SOURCE_DIR}/third_party/ps5-prefix" CACHE STRING
    "Install prefix of the cross-built WebKit dependencies")

# Fill this in once webkit-port/README.md lists the archives to link. Leaving
# it empty lets the compile-only object target build without the libraries.
set(PS5_WEBKIT_LINK_LIBS "" CACHE STRING
    "WebKit static archives to link into the final ps5browser executable")

# --- Generate the WebKit C API forwarding headers --------------------------
set(_wk_src "${CMAKE_SOURCE_DIR}/third_party/webkit/Source/WebKit")
set(_wk_fwd "${CMAKE_BINARY_DIR}/ps5-webkit-forwarding")
if(EXISTS "${_wk_src}")
  file(MAKE_DIRECTORY "${_wk_fwd}/WebKit")
  file(GLOB _wk_headers
    "${_wk_src}/Shared/API/c/*.h"
    "${_wk_src}/Shared/API/c/playstation/*.h"
    "${_wk_src}/UIProcess/API/C/*.h"
    "${_wk_src}/UIProcess/API/C/playstation/*.h")
  foreach(_h ${_wk_headers})
    get_filename_component(_n "${_h}" NAME)
    # Generic dirs are globbed first, so on a basename clash the generic header
    # wins (playstation headers have distinct names in practice).
    if(NOT EXISTS "${_wk_fwd}/WebKit/${_n}")
      file(CREATE_LINK "${_h}" "${_wk_fwd}/WebKit/${_n}" SYMBOLIC)
    endif()
  endforeach()
endif()

# The real build may expose its own forwarding headers once it has run.
set(_wk_build_fwd "${PS5_WEBKIT_BUILD_DIR}/WebKit/DerivedSources/ForwardingHeaders")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

set(_frontend_sources
  main.cpp
  ps5_app.cpp
  ps5_chrome_ui.cpp
  ps5_config.cpp
  ps5_engine_view.cpp
  ps5_log.cpp
  wk_web_view.cpp
)

# OBJECT library so the frontend can be compile-checked without linking WebKit.
add_library(ps5browser_frontend_ps5_objs OBJECT ${_frontend_sources})
target_compile_features(ps5browser_frontend_ps5_objs PRIVATE cxx_std_20)
target_link_libraries(ps5browser_frontend_ps5_objs PRIVATE ps5browser_warnings)
target_include_directories(ps5browser_frontend_ps5_objs PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/webkit-port/include")
# WebKit, SDL2 and nlohmann headers are third-party: include as SYSTEM so their
# warnings do not trip -Werror on our code.
target_include_directories(ps5browser_frontend_ps5_objs SYSTEM PRIVATE
  "${_wk_fwd}"
  "${_wk_build_fwd}"
  "${SDL2_INCLUDE_DIRS}"
  "${PS5BROWSER_THIRD_PARTY_DIR}")
# ps5browser::core / ::shell provide the public headers the frontend consumes.
target_link_libraries(ps5browser_frontend_ps5_objs PRIVATE ps5browser::core ps5browser::shell)

# The final executable. It cannot link until the WebKit archives exist, so it
# is excluded from the default build; fill PS5_WEBKIT_LINK_LIBS and build it
# explicitly once they are available.
add_executable(ps5browser EXCLUDE_FROM_ALL
  $<TARGET_OBJECTS:ps5browser_frontend_ps5_objs>)
target_link_libraries(ps5browser PRIVATE
  ps5browser::shell
  ps5browser::core
  ps5browser::spawn
  ${PS5_WEBKIT_LINK_LIBS}
  ${SDL2_LIBRARIES})
target_link_directories(ps5browser PRIVATE ${SDL2_LIBRARY_DIRS})
set_target_properties(ps5browser PROPERTIES OUTPUT_NAME "ps5browser" SUFFIX ".elf")
