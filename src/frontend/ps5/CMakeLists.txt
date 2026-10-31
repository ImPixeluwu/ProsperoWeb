# PS5 frontend (ps5browser executable). Activated by -DPS5BROWSER_FRONTEND=ps5.
#
# The WebKit static library is built separately by scripts/ps5/build-webkit.sh,
# whose "package" stage writes ${PS5_WEBKIT_BUILD_DIR}/ps5webkit-link.cmake:
# the C API include directories and the full link line (webkit-port/README.md,
# "Link contract"). When that file exists, the ps5browser executable links
# against it. Before it exists the frontend can still be compile-checked
# (target ps5browser_frontend_ps5_objs) against a generated forwarding-header
# directory that symlinks WebKit/<name>.h to the public C API headers in
# third_party/webkit.

if(NOT PS5)
  message(FATAL_ERROR "The ps5 frontend requires the PS5 toolchain "
                      "(-DCMAKE_TOOLCHAIN_FILE=.../prospero.cmake)")
endif()

set(PS5_WEBKIT_BUILD_DIR "${CMAKE_SOURCE_DIR}/build-wk-ps5" CACHE PATH
    "WebKit build directory (scripts/ps5/build-webkit.sh)")
set(PS5_WEBKIT_PREFIX "${CMAKE_SOURCE_DIR}/third_party/ps5-prefix" CACHE PATH
    "Install prefix of the cross-built WebKit dependencies")

# Overrides the exported link line when non-empty.
set(PS5_WEBKIT_LINK_LIBS "" CACHE STRING
    "Libraries to link into ps5browser instead of ps5webkit-link.cmake's list")

get_filename_component(_wk_build_dir "${PS5_WEBKIT_BUILD_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
set(_wk_link_file "${_wk_build_dir}/ps5webkit-link.cmake")
if(EXISTS "${_wk_link_file}")
  include("${_wk_link_file}")
  # Re-run CMake when build-webkit.sh rewrites the link file.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_wk_link_file}")
  message(STATUS "ps5 frontend: WebKit link line from ${_wk_link_file}")
else()
  set(PS5WEBKIT_INCLUDE_DIRS "")
  set(PS5WEBKIT_LINK_LIBRARIES "")
  message(STATUS "ps5 frontend: ${_wk_link_file} not found, ps5browser cannot link "
                 "(run scripts/ps5/build-webkit.sh package)")
endif()
if(PS5_WEBKIT_LINK_LIBS)
  set(PS5WEBKIT_LINK_LIBRARIES ${PS5_WEBKIT_LINK_LIBS})
endif()

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


find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

set(_frontend_sources
  main.cpp
  ps5_app.cpp
  ps5_audio_server.cpp
  ps5_chrome_ui.cpp
  ps5_config.cpp
  ps5_diagnostics.cpp
  ps5_engine_view.cpp
  ps5_ime.cpp
  ps5_log.cpp
  ps5_runtime_env.cpp
  wk_web_view.cpp
  wpe_loader.cpp
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
# The WebKit build's own header copy (PS5WEBKIT_INCLUDE_DIRS) wins over the
# forwarding headers, which only serve compile checks before it exists.
target_include_directories(ps5browser_frontend_ps5_objs SYSTEM PRIVATE
  ${PS5WEBKIT_INCLUDE_DIRS}
  "${_wk_fwd}"
  "${PS5_WEBKIT_PREFIX}/include/wpe-1.0"
  "${SDL2_INCLUDE_DIRS}"
  "${PS5BROWSER_THIRD_PARTY_DIR}")
# ps5browser::core / ::shell provide the public headers the frontend consumes.
target_link_libraries(ps5browser_frontend_ps5_objs PRIVATE ps5browser::core ps5browser::shell
  ps5browser::spawn_util ps5browser::audio_server)  # ps5browser/spawn/spawn.h (the broker role entry)

# The final executable. It cannot link until the WebKit archives exist, so it
# is excluded from the default build: cmake --build <dir> --target ps5browser.
add_executable(ps5browser EXCLUDE_FROM_ALL
  $<TARGET_OBJECTS:ps5browser_frontend_ps5_objs>)
target_link_libraries(ps5browser PRIVATE
  ps5browser::shell
  ps5browser::core
  ps5browser::spawn
  ps5browser::audio_server
  ${PS5WEBKIT_LINK_LIBRARIES}
  ${SDL2_LIBRARIES}
  SceImeDialog SceUserService)  # ps5_ime.cpp (also on SDL2's own link line)
target_link_directories(ps5browser PRIVATE ${SDL2_LIBRARY_DIRS})
# libWebKitPS5.a holds every WebKit object; drop what the browser never reaches.
target_link_options(ps5browser PRIVATE -Wl,--gc-sections)
set_target_properties(ps5browser PROPERTIES OUTPUT_NAME "ps5browser" SUFFIX ".elf")
