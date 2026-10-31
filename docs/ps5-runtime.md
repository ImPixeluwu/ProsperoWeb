# PS5 runtime layout and process model

How the `ps5browser` payload is laid out and runs on a jailbroken PS5. This is
the phase-1 (software raster) design; anything not yet verifiable on a console
is called out under "Must be verified on a console" and "Known risks".

## On-disk layout

The browser ships as a single statically-linked ELF plus its data directory,
installed under `/data/ps5browser/`:

```
/data/ps5browser/
  ps5browser.elf        the payload (UI + WebProcess + NetworkProcess, one ELF)
  chrome/
    chrome.html         the HTML browser chrome (from src/chrome-ui/)
    ...                 chrome assets (css/js/img)
  cacert.pem            CA bundle for TLS (curl SSL handle on PS5)
  fonts/                Noto, Liberation and DejaVu fonts + fonts.conf
                        (FONTCONFIG_FILE; see "Fonts" below)
  config.json           home url, user agent, language (see below);
                        shipped as config.json.sample
  LICENSES/             GPL-3.0 (the ELF), MPL-2.0 notice (cacert.pem), one
                        license text per font family (OFL 1.1, DejaVu)
  README-install.txt
  tabs.json             persisted session (core::Session; saved 5 s after a
                        tab/URL change and on Quit)
  history.json          persisted history
  bookmarks.json        persisted bookmarks
  cache/                network cache (WKWebsiteDataStore)
  storage/ localstorage/ indexeddb/   web storage (WKWebsiteDataStore)
  tmp/                  TMPDIR (created at startup)
  log.txt               shared log of all roles, fsync'ed per line (also stderr + klog)
```

`config.json` keys (all optional; defaults in `src/frontend/ps5/ps5_config.h`):

```json
{
  "home_url":   "https://duckduckgo.com/",
  "user_agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Safari/605.1.15",
  "language":   "en",
  "chrome_dir": "/data/ps5browser/chrome",
  "width":      1920,
  "height":     1080
}
```

`scripts/ps5/package.sh` assembles this directory as `dist/ps5browser/`,
plus `dist/homebrew/ps5browser/` (`homebrew.js` + `sce_sys/icon0.png`) for
`/data/homebrew/ps5browser/`: websrv lists every `/data/homebrew/<name>/`
with a `homebrew.js` (preferred) or `eboot.elf` and uses
`sce_sys/icon0.png` as the tile. The `homebrew.js` launches
`/data/ps5browser/ps5browser.elf` in place (`cwd` `/data/ps5browser`), so
the large ELF is not duplicated. websrv needs no `param.json` for homebrew
entries. Without websrv, elfldr loads it directly:
`echo file:/data/ps5browser/ps5browser.elf | nc -q0 PS5 9021` (elfldr
reads a raw ELF, a URI line or an HTTP GET `?uri=`, not a POST body).

Environment set in `main()` before the role dispatch, so every process role
gets it (`src/frontend/ps5/ps5_runtime_env.cpp`, existing values win; the UI
then overrides `SYSTEM_LANGUAGE` from `config.json`):

| Variable | Value | Read by |
|---|---|---|
| `FONTCONFIG_FILE` | `/data/ps5browser/fonts/fonts.conf` | fontconfig (WebProcess) |
| `FONTCONFIG_PATH` | `/data/ps5browser/fonts` | fontconfig |
| `PS5BROWSER_CA_BUNDLE` | `/data/ps5browser/cacert.pem` | curl SSL handle (NetworkProcess, patch 0004) |
| `SYSTEM_LANGUAGE` | `en-US` / `ru-RU` from `config.json` `language` | WTF `platformUserPreferredLanguages` (patch 0006) |
| `TMPDIR` | `/data/ps5browser/tmp` (created at startup) | temporary files |
| `HOME` | `/data/ps5browser` | home-relative defaults |

`elfldr_spawn_fds` copies the parent's `environ` into each child
(`elfldr_set_environ`, the shsrv pattern), so the auxiliary processes see
the same values.

### Fonts

The fonts are pinned (URL + sha256) in `scripts/ps5/fonts/fonts.lock` and
staged by `scripts/ps5/fonts/stage-fonts.sh`, which `package.sh` runs;
downloads are cached in `third_party/dl/fonts/`, archives are unpacked
into their own directory under `build-deps/fonts/`. 68 MB in all:

