# Architecture

This document describes how ProsperoWeb is put together: the processes on the console, the libraries in the payload, the code layout, the engine abstraction that lets one shell run on two engines, the rendering, input and media paths, the WebKit patch set, the build pipeline and the licensing. Runtime details (on-disk layout, environment, spawn contract) are in [ps5-runtime.md](ps5-runtime.md); the WebKit port is in [webkit-port/README.md](../webkit-port/README.md); the media engine is in [webkit-port/media/README.md](../webkit-port/media/README.md). The PS5 build runs on a real console; what has been checked there, and how, is in [console-verification.md](console-verification.md).

The project implements Option B of [research.md](research.md) ("port a real engine"): upstream WebKit's PlayStation port, built with the open ps5-payload-sdk instead of Sony's SDK.

## Overview

```
 PS5 (jailbroken, elfldr on :9021)
 +--------------------------------------------------------------------------------+
 |  prosperoweb.elf: one static multi-call ELF, /data/prosperoweb/prosperoweb.elf |
 |                                                                                |
 |  UI process  (argv[0] = anything else)                                         |
 |  +--------------------------------------------------------------------------+  |
 |  | Ps5App: SDL2 window surface (ABGR8888) -> sceVideoOut flip               |  |
 |  |   ShellController (src/shell) + core models (src/core)                   |  |
 |  |   content WKView/WKPage  --WKPagePaint-->  ARGB32 backbuffer --+         |  |
 |  |   chrome  WKView (chrome.html) --WKPagePaint--> chrome buffer -+-> blit  |  |
 |  |   SDL GameController (DualSense) / USB keyboard -> GamepadMapper         |  |
 |  |   ps5_spawn_self() (src/ps5/spawn, elfldr-derived)                       |  |
 |  +---------------+-------------------------------------+--------------------+  |
 |                  | rfork + ptrace inject               | rfork + ptrace inject |
 |                  | IPC socketpair end at fd 3          | IPC socketpair at fd 3|
 |  +---------------v------------------+  +---------------v--------------------+  |
 |  | WebProcess  (argv[0]=WebProcess) |  | NetworkProcess                     |  |
 |  |  WebCore + JSC (baseline+DFG JIT)|  |  (argv[0]=NetworkProcess)          |  |
 |  |  Skia CPU raster backing store   |  |  curl + OpenSSL, cacert.pem        |  |
 |  |  MediaPlayerPrivateFFmpeg        |  |  HTTP cache, cookies, storage      |  |
 |  |   ffmpeg decode, audio -> UI     |  |                                    |  |
 |  +----------------------------------+  +------------------------------------+  |
 +--------------------------------------------------------------------------------+
```

All three processes run the same ELF. `main()` in `src/frontend/ps5/main.cpp` dispatches on the basename of `argv[0]` (`WebProcess`, `NetworkProcess`, anything else is the UI), with `PROSPEROWEB_ROLE` as a fallback. WebKit's process launcher (patch 0005) creates a `SOCK_SEQPACKET` socketpair and calls `ps5_spawn_self({"WebProcess", "<id>", "3"}, {clientFd}, 1)`. The spawner re-launches our own ELF the way elfldr launches payloads (`rfork_thread` + `execve` of the `SceSpZeroConf` trampoline + ptrace injection) and duplicates the client end into the child as fd 3, where `AuxiliaryProcessMain` expects it. The full contract is in [ps5-runtime.md](ps5-runtime.md#spawn-mechanics-fd--argv-contract).

## Libraries in the payload

