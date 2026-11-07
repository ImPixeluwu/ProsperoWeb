# Changelog

All notable changes to ProsperoWeb are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Unreleased

### Added

- D-pad link navigation (off by default; menu → "D-pad link navigation: On/Off", persisted as `dpad_links` in `config.json`, `--dpad-links` on the host). The D-pad moves the pointer to the nearest clickable element in the pressed direction (links, buttons, form fields, ARIA widgets) and Cross clicks it, which is faster than steering the pointer with the stick. An element in the same row or column wins over a nearer diagonal one; with nothing clickable that way on screen, the page scrolls by half a view. The pointer, sticks and touchpad keep working as before.

## v0.2.1 - 2026-11-02

### Fixed

- Startup on consoles whose ELF loader keeps the launch connection open until the payload exits (etaHEN's bundled elfldr, and some jailbreak hosts). The loader request is now sent asynchronously and the UI connects to the spawn broker as soon as it is listening, instead of blocking on the loader's response while the broker timed out with no client. The browser now starts with no separate elfldr on a setup like ShadowMountPlus + kstuff-lite + a jailbreak loader.
- An empty `log.txt` when a previous root-owned log file blocked the title from writing; the title now reclaims the file.
- A crash from restarting the background loader-retry thread.

### Changed

- The "no ELF loader" screen keeps retrying in the background, so starting a loader afterwards is picked up without relaunching the title.

## v0.2.0 - 2026-11-01

### Added

- Ad blocker (on by default): a native WebKit content rule list compiled from EasyList, EasyPrivacy, Peter Lowe and uBlock Origin filter lists (request blocking and element hiding). YouTube page ads are hidden with injected CSS; pre- and mid-roll video ads are removed by a bundled user script. An optional "Block cookie banners" sub-toggle is off by default.
- SponsorBlock (off by default): a bundled user script that queries the public SponsorBlock API and skips YouTube sponsor segments, with per-category toggles (only `sponsor` on by default).
- An "Add-ons" area in the menu with the Ad blocker and SponsorBlock toggles, applied live and persisted in `config.json`.
- Filter-list updates: "Update filter lists now" and a daily auto-update fetch EasyList and EasyPrivacy over the network and apply on the next launch; the full snapshot (incl. uBlock Origin, Peter Lowe and the cookie list) ships with the app as the baseline.
- Natural scrolling toggle (off by default; otherwise two-finger scroll follows the mouse-wheel direction).
- These are independent implementations using the public filter lists and API, not uBlock Origin or the SponsorBlock extension.

### Changed

- The WebKit content-extensions engine is enabled in the build, so the ad blocker uses a native content rule list rather than page scripts for request blocking.
- Removed the non-functional "Settings" and "Quit" menu items; the session is now saved continuously, so closing the app with the PS button keeps the open tabs, history and bookmarks.
- The release CI builds the PS5 title again (fixed a configure-stage failure).

### Known limitations

- The ad blocker attaches a single merged, network-first rule list because of a console sandbox limit on shared-memory handles; YouTube video-ad removal is best-effort.
- On-device filter updates fetch only from EasyList's host (the one reachable from the console); the broader lists ship in the app and are not re-fetched.
- As in 0.1.0: a privileged ELF loader on `127.0.0.1:9021` is required, no DRM sites, no GPU acceleration, no WebAudio, and only firmware 13.42 is tested.

## v0.1.0 - 2026-11-01

First release. A WebKit web browser for the jailbroken PlayStation 5, driven by
the controller and verified on the developer's console (firmware 13.42).

### Added

- Native home-screen title `PRWB00001`, launched like any other app and placed on the console's Media row. The browsing profile is created in `/data/prosperoweb/`. The websrv tile is kept as a fallback launch path.
- Browsing: tabs, an address bar with URL and search normalisation, back, forward and reload, bookmarks, history and session restore.
- Input: a DualSense pointer on the left stick or the touchpad (slide, tap, two-finger scroll, press-and-slide to drag), tab switching and a menu; a USB keyboard; text entry through the console's own keyboard dialog with a built-in on-screen keyboard as the fallback.
- Media: HTML5 video and audio through an ffmpeg engine, with Media Source Extensions (YouTube-style players), progressive MP4 and WebM, and native HLS. Hardware decoding of 8-bit H.264 and HEVC through the console's `sceVideodec2`, with automatic fallback to software. Video is composed under the page; audio is mixed and played by the UI process.
- Fullscreen: the HTML5 Fullscreen API and a chrome-hiding fullscreen mode.
- JavaScript JIT (baseline and DFG) and WebAssembly, both switchable off.
- Bundled fonts covering Latin, Cyrillic, CJK and colour emoji.
- English and Russian interface.

### Known limitations

- A privileged ELF loader (elfldr on `127.0.0.1:9021`, started by etaHEN) is required: the sandboxed title cannot create its own processes.
- No DRM sites: there is no content decryption module, by design.
- No GPU acceleration: software rendering only.
- No WebAudio.
- Only firmware 13.42 is tested; other versions are untested.