| Family | Version | Files | Size |
|---|---|---|---|
| Noto Sans, Noto Serif | 2.015 | Regular, Bold, Italic, Bold Italic (hinted TTF) | 5.5 MB |
| Noto Sans Mono | 2.014 | Regular, Bold | 1.2 MB |
| Liberation Sans, Serif, Mono | 2.1.5 | Regular, Bold, Italic, Bold Italic | 4.4 MB |
| Noto Color Emoji | 2.051 | `NotoColorEmoji.ttf` (CBDT bitmaps) | 10.7 MB |
| Noto Sans CJK | 2.004 | `NotoSansCJK-{Regular,Bold}.ttc` (JP, KR, SC, TC, HK + Mono CJK) | 39.5 MB |
| DejaVu | 2.37 | as before, without DejaVuMathTeXGyre | 11 MB |

All are SIL OFL 1.1 except DejaVu (Bitstream Vera / public domain); the
license texts are in `LICENSES/`.

fontconfig is built with `/user/homebrew/etc/fonts/fonts.conf`, which does
not exist on the console, and no `conf.d` is loaded, so the generated
`fonts.conf` holds every mapping:

- `sans-serif`, `system-ui`, `cursive`, `fantasy` → Noto Sans, Liberation
  Sans, DejaVu Sans, Noto Sans CJK SC, Noto Color Emoji; `serif` → Noto
  Serif, Liberation Serif, DejaVu Serif, CJK, emoji; `monospace` →
  Liberation Mono, Noto Sans Mono, DejaVu Sans Mono, Noto Sans Mono CJK SC;
  `emoji` → Noto Color Emoji.
- Arial and Helvetica → Liberation Sans first, Times and Times New Roman →
  Liberation Serif first, Courier and Courier New → Liberation Mono
  (metric-compatible, so layouts sized for those fonts keep their line
  breaks), then the generic list.
- Common platform and web families (`-apple-system`, `BlinkMacSystemFont`,
  Segoe UI, Roboto, Helvetica Neue, Verdana, Tahoma, Open Sans, Inter,
  Ubuntu, Georgia, Consolas, Menlo, Monaco, Apple Color Emoji, Segoe UI
  Emoji, ...) → the matching generic list.
- A catch-all appends Noto Sans to every request, so an unknown family
  (and a request with no family) still resolves to a font instead of
  WebKit's last-resort path. The cost: an unknown first family in a CSS
  stack resolves to Noto Sans instead of falling through to the next one
  (`"Merriweather", Georgia, serif` renders in Noto Sans).
- Per-language CJK faces: WebKit's per-character fallback sorts all fonts
  for the page language with no family, and every face of the CJK
  collection claims every CJK language, so `ja`, `ko`, `zh-tw`, `zh-hk`,
  `zh-mo`, `zh-cn`, `zh-sg` and `zh` each append their own face.

Rules for editing the generator: every alias binds `same` and the
catch-all binds `strong`, because Skia's fontconfig font manager drops the
weak families after the last strong one and rejects a match whose family
is not in what is left; and each `<alias>` tests one family (blocks
listing several families did not take effect for all of them).

The cache directory `<cachedir>` is `/data/ps5browser/cache/fontconfig`;
fontconfig creates it (and its parents) on the first start, which scans
the fonts once (the CJK collections are the slow part). `fc-cache` is not
needed on the console.

`scripts/ps5/fonts/check-fonts.sh` checks the staged `fonts.conf` on the
host: it rewrites `<dir>`/`<cachedir>` to the staged files and a fresh
cache directory, runs `fc-match` for each mapped family, the language
fallbacks and an unknown family, and fails if a match leaves the bundled
fonts or the cache directory is not created.

Sizes of the current build (Release, `--gc-sections`, ffmpeg engine on):
`ps5browser.elf` 169 MB as linked, **162 MB shipped** (`llvm-strip
--strip-debug`, symbol table kept for backtraces), 129 MB with
`--strip-all` (not shipped: elfldr and the spawner only need program
headers, section headers and `SHT_RELA`, so `--strip-all` should also load;
untested). About 87 MB `.text`, 19 MB `.rodata`, 22 MB `.bss`, no `PT_TLS`.
`NEEDED`: `libkernel_web`, `libSceLibcInternal`, `libSceNet` plus, through
SDL2, `libSceSystemService`, `libSceUserService`, `libScePad`,
`libSceAudioOut`, `libSceVideoOut`, `libSceKeyboard`, `libSceImeDialog`
(all `.sprx` system modules). The whole package is about 235 MB, 68 MB of
it fonts.

The ELF path used when re-spawning the auxiliary processes is resolved by
`ps5_spawn_resolve_self_path()`: `PS5BROWSER_SELF_PATH` env →
`/data/ps5browser/ps5browser.elf`. The UI's `main()` exports
`PS5BROWSER_SELF_PATH` from its own `argv[0]` when that is an absolute
readable `*.elf` path (an explicit value wins).

