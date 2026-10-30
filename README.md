# ps5-browser

A homebrew web browser for the PlayStation 5. It is built on a port of upstream WebKit's PlayStation port to the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), with a small platform-independent core (tabs, history, bookmarks, gamepad input, on-screen keyboard) shared between a host development frontend and the PS5 frontend.

**Status:** work in progress.

## Layout

- `src/core/` — platform-independent models: tabs, URL normalizer, history, bookmarks, storage and session, gamepad mapper, pointer, on-screen keyboard, fullscreen state, DRM policy, injected page scripts.
- `src/shell/` — engine-independent browser logic (`ShellController`) and the interfaces a frontend implements: `EngineView`/`EngineViewClient`/`EngineFactory` for the web engine and `ChromeUi` for the browser chrome.
- `src/chrome-ui/` — the browser chrome as one offline HTML page (`chrome.html`, `chrome.css`, `chrome.js`) rendered from the shell state via `window.ps5chrome.render(state)`; `chrome.html?demo=1|osk|notice|menu` shows static demo states.
- `src/frontend/host-gtk/` — `ps5browser-host`, a desktop frontend on GTK 4 and WebKitGTK 6.0 for development.

## Build

TBD for the PS5 payload.

## Host build

The host frontend runs the same shell and chrome on a desktop. It needs CMake 3.20+, a C++20 compiler, GTK 4, WebKitGTK 6.0 and, optionally, SDL2 for a game controller (on Ubuntu 24.04: `libgtk-4-dev libwebkitgtk-6.0-dev libsdl2-dev`, plus `gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav` for video).

```sh
cmake -S . -B build-gtk -DPS5BROWSER_FRONTEND=host-gtk
cmake --build build-gtk -j4
ctest --test-dir build-gtk            # core and shell unit tests
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

The keyboard stands in for a DualSense; an SDL2 game controller, if connected, works at the same time.

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

- WebKitGTK has no public API to inject input events, so gamepad clicks, stick scrolling and `EngineView::key()` are JavaScript fallbacks on the host (`elementFromPoint().click()`, `scrollBy()`, dispatched `KeyboardEvent`s). They are untrusted: no user activation, so for example a page cannot enter fullscreen from a gamepad click. The PS5 frontend must deliver real engine events.
- The chrome is a full-window transparent web view over the page. GTK 4 has no per-pixel input shape for a child widget, so the chrome only receives the mouse while it has focus (OSK, menu, notice, address typing) or while the mouse is over the top band.
- Native HLS (`<video src=*.m3u8>`) is behind `WEBKIT_GST_ENABLE_HLS_SUPPORT=1` in WebKitGTK; the host sets it unless it is already set.
- The Ubuntu WebKitGTK build has no EME (`navigator.requestMediaKeySystemAccess` is undefined), so the EME detector never fires on the host. Known DRM-only sites still get the notice before navigation.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The ps5-payload-sdk crt/libc are GPLv3+ and statically linked into the payload, so the browser as a whole is distributed under the same license.

Vendored third-party code keeps its own license, see [third_party/README.md](third_party/README.md).
