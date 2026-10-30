# SPDX-License-Identifier: GPL-3.0-or-later
#
# Link-time stand-ins for libEGL / libGLESv2 (see gen-gl-stubs.py).
# Built with the payload SDK toolchain by scripts/ps5/build-webkit.sh and
# passed to WebKit as EGL_LIBRARIES / OpenGLES2_LIBRARY.
cmake_minimum_required(VERSION 3.20)
project(ps5_gl_stubs C)

add_library(EGL STATIC egl_stub.c)
add_library(GLESv2 STATIC glesv2_stub.c)
foreach (_t EGL GLESv2)
    target_compile_options(${_t} PRIVATE -O2 -g0 -w)
endforeach ()

install(TARGETS EGL GLESv2 ARCHIVE DESTINATION lib)
