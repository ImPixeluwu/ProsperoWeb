# SPDX-License-Identifier: GPL-3.0-or-later
#
# Injected with -DCMAKE_PROJECT_WebKit_INCLUDE=... by scripts/ps5/build-webkit.sh.
# WebKit's top-level CMakeLists.txt overwrites CMAKE_MODULE_PATH before
# project(), so our stand-ins for Sony's CMake modules (PlayStationModule,
# Sign, Findlibdl) are appended here, right after project(WebKit).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
