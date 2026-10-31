# ps5-browser

A homebrew web browser for the PlayStation 5. It ports upstream WebKit's PlayStation port to the open [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), adds an ffmpeg media engine and an elfldr-style process spawner, and runs a 10-foot browser shell (tabs, address bar, on-screen keyboard, gamepad pointer) that is shared with a WebKitGTK host frontend for development.

## Status

- The PS5 build compiles, links (`ps5browser.elf`, about 162 MB shipped) and packages.
- **It has not been run on a PS5 yet.** Every console-side behaviour is unverified; the operator checklist is [docs/console-verification.md](docs/console-verification.md).
- The shell, chrome and core run on the host with WebKitGTK and are covered by unit tests and [host screenshots](docs/screenshots/README.md).

## Features

- Tabs, address bar with URL/search normalisation, back/forward/reload, bookmarks and history, session restore.
- 10-foot HTML chrome with an on-screen keyboard (Latin, Cyrillic, symbols) for the address bar and page text fields; English and Russian UI.
- DualSense: virtual pointer on the left stick, trusted clicks and scrolling, tab switching, menu; USB keyboard.
- HTML5 Fullscreen API and a chrome-hiding fullscreen mode.
- HTML5 `<video>`/`<audio>` through an ffmpeg engine: progressive MP4/WebM and native HLS, software decoding (H.264, HEVC, VP8, VP9; AAC, MP3, Opus and others).
- DRM notice: known DRM-only sites and pages that request EME get a clear message instead of a broken player.

## Not working yet

- **YouTube, Twitch and other MSE players**: Media Source Extensions are not implemented; this is the next milestone.
- **DRM sites** (Netflix, Disney+, Kinopoisk, Okko, ...): no CDM, by design.
- **GPU acceleration**: software rendering only; no WebGL, no CSS 3D.
- **WebAudio** and **JIT** (JavaScriptCore runs on its interpreter, so WebAssembly is effectively unavailable).