## Process model

The payload is one multi-call ELF. `main()`
(`src/frontend/ps5/main.cpp`) dispatches on the role name:

- `argv[0]` basename `WebProcess` → `ps5_webkit_web_process_main()`
- `argv[0]` basename `NetworkProcess` → `ps5_webkit_network_process_main()`
- `argv[0]` basename `spawn-broker` → `ps5_spawn_broker_main()` (`src/ps5/spawn/`)
- otherwise → the UI process (`Ps5App`)

A `--role=<name>` argument and then `PS5BROWSER_ROLE` are honoured as
fallbacks when `argv[0]` is not a role name.

The four roles:

- **UI** — our `main()`. Owns the SDL2 window and the WebKit UI process
  (`WKView`/`WKPage`), composites content + chrome, feeds gamepad/keyboard
  input. Started from the websrv tile through hbldr, so it is the big app
  (title `FAKE00000`), which is what gives it the screen and the controller.
- **spawn-broker** — a plain payload that performs every spawn for the UI.
  Started on demand by the UI through websrv (daemon launch). Listens on an
  AF_UNIX socket where the console allows one and on `127.0.0.1:17771`.
  Exits when the UI is gone and its children have exited.
- **WebProcess** — WebKit web content process. One per web process (the number
  is bounded by WebKit; see risks). Spawned by the broker for the UI.
- **NetworkProcess** — WebKit networking process. Spawned the same way.

```
 websrv (plain payload, :8080)
   |  tile click: GET /hbldr?path=...ps5browser.elf      (hbldr, big app)
   |  first spawn: GET /hbldr?...&daemon=1&args=spawn-broker ...  (elfldr_spawn)
   v                                   v
 UI (big app FAKE00000)  <-- AF_UNIX socket, else TCP 127.0.0.1:17771 -->  spawn-broker (plain payload)
   |  WebKit IPC (SOCK_SEQPACKET pair; the client end goes                  |  rfork + ptrace + execve
   |  as SCM_RIGHTS over AF_UNIX, or is rdup'ed from the UI over TCP)       v
   +------------------------------------------------------------------>  WebProcess, NetworkProcess
                                                                          (children of the broker)
```

WebKit's `ProcessLauncherPlayStation.cpp` (patched for the homebrew port)
creates a `SOCK_SEQPACKET` socketpair, keeps the server end, and calls our
spawn contract.

### Audio path

The console mixes only the foreground application's audio. The WebProcess
is a plain payload: SDL's PS5 driver opens a sceAudioOut port there and the
port is drained at the real rate, but nothing is heard (verified on hardware).
Media audio is therefore forwarded to the UI process:

```
 WebProcess: FFmpegAudioDecoder -> RemoteAudioSink (writer thread)
     |  loopback TCP 127.0.0.1:$PS5BROWSER_AUDIO_PORT (17772), one connection per media element
     v
 UI: audio server thread (src/ps5/audio) -> Mixer (ring per stream, gain, clip) -> SDL device -> sceAudioOut
```

- The UI opens one SDL device (48 kHz, float32, stereo, 1024-sample buffers)
  and starts the server before WebKit is initialised
  (`src/frontend/ps5/ps5_audio_server.cpp`). AF_UNIX bind is refused under
  `/data`, hence TCP, like the broker.
- Protocol: `webkit-port/media/ffmpeg/FFmpegAudioWire.h` (hello `PS5A` v1,
  then pcm/flush/pause/volume/close frames; the server sends a played report
  every 20 ms). The WebProcess keeps about 100 ms queued on the server and
  derives its audio clock from the reports, so A/V sync follows what is
  actually heard (queued audio plus two SDL buffers).
- Fallbacks in the WebProcess: remote, then the local SDL sink (inaudible on
  the console), then the null sink. A server that goes away mid-stream turns
  the remote sink into a silent real-time pacer; video keeps playing.
- `PS5BROWSER_AUDIO_TEST=1` plays a 1 s 440 Hz tone through the server at UI
  startup, which checks UI-process audio without any page.

Expected log lines:

```
[UI] audio server: device opened: SDL driver ps5, device 2, 48000 Hz format 0x8120 2 ch 1024 samples
[UI] audio server: listening on tcp 127.0.0.1:17772
[UI] audio server: test tone played                                   (PS5BROWSER_AUDIO_TEST=1)
[WebProcess] media: audio sink: remote 127.0.0.1:17772 connected (stream 1, output latency 2048 frames, 100 ms ahead)
[WebProcess] media: audio sink: using remote, latency 43 ms
[UI] audio server: stream 1 connected (48000 Hz 2 ch)
[UI] audio server: stream 1: underrun (...)                           (at most once per stream)
[UI] audio server: stream 1 closed (N frames, U underruns, D dropped)
```

