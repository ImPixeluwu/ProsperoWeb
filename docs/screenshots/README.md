# Host screenshots

Screenshots of `prosperoweb-host` (the WebKitGTK 6.0 host frontend, see "Host build" in the top-level README) taken under Xvfb at 1920x1080 and downscaled to 1280x720, 256 colours. Everything except `host-demo-chrome.png` is the real shell driving real WebKitGTK views; the UI states (OSK, menu, fullscreen) are reached by pressing gamepad buttons through `--input`, which goes through the same `ShellController::on_button()` path as the keyboard and an SDL2 controller.

Host used: Ubuntu 24.04, GTK 4.14.5, WebKitGTK 6.0 2.52.6, GStreamer 1.24 (good/bad/libav).

## How they were produced

All commands run from the repository root after building with `-DPROSPEROWEB_FRONTEND=host-gtk` into `build-gtk`. `shot` below is this helper:

```sh
shot() {  # shot NAME AFTER_MS [host args...]
  name=$1; after=$2; shift 2
  prof=$(mktemp -d)
  WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 xvfb-run -a -s "-screen 0 1920x1080x24" \
    build-gtk/src/frontend/host-gtk/prosperoweb-host --profile "$prof" \
    --screenshot "/tmp/$name-full.png" --after-ms "$after" "$@"
  convert "/tmp/$name-full.png" -resize 1280x720 -strip -dither None -colors 256 \
    "PNG8:docs/screenshots/$name.png"
}
P="file://$PWD/docs/screenshots/pages"
W=https://en.wikipedia.org/wiki/PlayStation_5
```

`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` is needed only because bubblewrap cannot create user namespaces in the container these were taken in (`bwrap: setting up uid map: Permission denied`). The screenshot is taken with ImageMagick `import -window root` (the host picks it automatically on X11, see `HostApp::take_screenshot()`).

| File | Command | What it shows |
| --- | --- | --- |
| `host-demo-chrome.png` | `shot host-demo-chrome 3000 --demo` | `chrome.html?demo=1` over `about:blank`: three tabs (one loading), address bar, nav indicators, gamepad pointer ring. Static demo state, no shell state involved. |
| `host-page-wikipedia.png` | `shot host-page-wikipedia 8000 --url $W` | A real page loaded through the shell with the chrome band on top; the page view starts below the 96 px band. |
| `host-osk.png` | `shot host-osk 7000 --url $W --input triangle,l1,l1,l1,l1,l1,l1,l1,l1,l1,l1,l1,l1,down,down,right --input-delay-ms 5000 --input-step-ms 100` | Triangle opens the on-screen keyboard on the address bar with the current URL and the caret at its end; twelve L1 presses move the caret back to just after the `P` of `PlayStation_5` (typing and Square now act there), and the D-pad moved the key focus to `s`. The bottom row has the Clear and caret arrow keys; the hint bar lists the L1/R1 caret and Touchpad layout bindings. |
| `host-osk-page-field.png` | `shot host-osk-page-field 5000 --url "$P/form.html" --input square,down,right,right,right,cross,triangle,cross,options --input-delay-ms 2500 --input-step-ms 150` | Square opens the OSK for the focused page field; `r`, space, `r` typed, Options commits; the text lands in the page via `insert_text` and fires one `input` event. |
| `host-drm-notice.png` | `shot host-drm-notice 5000 --url https://www.netflix.com/` | Navigating to a known Widevine-only host shows the DRM notice before the load; Cross proceeds, Circle cancels. The home page (DuckDuckGo) stays behind it. |
| `host-menu-bookmarks.png` | `shot host-menu-bookmarks 8000 --url $W --input options,down,down,cross,options,cross --input-delay-ms 5000` | Options menu: "Add bookmark" on the loaded page, then the Bookmarks section listing it. |
| `host-fullscreen.png` | `shot host-fullscreen 7000 --url $W --input r3 --input-delay-ms 5000` | R3 (F11 on the keyboard) hides the chrome; the page view grows to the full window. |
| `host-chrome-events.png` | `shot host-chrome-events 9000 --url "$P/form.html" --input pointer:309x22,cross,wait,pointer:150x22,cross,wait,pointer:1881x70,cross --input-delay-ms 2500 --input-step-ms 700` | The chrome event path end to end: Cross with the pointer over the band clicks the HTML chrome, which posts `tab_new` (the "+" opens the home page in a second tab), `tab_select` (back to the first tab) and `menu_open` (the menu button) to the shell. `pointer:XxY` places the pointer like a mouse move, so the gamepad ring is hidden. |
| `host-video-mp4.png` | `shot host-video-mp4 15000 --url "$P/video.html?src=https://archive.org/download/BigBuckBunny_124/Content/big_buck_bunny_720p_surround.mp4"` | Progressive MP4 (H.264/AAC) plays through GStreamer: `readyState 4`, `currentTime` about 12 s, no error. |
| `host-video-hls.png` | `shot host-video-hls 15000 --url "$P/video.html?src=https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"` | HLS plays natively (`<video src=.m3u8>`, no hls.js): 1920x1080, `currentTime` about 11 s. Needs `WEBKIT_GST_ENABLE_HLS_SUPPORT=1`, which the host sets by default; without it `canPlayType()` is empty and playback fails with `MEDIA_ERR_SRC_NOT_SUPPORTED` (code 4). |

## Store artwork previews

`store-icon-preview.png` (the launch icon at 512 and 128 px) and `store-background-preview.png` (the home screen background at 1280x720) are written by `scripts/assets/render.sh` from `assets/*.svg`; see docs/store-submission.md.

## Probe pages

`pages/` holds the local test pages used above:

- `video.html?src=URL` plays a media URL muted and prints `canPlayType()`, `readyState`, `currentTime`, errors and media events under the video.
- `form.html` has an autofocused text field and counts `input` events.
- `eme.html` asks for `com.widevine.alpha` through `navigator.requestMediaKeySystemAccess()`. On this host's WebKitGTK build the API does not exist at all (also on `http://localhost`), so the EME detector script has nothing to hook and the script-message DRM path cannot be shown here; it is covered by the shell unit tests.

## PS5 loading screen

`ps5-splash.png` is the UI process's loading screen as the console draws it into the SDL window surface before the first WebKit frame (half the startup stages done). It is rendered on the host by `tests/ps5/splash_shot.cpp`, which runs the same drawing (`src/ps5/display/splash.cpp`) and FreeType text (`src/frontend/ps5/ps5_splash_text.cpp`) code:

```sh
g++ -std=c++20 -O2 -Isrc/ps5/display -Isrc/frontend/ps5 $(pkg-config --cflags freetype2) \
  tests/ps5/splash_shot.cpp src/frontend/ps5/ps5_splash_text.cpp \
  src/ps5/display/splash.cpp src/ps5/display/damage_region.cpp \
  $(pkg-config --libs freetype2) -o /tmp/splash_shot
/tmp/splash_shot dist/prosperoweb/fonts/NotoSans-Regular.ttf /tmp/ps5-splash.ppm 0.5
convert /tmp/ps5-splash.ppm -resize 1280x720 -strip -dither None -colors 256 \
  PNG8:docs/screenshots/ps5-splash.png
```
