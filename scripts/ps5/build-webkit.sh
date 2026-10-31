#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cross-build the WebKit PlayStation port for the PS5 payload SDK.
#
#   scripts/ps5/build-webkit.sh <configure|wtf|jsc|webcore|webkit|package|smoke|all>
#
# "package" bundles every object of bmalloc, WTF, JavaScriptCore, PAL,
# WebCore and WebKit (OBJECT libraries upstream, so CMake never archives
# them) into ${WEBKIT_BUILD_DIR}/lib/libWebKitPS5.a for the shell's link,
# and writes ${WEBKIT_BUILD_DIR}/ps5webkit-link.cmake (include directories
# and link libraries, consumed by src/frontend/ps5/CMakeLists.txt).
# "smoke" links webkit-port/smoke/wk_smoke.cpp with the link line documented
# in webkit-port/README.md (built, never run).
#
# Environment:
#   PS5_PAYLOAD_SDK   SDK root                        (/opt/ps5-payload-sdk)
#   PS5_DEPS_PREFIX   dependency prefix (build-deps)  (third_party/ps5-prefix)
#   WEBKIT_BUILD_DIR  build directory                 (build-wk-ps5)
#   JOBS              parallel compile jobs           (4)
#   USE_FFMPEG        ffmpeg <video> engine (0900)    (ON)
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
        -DCMAKE_CXX_FLAGS="-include ${PORT_DIR}/compat/libcxx18-compat.h" \
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
        -DUSE_THIN_ARCHIVES=OFF \
        -DENABLE_UNIFIED_BUILDS=ON \
        -DENABLE_STATIC_JSC=ON \
        -DENABLE_JIT=OFF -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF \
        -DENABLE_WEBASSEMBLY=OFF \
        -DENABLE_VIDEO=ON \
        -DENABLE_MEDIA_SOURCE=OFF \
        -DUSE_FFMPEG="${USE_FFMPEG:-ON}" \
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