See [docs/architecture.md](docs/architecture.md#non-goals).

## Requirements

- Build host: Linux x86-64 (Ubuntu 24.04 used), 4+ cores, 15 GB RAM, at least 10 GB free disk (SDK 0.7 GB, WebKit checkout 1.3 GB, WebKit build tree 3.2 GB, dependencies 0.7 GB, plus the frontend build and dist). The WebKit build takes several hours on 4 cores.
- Console: a jailbroken PS5 with ps5-payload-dev/elfldr (firmware coverage: [docs/research.md §2.2](docs/research.md#22-firmware-coverage-as-of-2026-10-30)), an FTP server, optionally websrv for a launcher tile.

## Build from scratch (PS5)

Host packages:

```sh
sudo apt-get install --assume-yes --no-install-recommends \
  clang-18 lld-18 llvm-18 cmake ninja-build ruby perl python3 gperf unifdef \
  pkg-config autoconf automake libtool bison flex wget unzip python3-pyelftools
```

The ps5-payload-sdk with the prebuilt pacbrew libraries (SDL2, curl, OpenSSL, FFmpeg, ...), bundle v0.41.1, into `/opt/ps5-payload-sdk`:

```sh
mkdir -p /tmp/ps5sdk && cd /tmp/ps5sdk
wget https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.41.1/ps5-payload-dev.tar.gz
echo "e811bc2e4676c5bb7f80a8fe928d2985dce599996a5ca6b54da3e72699287887  ps5-payload-dev.tar.gz" | sha256sum --check
tar --no-same-owner -xzf ps5-payload-dev.tar.gz
sudo cp -a opt/ps5-payload-sdk /opt/ && sudo chown -R root:root /opt/ps5-payload-sdk
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

Then, from the repository root:

```sh
scripts/ps5/build-deps.sh          # third_party/ps5-prefix: ICU, HarfBuzz, FreeType, libwpe, ...
scripts/ps5/fetch-webkit.sh        # third_party/webkit at the pinned commit + webkit-port/patches
scripts/ps5/build-webkit.sh all    # build-wk-ps5/: libWebKitPS5.a + ps5webkit-link.cmake
cmake -S . -B build-ps5 -DCMAKE_TOOLCHAIN_FILE=/opt/ps5-payload-sdk/toolchain/prospero.cmake \
      -DPS5BROWSER_FRONTEND=ps5 -DPS5BROWSER_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-ps5 -j4 --target ps5browser
scripts/ps5/package.sh             # dist/ps5browser/ + dist/homebrew/ps5browser/
```

All scripts are idempotent and resumable; their headers list the environment variables (`JOBS`, `PS5_DEPS_PREFIX`, `WEBKIT_BUILD_DIR`, `USE_FFMPEG`, ...). `package.sh` also needs `curl` and ImageMagick (`convert`). Details: [webkit-port/README.md](webkit-port/README.md).

## Install on the PS5

Copy `dist/ps5browser/` to `/data/ps5browser/` (and optionally `dist/homebrew/ps5browser/` to `/data/homebrew/ps5browser/` for the websrv tile) over FTP, then launch from the websrv tile or with `echo file:/data/ps5browser/ps5browser.elf | nc -q0 PS5_IP 9021`. The full steps are in `dist/ps5browser/README-install.txt`; the first-run test plan, with what to capture on failure, is [docs/console-verification.md](docs/console-verification.md).

## Host build, tests and screenshots

The host frontend runs the same shell and chrome on a desktop. It needs CMake 3.20+, a C++20 compiler, GTK 4, WebKitGTK 6.0 and, optionally, SDL2 for a game controller (on Ubuntu 24.04: `libgtk-4-dev libwebkitgtk-6.0-dev libsdl2-dev`, plus `gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav` for video).

```sh
cmake -S . -B build-gtk -DPS5BROWSER_FRONTEND=host-gtk
cmake --build build-gtk -j4
ctest --test-dir build-gtk            # core, shell, spawn helper and media unit tests
build-gtk/src/frontend/host-gtk/ps5browser-host --url https://en.wikipedia.org/wiki/PlayStation_5
```

Options: `--size WxH` (default 1920x1080), `--profile DIR` (default `$XDG_DATA_HOME/ps5browser`; session, history and bookmarks live in `DIR/state`), `--url URL`, `--screenshot PATH --after-ms N` (save a PNG and exit), `--demo[=KIND]` (chrome demo state over `about:blank`), `--lang en|ru`, `--user-agent UA`, `--chrome-dir DIR`, `--input LIST` (press gamepad buttons after start, for automation). `--help` lists them all.

The chrome is loaded from `--chrome-dir`, `$PS5BROWSER_CHROME_DIR`, the install location (`share/ps5browser/chrome-ui`) or the source tree, whichever has `chrome.html` first. `cmake --install` installs the binary and the chrome.

Headless, for example in CI or a container (`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` only where bubblewrap cannot create namespaces):

```sh
WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 xvfb-run -a -s "-screen 0 1920x1080x24" \
  build-gtk/src/frontend/host-gtk/ps5browser-host --demo --screenshot /tmp/chrome.png --after-ms 3000
```

See [docs/screenshots/](docs/screenshots/README.md) for screenshots and the commands that produced them.

### Host input mapping

The keyboard stands in for a DualSense; an SDL2 game controller, if connected, works at the same time. The PS5 uses the same button mapping.

| Key | Button | Action in page mode |
| --- | --- | --- |
| Arrows | D-pad | Scroll |
| Enter | Cross | Click at the pointer |
| Escape | Circle | Back / leave fullscreen |
| F1 | Triangle | Edit the address (OSK) |
| F2 | Square | OSK for the focused text field |
| `[` / `]` | L1 / R1 | Previous / next tab |
| Page Up / Page Down | L2 / R2 | Page up / down |
| F10 | L3 | Hide / show the chrome |
| F11 | R3 | Fullscreen (hide the chrome; leaves page fullscreen) |
| F9 | Options | Menu |
| F8 | Share (Create) | Reload |
| F7 | Touchpad | New tab |
| W A S D (held) | Left stick | Move the pointer |
| I J K L (held) | Right stick | Scroll |

In the OSK: D-pad moves, Cross types the key, Square deletes, Triangle inserts a space, L2 is Shift, R1 switches the layout, Options commits, Circle cancels. In the menu: D-pad up/down selects, Cross activates, Circle goes back.

While a text field in the page or the HTML address bar has focus, only the function keys stay mapped, so typing reaches the field. The real mouse goes straight to the page; moving it hides the gamepad pointer until a stick moves again.

Host limitations:

- WebKitGTK has no public API to inject input events, so gamepad clicks, stick scrolling and `EngineView::key()` are JavaScript fallbacks on the host (`elementFromPoint().click()`, `scrollBy()`, dispatched `KeyboardEvent`s). They are untrusted: no user activation, so for example a page cannot enter fullscreen from a gamepad click. The PS5 frontend delivers real engine events.
- The chrome is a full-window transparent web view over the page. GTK 4 has no per-pixel input shape for a child widget, so the chrome only receives the mouse while it has focus (OSK, menu, notice, address typing) or while the mouse is over the top band.
- Native HLS (`<video src=*.m3u8>`) is behind `WEBKIT_GST_ENABLE_HLS_SUPPORT=1` in WebKitGTK; the host sets it unless it is already set.
- The Ubuntu WebKitGTK build has no EME (`navigator.requestMediaKeySystemAccess` is undefined), so the EME detector never fires on the host. Known DRM-only sites still get the notice before navigation.

## Documentation

- [docs/architecture.md](docs/architecture.md): processes, libraries, code layout, rendering/input/video paths, patch set, build pipeline, licensing, non-goals.
- [docs/console-verification.md](docs/console-verification.md): first-run checklist for a real PS5.
- [docs/ps5-runtime.md](docs/ps5-runtime.md): on-disk layout, configuration, environment, process model and spawn contract.
- [webkit-port/README.md](webkit-port/README.md): the WebKit port, patches, link contract.
- [webkit-port/media/README.md](webkit-port/media/README.md): the ffmpeg media engine.
- [src/ps5/spawn/README.md](src/ps5/spawn/README.md): the elfldr-derived spawner and its origin.
- [docs/research.md](docs/research.md): scene research and the architecture decision.
- [docs/screenshots/README.md](docs/screenshots/README.md): host screenshots and probe pages.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The ps5-payload-sdk crt/libc are GPLv3+ and statically linked into the payload, the spawner is derived from GPL-3.0-or-later elfldr, and the SDK's FFmpeg includes x264, so the browser as a whole is distributed under the GPL. WebKit, Skia, ICU and the other libraries keep their own (compatible) licenses; see [docs/architecture.md](docs/architecture.md#licensing).

Vendored third-party code keeps its own license, see [third_party/README.md](third_party/README.md).
