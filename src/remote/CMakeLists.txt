add_library(prosperoweb_remote STATIC
  http.cpp
  pairing.cpp
  phone_page.cpp
  remote_api.cpp
  remote_server.cpp
  remote_service.cpp
)
add_library(prosperoweb::remote ALIAS prosperoweb_remote)

find_package(Threads REQUIRED)

target_include_directories(prosperoweb_remote PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(prosperoweb_remote PUBLIC cxx_std_20)
target_link_libraries(prosperoweb_remote
  PUBLIC prosperoweb::core prosperoweb::shell Threads::Threads
  PRIVATE prosperoweb_warnings)
