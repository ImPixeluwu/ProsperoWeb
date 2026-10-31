add_library(prosperobrowser_core STATIC
  bookmarks.cpp
  drm_policy.cpp
  file_storage.cpp
  fullscreen_controller.cpp
  gamepad_mapper.cpp
  history.cpp
  in_memory_storage.cpp
  on_screen_keyboard.cpp
  pointer_model.cpp
  repeat_timer.cpp
  session.cpp
  storage.cpp
  tab_manager.cpp
  text_utils.cpp
  url_normalizer.cpp
  utf16.cpp
)
add_library(prosperobrowser::core ALIAS prosperobrowser_core)

target_include_directories(prosperobrowser_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(prosperobrowser_core SYSTEM
  PUBLIC "${PROSPEROBROWSER_THIRD_PARTY_DIR}"
)
target_compile_features(prosperobrowser_core PUBLIC cxx_std_20)
target_link_libraries(prosperobrowser_core PRIVATE prosperobrowser_warnings)