package() {
    require_deps
    local out="${BUILD}/lib/libWebKitPS5.a" list="${BUILD}/lib/libWebKitPS5.objects"
    local dirs=()
    local d
    for d in \
        Source/bmalloc/CMakeFiles/bmalloc.dir \
        Source/WTF/wtf/CMakeFiles/WTF.dir \
        Source/JavaScriptCore/CMakeFiles/JavaScriptCore.dir \
        Source/JavaScriptCore/CMakeFiles/JavaScriptCoreJIT.dir \
        Source/JavaScriptCore/CMakeFiles/LowLevelInterpreterLib.dir \
        Source/WebCore/PAL/pal/CMakeFiles/PAL.dir \
        Source/WebCore/CMakeFiles/WebCore*.dir \
        Source/WebKit/CMakeFiles/WebKit*.dir; do
        for m in "${BUILD}"/${d}; do
            [[ -d "${m}" ]] && dirs+=("${m}")
        done
    done
    (( ${#dirs[@]} )) || die "no object directories found; build first"
    find "${dirs[@]}" -name '*.o' | sort > "${list}"
    log "archiving $(wc -l < "${list}") objects into ${out}"
    rm -f "${out}"
    "${SDK}/bin/prospero-ar" rcs "${out}" @"${list}"
    ls -l "${out}"
    write_link_cmake
}

use_ffmpeg() {
    grep -q '^#define USE_FFMPEG 1' "${BUILD}/cmakeconfig.h" 2>/dev/null
}

# The link contract (webkit-port/README.md): archives and libraries an
# executable using libWebKitPS5.a needs, one per line. -l libraries come
# from the SDK's target/user/homebrew/lib, which the SDK driver searches.
link_libs() {
    printf '%s\n' \
        "${BUILD}/lib/libWebKitPS5.a" "${BUILD}/lib/libSkia.a" \
        "${STUBS_BUILD}/libEGL.a" "${STUBS_BUILD}/libGLESv2.a" \
        "${PREFIX}/lib/libwpe-1.0.a" "${PREFIX}/lib/libxkbcommon.a" \
        "${PREFIX}/lib/libharfbuzz-icu.a" "${PREFIX}/lib/libharfbuzz.a" "${PREFIX}/lib/libfreetype.a" \
        "${PREFIX}/lib/libbrotlidec.a" "${PREFIX}/lib/libbrotlicommon.a" \
        "${PREFIX}/lib/libicui18n.a" "${PREFIX}/lib/libicuuc.a" "${PREFIX}/lib/libicudata.a"
    if use_ffmpeg; then
        # pkg-config --static --libs libavformat libavcodec libswscale
        # libswresample libavutil, and SDL2 for the engine's audio sink.
        printf '%s\n' -lavformat -lbz2 -lavcodec -llzma -lx264 -lswscale -lswresample -lavutil \
            -lSDL2 -lSceSystemService -lSceUserService -lScePad -lSceAudioOut -lsamplerate \
            -lSceVideoOut -lSceKeyboard -lSceImeDialog
    fi
    printf '%s\n' -lcurl -lpsl -lssl -lcrypto -lzstd -lxml2 -liconv -lsqlite3 -lfontconfig -lexpat \
        -lpng16 -ljpeg -lwebpdemux -lwebp -lsharpyuv -lz -lm -lpthread
}

write_link_cmake() {
    local out="${BUILD}/ps5webkit-link.cmake" l
    {
        echo "# Generated by scripts/ps5/build-webkit.sh package; do not edit."
        echo "set(PS5WEBKIT_BUILD_DIR \"${BUILD}\")"
        echo "set(PS5WEBKIT_INCLUDE_DIRS"
        echo "    \"${BUILD}/WebKit/Headers\""
        echo "    \"${PORT_DIR}/include\""
        echo "    \"${PREFIX}/include/wpe-1.0\")"
        echo "set(PS5WEBKIT_USE_FFMPEG $(use_ffmpeg && echo ON || echo OFF))"
        echo "set(PS5WEBKIT_LINK_LIBRARIES"
        while read -r l; do echo "    \"${l}\""; done < <(link_libs)
        echo ")"
    } > "${out}"
    log "wrote ${out}"
}

smoke() {
    require_deps
    local out="${BUILD}/smoke"
    mkdir -p "${out}"
    "${PORT_DIR}/toolchain/ps5-clang++" -std=c++23 -O2 -g0 \
        -I"${BUILD}/WebKit/Headers" -I"${PORT_DIR}/include" -I"${PREFIX}/include/wpe-1.0" \
        -c "${PORT_DIR}/smoke/wk_smoke.cpp" -o "${out}/wk_smoke.o"
    local libs
    mapfile -t libs < <(link_libs)
    "${PORT_DIR}/toolchain/ps5-clang++" -Wl,--gc-sections -o "${out}/wk_smoke.elf" "${out}/wk_smoke.o" "${libs[@]}"
    file "${out}/wk_smoke.elf"
    ls -l "${out}/wk_smoke.elf"
}

stage="${1:-}"
case "${stage}" in
    configure) configure ;;
    wtf)       build WTF bmalloc ;;
    jsc)       build jsc ;;
    webcore)   build PAL Skia WebCore WebCoreDOMAndRendering WebCoreInspector WebCoreJSBindings WebCoreStyle ;;
    webkit)    build WebKit WebKitShared WebKitUIProcess WebKitWebProcess WebKitNetworkProcess WebKitGPUProcess ;;
    package)   package ;;
    smoke)     smoke ;;
    all)
        require_deps
        configure
        build WTF bmalloc
        build jsc
        build PAL Skia WebCore WebCoreDOMAndRendering WebCoreInspector WebCoreJSBindings WebCoreStyle
        build WebKit WebKitShared WebKitUIProcess WebKitWebProcess WebKitNetworkProcess WebKitGPUProcess
        package
        smoke
        ;;
    *) die "usage: $0 <configure|wtf|jsc|webcore|webkit|package|smoke|all>" ;;
esac
