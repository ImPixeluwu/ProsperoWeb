include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PROSPEROBROWSER_GTK REQUIRED IMPORTED_TARGET gtk4 webkitgtk-6.0)
pkg_check_modules(PROSPEROBROWSER_SDL2 IMPORTED_TARGET sdl2)

add_executable(prosperobrowser-host
  gtk_engine_view.cpp
  host_app.cpp
  main.cpp
)

if(PROSPEROBROWSER_SDL2_FOUND)
  target_sources(prosperobrowser-host PRIVATE gamepad_sdl.cpp)
  target_link_libraries(prosperobrowser-host PRIVATE PkgConfig::PROSPEROBROWSER_SDL2)
  message(STATUS "prosperobrowser-host: SDL2 ${PROSPEROBROWSER_SDL2_VERSION} game controller support")
else()
  target_sources(prosperobrowser-host PRIVATE gamepad_none.cpp)
  message(STATUS "prosperobrowser-host: SDL2 not found, no game controller support")
endif()

target_link_libraries(prosperobrowser-host PRIVATE
  prosperobrowser::shell
  prosperobrowser::chrome_ui
  PkgConfig::PROSPEROBROWSER_GTK
  prosperobrowser_warnings)

install(TARGETS prosperobrowser-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