| Library | Source | Role |
|---|---|---|
| `libWebKitPS5.a` | upstream WebKit PlayStation port at a pinned commit + `webkit-port/patches/` | bmalloc, WTF, JavaScriptCore (LLInt only), PAL, WebCore, WebKit (C API) in one archive |
| `libSkia.a` | in-tree Skia | CPU raster painting |
| `libEGL.a`, `libGLESv2.a` | generated stubs (`webkit-port/stubs/`) | satisfy TextureMapper references; every call fails |
| ICU, HarfBuzz (+icu), FreeType (+brotli for WOFF2), libxkbcommon, libwpe | `scripts/ps5/build-deps.sh` into `third_party/ps5-prefix/` | text, shaping, fonts, input keysyms, WPE input events |
| curl, OpenSSL, libpsl, zstd, libxml2, SQLite, fontconfig, expat, libpng, libjpeg, libwebp, zlib, iconv | ps5-payload-sdk pacbrew bundle | networking, TLS, parsing, storage, images |
| FFmpeg 7.0.1 (with x264), SDL2 | ps5-payload-sdk pacbrew bundle | media engine (decode, audio sink), UI window and gamepad |
| SDK crt, libc, libc++ 18 | ps5-payload-sdk | runtime; system `.sprx` modules are resolved at load time |

