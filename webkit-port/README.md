# webkit-port

Overlay that builds the upstream WebKit **PlayStation port** (`-DPORT=PlayStation`) with the open [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) instead of Sony's SDK. Upstream WebKit is fetched at a pinned commit into `third_party/webkit/` (never committed) and patched with the files in `patches/`. All patches are guarded by `PS5_HOMEBREW`, so the upstream behaviour is unchanged without it.

## Layout

| Path | Purpose |
|---|---|
| `include/ps5webkit/entry.h` | `ps5_webkit_{web,network}_process_main()`: auxiliary process entry points, implemented in libWebKit, called by the shell's `main()` |
| `include/ps5webkit/spawn.h` | `ps5_spawn_self()` / `ps5_terminate_process()`: called by the WebKit process launcher, implemented by the shell |
| `cmake/PlayStationModule.cmake`, `cmake/Sign.cmake` | no-op stand-ins for Sony's proprietary CMake modules |
| `cmake/WebKitRequirementsConfig.cmake` | stand-in for Sony's `WebKitRequirements` package: empty `WebKitResources`/`ProcessLauncher`, `libwpe` (+ `WPE::libwpe`) from the dependency prefix |
| `cmake/Findlibdl.cmake` | reports libdl as absent (the SDK libc has `dlopen`) |
| `cmake/ps5-project-include.cmake` | appends `cmake/` to `CMAKE_MODULE_PATH` after `project(WebKit)` |
| `stubs/` | generated EGL/GLESv2 stubs (`libEGL.a`, `libGLESv2.a`): every call fails, see below |
| `patches/` | WebKit patches, applied in order by `scripts/ps5/fetch-webkit.sh` |
| `smoke/wk_smoke.cpp` | link test of the WebKit C API against the static libraries |

## Patches

| Patch | Why |
|---|---|
| `0001-cmake-playstation-homebrew-options` | `OptionsPlayStation.cmake`: `PS5_HOMEBREW` option; no `-sce-stdlib=v2`, no forced `-g`, no libc/libkernel lookup, no `--wrap=mmap`/`sce_sys` copy, `ENABLE_RESOURCE_USAGE` off, `ENABLE_GAMEPAD` allowed without the WPE backend, static `libWebKit` and no auxiliary executables |
| `0002-wtf-posix-osallocator` | `OSAllocatorPOSIX.cpp` instead of the memory-extra based PlayStation allocator |
| `0003-jsc-generic-shell` | generic `jsc.cpp` shell (no Sony libtestwrappers), MemoryExtra only if found |
| `0004-webcore-curl-ca-bundle` | TLS roots from a PEM file (`$PS5BROWSER_CA_BUNDLE`, default `/data/ps5browser/cacert.pem`) instead of Sony's CertificateStore |
| `0005-webkit-static-multicall-processes` | launcher calls `ps5_spawn_self()`/`ps5_terminate_process()`; entry points become `ps5_webkit_*_process_main()` inside libWebKit; no WebProcess/NetworkProcess executables |

Editing patches: change files in `third_party/webkit/`, then run `scripts/ps5/export-webkit-patches.sh`, which rewrites `patches/*.patch` (keeping each patch's header text) from the tree. Every changed path must belong to one topic listed in that script.

## Build

```sh
scripts/ps5/build-deps.sh          # dependency prefix third_party/ps5-prefix (ICU, harfbuzz-icu, libwpe, ...)
scripts/ps5/fetch-webkit.sh        # third_party/webkit at the pinned commit + patches
scripts/ps5/build-webkit.sh all    # or: configure | wtf | jsc | webcore | webkit
```

`JOBS` (default 4) sets the ninja parallelism; the build directory is `build-wk-ps5/`. Release builds use `-O2 -DNDEBUG -g0`.

## EGL / GLES stubs

The PlayStation port compiles TextureMapper/CoordinatedGraphics unconditionally, so libWebKit references EGL and GLES symbols even though the PS5 build draws without acceleration (WebProcess paints with Skia into shared memory, the UI process copies it out with `WKPagePaint()`). `stubs/gen-gl-stubs.py` generates a definition for every prototype in the SDK's `EGL/egl.h`, `EGL/eglext.h`, `GLES3/gl3.h`, `GLES2/gl2.h` and `GLES2/gl2ext.h`: EGL calls fail (`EGL_NO_DISPLAY`, `EGL_FALSE`, `NULL`, `eglGetError() == EGL_NOT_INITIALIZED`), GLES calls do nothing. Mesa's real `libEGL.so`/`libGLESv2.so` from the SDK can replace them later for accelerated compositing.
