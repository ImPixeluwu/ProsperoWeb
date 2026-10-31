# The spawner is PS5-target only (it uses ptrace and the PS5 kernel helpers).
# On the host the pure helpers and the spawn broker's protocol, server and
# client (POSIX sockets only) are compiled, so their unit tests can link.

add_library(ps5browser_spawn_util STATIC spawn_util.c)
add_library(ps5browser::spawn_util ALIAS ps5browser_spawn_util)
target_include_directories(ps5browser_spawn_util PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(ps5browser_spawn_util PUBLIC c_std_11)

# Spawn broker: wire format, server loop (spawns through a hook) and client.
# Logs through ps5_log_line/ps5_log_printf, which the executable (or the
# test binary) defines.
find_package(Threads REQUIRED)
add_library(ps5browser_spawn_broker STATIC
  broker_proto.cpp
  broker_server.cpp
  broker_client.cpp
)
add_library(ps5browser::spawn_broker ALIAS ps5browser_spawn_broker)
target_include_directories(ps5browser_spawn_broker PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(ps5browser_spawn_broker SYSTEM PUBLIC "${PS5BROWSER_THIRD_PARTY_DIR}")
target_compile_features(ps5browser_spawn_broker PUBLIC cxx_std_20)
target_link_libraries(ps5browser_spawn_broker
  PUBLIC ps5browser::spawn_util Threads::Threads
  PRIVATE ps5browser_warnings)

if(PS5)
  add_library(ps5browser_spawn STATIC
    spawn.c
    spawn_glue.cpp
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
  target_compile_features(ps5browser_spawn PUBLIC c_std_11 cxx_std_20)
  # elfldr.c/pt.c are vendored C; do not force -Werror on third-party code.
  target_compile_options(ps5browser_spawn PRIVATE -Wall)
  set_source_files_properties(spawn_glue.cpp PROPERTIES COMPILE_OPTIONS "-Wextra;-Werror")
  target_link_libraries(ps5browser_spawn PUBLIC ps5browser::spawn_util ps5browser::spawn_broker)
endif()