The link line and the list of system modules (`NEEDED`) are in [webkit-port/README.md](../webkit-port/README.md#link-contract-for-the-shell).

## Code layout

| Path | Content |
|---|---|
| `src/core/` | platform-independent models: tabs, URL normalizer, history, bookmarks, storage and session, gamepad mapper, pointer, on-screen keyboard, fullscreen state, DRM policy, injected page scripts (EME detector, gamepad key bridge) |
| `src/shell/` | `ShellController` and the interfaces a frontend implements: `EngineView`, `EngineViewClient`, `EngineFactory`, `ChromeUi` |
| `src/chrome-ui/` | the browser chrome as one offline HTML page (`chrome.html`, `chrome.css`, `chrome.js`) |
| `src/frontend/host-gtk/` | `prosperoweb-host`: GTK 4 + WebKitGTK 6.0 desktop frontend for development and screenshots |
| `src/frontend/ps5/` | `prosperoweb.elf`: multi-call `main`, `Ps5App` (SDL2 frame loop, compositor, input), `WkWebView`/`Ps5EngineView` (WebKit C API), `Ps5ChromeUi`, config, log, runtime environment, libwpe loader |
| `src/ps5/spawn/` | `ps5_spawn_self()` / `ps5_terminate_process()`: elfldr-derived spawner with fd passing ([README](../src/ps5/spawn/README.md)) |
| `webkit-port/` | CMake stand-ins for Sony's modules, toolchain wrappers, compat headers, GL stubs, patches, smoke test, ffmpeg media engine sources |
| `scripts/ps5/` | `build-deps.sh`, `fetch-webkit.sh`, `build-webkit.sh`, `export-webkit-patches.sh`, `package.sh`, pkg-config wrapper, meson cross file, ICU data filter |
| `tests/` | doctest unit tests run on the host: `core/`, `shell/` (fake engine and chrome), `ps5/` (pure spawn helpers), `media/` (engine-independent media logic) |
| `third_party/` | vendored nlohmann/json and doctest; fetched WebKit, dependency prefix and download cache (not committed) |

## Engine abstraction

`ShellController` owns the browser logic: tabs and their engine views, focus (page, chrome, OSK, menu, notice), the gamepad action dispatch, the pointer, the on-screen keyboard, fullscreen state, DRM policy, history, bookmarks and session persistence. It never talks to an engine directly.

- `EngineView` is one web view (one tab): `load`, `go_back`/`go_forward`/`reload`/`stop`, `set_size`/`set_visible`, trusted input (`pointer_move`, `pointer_button`, `scroll`, `key`, `insert_text`), `request_exit_fullscreen`, `run_javascript`, `set_user_agent`.
- `EngineViewClient` is how a view reports back: URL, title, load start/finish/failure, back/forward state, fullscreen enter/exit, `script_message` (JSON from injected scripts), new-window requests, `text_input_focused`.
- `EngineFactory` creates views; the controller asks it for one per tab.
- `ChromeUi` renders the chrome from a JSON state (`render(state)`, delivered to `window.ps5chrome.render`) and returns user events from the chrome page as JSON.

| | Host (`host-gtk`) | PS5 (`ps5`) |
|---|---|---|
| Engine | WebKitGTK 6.0 (`GtkEngineView`) | WebKit C API, PlayStation port (`Ps5EngineView` over `WkWebView`) |
| Chrome | transparent full-window `WebKitWebView` over the page | second `WKView` loading `chrome.html`, composited over declared rects |
| Input | keyboard as DualSense, optional SDL2 controller; JS fallbacks (untrusted) | SDL `GameController` + USB keyboard; trusted `WKPageHandle*Event` |
| Media | GStreamer | ffmpeg engine (patch 0900) |
| Profile | `--profile DIR`, files in `DIR/state/` | `/data/prosperoweb/` |

The same `ShellController`, core models and `chrome.html` run on both, so most behaviour can be developed and screenshotted on the host ([screenshots](screenshots/README.md)) and is covered by `tests/shell` with a fake engine.

## Rendering path

Phase 1 is software only. Accelerated compositing stays off (the PlayStation default), so the WebProcess paints with Skia into a shared-memory backing store and the UI process copies it out.

1. `WKViewClient.setViewNeedsDisplay` reports dirty rects for each view.
2. The frame loop (`Ps5App`, a self-re-arming `WKRunLoopCallOnMainThread` callback) calls `WKPagePaint(page, buffer, size, rect)` into an ARGB32 buffer: the content view below the 96 px chrome band, the chrome view into its own buffer.
3. The chrome buffer is composited over the content only on the rects the chrome state declares (top band, OSK band, modal rect for menu and notice, pointer).
4. The result is converted from ARGB32 (Skia N32) to the ABGR8888 SDL window surface; `SDL_UpdateWindowSurface` tiles it and submits the flip through `sceVideoOut`.

No EGL/GLES runs at runtime: CSS 3D transforms are not rendered in 3D, WebGL is unavailable, and video frames are painted through the same raster path.

## Input path

- **DualSense** arrives through SDL2's PS5 driver (`libScePad`) as `SDL_GameController` buttons and axes. `Ps5App` maps them to `core::Button`; the analog triggers become L2/R2 on edges; sticks are normalised and passed to `on_sticks`.
- **`core::GamepadMapper`** turns a button into an `Action` depending on focus (page, chrome, OSK, menu). The page-mode mapping is the table in the [README](../README.md#host-input-mapping).
- **`core::PointerModel`** moves a virtual pointer with the left stick; Cross clicks at the pointer. The right stick and the D-pad scroll.
- On the PS5 every page action becomes a trusted engine event: `WKPageHandleMouseEvent` (move, down, up), `WKPageHandleWheelEvent`, `WKPageHandleKeyboardEvent`. Clicks therefore carry user activation (fullscreen, autoplay with sound, popups behave as with a mouse).
- **Text entry**: on the PS5 Triangle (address), Square (page field) and a click that focuses a text field open Sony's keyboard dialog (`sceImeDialog`) through `shell::TextInputProvider`, pre-filled with the current text; the committed text navigates or replaces the field's value. The fallback, and the only keyboard on the host, is the built-in OSK: a core model (`core::OnScreenKeyboard`, Latin, Cyrillic and symbol layouts) drawn by `chrome.html`. See ps5-runtime.md, "Text input".
- **USB keyboard**: SDL key events are translated to WPE keysyms for `WKPageHandleKeyboardEvent`; `SDL_TEXTINPUT` text is inserted with `insert_text`.

## Video path

The PlayStation port has no media backend, so the project adds one: `MediaPlayerPrivateFFmpeg` (patch 0900, source of truth in `webkit-port/media/ffmpeg/`).

- It runs in the WebProcess. A demuxer thread (`libavformat`), a video decoder thread (`libavcodec` + `libswscale` to BGRA) and an audio decoder thread (`libswresample` to float stereo 48 kHz) feed packet queues and a ring buffer; the main thread presents frames on a timer.
- HTTP(S) goes through WebCore's `PlatformMediaResourceLoader` (so cookies, CORS, CSP and the NetworkProcess apply) behind a custom `AVIOContext`, with `Range` requests for seeking.
- HLS is native (`<video src=*.m3u8>`) through libavformat's `hls` demuxer: a fixed variant up to 1080p, VOD seeking, live, AES-128.
- Decoding is software only: H.264, HEVC, VP8, VP9; AAC, MP3, Opus, Vorbis, FLAC, AC-3, E-AC-3. No AV1.
- Audio is forwarded to the UI process (`RemoteAudioSink` to the audio server on `127.0.0.1:17772`), which mixes every stream into one SDL device (sceAudioOut); an SDL device in the WebProcess and a `NullAudioSink` (silent, clock still running) are the fallbacks. `PROSPEROWEB_MEDIA_AUDIO=remote|sdl|null` picks the first one tried.
- Video frames normally bypass page painting: the WebProcess hands them to a video overlay in the UI process, which scales them and composes them under the page (`PROSPEROWEB_VIDEO_OVERLAY=0` paints them into the page instead).
- Media Source Extensions (`MediaPlayerPrivateFFmpegMSE`) play YouTube-style players on the same decoders. Players are steered to H.264 up to 1080p by default (VP9 behind `PROSPEROWEB_MEDIA_VP9=1`), because decoding is software only and limited by the CPU cores the WebProcess gets. Details: [webkit-port/media/README.md](../webkit-port/media/README.md).

## Persistence

`core::FileStorage` writes one JSON file per key, atomically (write `.tmp`, rename). On the PS5 the directory is `/data/prosperoweb/`: `tabs.json` (session), `history.json`, `bookmarks.json`, `zoom.json` (page zoom per host), `user_agents.json` (user-agent preset per host), `userscripts.json` (installed user scripts), `config.json` (read-only settings). WebKit's own data (HTTP cache, cookies, localStorage, IndexedDB) lives in subdirectories configured through the website data store. On the host the same files are in `--profile DIR/state/`.

## DRM policy

The browser has no CDM: no Widevine, PlayReady or FairPlay, and it does not try to obtain one. `core::DrmPolicy` covers two cases:

- **Known DRM-only hosts** (Netflix, Disney+, Prime Video, Kinopoisk, Okko and others listed in `src/core/drm_policy.cpp`): the notice is shown before navigation; Cross proceeds, Circle cancels.
- **Any other page** that calls `navigator.requestMediaKeySystemAccess`: the injected EME detector posts a script message and the shell shows the same notice once per origin per session. The call itself fails as the engine reports no key systems.

The notice text (en/ru) is the wording from [research.md §7.3](research.md#73-notice-when-a-site-requests-drm-eme).

## WebKit patch set

All patches are guarded by `PS5_HOMEBREW`; the upstream behaviour is unchanged without it. Details and the editing workflow are in [webkit-port/README.md](../webkit-port/README.md#patches).

| Patch | Purpose |
|---|---|
| `0001-cmake-playstation-homebrew-options` | `PS5_HOMEBREW` build option: no Sony toolchain flags or libraries, static `libWebKit`, no auxiliary executables, `ENABLE_GAMEPAD` without the WPE backend, `USE_FFMPEG` option |
| `0002-wtf-posix-osallocator` | POSIX `OSAllocator` instead of the memory-extra allocator |
| `0003-jsc-generic-shell` | generic `jsc` shell without Sony test wrappers |
| `0004-webcore-curl-ca-bundle` | TLS roots from a PEM file (`$PROSPEROWEB_CA_BUNDLE`, default `/data/prosperoweb/cacert.pem`) |
| `0005-webkit-static-multicall-processes` | launcher calls `ps5_spawn_self()`/`ps5_terminate_process()`; process entry points become `ps5_webkit_*_process_main()` in libWebKit |
| `0006-wtf-payload-sdk-compat` | no Sony libc extensions; clang 18 attribute fix |
| `0007-webcore-playstation-build-fixes` | upstream PlayStation + Skia build breaks; `AF_INET` on FreeBSD |
| `0008-skia-freebsd-malloc-usable-size` | `malloc_usable_size()` from `<malloc_np.h>` |
| `0009-webkit-static-injected-bundle` | injected bundle linked into the executable (weak `WKBundleInitialize`) |
| `0010-webcore-timing-function-destructors` | out-of-line `*TimingFunction` destructors for clang 18 |
| `0900-ffmpeg-media-player` | `MediaPlayerPrivateFFmpeg`: software `<video>`/`<audio>`, progressive and HLS, on the SDK's ffmpeg |

## Build pipeline

```
 scripts/ps5/build-deps.sh     ICU, brotli, FreeType, HarfBuzz, libxkbcommon, libwpe -> third_party/ps5-prefix/
          |
 scripts/ps5/fetch-webkit.sh   WebKit @ pinned commit (sparse, blobless) + patches   -> third_party/webkit/
          |
 scripts/ps5/build-webkit.sh   configure, wtf, jsc, webcore, webkit, package, smoke  -> build-wk-ps5/lib/libWebKitPS5.a
          |                                                                             build-wk-ps5/ps5webkit-link.cmake
 cmake -DPROSPEROWEB_FRONTEND=ps5 ... --target prosperoweb                          -> build-ps5/.../prosperoweb.elf
          |
 scripts/ps5/package.sh        strip debug info, chrome, cacert.pem, fonts,          -> dist/prosperoweb/
                               fonts.conf, config sample, licenses, websrv tile         dist/homebrew/prosperoweb/
          |
 scripts/ps5/native/make-title.sh  native title folder and zip (loader stub eboot)   -> build-dist/PRWB00001.zip
```

Every stage is idempotent and resumable (stamps per dependency, per WebKit stage). The WebKit build takes several hours on 4 cores and about 6 GB of disk. The host frontend and all unit tests build independently with plain CMake.

## Licensing

The distributed `prosperoweb.elf` is **GPL-3.0-or-later**. This is forced, not chosen:

- the ps5-payload-sdk crt and libc are GPLv3+ with no linking exception and are statically linked into every payload ([research.md §4.1](research.md#41-license));
- the spawner (`src/ps5/spawn/elfldr.c`, `pt.c`) is derived from ps5-payload-dev/elfldr, GPL-3.0-or-later;
- the SDK's FFmpeg is built with x264 (GPL), which makes the combined media engine GPL.

The other components are GPL-compatible and keep their own licenses:

| Component | License |
|---|---|
| ProsperoWeb code, elfldr-derived spawner, SDK crt/libc | GPL-3.0-or-later |
| FFmpeg + x264 (SDK build) | GPL |
| WebKit (WebCore, JavaScriptCore, WebKit) | LGPL-2.1 / BSD-2-Clause |
| Skia | BSD-3-Clause |
| ICU | Unicode-3.0 |
| HarfBuzz, libxml2, fontconfig | MIT |
| FreeType | FTL |
| libwpe | BSD-2-Clause |
| curl | curl |
| OpenSSL 3 | Apache-2.0 |
| SQLite | public domain |
| SDL2 | Zlib |
| Noto (incl. CJK and Color Emoji), Liberation fonts | OFL-1.1 |
| DejaVu fonts | Bitstream Vera / DejaVu license |
| `cacert.pem` | MPL-2.0 |

`scripts/ps5/package.sh` writes these notices into `dist/prosperoweb/LICENSES/`. The corresponding source is this repository at the build commit plus the upstream sources named in `build-deps.sh` and `fetch-webkit.sh`.

## Non-goals

These are out of scope for the current phase, not forgotten:

- **DRM / EME CDMs.** No Widevine, PlayReady or FairPlay; no open engine can obtain one. DRM sites get a notice instead.
- **GPU compositing.** The Mesa zink/RADV path in pacbrew is unproven on hardware; the EGL/GLES stubs keep the build linkable until it is.
- **WebGL**, which needs the GPU path.
- **WebAudio** (`ENABLE_WEB_AUDIO` is off in the PlayStation port).
- **JIT.** JavaScriptCore is built with the baseline/DFG JIT (no FTL) and IPInt WebAssembly, both on by default (`JSC_useJIT=true`, `JSC_useWasm=true`; verified on a console, about 1.7x faster than the LLInt interpreter). `config.json` `"env"` turns them off (`docs/ps5-runtime.md`).