### Why a broker

On the console (firmware reporting `0x13420000`), launched from the websrv
tile, the UI reached `spawn: role=WebProcess starting`, the rfork child got
past `sys_budget_set(0)` (failing with `ENOENT`, non-fatal), and the kernel
panicked and powered the console off. The same spawner worked when the UI
was itself a plain payload started by elfldr (children spawned, profile
created; no picture, because a plain payload is not the foreground app).
hbldr never rforks from the big app either: websrv starts big apps from its
own plain-payload process. So the UI no longer spawns: `rfork` + `ptrace` +
`execve` run in the broker, a plain payload like the earlier working runs,
where `sys_budget_set(0)` exists again.

### Launch sequence

1. The websrv tile starts the UI through hbldr (`homebrew.js`).
2. WebKit's first `ps5_spawn_self()` tries the broker endpoints in order:
   AF_UNIX `$PS5BROWSER_SPAWN_SOCK` (if set), `/tmp/ps5browser-spawn.sock`,
   `/data/ps5browser/tmp/spawn.sock`, then TCP
   `127.0.0.1:$PS5BROWSER_SPAWN_PORT` (default `17771`).
3. Nobody listens: the UI sends websrv a plain HTTP/1.0 request
   (`PS5BROWSER_WEBSRV`, default `127.0.0.1:8080`):

   ```
   GET /hbldr?path=/data/ps5browser/ps5browser.elf&cwd=/data/ps5browser&daemon=1&args=spawn-broker%20--role%3Dspawn-broker&env=PS5BROWSER_ROLE%3Dspawn-broker%20PS5BROWSER_SPAWN_PORT%3D17771%20PS5BROWSER_SELF_PATH%3D/data/ps5browser/ps5browser.elf HTTP/1.0
   ```

   (`PS5BROWSER_SPAWN_SOCK` is forwarded only when the UI has it set.)

   websrv (`src/ps5/sys.c`, `sys_launch_daemon`) reads a `path` starting
   with `/` (or `file:`) from disk, splits `args` and `env` on spaces
   (`args_split`; a backslash escapes the next character) and starts the
   ELF with `elfldr_spawn`: `argv[0]` is the first `args` token, which is
   also the process name, so the broker shows up as `spawn-broker`. websrv
   answers `200` once the payload runs, `503` if it could not start it.
   The broker binds the first AF_UNIX path that works (console run 3:
   `/data` gives `EOPNOTSUPP`) and also listens on loopback TCP; it fails
   to start only if it can listen on nothing. It lives as long as the UI's
   connection (the UI never closes it): when the UI exits or crashes, the
   broker logs `broker: client disconnected, terminating N children and
   exiting`, kills the WebProcess/NetworkProcess and exits, so every UI run
   asks websrv once.
4. The UI polls all endpoints for up to 5 s, pings the broker, and sends the
   spawn request. Every later spawn reuses the connection.
