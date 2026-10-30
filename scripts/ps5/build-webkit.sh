#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cross-build the WebKit PlayStation port for the PS5 payload SDK.
#
#   scripts/ps5/build-webkit.sh <configure|wtf|jsc|webcore|webkit|all>
#
# Environment:
#   PS5_PAYLOAD_SDK   SDK root                        (/opt/ps5-payload-sdk)
#   PS5_DEPS_PREFIX   dependency prefix (build-deps)  (third_party/ps5-prefix)
#   WEBKIT_BUILD_DIR  build directory                 (build-wk-ps5)
#   JOBS              parallel compile jobs           (4)
#   WEBKIT_CMAKE_ARGS extra arguments for cmake
#
# Requires third_party/webkit fetched and patched (scripts/ps5/fetch-webkit.sh)
# and the dependency prefix complete ($PS5_DEPS_PREFIX/.done).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
PREFIX="$(realpath -m "${PS5_DEPS_PREFIX:-${ROOT}/third_party/ps5-prefix}")"
BUILD="$(realpath -m "${WEBKIT_BUILD_DIR:-${ROOT}/build-wk-ps5}")"
JOBS="${JOBS:-4}"
WEBKIT_DIR="${ROOT}/third_party/webkit"
PORT_DIR="${ROOT}/webkit-port"
STUBS_BUILD="${BUILD}/gl-stubs"
export PS5_PAYLOAD_SDK="${SDK}"

log() { printf '[build-webkit] %s\n' "$*"; }
die() { printf '[build-webkit] error: %s\n' "$*" >&2; exit 1; }

require_deps() {
    [[ -f "${PREFIX}/.done" ]] || die "dependency prefix ${PREFIX} is incomplete (no .done); run scripts/ps5/build-deps.sh"
    [[ -f "${WEBKIT_DIR}/.ps5-patches.stamp" ]] || die "WebKit is not fetched/patched; run scripts/ps5/fetch-webkit.sh"
}

build_stubs() {
    log "building EGL/GLESv2 link stubs"
    cmake -S "${PORT_DIR}/stubs" -B "${STUBS_BUILD}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${PORT_DIR}/toolchain/ps5.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF >/dev/null
    ninja -C "${STUBS_BUILD}" >/dev/null
}

configure() {
    [[ -f "${WEBKIT_DIR}/.ps5-patches.stamp" ]] || die "WebKit is not fetched/patched; run scripts/ps5/fetch-webkit.sh"
    build_stubs
    log "configuring ${BUILD}"
    local release_flags="-O2 -DNDEBUG -g0"
    # shellcheck disable=SC2086
    cmake -S "${WEBKIT_DIR}" -B "${BUILD}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${PORT_DIR}/toolchain/ps5.cmake" \
        -DCMAKE_PROJECT_WebKit_INCLUDE="${PORT_DIR}/cmake/ps5-project-include.cmake" \
        -DCMAKE_PREFIX_PATH="${PREFIX}" \
        -DCMAKE_FIND_ROOT_PATH="${PREFIX}" \
        -DWebKitRequirements_DIR="${PORT_DIR}/cmake" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="${release_flags}" \
        -DCMAKE_CXX_FLAGS_RELEASE="${release_flags}" \
        -DPORT=PlayStation \
        -DPLAYSTATION=ON \
        -DPS5_HOMEBREW=ON \
        -DPS5WEBKIT_INCLUDE_DIR="${PORT_DIR}/include" \
        -DWEBKIT_LIBRARIES_DIR="${PREFIX}" \
        -DICU_ROOT="${PREFIX}" \
        -DEGL_LIBRARIES="${STUBS_BUILD}/libEGL.a" \
        -DOpenGLES2_LIBRARY="${STUBS_BUILD}/libGLESv2.a" \
        -DUSE_SKIA=ON \
        -DUSE_SYSTEM_MALLOC=ON \
        -DENABLE_UNIFIED_BUILDS=ON \
        -DENABLE_STATIC_JSC=ON \
        -DENABLE_JIT=OFF -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF \
        -DENABLE_WEBASSEMBLY=OFF \
        -DENABLE_VIDEO=ON \
        -DENABLE_MEDIA_SOURCE=OFF \
        -DENABLE_FULLSCREEN_API=ON \
        -DENABLE_GAMEPAD=ON \
        -DENABLE_REMOTE_INSPECTOR=OFF \
        -DENABLE_RESOURCE_USAGE=OFF \
        -DENABLE_WEBGL=OFF \
        -DENABLE_XSLT=OFF \
        -DENABLE_WEB_AUDIO=OFF \
        -DENABLE_GPU_PROCESS=OFF \
        -DENABLE_MINIBROWSER=OFF \
        -DENABLE_API_TESTS=OFF \
        -DENABLE_LAYOUT_TESTS=OFF \
        -DUSE_WOFF2=OFF -DUSE_AVIF=OFF -DUSE_JPEGXL=OFF -DUSE_LCMS=OFF \
        ${WEBKIT_CMAKE_ARGS:-}
}

ensure_configured() {
    [[ -f "${BUILD}/build.ninja" ]] || configure
}

build() {
    require_deps
    ensure_configured
    log "ninja -j${JOBS} $*"
    ninja -C "${BUILD}" -j"${JOBS}" "$@"
}

stage="${1:-}"
case "${stage}" in
    configure) configure ;;
    wtf)       build WTF bmalloc ;;
    jsc)       build jsc ;;
    webcore)   build WebCore ;;
    webkit)    build WebKit ;;
    all)
        require_deps
        configure
        build WTF bmalloc
        build jsc
        build WebCore
        build WebKit
        ;;
    *) die "usage: $0 <configure|wtf|jsc|webcore|webkit|all>" ;;
esac
