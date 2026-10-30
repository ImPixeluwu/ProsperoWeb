add_library(ps5browser_core STATIC
  bookmarks.cpp
  file_storage.cpp
  history.cpp
  in_memory_storage.cpp
  session.cpp
  storage.cpp
  tab_manager.cpp
  text_utils.cpp
  url_normalizer.cpp
)
add_library(ps5browser::core ALIAS ps5browser_core)

target_include_directories(ps5browser_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(ps5browser_core SYSTEM
  PUBLIC "${PS5BROWSER_THIRD_PARTY_DIR}"
)
target_compile_features(ps5browser_core PUBLIC cxx_std_20)
target_link_libraries(ps5browser_core PRIVATE ps5browser_warnings)