5. The broker spawns the child with the UI's ELF path, `argv` and whole
   `environ` (so `FONTCONFIG_FILE`, `PS5BROWSER_CA_BUNDLE`, … reach the
   WebProcess whatever the broker's own environment is), and the
   descriptors at child fd 3, 4, …: over AF_UNIX the ones it received as
   `SCM_RIGHTS`; over TCP the UI names them (`src_pid`, `src_fds`) and the
   loader copies them out of the UI process with `rdup`. It replies with
   the pid, and reports children that exit on their own back to the UI.

If the broker cannot be reached or started, the spawn fails (WebKit then
shows no page) unless `PS5BROWSER_SPAWN_FALLBACK=direct` is set, which spawns
in-process — and panics the console when the UI is the big app. Without the
fallback, after two failures in a row the UI waits 30 s before the next
attempt (logged as `backing off`), so a broken broker does not make WebKit's
endless relaunches hammer websrv.

### Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `PS5BROWSER_SPAWN_MODE` | `broker` | `direct` spawns in-process (old behaviour); only for plain-payload (elfldr) launches |
| `PS5BROWSER_SPAWN_FALLBACK` | unset | `direct`: spawn in-process when no broker can be reached |
| `PS5BROWSER_SPAWN_SOCK` | unset | extra AF_UNIX broker socket tried first (then `/tmp/ps5browser-spawn.sock`, `/data/ps5browser/tmp/spawn.sock`) |
| `PS5BROWSER_SPAWN_PORT` | `17771` | loopback TCP port of the broker (UI and broker) |
| `PS5BROWSER_WEBSRV` | `127.0.0.1:8080` | websrv address (IPv4 literal) used to start the broker |
| `PS5BROWSER_SELF_PATH` | `/data/ps5browser/ps5browser.elf` | the ELF the broker runs as itself and spawns for the children |
| `PS5BROWSER_ROLE` | — | role fallback (`spawn-broker` for the broker) |
| `JSC_useJIT` | `false` | JavaScriptCore baseline/DFG JIT (see [JavaScript JIT and WebAssembly](#javascript-jit-and-webassembly)) |
| `JSC_useWasm` | `false` | WebAssembly (IPInt interpreter, plus the JIT's entry thunks when `JSC_useJIT` is on) |
| `PS5BROWSER_MEDIA_AUDIO` | unset | first media audio sink tried: `remote` (default when `PS5BROWSER_AUDIO_PORT` is set), `sdl` (output in the WebProcess, inaudible on the console) or `null` (silent) |
| `PS5BROWSER_AUDIO_PORT` | `17772` | loopback TCP port of the UI's audio server (UI and WebProcess); `off` disables the server and the forwarding |
| `PS5BROWSER_AUDIO_AHEAD_MS` | `100` | audio the WebProcess keeps queued in the UI's server (20-900 ms): lower is less latency, higher survives longer stalls |
| `PS5BROWSER_AUDIO_TEST` | unset | `1`: the UI plays a 1 s 440 Hz test tone at startup |
| `PS5BROWSER_MEDIA_FRAME_THREADS` | unset | `1`: frame-threaded video decoding (default: slice threads only) |

Any variable can also be set from `config.json` as `"env": {"NAME": "value"}`
(applied in every process over the defaults above).

## Spawn mechanics (fd / argv contract)

`webkit-port/include/ps5webkit/spawn.h` declares, and
`src/ps5/spawn/` defines:

```c
int ps5_spawn_self(const char* const* argv, const int* fds, int nfds);
int ps5_terminate_process(int pid);
```

The launcher calls, for example:

```c
const char* argv[] = { "WebProcess", "<processIdentifier>", "3", NULL };
int fds[] = { clientFd };          // the SOCK_SEQPACKET client end
int pid = ps5_spawn_self(argv, fds, 1);
```

`ps5_spawn_self` (in broker mode it forwards `argv`, `environ` and `fds` to
the broker, which runs these steps; in direct mode the UI runs them itself):

1. Builds the child fd table: `fds[i]` must appear in the child as fd `3+i`
   (`ps5_spawn_build_fd_table`). The WebKit IPC socket therefore lands at fd 3,
   which is exactly where `AuxiliaryProcessMain` reads it (`argv[2] == "3"`).
2. Maps the ELF image once (read-only `MAP_PRIVATE`) and keeps it.
3. Runs the elfldr-derived loader (`elfldr_spawn_fds`):
   `rfork_thread(RFPROC|RFCFDG|RFMEM)` → the child opens `/dev/deci_std*` as
   fds 0/1/2, `PT_TRACE_ME`, and `execve` of the `SceSpZeroConf` system
   trampoline; the parent breakpoints to just before `main`, sets the libc
   heap size, the process name (`argv[0]`) and the environment, maps our ELF,
   then — because `RFCFDG` wiped the fd table — injects each extra descriptor
   with `pt_rdup(child, src_pid, fds[i].parent_fd)` + `pt_dup2(child, tmp, 3+i)`
   (`src_pid` is the spawning process itself, or the UI when the broker
   runs over TCP), and detaches so `main()` runs our dispatcher.

Descriptor rules (from the contract):

- `fds[i]` → child fd `3+i`; open, blocking mode unchanged, close-on-exec
  cleared.
- The child inherits only 0/1/2 (the `deci_std*` devices, so stderr logging is
  forwarded to the launcher) plus the injected extras.
- The caller keeps ownership of `fds[]` and closes its copies after the call
  (which is synchronous: it returns after the broker's reply, so a
  descriptor the broker rdups from the UI is still open while it does);
  the child holds independent duplicates.
- The UI's environment (at least the `PS5BROWSER_*` variables) is copied
  into the child.

`ps5_terminate_process` = `kill(pid, SIGKILL)` + bounded `waitpid` reap,
done by the broker (`{"op":"terminate"}`); safe for an already-dead pid.

Kernel read/write for the loader comes from the payload arguments the SDK crt
parses (`<ps5/payload.h>`); the loader uses `kernel_*` from `<ps5/kernel.h>`.

## Rendering / compositing

Phase 1 uses the non-accelerated raster path (no EGL/GLES at runtime):

- WebKit paints into a backing store in the WebProcess; the UI process calls
  `WKPagePaint(page, buffer, size, rect)` into an ARGB32 buffer.
- `WKViewClient.setViewNeedsDisplay` marks dirty rects; the frame loop paints
  them.
- The SDL2 PS5 window surface is `SDL_PIXELFORMAT_ABGR8888`
  (`SDL_ps5video.c`), so the compositor swaps R/B when copying the ARGB32
  backbuffer into the window surface, then `SDL_UpdateWindowSurface` →
  `PS5_Tilemap_Blit` → `sceVideoOutSubmitFlip`.
- The content view is painted into a content-only cache at
  `ShellController::content_top()` (below the 96 px band unless the chrome is
  hidden), dirty rects only. The chrome view (a second `WKView` loading
  `chrome.html`, transparent background via `WKPageSetDrawsTransparentBackground`,
  patch 0011) has its own cache and is alpha-blended (premultiplied
  source-over) over the page into the window surface, only inside
  `ShellController::chrome_rects()` (band, OSK panel, pointer, whole window
  under a menu/notice) of the last few frames; see `Ps5App::composite_and_present`.

## Text input

Text entry goes through `shell::TextInputProvider`
(`src/shell/include/ps5browser/shell/text_input.h`). On the PS5 the provider
is `Ps5ImeTextInput` (`src/frontend/ps5/ps5_ime.*`), the console's own
keyboard dialog (`libSceImeDialog`); the shell's HTML OSK (`chrome.html`) is
the fallback. The GTK host has no provider and always uses the HTML OSK.

| Trigger | Request | On OK |
| --- | --- | --- |
| Triangle (page mode) | kind `url`, pre-filled with the tab URL, Enter label "Go" | `navigate()` (normalised, search fallback), as the OSK commit |
| Square (page mode) | kind from the focused field's `<input type>`, pre-filled with its value when known | the field's value is replaced (see below) |
| Cross click that focuses a text field (within 1 s) | same as Square | same as Square |

While the dialog is up the shell keeps its focus (it does not enter OSK
mode), keeps painting, and ignores gamepad buttons and sticks; Ps5App drops
SDL key/text events. Cancelling the dialog changes nothing. If the
dialog cannot open (`sceImeDialogInit` fails twice, once with the full and
once with SDL's plain parameters) or ends with an error, the same request
opens the HTML OSK with the same text.

Page fields: `core::kFocusReporterScript` posts
`{type:'focus_info', inputType, value}` right before each
`{type:'focus', editable:true}`. With a known value the dialog is pre-filled
with it and on OK the shell first selects the field's content
(`select()`, or `execCommand('selectAll')`) and then calls `insert_text()`, so
the result replaces the value (an empty result deletes it). Passwords,
`contenteditable` and values over 2048 characters are never sent (value
`null`): the dialog starts empty and the text is inserted at the caret, as
with the HTML OSK. The same replace semantics apply to the HTML OSK.

Selection: `PS5BROWSER_OSK=system|builtin` (environment, wins) or
`"osk": "system"|"builtin"` in `config.json`; default `system`.

`sceImeDialogParam` as filled by `Ps5SystemIme::open()` (layout from the
SDL2 PS5 port, 96 bytes, checked with `static_assert`; the console's
`wchar_t` is 16-bit, the buffers are `char16_t` UTF-16):

| Field | Value |
| --- | --- |
| `userId` | `sceUserServiceGetForegroundUser()`, else `sceUserServiceGetInitialUser()` (after `sceUserServiceInitialize(NULL)`, `0x80960003` = already initialised is fine) |
| `type` | URL 2 for `url`, MAIL 3 for `email`, NUMBER 4 for `number`, else DEFAULT 0 |
| `supportedLanguages` | 0 |
| `enterLabel` | GO 3 for `url`, SEARCH 2 for `search`, else DEFAULT 0 |
| `inputMethod` | DEFAULT 0 |
| `filter` | NULL |
| `option` | PASSWORD 0x04 for `password`, NO_AUTO_CAPITALIZATION 0x02 for `url`/`email`, else 0 (PS4 bit values, unverified on PS5) |
| `maxTextLength` | request length clamped to 1..2048 UTF-16 units |
| `inputTextBuffer` | 2049-unit buffer owned by `Ps5SystemIme`, pre-filled with the initial text, NUL-terminated |
| `posx`, `posy` | 960, 540 |
| `horizontalAlignment`, `verticalAlignment` | CENTER 1, CENTER 1 |
| `placeholder` | NULL |
| `title` | UTF-16 title (127 units max) |
| `reserved` | zero |

The retry after a failed `sceImeDialogInit` uses SDL's values: type, Enter
label and option 0, position 0,0, LEFT/TOP.

Interaction with SDL: the SDL2 PS5 driver implements the screen keyboard
with the same dialog. `SDL_StartTextInput()` would open it, so Ps5App sets
`SDL_HINT_ENABLE_SCREEN_KEYBOARD=0` first. SDL also checks the dialog
status inside every `SDL_PollEvent()` and, when it sees `FINISHED` before
we do, fetches the result, terminates the dialog and (on OK only) injects
a Return key. Ps5App therefore polls the dialog before `SDL_PollEvent()`;
if SDL still wins, the Return key marks the outcome as OK (the text is in
our buffer anyway) and is not forwarded to the page.

Log lines (`log.txt`, prefix `ime:`): `user id`, `open title=... kind=...
len=N max=N`, `init ok`, `status running/finished/none`, `status
cancelled`, `result "<text>"` (`<N bytes, hidden>` for passwords),
`error 0x...` with the failing call, `falling back to the built-in OSK`.

## JavaScript JIT and WebAssembly

JavaScriptCore is built with the baseline and DFG JITs and with WebAssembly
(`scripts/ps5/build-webkit.sh`: `ENABLE_JIT=ON`, `ENABLE_DFG_JIT=ON`,
`ENABLE_FTL_JIT=OFF`, `ENABLE_WEBASSEMBLY=ON`). Without the FTL there is no
B3, so WebAssembly has only the IPInt interpreter: `ENABLE_WEBASSEMBLY_BBQJIT`
and `ENABLE_WEBASSEMBLY_OMGJIT` depend on `ENABLE_FTL_JIT`
(`Source/cmake/WebKitFeatures.cmake`), and `useWasmFastMemory` is forced off
on PlayStation (`Source/JavaScriptCore/runtime/Options.cpp:1044`).

Both are **off at run time by default**: `setup_runtime_env()`
(`src/frontend/ps5/ps5_runtime_env.cpp`, run by `main()` for every role)
sets `JSC_useJIT=false` and `JSC_useWasm=false` unless already set, so the
browser behaves like the LLInt-only build until a test turns them on.
JavaScriptCore reads every `JSC_<option>` with `getenv()` when it
initialises its options (`Options.cpp:1120` → `overrideOptionWithHeuristic`,
`Options.cpp:400`). With `useJIT=false`:

- no executable memory is reserved: `enableAssembler()` returns before
  `ExecutableAllocator::initializeUnderlyingAllocator()`
  (`runtime/VM.cpp:210`, `:217`), so the `FixedVMPoolExecutableAllocator`,
  whose constructor makes the JIT page reservation
  (`jit/ExecutableAllocator.cpp:458` → `initializeJITPageReservation()`), is
  never created; `ExecutableAllocator::initialize()` and
  `JITOperationList::initialize()` (`runtime/InitializeThreading.cpp:101-102`)
  only allocate bookkeeping objects;
- `canUseJIT` is false (`VM.cpp:249`, `InitializeThreading.cpp:103-105`) and
  `disableAllJITOptions()` turns off the baseline, DFG, regexp and DOM JITs
  and switches to polling traps (`Options.cpp:881`).

With `useWasm=false`, `disableAllWasmOptions()` runs (`Options.cpp:858`),
`Wasm::isSupported()` is false (`wasm/WasmCapabilities.h:39`), so no Wasm
thunks or signalling memory are set up (`InitializeThreading.cpp:136`,
`:147`) and the `WebAssembly` global is not exposed.

To test, add to `/data/ps5browser/config.json` (the values are exported in
every process; the UI's environment also reaches the children):

```json
{ "env": { "JSC_useJIT": "true" } }
```

or `{ "env": { "JSC_useJIT": "true", "JSC_useWasm": "true" } }` for
WebAssembly as well (`JSC_useWasm` alone, without the JIT, leaves only the
IPInt interpreter; JSC allows it, but it is untested here). Each
process logs `env JSC_useJIT=true (config.json)` at start. If JIT memory
cannot be reserved, JSC falls back to the interpreter by itself. If the JIT
crashes, the WebProcess dies: klog shows `A user thread receives a fatal
signal`, `signal: 4 (SIGILL)`, `10 (SIGBUS)` or `11 (SIGSEGV)`, `proc name:
WebProcess`, and a `rip` that is not inside our ELF's text (JIT code lives in
an anonymous mapping); `log.txt` shows `broker: child pid=N exited (...)`
and `spawn: child pid=N exited (...)`, and WebKit shows the page as crashed
and relaunches the WebProcess. Note that on the console the broker has
reported crashed children as `exit 0` (run 5), so klog is the place to look.
`JSC_usePollingTraps=true` rules out JSC's signal-based VM traps, which the
JIT enables, as the cause.

## Must be verified on a console

- `rfork_thread(RFPROC|RFCFDG|RFMEM)` + ptrace spawn works for **long-lived**
  WebKit children (elfldr has only run short-lived payloads this way).
- `pt_rdup`/`pt_dup2` land the IPC socket at child fd 3 and the socket survives
  into `AuxiliaryProcessMain` with blocking mode intact.
- The environment copy reaches the child and WebKit reads `PS5BROWSER_*`.
- The SDL window surface really is the `sceVideoOut` scanout buffer at
  1920×1080 ABGR8888, and the R/B swap produces correct colours (not swapped).
- `WKPagePaint` output alpha/premultiplication composites correctly against the
  chrome overlay.
- DualSense mapping via `SDL_GameController` (buttons, analog triggers → L2/R2,
  sticks) and USB-keyboard key codes.
- The system keyboard dialog: `sceImeDialogInit` accepts the parameters
  above from a homebrew big app, shows the pre-filled text, and the status
  sequence is running → finished; the option bits and the URL / mail /
  number types are the PS4 values.
- The HTML5 Fullscreen API path (`WKViewClient.enterFullScreen/exitFullScreen`)
  and `WKViewRequestExitFullScreen`.
- The WebKit run loop: the public PlayStation API exposes only
  `WKRunLoopRunMain`/`WKRunLoopCallOnMainThread` (no "run once"), so the frame
  loop is a self-re-arming `WKRunLoopCallOnMainThread` callback that paces
  itself with `SDL_Delay`. Confirm this services IPC promptly and does not spin
  the CPU.

## Known risks

- **Spawn memory**: the broker maps the whole ELF image (162 MB, read-only),
  and the loader mallocs a mirror of the mapped image (~130 MB) per spawn
  while copying it into the child. websrv also reads the whole ELF into
  memory once to start the broker.
- **Broker launch**: depends on websrv still running on `127.0.0.1:8080`
  (verified, run 3) and on the broker being reachable: AF_UNIX bind under
  `/data` is refused (`EOPNOTSUPP`, run 3), so on the console it is expected
  to run over loopback TCP.
- **Cross-process rdup**: over TCP the loader rdups the IPC socket from the
  UI (a big app) into a child of the broker (a plain payload). The syscall
  runs in the child while its credentials are raised (root, all caps, no
  jail), but the kernel may still refuse a source process in another
  jail/sandbox; the log then shows `spawn: rdup(src_pid=..., fd=...) FAILED:
  errno N` and the spawn fails at stage `rdup`. Not yet verified.
- **Broker port**: `127.0.0.1:17771` accepts any local process, which can
  then ask the broker to spawn an ELF or to copy a descriptor out of any
  pid. Loopback only, same trust level as websrv's own HTTP API.
- **Audio port**: `127.0.0.1:17772` accepts PCM from any local process,
  which can then play sound through the browser. Loopback only.
- **Relocations**: the loader applies `R_X86_64_RELATIVE` only (~311k).
  The ELF also has 334 `GLOB_DAT`, 27 `R_X86_64_64` and one `JUMP_SLOT`
  relocation, left to the SDK crt at startup, the same as for every
  payload elfldr loads; a WebKit-sized binary has not been loaded this way
  before.

- **Memory**: three WebKit processes plus Skia backing stores at 1920×1080 on a
  fixed homebrew budget (`sys_budget_set(0)`); OOM is plausible. Consider
  `WKContextSetUsesSingleWebProcess` / limiting tabs.
- **Process limits**: the number of live WebProcesses the homebrew environment
  tolerates is unknown.
- **ptrace availability**: the spawner needs ptrace and the elevated authid
  trick (`pt.c`); under etaHEN/elfldr-hosted environments ptrace may already be
  in use or restricted.
- **No GPU/AC path**: CSS 3D, WebGL and hardware video are out of scope for
  phase 1; EGL/GLES are linked as stubs only.
- **`insert_text`** falls back to `document.execCommand('insertText', …)`; it is
  not a trusted editing command and may not work in every editable context.
- **`text_input_focused`** comes from the injected focus script, not from the
  engine (the public PlayStation C API has no editor-state callback), so it
  misses focus changes inside closed shadow roots and cross-origin frames
  without the user script.
- **System keyboard dialog**: whether the dialog takes the DualSense away from
  the app while it is up is unverified; the shell ignores pad input during a
  request anyway. Only one dialog can exist per process: a stale one (e.g.
  from SDL) is terminated before ours opens.
