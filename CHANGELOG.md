# Changelog

All notable changes to ProsperoWeb are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
