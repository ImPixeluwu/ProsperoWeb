# The spawner is PS5-target only (it uses ptrace and the PS5 kernel helpers).
# On the host the pure helpers and the spawn broker's protocol, server and
# client (POSIX sockets only) are compiled, so their unit tests can link.

add_library(prosperobrowser_spawn_util STATIC spawn_util.c)
add_library(prosperobrowser::spawn_util ALIAS prosperobrowser_spawn_util)
target_include_directories(prosperobrowser_spawn_util PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(prosperobrowser_spawn_util PUBLIC c_std_11)

# Spawn broker: wire format, server loop (spawns through a hook) and client.
# Logs through ps5_log_line/ps5_log_printf, which the executable (or the
# test binary) defines.
find_package(Threads REQUIRED)
add_library(prosperobrowser_spawn_broker STATIC
  broker_proto.cpp
  broker_server.cpp
  broker_client.cpp
)
add_library(prosperobrowser::spawn_broker ALIAS prosperobrowser_spawn_broker)
target_include_directories(prosperobrowser_spawn_broker PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(prosperobrowser_spawn_broker SYSTEM PUBLIC "${PROSPEROBROWSER_THIRD_PARTY_DIR}")
target_compile_features(prosperobrowser_spawn_broker PUBLIC cxx_std_20)
target_link_libraries(prosperobrowser_spawn_broker
  PUBLIC prosperobrowser::spawn_util Threads::Threads
  PRIVATE prosperobrowser_warnings)

if(PS5)
  add_library(prosperobrowser_spawn STATIC
    spawn.c
    spawn_glue.cpp
    cpuset_assign_ps5.cpp
    elfldr.c
    pt.c
  )
  add_library(prosperobrowser::spawn ALIAS prosperobrowser_spawn)
  target_include_directories(prosperobrowser_spawn
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_include_directories(prosperobrowser_spawn PUBLIC
    "${CMAKE_SOURCE_DIR}/webkit-port/include")
  target_compile_features(prosperobrowser_spawn PUBLIC c_std_11 cxx_std_20)
  # elfldr.c/pt.c are vendored C; do not force -Werror on third-party code.
  target_compile_options(prosperobrowser_spawn PRIVATE -Wall)
  # Our own sources keep the strict warnings.
  set_source_files_properties(spawn_glue.cpp cpuset_assign_ps5.cpp
    PROPERTIES COMPILE_OPTIONS "-Wextra;-Werror")
  target_link_libraries(prosperobrowser_spawn
    PUBLIC prosperobrowser::spawn_util prosperobrowser::spawn_broker
    PRIVATE prosperobrowser::cpu)
endif()
