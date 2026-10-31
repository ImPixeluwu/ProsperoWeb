# PS5 homebrew browser: scene research (2026-10-30)

> Historical record. The project is now called ProsperoWeb (https://github.com/kvaps/ProsperoWeb); this document was written when it was still ps5-browser (later briefly ProsperoBrowser), and its names, paths and status statements are left as they were. For the current state see the [README](../README.md) and [console-verification.md](console-verification.md).

## How to read this

This document merges six research notes written on 2026-10-30. Nothing was run on a console; every on-device behaviour is either cited or marked unknown. Each claim carries one evidence tag:

- `[verified in code]`: checked in the cited source, given as `file:line` in a repository at the commit listed in Sources (§8). Paths such as `sdk/crt/rtld.c` are relative to that repository's root.
- `[per authors]`: taken from a project's README, docs, release notes, help pages or issues (URL or README line given).
- `[secondary]`: press, forum or social-media reports.
- `[estimate]`: the researcher's own judgement, including all person-week figures and unsourced "well-known industry behaviour" (marked `[estimate]` in §7, where the video researcher used a separate "known but not fetched" tag; verify those on a device).
- **unknown**: could not be verified. Not to be read as "probably".

Source-note tags were normalised to this vocabulary: the SDK note's `[V]`/`[A]`/`[?]` became `[verified in code]`/`[per authors]`/unknown, and the engine note's `[per docs]` became `[per authors]`.

Contradictions between the input notes are kept side by side and flagged with **Contradiction:**.

## 1. Summary of findings

- **No browser engine port exists for PS5 or PS4 homebrew.** GitHub searches for WebKit, Chromium, Servo, NetSurf, litehtml and Ultralight on PS5/PS4 return nothing; the only console engine ports are NetSurf and a V8+Skia runtime on Switch (§6). `[verified in code]`/`[per authors]`
- **Every PS5 "browser app" in the scene is a local HTTP payload plus the console's built-in WebKit browser.** A payload opens the system browser at any URL with `sceSystemServiceLaunchWebBrowser(uri, NULL)` (SDK sample, shsrv `browse`, Payload Manager, CheatRunner, Web File Manager) and keeps running as a daemon behind it (§3.6, §3.8). `[verified in code]`
- **Home-screen tiles are cheap:** write `param.json` (`"applicationCategoryType": 66048`, `"webAppUri"`, `"downloadDataSize": 256` for localStorage) and call `sceAppInstUtilAppInstallTitleDir` (§3.7). `[verified in code]`
- **System browser engine:** WebKit with JavaScriptCore, UA `AppleWebKit/605.1.15 … Version/13.0`; actual WebKit revision and whether JSC JIT is enabled are unknown. The chrome is a React-Native/Hermes UI that is not fullscreen; an in-RAM GPL-3.0 patch makes it 1920x1080 (§3.1). `[verified in code]`/`[per authors]`
- **System browser input:** DualSense buttons arrive as `keydown` codes (Cross=13, Circle=27, D-pad 37–40), the pointer snaps to focusable elements, the on-screen keyboard is modal; no examined app uses the Gamepad API (§3.2). `[verified in code]`
- **System browser media:** native HLS plays (hardware path per authors), MSE is good enough for hls.js and Shaka, EME exposes PlayReady only (no Widevine, no FairPlay). Codecs beyond H.264/AAC, Fullscreen API, WebSocket, IndexedDB, Service Workers and `window.open` are unknown (§3.4). The inputs disagree on MSE, EME and fullscreen; see the contradictions in §3.4. `[verified in code]`/`[per authors]`
- **Firmware coverage:** some exploit path exists for 1.00–13.60; Relapse (browser exploit, MIT) covers 7.00–13.60; 14.00 reportedly fixes the Relapse bug. The SDK's kernel offset table covers 1.00–14.10, but that is SDK coverage, not exploit availability (§2). `[verified in code]`/`[secondary]`
- **The open SDK is usable and installed on this host:** ps5-payload-dev/sdk (clang/lld 18 wrappers, target `x86_64-sie-ps5`, PIE ELF), libc++ 18.1.8 with exceptions/RTTI/threads, and the pacbrew bundle v0.41.1 (SDL2, curl 8.18.0 + OpenSSL 3.5.2, FreeType/HarfBuzz, FFmpeg 7.0.1, QuickJS, RmlUi, Mesa 26.2.0). hello_world, an SDL2 window+gamepad program and a C++20 program were cross-compiled to PS5 ELFs (§4.9). `[verified in code]`
- **Licensing is fixed:** the SDK crt/libc are GPLv3+ with no linking exception and are statically linked into every payload, so any distributed binary is effectively GPL-3.0-or-later (§4.1). `[verified in code]`
- **Process model is the main blocker for real engines:** no open payload spawns its own ELF with `fork`/`posix_spawn`; the only proven pattern is elfldr's `rfork_thread` + `execve` of a signed system SELF + ptrace injection, which needs kernel R/W (§4.3). `[verified in code]`
- **Graphics:** the proven payload baseline is a CPU framebuffer at 1080p via `libSceVideoOut` (SDL2 port). Vulkan/GL via a Mesa RADV/zink port appeared in pacbrew on 2026-10-10 and is labelled "AI slop" by its own maintainer; Sony's `libSceGLSlimVSH` is a stub with no known user (§4.4). `[verified in code]`
- **Engine ranking (estimates):** upstream WebKit PlayStation port 25–40 person-weeks; hybrid QuickJS-extractor player 4–8; Ladybird 35–60 (clang ≥19 + Rust blocker); WPE 30–50; Servo 30–50 (no MSE); NetSurf 2–4 (no usable JS/media). Only a WebKit build might fit this host (6–12 h, 6–12 GB) (§5). `[estimate]`
- **DRM:** no open engine can obtain a Widevine or PlayReady CDM. YouTube, Twitch, Vimeo, Dailymotion, VK Video, OK.ru, Rutube, PeerTube, Internet Archive, Jellyfin/Emby/Plex and public HLS/IPTV work without DRM; Kinopoisk, Okko, Start, KION, Wink, Amediateka, Netflix, Disney+, Prime Video, Max, Apple TV+, Crunchyroll and Tubi do not (§7).
- **YouTube's TV UI (`youtube.com/tv`) requires a TV/console User-Agent**; the desktop web client needs MSE above 360p and since 2025 uses the SABR/UMP protocol (§7). `[per authors]`/`[secondary]`

## 2. Homebrew environments and firmware coverage

### 2.1 Environments: license and activity

Sources were shallow-cloned read-only; nothing was executed.

| Project | License (LICENSE file) | Last commit / release | FW range | What it gives apps | Browser launch? |
|---|---|---|---|---|---|
| ps5-payload-dev/elfldr | GPL-3.0 (file present) | 2a8da00, 2026-10-21 | Bootstrapped by any loader on port 9020 (BD-J, WebKit, Relapse, …) | ELF loader on :9021, raw TCP; also HTTP GET `?uri=` with `Access-Control-Allow-Origin: *`, so a web page can launch payloads; loads from `file:` or http(s) URL | No; web pages call it |
| ps5-payload-dev/websrv | GPL-3.0 | 7748df9, 2026-10-11 | Any jailbroken FW (needs elfldr) | HTTP :8080: launcher UI, `/elfldr`, `/fs`, `/smb`, `/mdns`; launches homebrew (`eboot.elf` + `homebrew.js`), daemons, payloads, titles; installs launcher tile | Indirect: tile opens the browser at 127.0.0.1:8080 |
| ps5-payload-dev/shsrv | GPL-3.0 | 6f32063, 2026-06-28 | Any (needs elfldr) | Shell on :2323: `launch`, `hbldr`, `pkg_install`, `suspend`, `http2_get` | Yes: `browse <URL>` |
| ps5-payload-dev/ftpsrv | GPL-3.0 | 35bae13, 2026-10-20 | Any | FTP server | No |
| ps5-payload-dev/klogsrv | GPL-3.0 | c1b64bd, 2026-04-14 | Any | /dev/klog on :3232 | No |
| ps5-payload-dev/sdk | GPL-3.0 | a7685ce, 2026-10-20 | n/a | Payload SDK, dynamic linking to Sony sprx stubs | Yes: `samples/browser` |
| LightningMods/etaHEN | GPL-3.0 | dafa13b, 2026-05-03; release 2.5B 2025-12-25 | Not stated in README | AIO HEN: Debug Settings toolbox, plugins, kstuff (fself/FPKG), elfldr :9021, FTP :1337, klog :9081, Direct PKG installer (:9090, WebUI :12800), Jailbreak IPC, Itemzflow, update blocker | Generic `sceShellUIUtilLaunchByUri` and `sceLncUtilLaunchApp`; no web-browser launch found |
| kstuff (sleirsgoevy); active fork EchoStretch/kstuff-lite | No LICENSE file (ps5-payload-dev/kstuff 3962120, 2025-01-26; kstuff-lite) | kstuff-lite pushed 2026-10-20, release v1.11 2026-09-20 | 1.00–13.60; FPKG "up to 11.40" + 11.60 fix | Kernel patches for fself/FPKG/NPDRM | n/a |
| hammer-83/ps5-jar-loader (BD-J) | No LICENSE file (`license: null`) | pushed 2025-03-09 | "7.61 and earlier" | Remote JAR loader; UMTX kernel r/w; Byepervisor (<3.00) | n/a |
| ps5-payload-dev/bdj-ipv6-hen (BD-J ISO) | README says GPLv3+, no LICENSE file | 9b90c6c, 2026-05-11 | 3.00–4.51 | BD-J Xlet running ELF payloads, downloads them online | n/a |
| PS5Dev/PS5-UMTX-Jailbreak (WebKit+UMTX) | Unlicense (file present) | 2cf6778, 2024-10-08 | 1.00–5.50 (ELF loader broken on ≥5.00) | Kernel r/w, loader :9021 | Loaded in the browser via the User's Guide trick |
| ntfargo/Relapse-Exploit (WebKit + `aio_multi_wait` UAF) | MIT (file present) | dd8e4e0, 2026-09-30 | 7.00–13.60 | Kernel r/w, loader :9021 | Runs in the browser |
| Gezine/Y2JB (YouTube app userland) | MIT | 79c1be4, 2026-08-03 | ≥4.03 (YouTube PKG pinned); Lapse kernel payload "only up to 10.01" | Userland → Lapse → :9021 | n/a |
| Gezine/Luac0re (PS2-emu Lua) | MIT (GitHub metadata) | pushed 2026-09-19 | JIT userland on "latest PS4/PS5 firmwares"; Poopsploit kernel ≤12.00 | Userland JIT | n/a |
| itsPLK/ps5-payload-manager | GPL-3.0 | pushed 2026-10-19 | Any jailbroken | Web payload dashboard + home-screen shortcut | Yes |
| owendswang/ps5-web-file-manager | GPL-3.0 | 04c11b1, pushed 2026-10-29 | Any jailbroken | Web file manager :8888 + Media-tab launcher | Yes (reopens browser after layout patch) |
| notmaj0r/CheatRunner | GPL-3.0 | pushed 2026-10-26 | Any jailbroken | Web cheat trainer :9999 | Yes |
| X-F1REBALL-X/wk-autoloader | GPL-3.0 | pushed 2026-10-30 | 7.00–13.60 | Replaces the Store tile with an offline (AppCache) Relapse page that auto-opens Elf Launcher | Store tile is a WebKit web view |

The ps5-payload-dev org (John Törnblom) is very active: sdk, elfldr, ftpsrv and pacbrew-repo were all pushed in October 2026 (`gh repo list ps5-payload-dev`). GitHub reports no SPDX license for some org repos, but the files carry GPL headers. `[verified in code]`

Evidence for the table:

- elfldr HTTP mode replies with `Access-Control-Allow-Origin: *`: `elfldr/socksrv.c:185-189` @2a8da00 `[verified in code]`. `?uri=` usage with curl: elfldr README lines 31-35 `[per authors]`. A web page launching payloads through it: `launchpad/docs/elfldr.js:2` (`ELFLDR_URL = "http://localhost:9021"`) and `:224` (`fetch(ELFLDR_URL + '?' + params)`) @033d2a3 `[verified in code]`.
- websrv title launch: `sys_launch_title()` uses `sceSystemServiceKillApp` + `sceSystemServiceLaunchApp`, `websrv/src/ps5/sys.c:378-412` @7748df9. Launcher tile: `install_launcher()` writes `/user/app/FAKE00000/sce_sys/param.json` and calls `sceAppInstUtilAppInstallTitleDir` (NID `Wudg3Xe3heE`) from constructor `sys_init()`, `sys.c:476-487,512-538,573`; tile `param.json` is `{"titleId":"FAKE00000","deeplinkUri":"http://127.0.0.1:8080", ...}`, `websrv/param.json:1-10` `[verified in code]`. README: websrv "can be used to launch homebrew via the Webkit browser embedded with the PS5" (README:4) `[per authors]`.
- shsrv: `launch_browser()` calls `sceSystemServiceLaunchWebBrowser(argv[1])`; dispatcher maps `"browse"` to it, `shsrv/bundles/launch/launch.c:36,42-54,89-90` @6f32063 `[verified in code]`.
- etaHEN: `ItemzLaunchByUri()` loads `/system_ex/common_ex/lib/libSceShellUIUtil.sprx` and calls `sceShellUIUtilLaunchByUri(uri,{size,userId})`, `Source Code/daemon/source/main.cpp:275-297` @dafa13b; URIs `pshomeui:navigateToHome?bootCondition=psButton` and `pssettings:play?mode=settings&function=debug_settings` at `main.cpp:430-446`; `launchApp("ITEM00001")` via `sceLncUtilLaunchApp` at `main.cpp:194-209,448`. A case-insensitive grep for `LaunchWebBrowser|webbrowser|WebView|deeplink` finds only error-code tables (`util/include/error_translator.hpp:5929-5995`) and a notification `"actionType": "DeepLink"`: no browser-launch feature `[verified in code]`. Feature list: https://github.com/LightningMods/etaHEN README "Features" `[per authors]`.
- kstuff-lite v1.11 notes: "Kstuff Lite 1.11 Beta Supports Firmware 1.00-13.60 … Added Fpkg Upto 11.40 … Fixed Fpkg For 11.60", https://github.com/EchoStretch/kstuff-lite/releases `[per authors]`.
- ps5-jar-loader: "vulnerabilities discovered in BD-J layer of PS5 firmware version 7.61 and earlier", https://github.com/hammer-83/ps5-jar-loader `[per authors]`.
- UMTX: FW list 1.00–5.50 and "FW >= 6.00 requires new WebKit exploit", `README.md:10-34` @2cf6778 `[per authors]`.
- Relapse: `supportedFirmware` list 7.00 … 13.60, gated by UA regex `/PlayStation 5\/(\d+\.\d+)/`, `src/firmware.js:3-14` @dd8e4e0 `[verified in code]`; README says the browser stage uses "JSC info leaks" `[per authors]`.
- Y2JB: `README.md:7-12,127-129` @79c1be4 `[per authors]`. Luac0re: https://github.com/Gezine/Luac0re README lines 9-16 `[per authors]`.
- Itemzflow (https://github.com/LightningMods/Itemzflow, GPL-3.0, "PS4 Game manager and Home menu alternative") was not audited for browser launch: unknown.

### 2.2 Firmware coverage (as of 2026-10-30)

| Entry / environment | FW range | Type | Source |
|---|---|---|---|
| PS5-UMTX-Jailbreak (WebKit/psfree + UMTX) | 1.00–5.50 (ELF loader broken ≥5.00 in this repo) | browser | README @2cf6778 (2024-10-08) `[per authors]` |
| idlesauce PS5-Exploit-Host | 3.00–4.51 (README list) | browser | https://github.com/idlesauce/PS5-Exploit-Host (pushed 2025-12-02) `[per authors]` |
| bdj-ipv6-hen | 3.00–4.51 | BD-J disc | README @9b90c6c (2026-05-11); also the repo description "firmwares 3.00-4.51" `[per authors]` |
| ps5-jar-loader (BD-JB + UMTX, Byepervisor <3.00) | ≤7.61 | BD-J disc | README (pushed 2025-03-09) `[per authors]` |
| Y2JB (YouTube app) + Lapse | userland ≥4.03; Lapse kernel ≤10.01 | app | README @79c1be4 `[per authors]` |
| Luac0re + Poopsploit | kernel payload ≤12.00; JIT userland "latest" | game (PS2 emu) | README (pushed 2026-09-19) `[per authors]` |
| **Relapse** | **7.00–13.60** | browser | `src/firmware.js` @dd8e4e0 (2026-09-30) `[verified in code]` |
| 14.00 / 14.10 | 14.00 reportedly fixes the Relapse browser bug; 14.10 reportedly requires that firmware for PSN access | — | https://tech-insider.org/ps5-firmware-14-10-psn-jailbreak-lockout-2026/ ; https://videocardz.com/newz/ps5-13-60-jailbreak-released-almost-every-ps5-can-now-be-jailbroken `[secondary]` |
| kstuff-lite (fself/FPKG) | 1.00–13.60; FPKG "up to 11.40" + 11.60 fix | kernel patch | release v1.11 (2026-09-20) `[per authors]` |
| etaHEN | Not stated in README. Latest GitHub release 2.5B (2025-12-25). "2.6b up to 12.70, 13.60 confirmed" is a social-media claim only | HEN | https://github.com/LightningMods/etaHEN/releases ; https://x.com/onejailbreak_/status/2089614100225495334 `[secondary]` |
| elfldr / websrv / shsrv / ftpsrv | Firmware-agnostic; need a kernel exploit plus the bootstrap loader | payload | READMEs `[per authors]` |
| ps5-payload-sdk kernel offset table | 1.00 … 14.10 (`sdk/crt/kernel.c:302` `case 0x1000000` … `:568` `case 0x14100000`; default `-ENOSYS` at `:578`) | SDK | `[verified in code]`. This is SDK coverage, not exploit availability |

Third-party native apps report working on 13.00/13.60 with ShadowMountPlus/kstuff (`RafaelNGP_xbox-cloud-gaming-ps5/README.md`) `[per authors]`. The SDK README lists the compatible loaders under "ELF loaders known to work".

**Contradiction (soft):** the SDK supports kernel offsets up to 14.10, but no public exploit covers anything above 13.60 per the sources above; 14.00 reportedly closes the Relapse path `[secondary]`.

## 3. The console's built-in browser

### 3.1 Engine, User-Agent and chrome

| Context | UA | Source |
|---|---|---|
| Ordinary browser | `Mozilla/5.0 (PlayStation; PlayStation 5/2.26) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/13.0 Safari/605.1.15` | matomo device-detector `Tests/fixtures/console.yml:381` @5b92fc6 `[verified in code]` |
| TV/media variant | `Mozilla/5.0 (PlayStation 5 3.03/SmartTV) AppleWebKit/605.1.15 (KHTML, like Gecko)` | same file, line 399 |
| TV/media variant, no FW | `Mozilla/5.0 (PlayStation 5/SmartTV) AppleWebKit/605.1.15 ...` | same file, line 417 |

- Relapse's firmware detection confirms the `PlayStation 5/<fw>` token is still present on 7.00–13.60, `src/firmware.js:12-13` `[verified in code]`.
- The real WebKit trunk revision per firmware is **unknown**. `605.1.15` is a frozen Safari token, not a build revision. psdevwiki.com/ps5 returned Cloudflare 403 and could not be checked.
- JavaScript engine: JavaScriptCore (Relapse README, "JSC info leaks") `[per authors]`. Whether the JIT is enabled in the shipping browser is **unknown**. Upstream WebKit's PlayStation port sets `ENABLE_JIT/DFG/FTL OFF` by default (`Source/cmake/OptionsPlayStation.cmake:176-178` @0c7b760, 2026-08-28) `[verified in code]`; that is a hint only, since the shipping build differs from upstream.
- Other upstream PlayStation-port defaults (same file; not proof of what ships): `ENABLE_FULLSCREEN_API ON` (:184), `ENABLE_GAMEPAD` only with experimental features (:198), `ENABLE_WEBGL OFF` (:230), `ENABLE_WEB_AUDIO OFF` (:219), `ENABLE_REMOTE_INSPECTOR ON` (:186). The media backend is not part of the upstream port `[verified in code]`.
- **Contradiction:** the engine study states the system browser "is Sony's build of the upstream WebKit PlayStation port" and that it "has no MSE for arbitrary sites" (engines note §0); the environment study says the shipping build differs from upstream and is unverifiable, and that MSE works well enough for hls.js and Shaka (§3.4). Neither claim about MSE carries a test on a console.
- **Chrome:** not fullscreen by default. owendswang/ps5-web-browser-fullscreen (GPL-3.0, pushed 2026-10-30) "make[s] the PS5 web browser fullscreen (1920x1080) while keeping the analog-stick pointer, X click, circle back and localhost access. Changes are applied in memory"; it detects "the browser's scripting engine" and credits Hermes bytecode definitions, so the chrome is a React-Native/Hermes UI. The patch must be reapplied after a reboot or a language change. https://github.com/owendswang/ps5-web-browser-fullscreen `[per authors]`
- Settings: a "Settings → System → Browser → Clear Cache" entry exists (CheatRunner README line 126) `[per authors]`.

### 3.2 Input (DualSense, remote, keyboard)

All `[verified in code]` unless marked.

- websrv launcher UI (`websrv/assets/main.js:220-288` @7748df9): `document.addEventListener("keydown")` with L2=118, L1=116, R1=117 and arrows 37/39. No Gamepad API anywhere in `assets/` (grep for `gamepad|getGamepads` is empty).
- Navigation relies on the browser pointer snapping to focusable elements: `generateCursorSnapOverlays()` creates fixed `<a tabIndex=0>` overlays so "the cursor snaps" (`carouselView.js:251-326`; `filePickerView.js:99-100`).
- Circle/back is handled through `popstate`/history; a comment says "you can listen to keycode 27" (`router.js:303`). Text input uses `window.prompt`/`confirm` (`main.js:83`, `filePickerView.js:318-323`).
- tv4play/plutotv/launchpad `input.js` key map: Cross 13 (Enter), Circle 27 (Escape), arrows 37–40, Triangle 112 (F1), Square 113 (F2) (`tv4play/webapp/input.js:4-12` @69fb2df). HDMI-CEC remote keys 131–216 (MediaTopMenu, MediaRewind, MediaFastForward, MediaPause, colour keys 152–155, ChannelUp/Down 157/158, MediaTrackNext/Prev 176/177, MediaStop 178, Guide 216) "arrive through the same keydown" (`input.js:14-35`; README:31-56). Some builds report only `e.key`, so code falls back to it (`input.js:103-119`). Key repeat is throttled in JS (`:38-42`).
- On-screen keyboard: focusing a text field opens the system OSK, which is modal: "On the console the keyboard is modal -- the page sees none of it while it is open -- so the field's own blur event is the reliable signal to resume" (`plutotv/webapp/input.js:77-86` @b6d8f3e). The TV4 author calls typing search queries "miserable even with the system keyboard" (README:196-197) `[per authors]`.
- Gamepad API: no examined app uses it; real support is **unknown**. USB mouse/keyboard in the browser: **unknown**.

### 3.3 Storage, networking, windows

- localStorage works: websrv `assets/localStore.js:43-206`; svtplay `docs/js/app.js:839` ("localStorage behaves, so it is the only backend here") `[verified in code]`. For web-app tiles, adding `"downloadDataSize": 256` to `param.json` is the change the commit messages call "enable local storage" / "add support for local storage" (svtplay f5d5e48 2026-09-01, tv4play 6fd0235, plutotv f79a998) `[per authors]`.
- AppCache works and persists for the Store-tile web view (wk-autoloader README) `[per authors]`.
- Standard CORS applies and the page origin cannot be spoofed; serving from `localhost` is used to satisfy services that allow a fixed origin list (tv4play README:141-155; plutotv README:280-304) `[per authors]`. Mixed content: an https page cannot `fetch` http payload endpoints (`launchpad/docs/elfldr.js:231-235`) `[verified in code]`.
- WebSocket, IndexedDB, Service Workers, `window.open`/tabs, multiple windows: **unknown**.

### 3.4 Media and DRM

- **Native HLS** (`video.canPlayType("application/vnd.apple.mpegurl")`) is "the path the console decodes in hardware": `tv4play/webapp/app.js:943-946,1558-1564` `[verified in code]`; README:90-94 `[per authors]`.
- **Known HLS bug:** native HLS "silently ignores the alternate audio group" (picture, no sound); workaround is hls.js over MSE (`app.js:1196-1200,1529-1554`) `[verified in code]`. Native HLS can stall with no events, so apps add a 15 s watchdog, retry with hls.js, then probe `MediaSource.isTypeSupported`/`canPlayType` per codec (plutotv README:215-236) `[per authors]`.
- **EME:** `navigator.requestMediaKeySystemAccess("com.microsoft.playready", …)` with avc1/mp4a succeeds. "The console reports PlayReady through EME, and neither Widevine nor FairPlay." (`tv4play/webapp/tv4.js:1114-1144` `[verified in code]`; tv4play README:174-191, plutotv README:238-241 `[per authors]`). Whether a commercial service actually issues a PlayReady licence to this browser is untested ("could not be established without trying it"). Widevine-only services (Netflix, YouTube TV and similar) will not play protected content.
- **Codecs:** H.264/AAC confirmed by the above. HEVC, VP9, AV1, Opus, 4K and HDR: **unknown**. svtplay only maps codec strings to names (`docs/js/app.js:1068-1078`). A third-party test page says "The WebKit build exposes FEWER codecs than the console hardware decodes" but publishes no matrix (https://tembrica.com/en/ps5-browser-test) `[per authors]`.
- **Fullscreen API:** no examined app calls `requestFullscreen`; behaviour **unknown**. The chrome has to be patched to get a 1080p viewport.
- **YouTube/Twitch:** reports that video sites play "flawlessly" are anecdotal; some sites (Spotify, Google Docs) fail (https://mp1st.com/news/there-is-a-hidden-ps5-web-browser-and-heres-how-to-access-it; https://www.gamepressure.com/newsroom/ps5-has-hidden-web-browser/z12843) `[secondary]`. Ars Technica testing (quoted by psu.com, mp1st.com and gamepressure, 2020–2021) found YouTube and Twitch video largely worked, but fullscreen did not; the video researcher infers MSE with at least H.264/AAC and "probably VP9" `[secondary]`; VP9 is otherwise unknown.
- **Contradiction (EME and decode path):** robots.net (https://robots.net/tech/ps5-browser-media-playback-codec-support-list/) claims no Widevine/EME at all, a "stripped down" MSE and software rather than hardware decoding `[secondary]`, which conflicts with the code-verified PlayReady probe and the authors' "decodes in hardware" statement above. Resolve with an on-console probe page (MSE `isTypeSupported` matrix, `requestMediaKeySystemAccess` probe, Fullscreen API).
- **Contradiction (Fullscreen API):** the video-services note says "the stock PS5 browser lacks" the Fullscreen API (based on the Ars Technica report); the environment note says behaviour is unknown and the upstream port enables it; the architecture section (verbatim below) lists "HTML5 Fullscreen API" among the features that come from Sony's WebKit. Unverified either way.

### 3.5 How to reach it without a jailbreak

The browser is hidden. Entry points: the account-linking web view (Settings → Users and Accounts → Linked Services → YouTube → Link) or a captive-portal URL (video note, `[secondary]`); the YouTube/Twitch account-link "Use Browser" route (https://bgr.com/1941821/how-to-use-ps5-web-browser/) `[secondary]`; the **User's Guide trick** `[per authors]` (`PS5-UMTX-Jailbreak/README.md:36-42` @2cf6778; idlesauce/PS5-Exploit-Host README, `A manuals.playstation.net 192.168.1.2`):

1. Point `manuals.playstation.net` to your own host via a fake DNS.
2. Set the PS5's primary DNS to that host (secondary `0.0.0.0`).
3. Open Settings → User's Guide.
4. Accept the untrusted-certificate prompt.

### 3.6 Launching it from a payload

- `sceSystemServiceLaunchWebBrowser(const char* uri, void* reserved /*NULL*/)` from `libSceSystemService`; call `sceUserServiceInitialize(0)` first. SDK sample: `int sceSystemServiceLaunchWebBrowser(const char *uri, void*);` with `main()` calling `sceSystemServiceLaunchWebBrowser(PS5_URL, 0)` (`sdk/samples/browser/main.c:29,48` @a7685ce); stub at `sdk/sce_stubs/libSceSystemService.c:1710`; constructor at `:35-40` initialises user service; links `-lSceSystemService -lSceUserService`. SDL's `SDL_OpenURL` does the same (`SDL/src/misc/ps5/SDL_sysurl.c:24-37`) `[verified in code]`. The sample is not an embeddable engine; it only opens the system browser.
- The same symbol exists in PS4 OpenOrbis headers (`include/orbis/SystemService.h`), per GitHub code search.
- Other launch APIs: `sceShellUIUtilLaunchByUri(uri, {size,userId})` from `/system_ex/common_ex/lib/libSceShellUIUtil.sprx` for `pshomeui:`/`pssettings:` deep links; `sceLncUtilLaunchApp` / `sceSystemServiceLaunchApp` for title IDs `[verified in code]` (etaHEN, Web File Manager). A browser-specific URI scheme is **unknown**.
- Browser title ID: NPXS20102 per a community spreadsheet (https://docs.google.com/spreadsheets/d/1SkQT1AZJCybePhOgGu6qzRbtChwn4AkIi_eO2nlDAbI) `[secondary]`, unverified; psdevwiki was unreachable.
- No open homebrew embeds `libSceWebKit2`/`libSceNKWebKit`; code-search hits are only exploit chains and the SDK sysmodule table `[verified in code]`.

### 3.7 Home-screen tile mechanics

- Write `/user/app/<TID>/sce_sys/param.json` + `icon0.png` (+ `pic1.png`), then call `sceAppInstUtilAppInstallTitleDir(TID, "/user/app/", 0)`; resolve it by NID `Wudg3Xe3heE` and fall back to `sceAppInstUtilAppInstallAll` `[verified in code]`.
- Two `param.json` variants: (a) websrv, `"deeplinkUri": "http://127.0.0.1:8080"`; (b) svtplay/tv4play/plutotv/launchpad, `"applicationCategoryType": 66048, "webAppUri": "<url>"`, plus `"downloadDataSize": 256` for localStorage. The tile appears on the Media tab.
- tv4play also writes a stub `/system_ex/app/<TID>/eboot.bin` + `param.json` after remounting `/system_ex` read-write (`tv4play/main.c` `remount_system_ex`, `install_app`, `main` @69fb2df; `sce_sys/param.json`) `[verified in code]`.
- Localhost apps put a tinyurl in `webAppUri` (`5n6c86jf` → `http://localhost:8080/`, `26bt55fj` → `http://127.0.0.1:8080/`) instead of a direct `http://localhost` URL (checked with curl HEAD). Why: **unknown** (possibly the field requires https or is validated).
- Persistence: tiles survive a reboot ("can be launched without having to jailbreak the console after a reboot", svtplay README:18-20) `[per authors]`. Localhost-served apps (tv4play, plutotv) still need websrv running, which needs a jailbreak each boot; the tv4play README's "no jailbreak" note (README:73-74) is **inconsistent** with that. Only remotely hosted apps (svtplay → github.io, launchpad) truly work without a jailbreak.
- **Contradiction (FAKE00000):** the environment note says websrv installs a launcher tile at `/user/app/FAKE00000` with `deeplinkUri`; the SDK note says websrv/shsrv's `hbldr` creates a fake app `FAKE00000` at `/system_ex/app/FAKE00000` from the PS Now eboot (`NPXS40106`) to host A/V homebrew (§4.3). The same title ID appears to be used for both purposes; how they interact is unknown.

### 3.8 Existing "daemon + system browser" apps

- websrv, Payload Manager, CheatRunner and Web File Manager run as background daemons (HTTP on 8080/8888/9999 …) while the user is in the browser.
- Payload Manager: `ps5_launch_browser()` → `sceSystemServiceLaunchWebBrowser(uri)`, https://github.com/itsPLK/ps5-payload-manager/blob/main/src/ps5_launcher.c lines 117-126 (default branch, 2026-10-30) `[verified in code]`.
- CheatRunner: `cr_browser_open_url()`, https://github.com/notmaj0r/CheatRunner/blob/main/src/cr_browser.c lines 7-16 `[verified in code]`. A launched game competes for resources: "The PS5 browser competes with the game for resources" (README:275) `[per authors]`.
- Web File Manager: `navigate_to_home()` (`pshomeui:` URI via ShellUIUtil), then `browser_patch()`, then `sceSystemServiceLaunchWebBrowser("http://127.0.0.1:<port>/#hash", NULL)` (`filemgr.c:50-62,1400-1423,1474-1475` @04c11b1) `[verified in code]`. The payload keeps running while the browser is closed and reopened (README line 117) `[per authors]`.
- websrv "Known Issues": "Homebrew sometimes crashes when there is already a previous homebrew running" (`websrv/README.md:66`) `[per authors]`.
- ps5-payload-dev web apps (svtplay, tv4play, plutotv, launchpad) are 10-foot web apps in the system browser that show the full pattern: D-pad focus model, CEC keys, HLS/hls.js/Shaka, PlayReady.

## 4. Open SDK, toolchain and runtime

Repositories and commits are in §8. Paths are relative to each repository (`sdk/` = ps5-payload-dev/sdk @a7685ce8, `elfldr/` @2a8da002, `pacbrew-repo/` @722187f0, `SDL/` = ps5-payload-dev/SDL @ee4c47dc, `websrv/` @7748df9d, `shsrv/` @6f320637, `jtplay/` @2945def1). The SDK moved: `github.com/ps5-payload-dev/ps5-payload-sdk` returns 404; it now lives at `github.com/ps5-payload-dev/sdk` (README title still "ps5-payload-sdk"); `john-tornblom/ps5-payload-sdk` is the archived original. Third-party mirrors (e.g. `JMUtechnologies/ps5-payload-sdk`) were ignored.

### 4.1 License

`sdk/README.md:75-77`: `include/freebsd` is BSD; "the rest are licensed under the GPLv3+". The crt (`sdk/crt/crt.c:1-15`), libc additions (`sdk/libc/dlfcn.c:1-15`) and toolchain wrappers (`sdk/host/bin/prospero-clang` header) carry plain GPLv3+ headers; no linking exception was found (`grep -n exception sdk/crt/*.c` is empty). Every payload statically links `crt1.o` + `libc.a`, so a distributed browser binary is effectively GPLv3+. Third-party projects acknowledge this (`Sp9nky_unofficial-stremio-ps5-port/THIRD_PARTY.md`: "ps5-payload-sdk libc.a, start-up code ... GPL-3.0-or-later") `[verified in code]`.

### 4.2 Compiler, libc, libc++

- Host compiler: stock upstream clang/lld, any version 15..23 (`sdk/Makefile.inc:23-26` LLVM_CONFIG candidates); README requires clang-18/lld-18 (`sdk/README.md:17`). The SDK has no compiler of its own `[verified in code]`.
- Target: `-target x86_64-sie-ps5` (`sdk/host/bin/prospero-clang:71`), flags `-fno-stack-protector -fno-plt -femulated-tls` (`:77`). TLS is emulated (`__emutls`, `sdk/libc/emutls.c`) `[verified in code]`.
- Default link libs: `crt1.o`, `-lc` (SDK's small libc.a), `-lkernel_web`, `-lSceLibcInternal -lSceNet` (`prospero-clang:26-28`); for clang ≥20 the driver appends crt itself (`:60-63`, via `SCE_PROSPERO_SDK_DIR`). `prospero-clang++` adds `-lunwind -lc++abi -lc++` and `-stdlib++-isystem target/include/c++/v1` (libc++ static) `[verified in code]`.
- Linker: `ld.lld -m elf_x86_64 -pie -T elf_x86_64.x --eh-frame-hdr -z max-page-size=0x4000 -mllvm -emulated-tls` (`sdk/host/bin/prospero-lld:25,62`; the installed wrapper also passes `--hash-style=gnu`). Output is a PIE ELF with 16 KiB pages `[verified in code]`.
- CMake toolchain: `CMAKE_SYSTEM_NAME FreeBSD`, `CMAKE_SYSTEM_VERSION 9`, `BUILD_SHARED_LIBS OFF`, PIC on, prefix `/user/homebrew` (`sdk/host/toolchain/prospero.cmake:25-26,51-52`). Meson and pkg-config wrappers exist (`prospero-meson`, `prospero-pkg-config`) `[verified in code]`.
- libc: the real libc is Sony's FreeBSD-derived `libSceLibcInternal.sprx` + `libkernel_web.sprx`; the SDK adds a gap-filling `libc.a` (regex, fts, netdb, locale stubs, raw syscall wrappers; `sdk/libc/*.c`). `libdl.a` and `libpthread.a` are empty archives (`sdk/libc/Makefile`); pthreads come from libkernel `[verified in code]`.
- C++: libc++/libc++abi/libunwind **LLVM 18.1.8**, static, RTTI=YES, EXCEPTIONS=YES, THREADS=YES, `LIBUNWIND_IS_BAREMETAL=YES` (`sdk/libcxx.sh:19,42-56`; pacbrew `libcxx` 18.1.8). `sdk/samples/hello_cxx23/main.cpp` uses `<print>`, `<ranges>`, `std::thread`, `throw/catch` `[verified in code]`.
- Locale: `setlocale` from Sony libc; `newlocale/uselocale/*_l` are dummy "C"-only stubs (`sdk/libc/no-locale.c:262-560`; newlocale returns a calloc'ed dummy at `:375-377`). iconv is not in libc (GNU libiconv 1.17 package). ICU is missing `[verified in code]`.

### 4.3 Runtime linking and process model

- The crt contains its own runtime linker (`sdk/crt/rtld*.c`). `payload_init` (`sdk/crt/crt.c:50-80`) initialises syscalls, kernel R/W, klog, patches, then `__rtld_init` (`sdk/crt/rtld.c:520-566`) brings up the `.sprx` loader, the `.so` loader and `dlfcn` `[verified in code]`.
- `.sprx` system libs are looked up in `/system/priv/lib`, `/system/common/lib`, `/system_ex/...` and a randomized path (`sdk/crt/rtld.c:181-221`), loaded via `sceSysmoduleLoadModuleInternal(<id>)` from a 137-entry table, then `sceKernelLoadStartModule(path)` (`sdk/crt/rtld_sprx.c:60-200,233-246,446-478`); symbols resolved name→NID `[verified in code]`.
- `.so` homebrew shared libs: own ELF loader (`sdk/crt/rtld_so.c`), search path `LD_LIBRARY_PATH`, `/user/homebrew/lib`, cwd (`sdk/crt/rtld.c:223-246`); `dlopen/dlsym/dlclose/dladdr` forward to it (`sdk/libc/dlfcn.c:23-73`, `sdk/crt/rtld_dlfcn.c:63,131`); sample `sdk/samples/hello_so`. dlopen of our own ELF `.so` works `[verified in code]`.
- Link-time stubs are empty `.so` files with soname `X.sprx` generated from the `aerolib.csv` NID db (`sdk/sce_stubs/Makefile`). 34 ship, including libSceAgc (202 symbols), libSceAgcDriver, libSceAppInstUtil, libSceAudioOut, libSceGLSlimVSH (203 EGL/GLES symbols), libSceGnmDriver, libSceHttp, libSceHttp2, libSceImeDialog, libSceKeyboard, libSceLibcInternal, libSceNet, libSceNetCtl, libSceNotification, libScePad, libScePosixForWebKit, libSceRegMgr, libSceRemoteplay, libSceSsl, libSceSysmodule, libSceSystemService, libSceUserService, libSceVideoOut, libkernel, libkernel_sys, libkernel_web, libmonosgen-2.0 `[verified in code]`.
- Loadable by name but not stubbed (sysmodule table in `sdk/crt/rtld_sprx.c`): libSceVdecCore/Savc2/Shevc/Svp9 (`:183-186`), libSceWebKit2/libSceNKWebKit/libSceWeb (`:139-140,195-197`), libSceLibreSsl(3) (`:130-131`), libSceHttpCache. No libSceMouse, libSceAvPlayer, libSceIme (non-dialog) or libSceVideodec2 entries. Third-party apps add their own stubs (e.g. `Sp9nky_unofficial-stremio-ps5-port/native/stubs/videodec2.c`) `[verified in code]`.
- No SCE API headers ship: `sdk/include/ps5/` has only `kernel.h, klog.h, mdbg.h, iommu.h, nid.h, payload.h`; samples declare prototypes by hand. Khronos headers ship (`sdk/include/khronos/{EGL,GLES2,KHR}`) `[verified in code]`.
- Kernel R/W API for payloads: `sdk/include/ps5/kernel.h` (`kernel_copyin/copyout`, `kernel_set_ucred_authid/caps`, `kernel_set_proc_rootdir`, `kernel_mprotect`, …); primitives arrive from the loader in `payload_args_t` (`sdk/crt/payload.h`) `[verified in code]`.
- **How elfldr runs payloads.** It bootstraps via ptrace into `SceRedisServer` and runs in the background; each payload runs in its own process (`elfldr/README.md:5-9` `[per authors]`; `pt_attach` at `elfldr/main.c:83` `[verified in code]`). Spawn (`elfldr/elfldr.c:603-700`): `rfork_thread(RFPROC|RFCFDG|RFMEM)` (`:624`) → child `ptrace(PT_TRACE_ME)` + `execve("/system/vsh/app/NPXS40112/eboot.bin")` = SceSpZeroConf (`:74,593`) → parent breaks after libkernel relocation, sets libc heap to unlimited (`elfldr_set_heap_size(pid,-1)` `:682`), breaks before `main()`, maps the payload ELF in (`elfldr_exec`) `[verified in code]`.
- **Contradiction (elfldr exec target):** the engine note describes elfldr as `rfork_thread(RFPROC|RFCFDG|RFMEM, …)` then `execve()` on a temp `.self` (`elfldr/selfldr.c:121,147`), while the SDK note describes `execve` of the SceSpZeroConf system eboot (`elfldr/elfldr.c:74,593,624`). Both are cited at the same commit; they may be two code paths (SELF loading vs ELF loading), which neither note confirms.
- Privileges (`elfldr/elfldr.c:440-510`): during load it escapes jail / root vnode / uid 0 / caps 0xff…, then restores the host process' jaildir, rootdir, caps and authid before detaching. The payload starts in SceSpZeroConf's sandbox and can elevate itself with the kernel API (pattern in `sdk/samples/test_privileges/main.c`: authid `0x4800000000010003` and full caps) `[verified in code]`.
- **A/V homebrew needs a foreground "big app".** shsrv/websrv provide `hbldr`, which creates fake app `FAKE00000` at `/system_ex/app/FAKE00000` from the PS Now eboot (`NPXS40106`), starts it with `sceSystemServiceLaunchApp`, ptrace-follows it from `SceSysCore.elf` and injects the ELF (`websrv/src/ps5/hbldr.c` ~L80-330) `[verified in code]`. shsrv README: "To run payloads that use audio or video, you can use the hbldr command" (`shsrv/README.md:39`) `[per authors]`.
- Alternative packaging by newer third-party apps: directory-style native titles (`PPSAxxxxx` with `eboot.bin` + `sce_sys/param.json`) under `/data/homebrew`, installed by ShadowMountPlus (drakmor, ★1.2k) + kstuff, built with ps5-payload-sdk + `ps5-native-app-boilerplate` crt (`ZiZc3_VLC/README.md` Install; Stremio `THIRD_PARTY.md`) `[per authors]`. The `blackbearreloaded/*` repos (ps5-opengl, ps5-native-app-boilerplate, ps5-homebrew-ui) are deleted from GitHub; only mirrors exist (`darkxex/ps5-opengl`, `JMUtechnologies/ps5-native-app-boilerplate`, `leolobato/ps5-native-app-boilerplate` → upstream `drakmor/ps5-native-app-boilerplate`). Ecosystem churn is high.
- **Spawning own processes.** `fork`, `vfork`, `rfork`, `pdfork` exist as raw syscall wrappers (`sdk/libc/syscalls.c:49,220,640,2081`); `execve`/`fexecve` at `syscalls.c:1371,1831`; `rfork_thread`, `execve`, `waitpid`, `kill`, `sigaction`, `pipe`, `socketpair`, `shm_open/shm_unlink`, `sem_open`, `kqueue`, `mmap/mprotect` in libkernel stubs; `posix_spawn(p)` in the libSceLibcInternal stub; `sceKernelSpawn` in the libkernel_sys stub `[verified in code]` that the symbols exist; whether they work is **unknown**.
- Actual use: process creation in open homebrew is only `rfork_thread` + `execve` of a signed system eboot + ptrace injection (`elfldr/elfldr.c:624`, `shsrv/elfldr.c:675`, `websrv/src/ps5/elfldr.c:682`). shsrv uses `rfork_thread(RFPROC|RFCFDG, …)` without exec for builtins (`shsrv/builtin.c:152`). No open PS5 payload uses plain `fork()`+`execve()` of its own ELF; an unsigned ELF cannot be execve'd. websrv's CrispyDoom build patches `fork()` out (`websrv/homebrew/CrispyDoom/build.sh:55`); `popen/pclose` are unimplemented stubs (`sdk/libc/popen.c:20-31`) `[verified in code]`.
- Consequence: a multi-process engine needs a custom spawn helper re-implementing elfldr's spawn (rfork_thread → execve system eboot → ptrace → map ELF), which needs kernel R/W. Single-process is the safe assumption `[estimate]`.

### 4.4 Threads, memory, JIT

- pthreads from libkernel; C11 `thrd_*` wrappers (`sdk/libc/thrd.c:52-122`); crt sets `__isthreaded=1` (`sdk/crt/crt.c:63-67`) `[verified in code]`.
- mmap: non-exec → raw syscall; **PROT_EXEC → map RW then `kernel_mprotect`**, which writes the protection byte into the `vm_map_entry` via kernel R/W (`sdk/libc/mman.c:76-111`, `sdk/crt/kernel.c:1584-1624`). An LLVM 14 MCJIT example ships in `pacbrew-repo/llvm/ps5-jit-example/main.c` `[verified in code]` (mechanism); W^X/RWX semantics are untested (unknown). Note: the `llvm` package is not in the prebuilt bundle (§4.8).
- The SDK also stubs `sceKernelJitCreateSharedMemory` and `sceKernelJitCreateAliasOfSharedMemory` (`sdk/sce_stubs/libkernel_sys.c:3318-3324`) `[verified in code]`. Third-party native titles use `sceKernelJitCreateSharedMemory/JitMapSharedMemory` and note it "needs the process jailbroken first" (`ZiZc3_VLC/ps5/compat/mmap.c:1-20,300-310`) `[per authors]`. The PS4 technique is one RX mapping plus one RW alias, never RWX on one mapping (https://github.com/CTurt/cturt.github.io/blob/master/ps4-3.html) `[per authors]`; a PS5 kexp README claims a jitshm RWX region works from a jailbroken payload (https://github.com/itsPLK/ps5-kexp) `[per authors]`. The engine researcher's conclusion: design interpreter-only, treat JIT as a bonus `[estimate]`.
- Memory: in a native-title context "flexible memory is about 400 MiB … direct memory: gigabytes"; plain anonymous mmap fails in a title process, so VLC wraps mmap (`ZiZc3_VLC/ps5/compat/mmap.c:3-17`) `[per authors]`. elfldr payloads get an unlimited libc heap (`elfldr/elfldr.c:682`) `[verified in code]`. The per-process ceiling for elfldr payloads is **unknown**.
- **Contradiction (memory budget):** the engine note's WPE estimate (150–300 MB per simple page, 500 MB–1 GB+ with YouTube) says it "fits in 8 GB" `[estimate]`; the only cited per-process figure is ~400 MiB flexible memory in a title process. The usable budget for a payload is unverified.
- CPU: x86-64 with AVX2 (used by the SDL tilemap); the CPU model was not verified in the SDK note, while the engine note assumes 8 Zen2 cores `[estimate]`.

### 4.5 Graphics

- **SDL2 PS5 video driver** (`SDL/src/video/ps5/SDL_ps5video.c`) `[verified in code]`: `sceVideoOutOpen(0xff,0,0,NULL)` (`:206`); 64 MiB direct memory via `sceKernelAllocateMainDirectMemory`+`sceKernelMapDirectMemory` (`:210-219`); 2 buffers, `sceVideoOutSetBufferAttribute2(..., 0x8000000022000000, ...)`, `sceVideoOutRegisterBuffers2` (`:236-241`); flip rate 0 = 60 Hz (`:232`). Mode 1920×1080 ABGR8888 @60 (`:197-200`); a 3840×2160 mode is defined but its registration is commented out (`:118-124`). Pure software framebuffer: the surface is CPU-detiled into the GPU tiled layout with AVX2 (`SDL_ps5tilemap.c:26,30,106`, tile 512×128, `_mm256_*`), then `sceVideoOutSubmitFlip` and wait on the flip event queue, vsync-locked (`SDL_ps5video.c:84-108`). GL via OSMesa (software) loaded with dlopen `libOSMesa.so.8` (`SDL_ps5osmesa.c:33-91`).
- **pacbrew SDL2 patch** (`pacbrew-repo/SDL2/0001-ps5-opengl-opengles-and-vulkan-through-mesa.patch:1-11`) `[verified in code]`: OSMesa removed; GL/GLES windows via Mesa EGL "videoout" platform (zink on RADV), Vulkan via RADV `libvulkan.so.1` with VK_KHR_display; the display reports Mesa's 3840×2160 swapchain as its only mode. Built `-DSDL_OPENGL=YES -DSDL_OPENGLES=YES -DSDL_VULKAN=YES`.
- **Mesa caveat** (`pacbrew-repo/mesa/PKGBUILD:10-14`) `[verified in code]`: "These patches are AI slop generated by claude.ai. It seems to work, but comments in the code are not to be trusted ... some human domain expert needs to go through all of these patches". Added 2026-10-10 (commit fa5edf73b1, "mesa: add AI slop implementation of vulkan and opengl"); patches originate from `mihawk-99/PS5_Mesa`. Built `-Dvulkan-drivers=amd -Dgallium-drivers=zink -Dplatforms=videoout -Degl=enabled -Dgles2=enabled -Dllvm=disabled` (`mesa/PKGBUILD:133-158`). The RADV winsys calls `sceAgcDriverSubmitDcb`, `sceAgcInit`, `sceVideoOut*`, `sceKernelAllocateDirectMemory`.
- Hardware GPU APIs: Vulkan via RADV-on-AGC (pacbrew mesa 26.2.0) `[verified in code]`, used by ZiZc3/VLC ("Vulkan (RADV)", `ZiZc3_VLC/ps5/build-title.sh:19,104` links `libvulkan_radeon.ps5.a` from mihawk-99 PS5_Vulkan) and xbox-cloud-gaming (`vkCreateInstance`). GL/GLES via zink (pacbrew) or the "ps5-opengl" SDK (Mesa+OpenGNM, OpenGL 4.6 per Stremio `THIRD_PARTY.md`) `[per authors]`. Sony's `libSceGLSlimVSH` is stubbed but has no known open user (usability unknown). `libSceAgc`/`AgcDriver`/`GnmDriver` are raw command-buffer APIs with no shader compiler, consumed only by Mesa's RADV winsys `[verified in code]`.
- 4K: SDL framebuffer path is 1080p; the Mesa path is a 3840×2160 swapchain; VLC claims "Up to 4K at 60 Hz output" and HDR10 `[per authors]`.
- The Mesa shared libraries (`libEGL/libGL/libGLESv2/libgallium-26.2.0.so`, `libvulkan.so`) are the only shared libs in the bundle; SDL2 would dlopen them on the console. Unverified: the smoke test only links.

### 4.6 Input, audio, video decode

- Gamepad: `scePadInit`, `scePadOpen(user_id, PS5_PAD_PORT_TYPE_STANDARD, ...)`, `scePadReadState`, `scePadSetVibration`, `scePadSetLightBar` (`SDL/src/joystick/ps5/SDL_ps5joystick.c:226,355,361,412,442,461`); users via `sceUserServiceGetLoginUserIdList` (`:305`). Media remote via `scePadOpen(..., PORT_TYPE_REMOTE_CONTROL)` (`SDL/src/video/ps5/SDL_ps5remote.c:123-156`) `[verified in code]`.
- USB keyboard: `sceKeyboardInit/Open/ReadState` (`SDL/src/video/ps5/SDL_ps5keyboard.c:105-108,146,179,248`) `[verified in code]`.
- On-screen keyboard: `sceImeDialogInit/GetStatus/GetResult/Term` backs `SDL_StartTextInput` (`SDL_ps5keyboard.c:113-117,189,212,231,342`; `ShowScreenKeyboard` in `SDL_ps5video.c`); also used by xbox-cloud-gaming-ps5 and the Stremio port `[verified in code]`.
- Mouse: no `libSceMouse` stub, not in the sysmodule table, no SDL PS5 mouse code. OpenNOW PS5 mentions a "mouse sensitivity" setting `[per authors]`; how it works is unknown.
- Audio: `sceAudioOutInit`, `sceAudioOutOpen(USER_ID_SYSTEM, ..., samples, 48000, fmt)`, blocking `sceAudioOutOutput`; S16/F32 mono/stereo, fixed 48 kHz (`SDL/src/audio/ps5/SDL_ps5audio.c:52-56,70-78,91,133`) `[verified in code]`.
- Software decode is what ps5-payload-dev players use: jtplay `avcodec_find_decoder` + `thread_count=0` + `sws_scale` to YUV420P + `SDL_UpdateYUVTexture` (`jtplay/src/player.cpp:358,364,845-863,1216-1227`), with RmlUi + QuickJS + curl (`jtplay/CMakeLists.txt:8-20`). FFplay and RetroArch (`--enable-sdl2 --enable-ffmpeg --disable-vulkan --disable-opengl`, `websrv/homebrew/RetroArch/build.sh:48-83`) are also software `[verified in code]`.
- Hardware decode via `libSceVideodec2` is used by third-party GPL-3.0 apps: ZiZc3/VLC (H.264/HEVC up to 4K 10-bit), RafaelNGP/xbox-cloud-gaming-ps5 (1080p60 H.264), OpenCloudGaming/OpenNOW-PS5, Sp9nky/unofficial-stremio-ps5-port (`src/hwdec_ps5.cpp`, `native/stubs/videodec2.c`, ported from Nuvio PS5). `sceVideodec2*` calls `[verified in code]` by grep; capability claims `[per authors]`.
- `libSceAvPlayer`: no open PS5 user (`gh search code "sceAvPlayerInit ps5"` → 0); usability unknown. Kodi: VivaLaVent/kodi-ps5 → VivaLaVent/PS5MC (Kodi 21/22 fork) `[per authors]`, not analysed.

### 4.7 Networking

- BSD sockets from libkernel; elfldr/websrv/ftpsrv/shsrv are socket servers on 9021/8080/2121/2323 (READMEs + `elfldr/socksrv.c`) `[verified in code]`.
- DNS: SDK `gethostbyname(_r)`/`gethostbyaddr` → `sceNetResolverStartNtoa/Aton`, IPv4 only (`sdk/libc/netdb.c:40-49,112-130,279,335`); `getaddrinfo` comes from system libs (stub in `libScePosixForWebKit.c`; also libkernel) `[verified in code]` (symbol only). The VLC recipe overrides `getaddrinfo` (`ZiZc3_VLC/ps5/build-title.sh:111`), suggesting the system one is insufficient in a title context `[per authors]` (inferred).
- TLS: OpenSSL 3.5.2 static is the default; curl 8.18.0 is built `--with-openssl` with a Mozilla CA bundle at `/user/homebrew/etc/ca-bundle.crt` via `mk-ca-bundle.pl` (`pacbrew-repo/curl/PKGBUILD:30-36`). System `libSceSsl`/`libSceHttp`/`libSceHttp2` stubs exist; `sdk/samples/http2_get` uses `sceHttp2*` `[verified in code]`.
- No nghttp2 (curl has no HTTP/2) and no brotli (no `br` content-encoding) `[verified in code]`.

### 4.8 Package list (pacbrew-repo)

pacbrew is built from source on Debian/Ubuntu with `makepkg` + `pacman-package-manager` from apt, then `./ci-libs.sh` (whose first line is `sudo pacman --remove ps5-payload-dev`; `pacman.conf` has only `SigLevel = Never`). **No binary pacman repository exists**; the only binary distribution is the release tarball (§4.9) `[per authors]`. The list below is from the PKGBUILDs at tag v0.41.1 (codeload tarball sha256 `2ac52bf30fef06e312e838ed58e4031a3adb482f5e97735dcca409d0a163000d`) `[verified in code]`. Licenses are those recorded from PKGBUILD `license` fields in the SDK note; "—" means the inputs did not record one. "No" in the last column means a PKGBUILD exists but the package is not in the prebuilt bundle.

| Area | Packages (version) | License (as recorded) | In prebuilt bundle |
|---|---|---|---|
| SDK/toolchain | sdk (git), libcxx 18.1.8, openmp 18.1.8, openlibm 0.8.6, llvm 14.0.6 (MCJIT, X86 only) | GPLv3+ (sdk); Apache2 (libcxx/openmp/llvm/openlibm, as grouped) | yes; llvm **no** |
| Windowing/UI | SDL2 (git, ps5-payload-dev/SDL + mesa patch), SDL2_gfx 1.0.4, SDL2_image 2.8.2, SDL2_mixer 2.8.0, SDL2_net 2.2.0, SDL2_ttf 2.22.0, SDL2_kitchensink 2.0.0, imgui 1.91.0, rmlui 6.2 | SDL2 recorded "LGPL" (sic; SDL is zlib); add-ons zlib/MIT; imgui/rmlui MIT | yes |
| GPU | mesa 26.2.0 (RADV Vulkan + zink GL/GLES + EGL "videoout"), glew 2.3.1, glu 9.0.3, glm 1.0.1, vulkan-headers 1.4.354 | MIT etc. | yes |
| Text/fonts | freetype 2.13.2, harfbuzz 9.0.0, fontconfig 2.15.0, libfribidi 1.0.15, libiconv 1.17 | FTL/GPL2, MIT, custom, LGPL2.1, — | yes |
| Images | libpng 1.6.43, libjpeg-turbo 3.0.2, libwebp 1.4.0, giflib 5.2.2; libyuv r2828+ce488afb | — | yes; libyuv **no** |
| Markup/data | libxml2 2.13.5, expat 2.6.2, tinyxml2 10.0.0, lexbor 3.0.0 (HTML/CSS parser), sqlite 3.46.1, libpsl 0.21.5, json-c 0.19, jansson 2.14, yaml-cpp 0.9.0, fmt 12.1.0, fast_float 6.1.6, libconfig 1.7.3, libb64 1.2.1 | MIT, MIT, apache, Apache-2.0, rest — | yes; lexbor **no** |
| Scripting | libquickjs 2026.06.04, lua 5.4.6, luajit 2.1.1748459687 (ffi on) | MIT (quickjs) | yes |
| TLS | openssl 3.5.2 (BSD-x86_64, static, no-apps); mbedtls 3.6.4, wolfssl 5.7.2, libressl 4.1.0, bearssl 0.6 | Apache-2.0; Apache2 / GPL-2+ / ISC / MIT | openssl yes; others **no** |
| Network | curl 8.18.0, libevent 2.1.12, enet 1.3.18, libmicrohttpd 1.0.1, libssh 0.12.0, libssh2 1.11.1, libsmb2 6.2, libnfs 5.0.3, libmicrodns (git), miniupnpc 2.2.4, libnatpmp 20230423, dht 0.27, libutp r207.490874c, libsodium 1.0.19 | curl; rest — | yes |
| Compression/archive | zlib 1.3.2, zstd 1.5.6, xz 5.4.6, bzip2 1.0.8, libdeflate 1.19, libarchive 3.7.4, libminizip 1.3.2, file 5.46; lz4 1.10.0, libzip 1.11.2, 7zip 2409 | various | yes; lz4/libzip/7zip **no** |
| Media | ffmpeg 7.0.1 (static, `--enable-gpl --enable-version3`, openssl, libass, x264), libx264 (stable), libvpx 1.14.1, opus 1.5.2, libvorbis 1.3.7, libogg 1.3.6, flac 1.4.3, lame 3.100, mpg123 1.32.7, faad2 2.11.1, libmpeg2 0.5.1, libtheora 1.1.1, libass 0.17.3, openal 1.24.3, libsndfile 1.2.2, libsamplerate 0.2.2, libmodplug 0.8.9.0, libmad 0.16.4; a52dec 0.8.0 | GPL (ffmpeg); rest — | yes; a52dec **no** |
| Daemons | shsrv (git), websrv (git); elfldr, ftpsrv, gdbsrv, klogsrv, offact (git) | — | shsrv/websrv yes; rest **no** |
| Games/engines | love 11.5; devilutionx 1.5.5, eduke32 20240725, fbneo (git), lakesnes (git), lbreakouthd 1.1.8, mednafen 1.32.1, scummvm 2.8.1 | — | love yes; rest **no** |

Total: 109 PKGBUILDs at v0.41.1, 23 of them not in the prebuilt bundle.

- **Contradiction (package count):** the SDK note counts **115 entries** in pacbrew-repo @722187f0 (2026-10-16), while the toolchain note lists **109 PKGBUILDs** at tag v0.41.1 (2026-10-10). The six-entry difference was not itemised; it may come from the different snapshots or from counting non-package directories. Unresolved.
- **Absent** (checked with `ls pacbrew-repo`): ICU, glib, cairo, pixman, libsoup, nghttp2, brotli, woff2, lcms2, libavif/dav1d/aom, libepoxy, gperf/ruby/perl-at-runtime, Skia, Python `[verified in code]`.
- Installed pkg-config versions (`prospero-pkg-config --modversion`) mostly match the PKGBUILDs; minor mismatches: SDL2_gfx 1.0.2 vs 1.0.4, libsmb2 6.1.0 vs 6.2, openlibm 0.8.0 vs 0.8.6, freetype2 26.1.20 (libtool version) vs 2.13.2, sdl2 2.30.12 (PKGBUILD says git), x264 0.165.3222, libavcodec 61.3.100 / libavformat 61.1.100 / libavutil 59.8.100. The bundle has 108 static `.a` in `target/user/homebrew/lib` `[verified in code]`.

### 4.9 Host install proof (Ubuntu 24.04, compile/link only)

Nothing was run on a console.

- Prerequisites per README "Prerequisites": required `bash clang-18 lld-18 wget`, optional `socat cmake meson pkg-config python3 python3-pyelftools`. Already present: cmake 3.28.3, make 4.3, pkg-config 1.8.1, git, curl, wget.
- Commands that worked:

```
sudo apt-get install --assume-yes --no-install-recommends clang-18 lld-18 clang lld unzip python3-pyelftools
sudo apt-get install --assume-yes --no-install-recommends llvm-18
```

- **Problem:** the README's list is not sufficient on Ubuntu. `bin/prospero-llvm-config` looks for `llvm-config-23 … llvm-config-15`/`llvm-config`, and `prospero-ar/nm/strip/objcopy/ranlib` exec `llvm-*` from that bindir, which are in the `llvm-18` package (also provides `llvm-readelf`). pacbrew CI installs `llvm-18-dev`, which pulls it in. Versions: Ubuntu clang 18.1.3 (1ubuntu1), Ubuntu LLD 18.1.3.
- **SDK release v0.45** (2026-10-21): `https://github.com/ps5-payload-dev/sdk/releases/download/v0.45/ps5-payload-sdk.zip`, sha256 `5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c` (matches GitHub digest), 8.8 MB, 3156 entries, no absolute/`..` paths, no symlinks. README way is `sudo unzip -d /opt ps5-payload-sdk.zip`; it was extracted elsewhere and used only for comparison.
- **pacbrew bundle v0.41.1** (2026-10-10), **installed at `/opt/ps5-payload-sdk`**: `https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.41.1/ps5-payload-dev.tar.gz`, sha256 `e811bc2e4676c5bb7f80a8fe928d2985dce599996a5ca6b54da3e72699287887` (matches GitHub digest), 228 MB compressed, 707 MB installed, 6891 entries. All paths under `opt/ps5-payload-sdk/`; all 97 symlinks relative and inside the tree; one hardlink (libdeflate-gunzip). It is CI's `tar -czf ps5-payload-dev.tar.gz -C / opt/ps5-payload-sdk` after `ci-libs.sh` (`.github/workflows/ps5-payload-libs.yml`). Install: `tar --no-same-owner -xzf ps5-payload-dev-v0.41.1.tar.gz` into an empty dir, then `sudo cp -a …/opt/ps5-payload-sdk /opt/ && sudo chown -R root:root /opt/ps5-payload-sdk`. Its SDK core is slightly older than v0.45: `toolchain/` is identical, `target/lib/libSce*.so` stubs and `crt1.o` differ in bytes, the bundle adds OpenMP headers; hello_world builds the same with both.
- Wiring: Makefiles require `PS5_PAYLOAD_SDK` (`include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk`); `toolchain/prospero.sh` and `prospero.cmake` locate themselves. `prospero.cmake` sets `CMAKE_SYSROOT=$SDK/target`, FIND_ROOT_PATH ONLY, install prefix `/user/homebrew`, `PKG_CONFIG_EXECUTABLE=prospero-pkg-config`, `PS5_DEPLOY=bin/prospero-deploy`, and variables `PS5`/`PROSPERO`. `prospero-pkg-config` runs host `pkg-config --static` with `PKG_CONFIG_SYSROOT_DIR=$SDK/target` and libdir under `target/user/homebrew/lib/pkgconfig`, so it never sees host libs. `prospero-fetchpkg` downloads Sony PKG update pieces with TLS verification disabled; it is not a package manager; do not use it.
- Not installed: `pacman-package-manager`/`makepkg` 6.0.2-6ubuntu2 (available in apt), docker/podman (podman 4.9.3 available). Unofficial images exist (`rrooij/ps5-payload-sdk-docker`, `BenNoxXD/PS5-Payload-SDK-Docker`).

ELF evidence:

```
# 4a. SDK samples/hello_world
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make -C work/hello_world-opt -j4
hello_world.elf: ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV), dynamically linked, with debug_info, not stripped
size=112384   (v0.45 zip SDK: size=112576, same NEEDED)
  (NEEDED) [libkernel_web.sprx]  (NEEDED) [libSceLibcInternal.sprx]  (NEEDED) [libSceNet.sprx]  (FLAGS_1) PIE
  Type: DYN, Entry point address: 0x64

# 4b. SDL2 window + fill colour + gamepad polling (direct and CMake builds identical)
$PS5_PAYLOAD_SDK/bin/prospero-clang -Wall -Werror -O2 -o sdl2_direct.elf main.c $($PS5_PAYLOAD_SDK/bin/prospero-pkg-config --cflags --libs sdl2)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=/opt/ps5-payload-sdk/toolchain/prospero.cmake -DCMAKE_BUILD_TYPE=Release
--   Found sdl2, version 2.30.12
sdl2_smoke.elf: ELF 64-bit LSB pie executable, x86-64 ... size=2814776 bytes
NEEDED: libSceSystemService.sprx libSceUserService.sprx libScePad.sprx libSceAudioOut.sprx libSceVideoOut.sprx
        libSceKeyboard.sprx libSceImeDialog.sprx libkernel_web.sprx libSceLibcInternal.sprx libSceNet.sprx
undefined imports include scePadInit/scePadOpen/scePadReadState/scePadGetHandle/scePadSetLightBar

# 4c. C++20 + libc++ (vector/string/unique_ptr) via CMake
cxx_smoke.elf: ELF 64-bit LSB pie executable, x86-64 ... size=590504; NEEDED: libkernel_web.sprx libSceLibcInternal.sprx libSceNet.sprx (libc++ static)
```

- Cost: about 3.5 min wall clock (apt + 228 MB download at ~45 MB/s); each build takes seconds. Disk: ~2.1 GB total (apt ~0.4 GB, /opt 707 MB, scratch 978 MB of which 707 MB extracted copy + 227 MB downloads, removable by the user).
- Adopt in the project: `cmake -S . -B build-ps5 -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake` (or `prospero-cmake`), deps via `pkg_check_modules(X REQUIRED IMPORTED_TARGET sdl2 freetype2 harfbuzz libcurl openssl …)`, `.elf` suffix, deploy later with `$PS5_PAYLOAD_SDK/bin/prospero-deploy -h $PS5_HOST -p 9021 app.elf`. Pin bundle v0.41.1 (sha256 e811bc2e…7887) or SDK v0.45 (sha256 5fac6409…1a2c). On this host zsh `grep` is aliased to ugrep, so scripts need `command grep`.

### 4.10 Feasibility matrix: cross-compiling a browser engine with this SDK

| Item | Status | Evidence |
|---|---|---|
| Arch | x86-64, AVX2 used by SDL tilemap; CPU model not verified | `prospero.cmake` comment; `SDL_ps5tilemap.c:106` |
| C++ stdlib | libc++ 18.1.8 static, C++23 sample | `sdk/libcxx.sh:19`; `samples/hello_cxx23` |
| Exceptions / RTTI | on / on (libunwind baremetal) | `sdk/libcxx.sh:42-56` |
| Threads | pthreads (libkernel), std::thread, C11 thrd | stubs; `sdk/libc/thrd.c` |
| `thread_local` | emulated TLS only | `prospero-clang:77`, `prospero-lld:62` |
| Shared libs / dlopen | yes, SDK rtld for `.so` + `.sprx` | `sdk/crt/rtld.c`, `samples/hello_so` |
| JIT (RWX) | via `kernel_mprotect` or `sceKernelJit*` (needs jailbreak) | `sdk/libc/mman.c:76-111`; `ZiZc3_VLC/ps5/compat/mmap.c` |
| fork/exec own ELF | no working path; only rfork_thread + execve(system eboot) + ptrace | `elfldr/elfldr.c:603-700` |
| posix_spawn / sceKernelSpawn | symbols exist, no user found | stubs |
| popen | unimplemented | `sdk/libc/popen.c:20-31` |
| signals | symbols in libkernel, not exercised | stubs |
| pipes / socketpair / unix sockets | present; `pipe2` wrapper; `sys/un.h` | `sdk/libc/pipe2.c`, `include/freebsd/sys/un.h` |
| shm_open / SysV shm | symbols + headers; cross-process use unverified | `include/freebsd/sys/shm.h` |
| locale / iconv / ICU | "C"-only locale stubs; libiconv package; ICU missing | `sdk/libc/no-locale.c:262-560` |
| Fonts / images | FreeType, HarfBuzz, Fontconfig, FriBiDi; PNG, JPEG, WebP, GIF; no AVIF/JXL | pacbrew |
| Net/TLS | curl 8.18 HTTP/1.1 + OpenSSL 3.5.2 + CA bundle; no HTTP/2, no brotli | pacbrew curl/openssl PKGBUILD |
| GPU | Vulkan (RADV) + GL/GLES (zink) via fresh "AI slop" Mesa; else CPU framebuffer 1080p | `mesa/PKGBUILD:10-14`; `SDL_ps5video.c` |
| Video decode | HW `libSceVideodec2` (own stubs) or FFmpeg 7.0.1 SW | §4.6 |
| Input | DualSense, media remote, USB keyboard, IME dialog; mouse unverified | §4.6 |
| Audio | sceAudioOut 48 kHz S16/F32 stereo | §4.6 |
| A/V context | needs "big app" (hbldr fake app) or native-title packaging | `shsrv/README.md:39`; `websrv/src/ps5/hbldr.c` |
| Licensing | SDK crt/libc GPLv3+ → binary GPLv3+ | `sdk/README.md:75-77` |

Open questions (all unknown): whether plain `fork()`, `posix_spawn`, `sceKernelSpawn` succeed from a payload or title process; whether `kernel_mprotect`-based RWX passes the hypervisor's executable-page restrictions at scale; the per-process memory ceiling for elfldr payloads; usability of `libSceGLSlimVSH`; the mouse API.

## 5. Browser engine candidates

Scope: an engine embedded in a ps5-payload-sdk payload (clang, x86-64, FreeBSD-derived kernel, software framebuffer as the working premise). Snapshots (all `--depth 1`): WebKit/WebKit (sparse) `22efb76e6947` (2026-10-30), LadybirdBrowser/ladybird `b5f8d768` (2026-10-30), servo/servo (sparse) `fda2a0e` (2026-10-30), netsurf-browser/netsurf (GitHub mirror) `d06cd9d` (2026-10-28). Paths are `repo:path:line`.

### 5.1 Platform facts that shape the evaluation

- Toolchain is clang 18 + libc++ 18.1.8 (`sdk:README.md:17`, `sdk:libcxx.sh:19`) `[verified in code]`; some engines need clang ≥19.
- fork/exec is "not freely usable" rather than absent: raw wrappers exist (§4.3); multi-process is possible only via jailbreak-specific spawn tricks. A generic `posix_spawn("/path/WebProcess")` is assumed not to work `[estimate]`. Plan for single-process or a custom elfldr-style launcher.
- JIT is an open question leaning "possible" (§4.4); design interpreter-only `[estimate]`. A dual-mapping JIT needs engine support: JSC has it (`ENABLE_SEPARATED_WX_HEAP` / "fast JIT permissions"), SpiderMonkey and V8 partially `[estimate]`.
- GPU: `libSceAgc` (202 symbols) and `libSceGLSlimVSH` (203 symbols, looks like Sony's GL ES for system apps) are stubbed; `libScePosixForWebKit` and `libkernel_web` also exist `[verified in code]`. The engine study keeps the "software framebuffer only" premise but flags GLSlim as a high-value spike: a usable GLES2 would unblock WebKit's TextureMapper/Skia-GL compositor. Note that the SDK study separately found the pacbrew Mesa RADV/zink path (§4.5), which the engine study does not take into account.

Evaluation criteria used for every candidate: license, toolchain fit with clang 18, mandatory dependencies, process model, rendering/compositing without a GPU, video (MSE, native HLS, EME), JS engine and JIT needs, build size on this host, and effort.

### 5.2 WebKit: upstream PlayStation port (most relevant)

- Upstream contains `Source/cmake/OptionsPlayStation.cmake` (480 lines), Sony's own PS4/PS5 port `[verified in code]`.
- Toolchain: Sony SDK flags `-sce-stdlib=v2` (`:25`), `sign()` of SELFs (`:399,420`), `PlayStation4` system name (`:260`).
- Interpreter only: `ENABLE_JIT`, `ENABLE_DFG_JIT`, `ENABLE_FTL_JIT` OFF (`:176-178`). With JIT off on x86-64, C_LOOP stays OFF (`WebKitFeatures.cmake:111-117`), so JSC runs LLInt, which needs no executable heap. WebAssembly is ON only when C_LOOP is off (`WebKitFeatures.cmake:135-139`), but WASM in practice needs BBQ/OMG JIT, so it is effectively unavailable without JIT `[estimate]`.
- Dependencies: no glib, GStreamer or libsoup. Uses curl ≥7.85 and OpenSSL/BoringSSL (`:105,111,271-274,306`); ICU ≥70.1, HarfBuzz+ICU, FreeType, Fontconfig, PNG, JPEG ≥1.5.2, WebP+demux, libxml2, SQLite3, zlib, libpsl, Brotli (`:56,96-159`); Cairo or Skia (`:276-282`); **EGL and OpenGLES2 REQUIRED** (`:106,110`) `[verified in code]`. Of these, ICU and Brotli (and Cairo/Skia) are absent from pacbrew (§4.8).
- Compositing: `USE_TEXTURE_MAPPER ON`, `USE_COORDINATED_GRAPHICS` (`:316,329`), a GPU compositor, hence the GL requirement.
- Process model: multi-process, `/app0/WebProcess.self`, `/app0/NetworkProcess.self` (`ProcessLauncherPlayStation.cpp:46`), launched via a proprietary `PlayStation::launchProcess` from `process-launcher.h` (`WebKitRequirements::ProcessLauncher`) or `WPE::PlayStation` (`ProcessLauncherPlayStation.cpp:37-90`, `OptionsPlayStation.cmake:73-89`). IPC is a `SOCK_SEQPACKET` socketpair (`ProcessLauncherPlayStation.cpp:65`), plain FreeBSD. The launcher is one small function that returns a pid and takes a socket fd: exactly where an elfldr-style launcher would plug in `[verified in code]`. `ENABLE_WEBKIT_LEGACY` (single-process WebKit1) is OFF even here (`OptionsPlayStation.cmake:27`).
- Features: Fullscreen API ON (`:184`); Gamepad experimental-only (`:198`); WebGL, MathML, XSLT, Web Audio, GPU process, async scrolling OFF (`:217-233`).
- Video: `GPUProcess/media/playstation/RemoteMediaPlayerProxyPlayStation.cpp` and `VideoLayerRemotePlayStation.cpp` exist (`Source/WebKit/PlatformPlayStation.cmake:32,90`), but the decoder/MediaPlayerPrivate backend is not upstream; a homebrew port gets `<video>` with no media backend `[verified in code]`.
- Not available: Sony's `WebKitLibraries/playstation` / `WebKitRequirements` package and the `SceVsh*` libraries (`:31-36,96-151`) are part of the official Sony SDK, unavailable to homebrew and a licensing/legal problem.
- Value and effort `[estimate]`: the port proves WebCore/JSC/WTF compile and run on the PS4/PS5 libc and kernel with clang, interpreter-only. Retargeting means rewriting the CMake glue, replacing Sony requirement libraries with open builds, and writing a launcher plus a compositor path. Lowest-risk route to a working WebKit.

### 5.3 WPE WebKit 2.x / WebKitGTK

- License: LGPL-2.1 (WebCore) and BSD-2 (JSC/WTF); fine if WebKit modifications are published (https://webkit.org/licensing-webkit/) `[per authors]`.
- Toolchain: C++23 (`OptionsCommon.cmake:1`); clang 18 still accepted with a workaround block, "FIXME: Remove once Clang 18 does no longer need to be supported" (`WebKitCompilerFlags.cmake:243-251`) `[verified in code]`. The window is closing.
- Mandatory deps (`OptionsWPE.cmake:14-26,259`) `[verified in code]`: GLib ≥2.70 (GioUnix), HarfBuzz ≥2.7.4 + ICU, ICU ≥70.1, JPEG, PNG, WebP+demux, zlib, **libepoxy ≥1.5.4**, **libsoup3**, xkbcommon, libxml2 ≥2.9.13, SQLite3, FreeType ≥2.9. Optional: libxslt, libwpe, AVIF/JPEG-XL/WOFF2/LCMS2, Skia (bundled, `USE_SKIA` ON at `:154`). Video: GStreamer ≥1.18-ish + base/good/bad + gst-libav.
- Process model: UI + WebProcess + NetworkProcess (+ GPUProcess, `ENABLE_GPU_PROCESS ON`, `:76`), spawned via `g_subprocess_launcher_spawnv` → posix_spawn or fork/exec (`Source/WebKit/UIProcess/Launcher/glib/ProcessLauncherGLib.cpp:200-258`) `[verified in code]`. No single-process mode in WebKit2. Running Web+Network as threads is impractical (WTF main-thread/RunLoop singletons, `WebProcess::singleton()`, per-process globals) `[estimate]`.
- Rendering: since 2.46 tiles are painted with Skia on the CPU by default ("Skia CPU renderer is used by default … `WEBKIT_SKIA_ENABLE_CPU_RENDERING=0`", https://wpewebkit.org/release/wpewebkit-2.46.1.html, https://blogs.igalia.com/carlosgc/?p=1016) `[per authors]`, but compositing is still GL: libepoxy is required and the Skia compositor is Ganesh-based (`Source/WebCore/platform/graphics/skia/SkiaPaintingEngine.cpp:64-76`) `[verified in code]`. Without a GPU: llvmpipe needs an LLVM JIT (jitshm), softpipe is very slow.
- Video: GStreamer `MediaPlayerPrivateGStreamer`, MSE ON (`GStreamerDefinitions.cmake:1-9`); native HLS opt-in only via `WEBKIT_GST_ENABLE_HLS_SUPPORT=1` (`GStreamerRegistryScanner.cpp:660-663`); EME experimental (`OptionsWPE.cmake:52`) with ClearKey and Thunder/OpenCDM (`OptionsWPE.cmake:375-376`; `gstreamer/eme/CDMThunder.cpp`), which only hosts OEM-licensed Widevine/PlayReady binaries `[verified in code]`. Homebrew cannot get a Widevine CDM on any engine.
- Gamepad ON (`OptionsWPE.cmake:75`) via libmanette or WPE platform (own backend from `libScePad` needed); Fullscreen ON (`WebKitFeatures.cmake:254`).
- FreeBSD: WebKitGTK is in FreeBSD ports; JIT disabled only on armv6/7/powerpc (https://code.fizz.buzz/mirror/freebsd-ports/commit/895bc805a975e4d6848427d88063cc8651e36fe4, 2021) `[per authors]`.
- Memory: ~150–300 MB for a simple page; 500 MB–1 GB+ with YouTube and MSE buffers `[estimate]` (see the memory contradiction in §4.4).
- Build: "very powerful desktops … or build farms", a 2 GB laptop took ~3 days (https://lists.webkit.org/pipermail/webkit-gtk/2020-January/003577.html); unified-source TUs need ~2–4 GB RAM per job, Fedora builders OOM'd at 4 GB/vCPU, debuginfo ~12 GB (https://pagure.io/releng/issue/11952); a Yocto WPE tree ~30 GB in 2018 (https://developer.resources.solid-run.com/wiki/x/doAWEQ) `[per authors]`. On 8 cores/32 GB a release build without tests ~1.5–3 h, 6–12 GB build dir at `-g0` (>25 GB with debug info) `[estimate]`.
- Effort: 30–50 person-weeks to first YouTube playback (glib/libsoup/GStreamer, a GL path, launcher, framebuffer/pad backend, ffmpeg in GStreamer); heavier than 5.2 `[estimate]`.

### 5.4 Ladybird (LibWeb/LibJS, BSD-2)

- Toolchain is the hard blocker: clang ≥19 or gcc ≥14 (`ladybird:Meta/Utils/find_compiler.py:21-22`) and Rust 1.98 (`rust-toolchain.toml`); ~25 Rust crates in the workspace (LibJS, LibWeb, LibGfx, LibIPC, LibURL, LibUnicode, LibRegex, LibWasm, HTML parser…) `[verified in code]`. No Rust target for the PS5 payload environment; `x86_64-unknown-freebsd` std against the SDK libc is plausible but unproven `[estimate]`.
- Deps (vcpkg.json) `[verified in code]`: angle, brotli, cpptrace, curl, dbus, ffmpeg, fontconfig, harfbuzz, icu, libavif, libjpeg-turbo, libpng, libwebp, libxml2, libpsl, libproxy, mimalloc, openssl, qtbase (UI only), sdl3, simdjson, simdutf, skia, sqlite3, tiff, vulkan(+headers, VMA), woff2, wuffs, zlib, ….
- Process model: hard multi-process: AudioServer, Compositor, ImageDecoder, MediaServer, RequestServer, VideoDecoderServer, WebContent, WebWorker (`Meta/CMake/ladybird_helper_processes.cmake:1-10`), spawned via `Core::Process::spawn` (`Libraries/LibWebView/HelperProcess.cpp:174-294`) using posix_spawn or fork (`Libraries/LibCore/Process.cpp:24,189,280`); even Android uses separate `android:process` services (`UI/Android/src/main/AndroidManifest.xml:53-63`); a CMake check enforces Skia confinement to the Compositor process (`CMakeLists.txt:144-149`). No in-process option `[verified in code]`.
- Rendering: Skia in the Compositor, with a CPU raster path via `SkSurfaces::WrapPixels` (`Services/Compositor/PaintingSurface.cpp:159,170`) `[verified in code]`; software-only is feasible.
- Video: LibMedia demuxers for ISOBMFF, Matroska/WebM, Ogg, MP3, ADTS, FLAC, WAV (`Libraries/LibMedia/Containers/`); codecs H.264, H.265, VP8, VP9, AV1, AAC, Opus, Vorbis via FFmpeg (`Libraries/LibMediaCodecs/CodecID.h:15-39`, `LibMediaCodecs/FFmpeg/`); MSE implemented (`LibMedia/MediaSourceExtensions/`) `[verified in code]`, shipped May 2026 with adaptive YouTube (https://ladybird.org/newsletter/2026-05-31/), Twitch and hls.js players by August 2026 (https://ladybird.org/newsletter/2026-08-31) `[per authors]`. No native HLS (no `mpegurl` in `Libraries/`); EME ClearKey only (`LibWeb/EncryptedMediaExtensions/KeySystem.h:27`, `Algorithms.cpp:407-415`); Fullscreen (`LibWeb/Fullscreen/`) and Gamepad (`LibWeb/CMakeLists.txt:397`) exist `[verified in code]`.
- JS: LibJS bytecode interpreter; optimizing JIT on by default only on Linux and macOS (`Meta/CMake/cmake_options.cmake:12-19`) `[verified in code]`. On PS5 interpreter-only; YouTube-class sites usable but sluggish, slower than JSC LLInt `[estimate]`.
- FreeBSD: package list documented (`Documentation/BuildInstructionsLadybird.md:189`), no FreeBSD CI workflow `[verified in code]`.
- Build: shallow source 446 MB / 46k files (measured); 30 GB free disk recommended, clean build ~5.7 GB, 40–90 min on 4+ cores (https://computingforgeeks.com/install-ladybird-browser/) `[per authors]`; vcpkg also builds Skia, ffmpeg, ICU, Qt, ANGLE.
- Effort: 35–60 person-weeks (newer clang/libc++ for PS5, Rust std port, collapsing 8 services or an elfldr-style spawner, Skia/ffmpeg/curl ports, fast-moving upstream) `[estimate]`. Best open-source video story among non-Blink/Gecko engines, worst toolchain fit.

### 5.5 Servo (MPL-2.0, Rust)

- Toolchain: Rust 1.99.1 (`servo:rust-toolchain.toml`); embeds SpiderMonkey via `mozjs` 0.26.13 (`Cargo.toml:146`), a large C++ build with its own Python build system; no PS5 Rust target (x86_64-unknown-freebsd + custom libc) `[estimate]`; Servo knows FreeBSD as a host triple (`python/servo/platform/__init__.py:27-28,73-74`).
- Process model: single-process by default, `multiprocess: false` (`components/config/opts.rs:41-42,249`) `[verified in code]`. Best fit of all candidates.
- Rendering: WebRender 0.70 (`Cargo.toml:288`) is a GL renderer; "software" is surfman's `create_software_adapter()` (OSMesa/llvmpipe), described in code as "This will generally have bad performance" (`components/shared/paint/rendering_context.rs:283-304`) `[verified in code]`.
- Video: servo-media with GStreamer as real backend (`components/media/backends/{gstreamer,dummy,ohos,auto}`, `Cargo.toml:108-120`); **no MSE** (no MediaSource in `components/script/dom/`) `[verified in code]`; NLnet funded "initial support for MSE" starting June 2026 (https://nlnet.nl/project/Servo-multimedia/) `[per authors]`; no EME; Gamepad and fullscreen exist (`dom/gamepad`, `dom/fullscreen`, `prefs.rs:161-163`). Without MSE, YouTube, Twitch and most online cinemas will not work.
- JS: SpiderMonkey with Baseline/Ion disabled via prefs (`prefs.rs:277-283,305`); interpreter-only usable but slow. An OpenHarmony port exists (`media/backends/ohos`).
- Build: "target directory … takes 26 GB", 8 GB RAM not enough for incremental `script` (https://mail-archive.com/dev-servo@lists.mozilla.org/msg02412.html, old) `[per authors]`.
- Effort: 30–50 person-weeks to render pages; video blocked on upstream MSE `[estimate]`.

### 5.6 NetSurf (GPL-2 frontend, MIT libraries)

- Framebuffer frontend exists (`frontends/framebuffer/`), plus amiga, atari, beos, riscos, gtk, qt, windows, monkey `[verified in code]`. 28 MB shallow source, plain C99, builds in minutes.
- JS: Duktape 2.7 (`content/handlers/javascript/duktape/duktape.h:185`, `DUK_VERSION 20700L`), ES5.1 only. DOM bindings are 67 `.bnd` files with no XMLHttpRequest, fetch, MediaSource or HTMLMediaElement `[verified in code]`. No video. Online cinemas: no.
- Effort: 2–4 person-weeks for framebuffer + pad; useful only as a text/simple-site browser or UI shell `[estimate]`.

### 5.7 Dismissed

| Engine | License | Verdict |
|---|---|---|
| litehtml | BSD-3 | Layout only, no JS; UI renderer, irrelevant for sites |
| Ultralight | Proprietary (listed as "Paid, Proprietary" on AlternativeTo; URL not captured, unverified) | Excluded by policy; no PS5/FreeBSD binaries |
| Sciter | Proprietary (binary-only EULA, "unfree" in nixpkgs, https://mynixos.com/nixpkgs/package/libsciter) | Excluded; own script engine, not a web browser |
| Flow (Ekioh) | Proprietary | Excluded (used on set-top boxes, closed) |
| CEF / Chromium / Thorium | BSD-3 | Technically best, but mandatory multi-process (zygote, sandbox), GN/Ninja, its own rolling clang+libc++; ≥100 GB disk / 16 GB RAM (https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md) `[per authors]`; V8 jitless is slow; person-years. Not realistic |
| Gecko / GeckoView / Goanna | MPL-2 | GeckoView Android-only; Gecko needs Rust + 30–40 GB build and is multi-process (Fission); Goanna is a frozen older fork. Not realistic |

### 5.8 Hybrid option (light HTML UI + native ffmpeg player)

Works for direct media URLs (`.mp4`/`.m3u8` from IPTV lists, DLNA, self-hosted Jellyfin/Plex APIs); 1080p H.264/HEVC software decode on 8 Zen2 cores is feasible `[estimate]`; `dlnaplay`, `jtplay`, `tvhp` already exist in the scene. It does not work for YouTube, Twitch, VK Video or Rutube: stream URLs come from obfuscated JS (YouTube `n`/signature transforms, PO tokens; Twitch GraphQL + playback access tokens), players expect MSE and often fetch streams, workers and WASM, and no lightweight engine can run these bundles. The only working hybrid is site-specific extractors (yt-dlp-style logic in C/Lua/QuickJS) feeding the native player. QuickJS (MIT, ES2023, interpreter, ~1 MB) is realistic for extractor scripts, not whole sites. Fragile but cheap: 4–8 person-weeks for player + YouTube/Twitch/Rutube extractors `[estimate]`.

### 5.9 Cross-cutting answers

- Native HLS vs MSE+hls.js: WebKit GStreamer ports have native HLS only with `WEBKIT_GST_ENABLE_HLS_SUPPORT=1`, otherwise hls.js over MSE; WebKit PlayStation port: whatever your own MediaPlayerPrivate implements (an ffmpeg backend gives native HLS cheaply); Ladybird: MSE + hls.js only; Servo: neither today (servo-media feeds bytes via appsrc `[estimate]`); NetSurf/litehtml: none; hybrid player: native via ffmpeg.
- Software compositing at 1080p on 8×Zen2 `[estimate]`: Ladybird's Skia raster is CPU-only and SIMD-optimised; static/scroll content OK at 1080p30–60 with damage tracking; video needs YUV→RGB + blit per frame (~8 MB/frame BGRA), 1–2 cores. WebKit (WPE/PlayStation) painting can be CPU but the compositor needs GL: supply GLES2 (GLSlim, Mesa softpipe/llvmpipe) or write a software TextureMapper; softpipe at 1080p is low double-digit fps for compositing-heavy pages, llvmpipe is fine but needs JIT pages. Servo: same problem ("bad performance"). NetSurf: trivially fast for its feature set.

### 5.10 Ranking

| Rank | Option | License | Process model fit | Video for YouTube/Twitch | JIT needed? | Toolchain fit (clang 18) | Effort (person-weeks) `[estimate]` | Main blockers |
|---|---|---|---|---|---|---|---|---|
| 1 | WebKit, PlayStation port as base (open deps, curl+OpenSSL, Cairo/Skia, custom ffmpeg MediaPlayerPrivate + MSE) | LGPL/BSD | Multi-process via pluggable `ProcessLauncher`; elfldr-style spawn; socketpair IPC fine | Yes, once an ffmpeg-backed MediaPlayerPrivate and MSE SourceBuffer backend exist | No (LLInt) | Good | 25–40 (≈12–20 to render pages, +10–20 for MSE video) | GL for TextureMapper; replacing Sony-private libs; process spawn; huge build |
| 2 | Hybrid: QuickJS extractors + ffmpeg player + litehtml/NetSurf UI | MIT/BSD/GPL | Single process | Supported sites only, via extractors; native HLS | No | Excellent | 4–8 (+ upkeep) | Not a browser; breaks when sites change; no logged-in/DRM content |
| 3 | Ladybird | BSD-2 | Bad: 8 mandatory helpers | Best OSS pipeline (MSE, H.264/VP9/AV1, Twitch, hls.js), ClearKey only | No | Bad: clang ≥19 + Rust 1.98 | 35–60 | Toolchain + Rust std; process collapse; moving target; disk |
| 4 | WPE WebKit (Linux-style) | LGPL/BSD | Bad: GSubprocess posix_spawn | Yes via GStreamer | No | OK | 30–50 | glib/libsoup/GStreamer/Mesa; GL-only compositor |
| 5 | Servo | MPL-2 | Best (single-process) | No MSE today | No (configurable) | Rust/mozjs, no target | 30–50 | No MSE; software GL; build size |
| 6 | NetSurf | GPL-2/MIT | Single process | None | No | Excellent | 2–4 | ES5-only JS, no XHR/fetch, no media |
| — | Chromium/CEF/Gecko | BSD/MPL | Mandatory multi-process | Yes | Jitless possible | Bad | Person-years | Size, toolchain, sandbox/zygote |
| — | Ultralight, Sciter, Flow | Proprietary | — | — | — | — | — | Excluded by policy |

DRM: no option gets Widevine/PlayReady; only ClearKey is available (WebKit GStreamer, Ladybird). Two cheap spikes would de-risk rank 1: (1) call `libSceGLSlimVSH` from a payload for an EGL/GLES2 context; (2) spawn a second ELF process with an elfldr-style launcher and pass it a socketpair fd.

### 5.11 What cannot be built on this host (4 cores / 15 GB RAM / 27 GB free)

- Chromium/CEF/Blink forks: ≥100 GB disk, 16 GB+ RAM recommended `[per authors]`. Impossible.
- Gecko: 30–40 GB with objdir and toolchains, plus Rust `[estimate]`. Not feasible.
- Servo: ~26 GB `target/` `[per authors]`, plus mozjs and GStreamer, RAM pressure in `script`. Not feasible.
- Ladybird: 30 GB free recommended `[per authors]`; vcpkg also compiles Skia, ffmpeg, ICU, Qt, ANGLE. Not feasible as-is; might squeeze in with Qt/ANGLE/Vulkan removed and a pruned cache, with no headroom.
- WebKit (WPE or PlayStation-style): ~2–4 GB per compile job, so at most `-j3`–`-j4` with 15 GB `[per authors]`; release build `-g0` without tests ~6–12 GB build dir and **6–12 h** `[estimate]`. Technically possible but not now; the only full engine that might fit. A full checkout with LayoutTests (472,847 files; use a sparse `Source/` + `Tools/`), debug builds and cross-compiled deps would exceed the disk.
- NetSurf, litehtml, QuickJS, minimal ffmpeg: build in minutes.

## 6. Existing homebrew browsers and wrappers (PS5, PS4, Switch)

No PS5/PS4 search ("ps5 browser homebrew", "ps4 web browser homebrew", "orbis browser webkit", "ps5 webkit port", "netsurf ps5", "litehtml ps5", "ps5 servo", "ps5 ultralight") found a WebKit, Chromium, Servo or NetSurf port; results are only web-UI tools. No Qt, GTK or nanogui port was found either.

| Project | Platform | License | Status | What it is |
|---|---|---|---|---|
| ps5-payload-dev/svtplay, tv4play, plutotv | PS5 | GPL-3.0 (plutotv: no LICENSE file) | active, 2026-09 | 10-foot web apps in the system browser, installed as `webAppUri` tiles; tv4/pluto served from localhost via websrv |
| ps5-payload-dev/launchpad | PS5 | GPL-3.0 | 2026-09-01 | Remote web payload catalogue (`webAppUri` → tinyurl → `http://launchpad.sladdsallad.com/`), launches payloads via elfldr HTTP |
| ps5-payload-dev/websrv | PS5 | GPL-3.0 | active | Homebrew launcher UI inside the system browser |
| itsPLK/ps5-payload-manager, owendswang/ps5-web-file-manager, notmaj0r/CheatRunner, ItsBlurf/BFpilot | PS5 | GPL-3.0 / MIT (BFpilot) | active | Local HTTP daemon + system browser apps with home-screen shortcuts |
| owendswang/ps5-web-browser-fullscreen | PS5 | GPL-3.0 | active, 2026-10-30 | In-RAM patch removing the browser chrome (1920x1080) |
| X-F1REBALL-X/wk-autoloader | PS5 | GPL-3.0 | active | Re-points the Store-tile web view to a local page (AppCache) |
| ps5-payload-dev/jtplay, tvhp, dlnaplay | PS5 | GPL-3.0 | active | Native SDL2 + RmlUi (HTML/CSS subset) + QuickJS + curl + FFmpeg apps (jtplay); proves an HTML/CSS-ish UI + JS engine + HTTPS runs as a payload `[verified in code]` |
| ZiZc3/VLC 0.6.0, RafaelNGP/xbox-cloud-gaming-ps5, OpenCloudGaming/OpenNOW-PS5, Sp9nky/unofficial-stremio-ps5-port | PS5 | GPL-3.0 (OpenNOW: GPL-3.0-or-later) | active, Oct 2026 | Native titles with Vulkan/GL UIs and Videodec2 hardware decode; not browsers (§4.5–4.6) |
| VivaLaVent/PS5MC (Kodi) | PS5 | GPL (Kodi) `[per authors]` | not cloned | Kodi 21/22 fork |
| etonedemid/netsurf-switch | Switch | no LICENSE file (NetSurf is GPLv2) | pushed 2026-10-22 | Real NetSurf port: SDL2 front end, QuickJS JS, ffmpeg `<video>`/`<audio>`, CSS grid/flex (https://github.com/etonedemid/netsurf-switch) `[per authors]` |
| natureglass/Brewser | Switch | MPL-2.0 | pushed 2026-09-17 | "Homebrew web runtime … V8 + Skia + WebGL2" NRO (https://github.com/natureglass/Brewser) `[per authors]` |
| libnx `applets/web.h` (`webPageCreate`/`webConfigShow`) | Switch | ISC | maintained | Official API to open the system NetFront-NX/WebKit web applet at a URL; Switch equivalent of `sceSystemServiceLaunchWebBrowser` (https://switchbrew.org/wiki/Internet_browser) `[per authors]` |
| coderman64/netsurf-3ds | 3DS | GPL (NetSurf) | WIP | NetSurf port (https://gamebrew.org/wiki/Netsurf_3DS) `[secondary]` |

Note: the Switch NetSurf port runs QuickJS and ffmpeg media, while upstream NetSurf on its own has Duktape ES5.1 and no media elements (§5.6); the Switch port's capabilities are per its authors only.

## 7. Online video services: DRM and delivery

Scope: which services need EME/DRM (Widevine, PlayReady, FairPlay), which do not, and what the DRM-free ones need from a browser. DRM circumvention is out of scope and was not researched. Tag mapping for this section: vendor help pages, docs and engineering blogs are `[per authors]`; forums, user reports and news are `[secondary]`; claims the researcher marked as well-known industry behaviour without a fetched source are `[estimate]` and must be verified on a device; unknown or conflicting items are **unknown**.

Caveat: public documentation for Russian OTT services (Kinopoisk, Okko, ivi, Premier, Start, KION, Wink, More.tv, Amediateka) is very thin; their DRM verdicts rest on indirect evidence. The cheapest confirmation is to log every `navigator.requestMediaKeySystemAccess(keySystem, ...)` call per origin.

| Service | Market | DRM | Delivery | MSE / native HLS | Codecs | Heavy SPA | TV / 10-foot web UI | Evidence |
|---|---|---|---|---|---|---|---|---|
| YouTube | Global; RU (throttled since 2024) | None for regular/UGC; Widevine/PlayReady/FairPlay for paid movies, some Premium/Primetime channels, YouTube TV (US) | DASH-style adaptive via MSE; since 2025 the WEB client uses proprietary SABR/UMP (POST-based, JS-driven); classic adaptive URLs removed for WEB; live HLS/DASH | MSE required above the 360p progressive fallback (since 2015 non-MSE gets low-res fallback, now unreliable); no native HLS path on desktop web | VP9, AV1, H.264; Opus, AAC; chosen via `isTypeSupported` | Yes (Polymer, ES2017+) | `youtube.com/tv` (Leanback, built for Cobalt) redirects or blocks desktop browsers; works only with a TV/console UA spoof per reports 2022–late 2024; 2026 status unverified | SABR/MSE/Leanback UA `[secondary]`; codecs `[estimate]` |
| Twitch | Global, RU | None for streams/VODs | Low-latency HLS; web player derived from Amazon IVS: WebAssembly + Web Worker + MSE; native HLS on iOS Safari | MSE + WASM + Workers on desktop | H.264/AAC mostly; HEVC/AV1 "Enhanced Broadcasting" rolling out | Yes (React, IRC-over-WebSocket chat) | No | IVS docs `[per authors]`; React/WebSocket `[estimate]` |
| Vimeo | Global | None, except Vimeo OTT Enterprise paid add-on | HLS and DASH (MSE); progressive MP4 on some plans | MSE preferred; Safari native HLS | H.264, HEVC (HDR), AAC | Moderate | No | Vimeo help `[per authors]` |
| Dailymotion | Global (FR) | None for regular content | HLS (Dailymotion created hls.js) | hls.js or native HLS | H.264/AAC | Yes (React) | No | hls.js origin `[secondary]`; rest `[estimate]` |
| VK Video | RU | None for UGC `[estimate]`; licensed films unknown | DASH via own MSE player; MP4 fallback "works everywhere"; HLS also present | MSE for DASH | AV1 (since Sept 2024), VP9, H.265, H.264; AAC/Opus | Yes | Smart TV app is a web app (ZombieBox → VK web-player core) for Tizen/webOS; no public `/tv` URL found (unknown) | Habr VK blog Nov 2025 `[per authors]`; comss.ru `[secondary]` |
| OK.ru video | RU | None for UGC `[estimate]` | VK's platform: DASH/HLS, AV1 since 2024 | MSE | AV1/VP9/H.264 | Yes | No | AV1 `[secondary]`; rest `[estimate]` |
| Rutube | RU | None for UGC `[estimate]`; premium unknown | HLS; 380+ CDN servers | hls.js-style MSE; native HLS on Safari | H.264 baseline; 8K/HDR tests | Yes | Smart TV apps only | RUTUBE TECH on kod.ru Jul 2026 `[per authors]` |
| Kinopoisk (HD) | RU | DRM (Widevine/PlayReady on web) | DASH (likely) via MSE+EME | EME required | H.264/HEVC | Yes | TV apps | comss.ru 2021 `[secondary]`; rest `[estimate]` |
| Okko | RU | DRM: PlayReady + Widevine "at system level" | DASH/HLS + EME | EME | H.264/HEVC | Yes | TV apps | Habr user report `[secondary]` |
| ivi | RU | DRM for licensed; free/AVOD unknown | DASH/HLS + EME | EME | H.264/HEVC | Yes | TV apps | comss.ru `[secondary]` |
| Premier | RU | Likely DRM for licensed; some free episodes may be clear (unknown) | HLS/DASH | unknown | H.264 | Yes | TV apps | unknown |
| Start, KION, More.tv | RU | Likely DRM (unknown); More.tv some free content | unknown | unknown | unknown | Yes | TV apps | unknown |
| Wink | RU | DRM; FAS (Sept 2025) says Wink uses Russian content-protection systems, not necessarily Widevine | unknown | EME with a non-standard key system possible (unknown) | unknown | Yes | TV apps | sostav.ru 2025-09-11 `[secondary]` |
| Amediateka | RU | DRM (HBO-style catalogue) | DASH + EME | EME | H.264/HEVC | Yes | TV apps | `[estimate]` |
| Netflix | Global (not RU since 2022) | Widevine (Chrome/Firefox/Edge/Opera), PlayReady (Edge 4K), FairPlay (Safari) | DASH-like MSE+EME | EME mandatory | H.264, HEVC, VP9, AV1; AAC, E-AC3 | Yes | Native/Cobalt-like TV apps only | Netflix help `[per authors]` |
| Disney+, Prime Video, Max, Apple TV+ | Global (not RU) | Widevine/PlayReady/FairPlay (Apple TV+: FairPlay on Safari, Widevine on Chrome/Firefox) | DASH/HLS + EME | EME mandatory | H.264/HEVC (+AV1 Prime) | Yes | No | `[estimate]` |
| Crunchyroll | Global | DRM on all streams (Widevine/PlayReady/FairPlay); reportedly forced ~March 2024 | DASH + EME (Shaka, `SHAK-6*` errors) | EME mandatory | H.264/AAC | Yes | No | Help article title `[per authors]`; rest `[estimate]` |
| Plex (personal media) | Global | None | Direct play or transcode to HLS/DASH | Direct: plain `<video src>`; transcode: MSE | Browser-dependent; often transcodes VP9/AV1/Opus | Yes (React, plex.tv sign-in) | 10-foot layout via HTPC app, not web (unknown) | Plex support `[per authors]`; forums `[secondary]` |
| Plex free movies / FAST | Global (not RU) | Some licensed titles may use Widevine (unknown) | HLS | unknown | H.264 | Yes | — | unknown |
| Jellyfin | Self-hosted | None | Direct play or HLS (TS/fMP4) transcode/remux via hls.js | MSE for hls.js; direct play needs only `<video>` | Browser-dependent; H.264+AAC universally safe | Yes (ES2015+) | "TV layout" selectable in UI, no special UA `[estimate]` | Jellyfin docs `[per authors]` |
| Emby | Self-hosted | None | As Jellyfin | As Jellyfin | As Jellyfin | Yes | "TV mode" in web app | `[estimate]` |
| Internet Archive | Global | None | Progressive MP4 (H.264+AAC, faststart), JW Player 8 | No MSE; plain `<video src=mp4>` | H.264 8-bit yuv420 + AAC/MP3 | Light | No | IA blog `[per authors]`; TASVideos `[secondary]` |
| PeerTube | Federated | None | HLS (fMP4) + hls.js + p2p-media-loader (WebRTC data channels); HTTP fallback | MSE for hls.js | H.264 + AAC | Yes (Angular) | No | p2p-media-loader docs `[per authors]` |
| Tubi | US/global (not RU) | Widevine reported on web HLS (2022) | HLS + EME | EME likely | H.264 | Yes | No | forum `[secondary]` (weak) |
| Pluto TV | US/EU (not RU) | Free live: HLS with SSAI, generally no DRM in browser; some premium/1080p variants DRM-only on some devices; one report of needing DRM in Firefox | HLS (stitched, SSAI, SCTE-35), some DASH | hls.js or native HLS | H.264/AAC | Yes | No | Paramount spec `[per authors]`; Streamlink, forums `[secondary]` |
| Smotrim.ru (VGTRK live) | RU | None for live `[estimate]`; some geo-restricted to RU IPs | HLS live (`player.vgtrk.com`) | hls.js or native HLS | H.264/AAC | Moderate | No | `[estimate]` (a Kodi add-on exists) |
| 1tv.ru | RU | Live none `[estimate]`; some archive unknown | HLS | hls.js or native | H.264/AAC | Moderate | No | `[estimate]` |
| Public IPTV `.m3u`/`.m3u8` | Global | None, or HLS AES-128 clear-key (key over plain HTTP, no EME/CDM) | HLS (MPEG-TS or fMP4) | hls.js (transmuxes TS→fMP4) or native HLS | H.264/AAC mostly; some HEVC/MP2 | No | Our own UI | `[estimate]` |

### 7.1 Works without DRM?

| Service | Works without DRM |
|---|---|
| YouTube | Yes for normal videos (paid movies, YouTube TV: no) |
| Twitch, Dailymotion, OK.ru, PeerTube, Internet Archive | Yes |
| Vimeo | Yes (except Vimeo OTT Enterprise DRM channels) |
| VK Video | Yes (UGC); licensed catalogue unknown |
| Rutube | Yes (UGC); premium unknown |
| Jellyfin / Emby / Plex (personal media) | Yes |
| Smotrim / 1tv live | Yes (likely; geo-restricted to RU) |
| Generic IPTV `.m3u8` | Yes (incl. AES-128 clear-key HLS) |
| Pluto TV | Partial (free live yes, some variants DRM) |
| Plex free movies/FAST | Partial / unknown |
| ivi, Premier, More.tv | Partial at best (possibly some free titles); paid catalogue no |
| Tubi | No (Widevine reported) |
| Kinopoisk, Okko, Start, KION, Wink, Amediateka | No |
| Netflix, Disney+, Prime Video, Max, Apple TV+, Crunchyroll | No |

### 7.2 Minimal capability set for the "Yes" group

1. JS: ES2020 (async/await, optional chaining, BigInt, `Promise.allSettled`), ES modules, Web Workers; WebAssembly for Twitch (IVS wasm worker); Custom Elements/Shadow DOM (YouTube), `IntersectionObserver`, `ResizeObserver`, `fetch` with streaming `ReadableStream` (YouTube SABR), `AbortController`, `TextDecoder`, `crypto.subtle`/`getRandomValues`.
2. MSE: `MediaSource`, `SourceBuffer` with fMP4 `video/mp4; codecs="avc1.*"`, `mp4a.40.2`. MSE in a Worker is a plus for Twitch, not required. Optionally `video/webm; codecs="vp9"` and `opus`.
3. Codecs: H.264 High + AAC-LC is the mandatory baseline and covers every "Yes" service; VP9/Opus and AV1 are optional. Report them honestly in `isTypeSupported`.
4. HLS: hls.js on MSE is enough; native `<video src=*.m3u8>` is a bonus. AES-128 clear-key HLS works through hls.js with WebCrypto, no EME.
5. DASH: done in JS by sites (dash.js/Shaka/own players) on MSE.
6. Progressive MP4 in a plain `<video>` with HTTP Range (Internet Archive, Jellyfin/Plex direct play).
7. Networking: WebSocket (Twitch chat, VK/OK realtime), CORS, HTTP Range, cookies with `SameSite`; WebRTC DataChannel for PeerTube P2P (optional).
8. UI: Fullscreen API on `<video>` and elements (the video note says the stock PS5 browser lacks it; see the contradiction in §3.4), Gamepad API, `keydown` mapping of the D-pad (needed for TV layouts such as Jellyfin's), Page Visibility API, Media Session API (nice to have).
9. EME: expose no key systems; `requestMediaKeySystemAccess` rejects with `NotSupportedError` (spec-compliant); intercept the call to show our own notice.

YouTube Leanback needs a TV/Cobalt-like UA to be served at all; a per-origin "TV mode" UA toggle is a product decision, not a DRM matter. These items describe what a self-built engine must provide; for the system browser (which exposes PlayReady through EME, §3.4), the per-service result depends on what Sony's WebKit supports (unknown beyond §3.4). WASM without JIT is effectively unavailable in a JIT-off WebKit build `[estimate]` (§5.2), which matters for Twitch's player in any interpreter-only engine.

### 7.3 Notice when a site requests DRM (EME)

Trigger: a page calls `navigator.requestMediaKeySystemAccess(...)` (or `video.setMediaKeys`). Show it once per origin per session; do not re-show when a site retries several key systems in a row.

English:

> This site requested protected (DRM) playback. This browser does not include a DRM module (Widevine, PlayReady or FairPlay), so protected videos cannot be played here. Free, non-protected videos on other sites are not affected. To watch this content, please use the service's official PS5 app or another supported device.

Short toast: *"Protected (DRM) video isn't supported in this browser. Use the official app for this service."*

Russian:

> Этот сайт запросил воспроизведение защищённого (DRM) видео. В этом браузере нет модуля DRM (Widevine, PlayReady или FairPlay), поэтому защищённое видео здесь воспроизвести нельзя. Обычное незащищённое видео на других сайтах работает как прежде. Чтобы посмотреть этот контент, воспользуйтесь официальным приложением сервиса для PS5 или другим поддерживаемым устройством.

Short variant: *«Защищённое (DRM) видео в этом браузере не поддерживается. Используйте официальное приложение сервиса.»*

Note for Option A (shell over the system browser): the wording above was written for a self-ported engine. The console's own WebKit does expose PlayReady (§3.4), so the shell's notice must say "no Widevine or FairPlay; whether the console's PlayReady works for this service is not guaranteed" and must be shown before navigation, because the shell cannot hook `requestMediaKeySystemAccess` inside third-party pages.

**Contradiction (notice wording vs system browser):** the notice says the browser has no PlayReady module. That is true for a self-built engine, but the console's built-in browser does expose PlayReady through EME (§3.4). If the project wraps the system browser, the wording must not claim PlayReady is absent.

## 8. Sources

Repositories (commit as inspected; shallow clones, read-only):

1. https://github.com/ps5-payload-dev/sdk @a7685ce8e97ce1f8a4f38253a41d54241c9124f1 (2026-10-20); release v0.45 (2026-10-21)
2. https://github.com/ps5-payload-dev/elfldr @2a8da002e5292481ea964daa7342bbf4855f4c33 (2026-10-21)
3. https://github.com/ps5-payload-dev/pacbrew-repo @722187f0ee246c0d7d57401c8a97de1c7f4e2ff0 (2026-10-16); release/tag v0.41.1 (2026-10-10); mesa commit fa5edf73b1
4. https://github.com/ps5-payload-dev/SDL @ee4c47dc0d617b3bc8f35108f9956baf228a1322 (2026-09-24)
5. https://github.com/ps5-payload-dev/websrv @7748df9d2a43822a5ebfda149f43d4f2696d640b (2026-10-11)
6. https://github.com/ps5-payload-dev/shsrv @6f320637d56d344a0e7797753099e33238bbf146 (2026-06-28)
7. https://github.com/ps5-payload-dev/jtplay @2945def19b838534676a2f92801561f892284490 (2026-08-04)
8. https://github.com/ps5-payload-dev/launchpad @033d2a33df5beadbf3b7f9b80a4cae4369f1dca3 (2026-09-01)
9. https://github.com/ps5-payload-dev/ftpsrv @35bae13 (2026-10-20)
10. https://github.com/ps5-payload-dev/klogsrv @c1b64bd (2026-04-14)
11. https://github.com/ps5-payload-dev/tv4play @69fb2df (commit 6fd0235 cited); https://github.com/ps5-payload-dev/plutotv @b6d8f3e (commit f79a998 cited); https://github.com/ps5-payload-dev/svtplay (commit f5d5e48 cited)
12. https://github.com/ps5-payload-dev/bdj-ipv6-hen @9b90c6c (2026-05-11)
13. https://github.com/ps5-payload-dev/kstuff @3962120 (2025-01-26); https://github.com/EchoStretch/kstuff-lite/releases (v1.11)
14. https://github.com/ZiZc3/VLC @c1862643f84b68d3b4f01cb484b887196093bfa2 (2026-10-22)
15. https://github.com/RafaelNGP/xbox-cloud-gaming-ps5 @dbf20786afc23207b461b2203dff25df86ff4804 (2026-10-30)
16. https://github.com/OpenCloudGaming/OpenNOW-PS5 @2a2349b71db6b95a937cd5b2abc4f77acf9ebed1 (2026-10-18)
17. https://github.com/Sp9nky/unofficial-stremio-ps5-port @b8444a661b0b304d1298463151065297840c15e9 (2026-10-14)
18. https://github.com/LightningMods/etaHEN @dafa13b (2026-05-03); https://github.com/LightningMods/etaHEN/releases
19. https://github.com/LightningMods/Itemzflow
20. https://github.com/hammer-83/ps5-jar-loader
21. https://github.com/PS5Dev/PS5-UMTX-Jailbreak @2cf6778 (2024-10-08)
22. https://github.com/ntfargo/Relapse-Exploit @dd8e4e0 (2026-09-30)
23. https://github.com/Gezine/Y2JB @79c1be4 (2026-08-03)
24. https://github.com/Gezine/Luac0re
25. https://github.com/idlesauce/PS5-Exploit-Host
26. https://github.com/itsPLK/ps5-payload-manager (`src/ps5_launcher.c`)
27. https://github.com/owendswang/ps5-web-file-manager @04c11b1
28. https://github.com/notmaj0r/CheatRunner (`src/cr_browser.c`)
29. https://github.com/X-F1REBALL-X/wk-autoloader
30. https://github.com/owendswang/ps5-web-browser-fullscreen
31. https://github.com/ItsBlurf/BFpilot
32. https://github.com/itsPLK/ps5-kexp
33. https://github.com/VivaLaVent/PS5MC (formerly VivaLaVent/kodi-ps5)
34. drakmor/ps5-native-app-boilerplate and mirrors JMUtechnologies/ps5-native-app-boilerplate, leolobato/ps5-native-app-boilerplate; darkxex/ps5-opengl (mirror of deleted blackbearreloaded/ps5-opengl); mihawk-99/PS5_Mesa, mihawk-99 PS5_Vulkan; ShadowMountPlus (drakmor)
35. john-tornblom/ps5-payload-sdk (archived original); JMUtechnologies/ps5-payload-sdk (mirror, ignored); rrooij/ps5-payload-sdk-docker; BenNoxXD/PS5-Payload-SDK-Docker
36. https://github.com/matomo-org/device-detector @5b92fc6 (`Tests/fixtures/console.yml`)
37. https://github.com/WebKit/WebKit @0c7b760 (2026-08-28) and @22efb76e6947 (2026-10-30, sparse)
38. https://github.com/LadybirdBrowser/ladybird @b5f8d768 (2026-10-30)
39. https://github.com/servo/servo @fda2a0e (2026-10-30, sparse)
40. https://github.com/netsurf-browser/netsurf @d06cd9d (2026-10-28, GitHub mirror)
41. https://github.com/etonedemid/netsurf-switch
42. https://github.com/natureglass/Brewser
43. https://github.com/CTurt/cturt.github.io/blob/master/ps4-3.html

Documentation, articles and reports:

44. https://switchbrew.org/wiki/Internet_browser
45. https://gamebrew.org/wiki/Netsurf_3DS
46. https://tembrica.com/en/ps5-browser-test
47. https://mp1st.com/news/there-is-a-hidden-ps5-web-browser-and-heres-how-to-access-it
48. https://www.gamepressure.com/newsroom/ps5-has-hidden-web-browser/z12843
49. https://www.psu.com/news/ps5-has-a-hidden-web-browser-here-is-how-to-access-it/
50. https://bgr.com/1941821/how-to-use-ps5-web-browser/
51. https://robots.net/tech/ps5-browser-media-playback-codec-support-list/
52. https://tech-insider.org/ps5-firmware-14-10-psn-jailbreak-lockout-2026/
53. https://videocardz.com/newz/ps5-13-60-jailbreak-released-almost-every-ps5-can-now-be-jailbroken
54. https://x.com/onejailbreak_/status/2089614100225495334
55. https://docs.google.com/spreadsheets/d/1SkQT1AZJCybePhOgGu6qzRbtChwn4AkIi_eO2nlDAbI
56. https://webkit.org/licensing-webkit/
57. https://wpewebkit.org/release/wpewebkit-2.46.1.html
58. https://blogs.igalia.com/carlosgc/?p=1016
59. https://code.fizz.buzz/mirror/freebsd-ports/commit/895bc805a975e4d6848427d88063cc8651e36fe4
60. https://lists.webkit.org/pipermail/webkit-gtk/2020-January/003577.html
61. https://pagure.io/releng/issue/11952
62. https://developer.resources.solid-run.com/wiki/x/doAWEQ
63. https://ladybird.org/newsletter/2026-05-31/
64. https://ladybird.org/newsletter/2026-08-31
65. https://computingforgeeks.com/install-ladybird-browser/
66. https://mail-archive.com/dev-servo@lists.mozilla.org/msg02412.html
67. https://nlnet.nl/project/Servo-multimedia/
68. https://mynixos.com/nixpkgs/package/libsciter
69. https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md
70. https://chrome.google.com/webstore/detail/tv-for-youtube/mbkenkpbobncoehadfogibjlhnaopcpb/reviews
71. https://reddit.birdcat.cafe/r/htpc/comments/y5o7mi/youtube_leanback_tv_useragent_for_4k_60fps
72. https://www.linuxlinks.com/vacuumtube-unofficial-wrapper-youtube-leanback/
73. https://ie.electronic.partners/news/sorry-htpc-owners-youtube-is-killing-its-web-based-tv-interface/
74. https://openuserjs.org/scripts/thecyberd3m0n/Youtube_TV_in_browser_Youtube_Leaning
75. https://advisories.mageia.org/MGAA-2025-0098.html
76. https://jsr.io/@luanrt/googlevideo/doc/sabr-stream
77. https://lwn.net/Articles/644323
78. https://docs.aws.amazon.com/ivs/latest/LowLatencyUserGuide/web-getting-started.html
79. https://awesome.ecosyste.ms/projects/github.com%2FpeaBerberian%2Fwasp-hls
80. https://siliconangle.com/2015/07/22/twitch-joins-the-list-of-sites-moving-to-ditch-flash-for-html5/
81. https://help.vimeo.com/hc/en-us/articles/12427018635921-DRM-with-Vimeo-OTT
82. https://help.vimeo.com/hc/en-us/articles/31622243101969-Digital-Rights-Management-DRM-for-my-videos
83. https://medium.com/vimeo-engineering-blog/drm-at-vimeo-9c3b9537119a
84. https://habr.com/ru/post/970634
85. https://www.comss.ru/page.php?id=14503
86. https://kod.ru/kak-rabotayut-sovremennye-videopleery
87. https://www.comss.ru/page.php?id=9082
88. https://habr.com/en/articles/483238
89. https://www.sostav.ru/publication/fas-trebuet-ot-onlajn-kinoteatrov-raskryt-dannye-o-softe-dlya-zashchity-kontenta-78055.html
90. https://help.netflix.com/en/node/23742
91. https://help.crunchyroll.com/hc/en-us/articles/20757542357396-What-do-I-do-with-a-DRM-Error
92. https://jellyfin.org/docs/general/clients/codec-support/
93. https://support.plex.tv/articles/200392226-plex-web-app-player/
94. https://forums.plex.tv/t/vp9-direct-play-in-plex-web/886683
95. https://novage.com.ua/p2p-media-loader/overview
96. https://www.npmjs.com/package/p2p-media-loader-hlsjs
97. https://blog.archive.org/tag/h-264/
98. https://tasvideos.org/Forum/Posts/430059
99. https://www.paramount.com/pluto-tv-live-stream-specifications
100. https://github.com/streamlink/streamlink/discussions/4699
101. https://forum.endeavouros.com/t/pluto-tv-website-and-drm/73022
102. https://openapps.pro/packages/hls-js

## Architecture options and decision

This section is the author's synthesis of the findings above. Everything it relies on is cited in the sections it references.

### Hard constraints that shape the choice

1. **Process model.** No open PS5 payload spawns a second ELF with `fork`/`posix_spawn`; the only working pattern is the elfldr trick (`rfork_thread` + `execve` of a system SELF + ptrace injection), which needs elevated privileges (see "SDK and runtime"). A browser engine that *requires* multi-process (WebKit2/WPE, Chromium, Gecko) therefore needs a custom spawn helper, which no one has written and which cannot be tested here.
2. **Graphics.** The baseline for payloads is a CPU framebuffer flipped through `libSceVideoOut` (SDL2 port). GL/Vulkan via Mesa zink/RADV appeared in pacbrew in October 2026 and is flagged by its own authors as untrusted; Sony's `libSceGLSlimVSH` has a stub only. Engines whose compositor requires EGL/GLES2 (upstream WebKit PlayStation port, WPE) are blocked until that path is proven on hardware.
3. **Video decode.** Software decoding (ffmpeg 7.0.1) is what the ps5-payload-dev players use. Hardware decoding through `libSceVideodec2` is used by third-party GPL ports (VLC, Stremio, cloud-gaming clients) but needs hand-written prototypes and a "big application" context. The system browser decodes natively HLS in hardware.
4. **DRM.** No open engine can obtain a Widevine or PlayReady CDM. The system browser exposes PlayReady only (per the examined web apps); Widevine-only services cannot work in any variant and the project does not try to change that.
5. **Build host.** 4 cores, 15 GB RAM, 27 GB disk, no console. A WebKit build is the only full-engine build that could fit at all (6-12 h, 6-12 GB, no debug info); Chromium, Gecko, Servo and Ladybird do not fit. Nothing built here can be run before the operator tests it on a console.
6. **Licensing.** The SDK's crt and libc are GPLv3+ without a linking exception and are statically linked into every payload, and the fullscreen patch and all comparable launcher apps are GPL-3.0. The project is therefore GPL-3.0-or-later regardless of the engine choice.

### Option A: shell over the system WebKit browser ("wrapper")

A GPL-3 payload (C, ps5-payload-sdk) runs as a background daemon on `127.0.0.1`: it serves the browser UI (tabs, address bar, bookmarks, history, settings, a DRM notice page, a capability probe page), persists state to disk, registers a home-screen tile (`webAppUri`, the same mechanism as svtplay/launchpad/websrv) and opens the console's built-in browser on itself with `sceSystemServiceLaunchWebBrowser` (SDK sample `samples/browser`). Optionally it applies the in-RAM fullscreen patch (owendswang/ps5-web-browser-fullscreen, GPL-3.0) to get a 1920x1080 viewport.

- Rendering, JS, networking, MSE, native HLS, hardware video decode, on-screen keyboard and DualSense pointer all come from Sony's WebKit; the HTML5 Fullscreen API is unknown for the shipped build (§3.4) and is probed, not assumed. This is the only option in which YouTube, Twitch, VK Video and Rutube players are known (from press reports and from the existing launcher apps) to run on the console today.
- **Tabs** are implemented by the shell: a tab is a persisted entry (URL, title, favicon, scroll-less); switching navigates the system browser to that URL; the shell page is the first history entry, so the browser's own "back" returns to it. If the console's WebKit supports `window.open` with named targets, tabs become real windows; this is feature-detected at runtime, never assumed.
- **Address bar, bookmarks, history** live in the shell page and in the daemon's JSON store on disk.
- **DRM notice**: the shell cannot run code inside third-party pages, so it cannot intercept `requestMediaKeySystemAccess` there. Instead the shell keeps a list of services known to require Widevine (from the "Online video services" section) and shows the notice before navigating, plus a probe page the operator can use to confirm what key systems the console offers.
- **Gamepad**: DualSense arrives as ordinary `keydown` codes (Cross=13, Circle=27, D-pad=37-40 ...); the shell maps those. The Gamepad API is probed, not assumed.
- Effort: 1-3 person-weeks. Risk: medium-low; the exact system-browser behaviour (window.open, Fullscreen API, EME key systems, codecs) is unknown until the operator runs the probe page.
- Limits: no control over the engine; "tabs" are navigation-level, not engine-level; the fullscreen patch is firmware-specific and in-memory; a browser-level exploit fix in a future firmware does not affect this (the browser itself stays), but a removal of the hidden browser would.

**Variant A2 (later extension, not for the first version):** the daemon additionally acts as a rewriting HTTP proxy (`/go/<url>`) so that third-party pages become same-origin and the shell can overlay its own tab bar and inject the EME notice into every page. This is how Ultraviolet-class web proxies work; it gives engine-level tabs in one window but breaks on complex sites and needs Service Workers on a loopback origin, which is unverifiable here. Recorded as an option, not pursued.

### Option B: port a real engine (upstream WebKit PlayStation port)

Build WebKit from the upstream PlayStation port (`OptionsPlayStation.cmake`, JIT off, curl/OpenSSL, no glib/GStreamer) against the ps5-payload-sdk, write a video backend on ffmpeg (+MSE), a spawn helper for WebContent/Network processes, and a compositor over Mesa zink/RADV or the CPU. Estimated 25-40 person-weeks by the engine study, plus 6-12 h per build on this host and no way to test. Two hardware tests (EGL context via `libSceGLSlimVSH` or Mesa; elfldr-style spawn with a passed socketpair) would de-risk it. The only path that gives a browser fully under project control, but out of scope for this engagement.

### Option C: hybrid native media app

SDL2 + RmlUi/lexbor + QuickJS + ffmpeg (all in pacbrew; jtplay proves the stack): a native app with an HTML-like UI, per-site stream extractors and a native HLS/MP4 player. 4-8 person-weeks, but it is not a browser: no site's own player runs, extractors break whenever sites change, and online cinemas with JS-only playback flows cannot be covered. Rejected for this task.

### Option D: lightweight engine (NetSurf, litehtml)

2-4 person-weeks for a framebuffer browser, but ES5-only JS with no XHR/fetch/media elements (NetSurf) or no JS at all (litehtml): none of the target video sites load. Rejected.

### Decision

**Recommended: Option A**, with the design kept open for Option A2 and with a probe page that turns every "unknown" about the system browser into a one-minute test on real hardware. The recommendation is made under the explicit assumption that "homebrew browser" may be a homebrew *application* that provides the browser experience (tabs, address bar, fullscreen, bookmarks, video) on top of the console's WebKit, rather than a self-ported engine. If the operator requires a self-ported engine, Option B is the only candidate and should be planned as a separate multi-month project with console access.

### What works and what cannot work with Option A

- Works (no DRM): YouTube, Twitch, Vimeo, Dailymotion, VK Video, OK.ru, Rutube, PeerTube, Internet Archive, Jellyfin/Emby/Plex personal media, public HLS/IPTV, likely Smotrim/1tv live TV.
- Possibly works (PlayReady is the only EME key system the console exposes; the project does nothing special for it): services that offer PlayReady on the web. Unverified.
- Cannot work (Widevine-only or app-only): Kinopoisk, Okko, Start, KION, Wink, Amediateka, Netflix, Disney+, Prime Video, Max, Apple TV+, Crunchyroll, Tubi. The shell says so before navigating.

## Outcome (2026-10-31)

The operator chose **Option B**: a self-ported engine rather than a shell over the system browser. What was built on this host, compile and link only, nothing run on a console: the upstream WebKit PlayStation port built with the ps5-payload-sdk (patches 0001-0010, JavaScriptCore without JIT, Skia CPU raster, curl/OpenSSL), an ffmpeg `<video>`/`<audio>` engine with progressive and native HLS playback (patch 0900, no MSE yet), an elfldr-derived spawner that starts WebProcess and NetworkProcess from the same multi-call ELF with the IPC socket at fd 3, a 10-foot shell (tabs, OSK, gamepad pointer, bookmarks, history, DRM notice) shared with a WebKitGTK host frontend, and a packaging script. The EGL/GLES path was not used: compositing is non-accelerated (`WKPagePaint` into the SDL framebuffer), which removes the graphics blocker listed under the hard constraints. See [architecture.md](architecture.md) for the design and [console-verification.md](console-verification.md) for the first test on hardware.
