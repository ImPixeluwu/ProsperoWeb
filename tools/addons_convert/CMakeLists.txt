# Build-time ad-block converter: wraps prosperoweb::core::adblock so the
# staging script and the on-device updater share one implementation.
add_executable(addons_convert main.cpp)
target_link_libraries(addons_convert PRIVATE prosperoweb::core prosperoweb_warnings)
target_include_directories(addons_convert SYSTEM PRIVATE "${PROSPEROWEB_THIRD_PARTY_DIR}")
