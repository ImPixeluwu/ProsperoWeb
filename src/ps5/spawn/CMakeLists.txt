# The spawner is PS5-target only (it uses ptrace and the PS5 kernel helpers).
# On the host only the pure helper is compiled, so its unit tests can link.

add_library(ps5browser_spawn_util STATIC spawn_util.c)
add_library(ps5browser::spawn_util ALIAS ps5browser_spawn_util)
target_include_directories(ps5browser_spawn_util PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ps5browser_spawn_util PUBLIC c_std_11)

if(PS5)
  add_library(ps5browser_spawn STATIC
    spawn.c
    elfldr.c
    pt.c
  )
  add_library(ps5browser::spawn ALIAS ps5browser_spawn)
  target_include_directories(ps5browser_spawn
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_include_directories(ps5browser_spawn PUBLIC
    "${CMAKE_SOURCE_DIR}/webkit-port/include")
  target_compile_features(ps5browser_spawn PUBLIC c_std_11)
  # elfldr.c/pt.c are vendored C; do not force -Werror on third-party code.
  target_compile_options(ps5browser_spawn PRIVATE -Wall)
  target_link_libraries(ps5browser_spawn PUBLIC ps5browser::spawn_util)
endif()
