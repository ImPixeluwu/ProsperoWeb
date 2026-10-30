# SPDX-License-Identifier: GPL-3.0-or-later
#
# The SDK toolchain file, with the compilers replaced by the wrappers next to
# this file (see ps5-clang for why).
include("$ENV{PS5_PAYLOAD_SDK}/toolchain/prospero.cmake" OPTIONAL RESULT_VARIABLE _ps5_tc)
if (NOT _ps5_tc)
    include("/opt/ps5-payload-sdk/toolchain/prospero.cmake")
endif ()
set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/ps5-clang" CACHE PATH "" FORCE)
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/ps5-clang++" CACHE PATH "" FORCE)
# The SDK toolchain prefers <Pkg>Config.cmake files; WebKit relies on its own
# Find modules and their target names (e.g. HarfBuzz::HarfBuzz rather than
# harfbuzz's harfbuzz::harfbuzz).
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG FALSE)
