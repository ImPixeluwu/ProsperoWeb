include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PROSPEROWEB_GTK REQUIRED IMPORTED_TARGET gtk4 webkitgtk-6.0)
pkg_check_modules(PROSPEROWEB_SDL2 IMPORTED_TARGET sdl2)

add_executable(prosperoweb-host
  gtk_engine_view.cpp
  host_app.cpp
  main.cpp
)

if(PROSPEROWEB_SDL2_FOUND)
  target_sources(prosperoweb-host PRIVATE gamepad_sdl.cpp)
  target_link_libraries(prosperoweb-host PRIVATE PkgConfig::PROSPEROWEB_SDL2)
  message(STATUS "prosperoweb-host: SDL2 ${PROSPEROWEB_SDL2_VERSION} game controller support")
else()
  target_sources(prosperoweb-host PRIVATE gamepad_none.cpp)
  message(STATUS "prosperoweb-host: SDL2 not found, no game controller support")
endif()

target_link_libraries(prosperoweb-host PRIVATE
  prosperoweb::shell
  prosperoweb::chrome_ui
  PkgConfig::PROSPEROWEB_GTK
  prosperoweb_warnings)

install(TARGETS prosperoweb-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
