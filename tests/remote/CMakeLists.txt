add_executable(prosperoweb_remote_tests
  main.cpp
  test_http.cpp
  test_pairing.cpp
  test_remote_api.cpp
  test_remote_server.cpp
)
target_link_libraries(prosperoweb_remote_tests PRIVATE prosperoweb::remote prosperoweb_warnings)

foreach(suite IN ITEMS http pairing api server)
  add_test(NAME remote.${suite}
           COMMAND prosperoweb_remote_tests --test-suite=remote.${suite} --no-version)
endforeach()
