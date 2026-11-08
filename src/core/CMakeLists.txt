add_library(prosperoweb_core STATIC
  adblock.cpp
  addons_config.cpp
  bookmarks.cpp
  drm_policy.cpp
  file_storage.cpp
  fullscreen_controller.cpp
  gamepad_mapper.cpp
  history.cpp
  i18n.cpp
  legacy_profile.cpp
  in_memory_storage.cpp
  on_screen_keyboard.cpp
  pointer_model.cpp
  popup_policy.cpp
  repeat_timer.cpp
  session.cpp
  sponsorblock.cpp
  storage.cpp
  tab_manager.cpp
  text_utils.cpp
  touchpad_model.cpp
  url_normalizer.cpp
  utf16.cpp
)
add_library(prosperoweb::core ALIAS prosperoweb_core)

target_include_directories(prosperoweb_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(prosperoweb_core SYSTEM
  PUBLIC "${PROSPEROWEB_THIRD_PARTY_DIR}"
)
target_compile_features(prosperoweb_core PUBLIC cxx_std_20)
target_link_libraries(prosperoweb_core PRIVATE prosperoweb_warnings)
