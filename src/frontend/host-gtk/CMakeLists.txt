find_package(PkgConfig REQUIRED)
pkg_check_modules(PS5BROWSER_GTK REQUIRED IMPORTED_TARGET gtk4 webkitgtk-6.0)
pkg_check_modules(PS5BROWSER_SDL2 IMPORTED_TARGET sdl2)

add_executable(ps5browser-host
  gtk_engine_view.cpp
  host_app.cpp
  main.cpp
)

if(PS5BROWSER_SDL2_FOUND)
  target_sources(ps5browser-host PRIVATE gamepad_sdl.cpp)
  target_link_libraries(ps5browser-host PRIVATE PkgConfig::PS5BROWSER_SDL2)
  message(STATUS "ps5browser-host: SDL2 ${PS5BROWSER_SDL2_VERSION} game controller support")
else()
  target_sources(ps5browser-host PRIVATE gamepad_none.cpp)
  message(STATUS "ps5browser-host: SDL2 not found, no game controller support")
endif()

target_link_libraries(ps5browser-host PRIVATE
  ps5browser::shell
  ps5browser::chrome_ui
  PkgConfig::PS5BROWSER_GTK
  ps5browser_warnings)

install(TARGETS ps5browser-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
