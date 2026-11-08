# PS5 runtime layout and process model

How the `prosperoweb` payload is laid out and runs on a jailbroken PS5. This is
the phase-1 (software raster) design. It runs on a real console (see
`console-verification.md`); what the console runs have confirmed and what is
still open is listed under "Verified on a console", "Still to verify on a
console" and "Known risks".

## On-disk layout

The browser ships as a single statically-linked ELF plus its data directory,
installed under `/data/prosperoweb/`:

```
/data/prosperoweb/
  prosperoweb.elf       the payload (UI + WebProcess + NetworkProcess, one ELF)
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

### Upgrading from ProsperoBrowser or ps5browser

Earlier releases installed to `/data/prosperobrowser/` (ProsperoBrowser)
and, before that, `/data/ps5browser/` (ps5browser). On the first start the
UI picks the newest of these that holds a profile (`/data/prosperobrowser/`
first; a directory with none of the items below is skipped) and copies
(never moves) `tabs.json`, `history.json`, `bookmarks.json`, `config.json`,
`storage/`, `localstorage/`, `indexeddb/` and `cache/` from it into
`/data/prosperoweb/`, each only if it is missing there, and logs
`migrate: copied <n> items from <dir>`. Items are not mixed from two old
directories. In the copied `config.json`, paths and `file://` URLs under
either old directory (such as `chrome_dir` from an old sample) are pointed
at `/data/prosperoweb`. It then writes
`/data/prosperoweb/.migrated-legacy-profile` and never copies again;
delete that file to repeat the copy. A failed item is logged
(`migrate: failed: ...`) and no marker is written, so the next start
retries. The old directories are left as they were and can be deleted by
hand once the new install works, together with their websrv tiles
(`/data/homebrew/prosperobrowser/`, `/data/homebrew/ps5browser/`).

For one release `PROSPEROBROWSER_*` and `PS5BROWSER_*` environment
variables (and `config.json` `"env"` keys) are still accepted: each one is
used as its `PROSPEROWEB_*` counterpart unless that is set too; when both
old names are set, the `PROSPEROBROWSER_*` one wins. `log.txt` gets one
`env: deprecated variable names used (...)` line per launch (or one
`env <NAME>: deprecated name in config.json` line per key; a key whose
newer name is also in the file is logged as ignored).

`config.json` keys (all optional; defaults in `src/frontend/ps5/ps5_config.h`):

```json
{
  "home_url":   "https://duckduckgo.com/",
  "user_agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Safari/605.1.15",
  "language":   "en",
  "chrome_dir": "/data/prosperoweb/chrome",
  "width":      1920,
  "height":     1080
}
```

`language` picks the UI language and the language WebKit reports to pages
(`Accept-Language`, `navigator.language`): `"en"`, `"es"` or `"ru"` (a tag such
as `"es-MX"` also selects Spanish). The UI strings live in one table,
`src/core/i18n.cpp`; a string with no translation shows its English text.

`scripts/ps5/package.sh` assembles this directory as `dist/prosperoweb/`,
plus `dist/homebrew/prosperoweb/` (`homebrew.js` + `sce_sys/icon0.png`) for
`/data/homebrew/prosperoweb/`: websrv lists every `/data/homebrew/<name>/`
with a `homebrew.js` (preferred) or `eboot.elf` and uses
`sce_sys/icon0.png` as the tile. The `homebrew.js` launches
`/data/prosperoweb/prosperoweb.elf` in place (`cwd` `/data/prosperoweb`), so
the large ELF is not duplicated. websrv needs no `param.json` for homebrew
entries. Without websrv, elfldr loads it directly:
`echo file:/data/prosperoweb/prosperoweb.elf | nc -q0 PS5 9021` (elfldr
reads a raw ELF, a URI line or an HTTP GET `?uri=`, not a POST body).

Environment set in `main()` before the role dispatch, so every process role
gets it (`src/frontend/ps5/ps5_runtime_env.cpp`, existing values win; the UI
then overrides `SYSTEM_LANGUAGE` from `config.json`):

| Variable | Value | Read by |
|---|---|---|
| `FONTCONFIG_FILE` | `/data/prosperoweb/fonts/fonts.conf` | fontconfig (WebProcess) |
| `FONTCONFIG_PATH` | `/data/prosperoweb/fonts` | fontconfig |
| `PROSPEROWEB_CA_BUNDLE` | `/data/prosperoweb/cacert.pem` | curl SSL handle (NetworkProcess, patch 0004) |
| `SYSTEM_LANGUAGE` | `en-US` / `es-ES` / `ru-RU` from `config.json` `language` | WTF `platformUserPreferredLanguages` (patch 0006) |
| `TMPDIR` | `/data/prosperoweb/tmp` (created at startup) | temporary files |
| `HOME` | `/data/prosperoweb` | home-relative defaults |

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

The cache directory `<cachedir>` is `/data/prosperoweb/cache/fontconfig`;
fontconfig creates it (and its parents) on the first start, which scans
the fonts once (the CJK collections are the slow part). `fc-cache` is not
needed on the console.

`scripts/ps5/fonts/check-fonts.sh` checks the staged `fonts.conf` on the
host: it rewrites `<dir>`/`<cachedir>` to the staged files and a fresh
cache directory, runs `fc-match` for each mapped family, the language
fallbacks and an unknown family, and fails if a match leaves the bundled
fonts or the cache directory is not created.

Sizes of the current build (Release, `--gc-sections`, ffmpeg engine on):
`prosperoweb.elf` 183 MB as linked, **176 MB shipped** (`llvm-strip
--strip-debug`, symbol table kept for backtraces), 140 MB with
`--strip-all` (not shipped: elfldr and the spawner only need program
headers, section headers and `SHT_RELA`, so `--strip-all` should also load;
untested). About 87 MB `.text`, 19 MB `.rodata`, 22 MB `.bss`, no `PT_TLS`.
`NEEDED`: `libkernel_web`, `libSceLibcInternal`, `libSceNet` plus, through
SDL2, `libSceSystemService`, `libSceUserService`, `libScePad`,
`libSceAudioOut`, `libSceVideoOut`, `libSceKeyboard`, `libSceImeDialog`
(all `.sprx` system modules). The whole package is about 236 MB, 68 MB of
it fonts.

The ELF path used when re-spawning the auxiliary processes is resolved by
`ps5_spawn_resolve_self_path()`: `PROSPEROWEB_SELF_PATH` env →
`/data/prosperoweb/prosperoweb.elf`. The UI's `main()` exports
`PROSPEROWEB_SELF_PATH` from its own `argv[0]` when that is an absolute
readable `*.elf` path (an explicit value wins).

## Process model

The payload is one multi-call ELF. `main()`
(`src/frontend/ps5/main.cpp`) dispatches on the role name:

- `argv[0]` basename `WebProcess` → `ps5_webkit_web_process_main()`
- `argv[0]` basename `NetworkProcess` → `ps5_webkit_network_process_main()`
- `argv[0]` basename `spawn-broker` → `ps5_spawn_broker_main()` (`src/ps5/spawn/`)
- otherwise → the UI process (`Ps5App`)

A `--role=<name>` argument and then `PROSPEROWEB_ROLE` are honoured as
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
   |  tile click: GET /hbldr?path=...prosperoweb.elf          (hbldr, big app)
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
     |  loopback TCP 127.0.0.1:$PROSPEROWEB_AUDIO_PORT (17772), one connection per media element
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
- `PROSPEROWEB_AUDIO_TEST=1` plays a 1 s 440 Hz tone through the server at UI
  startup, which checks UI-process audio without any page.

Expected log lines:

```
[UI] audio server: device opened: SDL driver ps5, device 2, 48000 Hz format 0x8120 2 ch 1024 samples
[UI] audio server: listening on tcp 127.0.0.1:17772
[UI] audio server: test tone played                                   (PROSPEROWEB_AUDIO_TEST=1)
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
   AF_UNIX `$PROSPEROWEB_SPAWN_SOCK` (if set), `/tmp/prosperoweb-spawn.sock`,
   `/data/prosperoweb/tmp/spawn.sock`, then TCP
   `127.0.0.1:$PROSPEROWEB_SPAWN_PORT` (default `17771`).
3. Nobody listens: the UI sends websrv a plain HTTP/1.0 request
   (`PROSPEROWEB_WEBSRV`, default `127.0.0.1:8080`):

   ```
   GET /hbldr?path=/data/prosperoweb/prosperoweb.elf&cwd=/data/prosperoweb&daemon=1&args=spawn-broker%20--role%3Dspawn-broker&env=PROSPEROWEB_ROLE%3Dspawn-broker%20PROSPEROWEB_SPAWN_PORT%3D17771%20PROSPEROWEB_SELF_PATH%3D/data/prosperoweb/prosperoweb.elf HTTP/1.0
   ```

   (`PROSPEROWEB_SPAWN_SOCK` is forwarded only when the UI has it set.)

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
   `environ` (so `FONTCONFIG_FILE`, `PROSPEROWEB_CA_BUNDLE`, … reach the
   WebProcess whatever the broker's own environment is), and the
   descriptors at child fd 3, 4, …: over AF_UNIX the ones it received as
   `SCM_RIGHTS`; over TCP the UI names them (`src_pid`, `src_fds`) and the
   loader copies them out of the UI process with `rdup`. It replies with
   the pid, and reports children that exit on their own back to the UI.

If the broker cannot be reached or started, the spawn fails (WebKit then
shows no page) unless `PROSPEROWEB_SPAWN_FALLBACK=direct` is set, which spawns
in-process — and panics the console when the UI is the big app. Without the
fallback, after two failures in a row the UI waits 30 s before the next
attempt (logged as `backing off`), so a broken broker does not make WebKit's
endless relaunches hammer websrv.

### Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `PROSPEROWEB_SPAWN_MODE` | `broker` | `direct` spawns in-process (old behaviour); only for plain-payload (elfldr) launches |
| `PROSPEROWEB_SPAWN_FALLBACK` | unset | `direct`: spawn in-process when no broker can be reached |
| `PROSPEROWEB_SPAWN_SOCK` | unset | extra AF_UNIX broker socket tried first (then `/tmp/prosperoweb-spawn.sock`, `/data/prosperoweb/tmp/spawn.sock`) |
| `PROSPEROWEB_SPAWN_PORT` | `17771` | loopback TCP port of the broker (UI and broker) |
| `PROSPEROWEB_WEBSRV` | `127.0.0.1:8080` | websrv address (IPv4 literal) used to start the broker |
| `PROSPEROWEB_ELFLDR` | `127.0.0.1:9021` | elfldr address (IPv4 literal) used to start the broker |
| `PROSPEROWEB_ETAHEN_ASK` | `1` | `0` disables asking etaHEN to launch its bundled elfldr when 9021 is refused |
| `PROSPEROWEB_ETAHEN_UTIL_SOCK` | `/system_tmp/etaHEN_util_service` | etaHEN utility-daemon command socket (AF_UNIX) used for the launch-elfldr request |
| `PROSPEROWEB_BROKER_LOADER` | `auto` | how the UI starts the broker: `elfldr`, `websrv`, or `auto` (both; elfldr first in the native title, websrv first under the websrv tile) |
| `PROSPEROWEB_SELF_PATH` | `/data/prosperoweb/prosperoweb.elf`; native title: `<install folder>/prosperoweb.elf` | the ELF the broker runs as itself and spawns for the children |
| `PROSPEROWEB_TITLE_DIR` | found | native title: the install folder as the broker and children see it, instead of searching the ShadowMountPlus scan paths ([native-title.md](native-title.md)) |
| `PROSPEROWEB_ROLE` | — | role fallback (`spawn-broker` for the broker) |
| `JSC_useJIT` | `true` | JavaScriptCore baseline/DFG JIT (see [JavaScript JIT and WebAssembly](#javascript-jit-and-webassembly)) |
| `JSC_useWasm` | `true` | WebAssembly (IPInt interpreter, plus the JIT's entry thunks when `JSC_useJIT` is on) |
| `PROSPEROWEB_MEDIA_AUDIO` | unset | first media audio sink tried: `remote` (default when `PROSPEROWEB_AUDIO_PORT` is set), `sdl` (output in the WebProcess, inaudible on the console) or `null` (silent) |
| `PROSPEROWEB_AUDIO_PORT` | `17772` | loopback TCP port of the UI's audio server (UI and WebProcess); `off` disables the server and the forwarding |
| `PROSPEROWEB_AUDIO_AHEAD_MS` | `100` | audio the WebProcess keeps queued in the UI's server (20-900 ms): lower is less latency, higher survives longer stalls |
| `PROSPEROWEB_AUDIO_TEST` | unset | `1`: the UI plays a 1 s 440 Hz test tone at startup |
| `PROSPEROWEB_MEDIA_FRAME_THREADS` | unset | `1`: frame-threaded video decoding (default: slice threads only) |
| `PROSPEROWEB_PRESENT_THREAD` | unset | `0`: present on the main thread (blocks the run loop in the flip wait; for A/B comparisons, see [Performance](#performance)) |
| `PROSPEROWEB_VIDEO_PORT` | `17773` | loopback TCP port of the UI's video overlay server (UI and WebProcess); `off` disables the server and the overlay |
| `PROSPEROWEB_VIDEO_OVERLAY` | on (PlayStation) | `0`: media players paint video into the page instead of the overlay |
| `PROSPEROWEB_VIDEO_TRANSPORT` | auto | `shm`, `file` or `tcp`: force the overlay's pixel transport (auto tries them in that order) |
| `PROSPEROWEB_VIDEO_SHM_DIR` | `/data/prosperoweb/tmp` | directory of the overlay's `file` transport segments (unlinked once mapped) |
| `PROSPEROWEB_VIDEO_SCALE` | `bilinear` | `nearest`: cheaper overlay scaling in the UI |
| `PROSPEROWEB_VIDEO_DECODE` | `ui` | who decodes H.264/HEVC: `ui` (the UI's hardware decoder, falling back to the WebProcess) or `webprocess` (software only; the UI then does not load the decoder either); see [Hardware video decoding](#hardware-video-decoding) |
| `PROSPEROWEB_HWDEC` | native title: on; websrv payload: off | native title: `0` does not load `libSceVideodec2`; payload: `1` opts in to the background probe (off by default, see below) |
| `PROSPEROWEB_HWDEC_PROBE` | on demand | `startup`: probe the hardware decoder right after the UI started instead of on the first video stream (in the background either way, 3 s watchdog per call) |
| `PROSPEROWEB_HWDEC_AGC` | on | `0`: the probe does not load `libSceAgcDriver`/`libSceAgc` before the decoder |
| `PROSPEROWEB_HWDEC_HEVC` | on | `0`: the UI refuses HEVC streams (H.264 only) |
| `PROSPEROWEB_CPU_MONITOR` | on | `0`: no `cpu:` lines |
| `PROSPEROWEB_TRACE` | off | `1`: per-message diagnostics (WebKit IPC hop timings for `scripts/ps5/native/ipc-latency.py`, drawing-area updates, cookie decisions, the native title's syscall traces), every log line mirrored to klog and stderr, and in the native title the children's stderr and the pages' console messages kept in `stderr-<role>.txt`. Off, only start-up lines and failures go to klog and are synced. |
| `PROSPEROWEB_CPU_WIDEN` | on | `0`: never try to widen a restricted CPU affinity |

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
- The UI's environment (at least the `PROSPEROWEB_*` variables) is copied
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
  caches into the window surface (an AVX2 byte shuffle fused into the copy,
  `src/ps5/display/compose.cpp`), then `SDL_UpdateWindowSurfaceRects` →
  `PS5_Tilemap_Blit` → `sceVideoOutSubmitFlip` on the presenter thread
  (`ps5_presenter.h`).
- The content view is painted into a content-only cache at
  `ShellController::content_top()` (below the 96 px band unless the chrome is
  hidden), dirty rects only. The chrome view (a second `WKView` loading
  `chrome.html`, transparent background via `WKPageSetDrawsTransparentBackground`,
  patch 0011) has its own cache and is alpha-blended (premultiplied
  source-over) over the page into the window surface, only inside
  `ShellController::chrome_rects()` (band, OSK panel, whole window under a
  menu/notice) of the last few frames; see `Ps5App::composite_and_present`.
- The gamepad pointer is not part of the chrome on the console
  (`Config::native_pointer`): the compositor blends a ring sprite
  (`src/ps5/display/pointer.*`) over the composed frame and recomposes only
  its old and new rects when it moves, so pointer motion repaints neither
  WKView. The page gets at most one mouse move per frame, only when the
  pointer's integer position changed.
- The chrome renders incrementally (`chrome.js` patches the DOM): a state
  push that changes nothing visible repaints nothing, and running CSS
  animations (tab spinners) are not restarted.
- Damage flows end to end as a small coalesced region
  (`display::Region`, at most 8 rects): `setViewNeedsDisplay` rects →
  `WKPagePaint` per rect → compose those rects (plus chrome rects that
  appeared, moved or vanished, and chrome repaints inside chrome rects) →
  present those rects. An idle page composes and presents nothing.

## Performance

### Where a video frame's time goes (non-accelerated path)

```
WebProcess main thread                          UI main thread                         presenter thread
------------------------------------------      -----------------------------------    -------------------------------
FFmpeg tick (8 ms) picks the frame, repaint()
DrawingArea display: Skia rasters the dirty
  rect (the video scaled into the element)
  into a ShareableBitmap
Messages::DrawingAreaProxy::Update  ------->    BackingStore::incorporateUpdate (copy)
                                    <-------    DisplayDidRefresh (sent at once)
                                                setViewNeedsDisplay -> request_frame()
                                                frame(): WKPagePaint rect -> page cache
                                                compose rect -> window surface (swap,
                                                  chrome blend)                -------> SDL_UpdateWindowSurfaceRects:
                                                                                        de-tile bounding box, flip,
                                                                                        wait for the flip (<= 16.7 ms)
                                                frame() requested again  <--------------
```

The media stats line (`media: stats: ... dropped (+N presenter)`) counts
frames the WebProcess's FFmpeg tick skipped because the next one was already
due: its main thread ran late, i.e. painting in the WebProcess (or anything
else on that thread) took longer than a frame. Decoder drops are 0 on the
console, so decoding keeps up; the presenter drops show the paint path is the
limit.

### The perf line (log.txt, UI)

Every 5 s while anything is painted or presented, one line:

```
perf 5.0s: ui 61.2 fps | ndisp page 24.0/s 230kpx chrome 1.0/s 3kpx | paint page 24.0/s 0.41/1.20ms 230kpx chrome 1.0/s 0.05/0.05ms 3kpx | compose 24.0/s 0.20/0.50ms 233kpx | present 24.0/s 9.10/16.80ms 1.0 rects 233kpx waits 0
```

- `ui` — frame-loop iterations per second (paced by flips, the 16 ms tick
  and `setViewNeedsDisplay`).
- `ndisp page|chrome` — `setViewNeedsDisplay` calls per second and their
  average area (kpx = 1000 pixels). For a playing video this is the
  WebProcess update rate the UI sees: below the video frame rate means the
  WebProcess paints slower than the video plays.
- `paint page|chrome` — `WKPagePaint` calls per second, avg/max ms, average
  area (copies from the UI backing store into our caches).
- `compose` — rects composed into the window surface per second, avg/max ms,
  average area.
- `present` — `SDL_UpdateWindowSurfaceRects` per second, avg/max ms
  **including the wait for the flip** (so up to ~16.7 ms is normal, it runs
  on the presenter thread), rects per present, average area.
- `waits` — frames whose damage had to wait because the previous flip was
  still in flight (it is composed right after the flip).
- `video` — composes that showed a new video overlay frame per second and
  their visible video area: the video frame rate reaching the screen.

Reading it: `ndisp page` ≈ video fps and `present` ≈ the same → the UI keeps
up. `ndisp page` well below the video fps with low `paint`/`compose` times →
the WebProcess is the bottleneck (see the video overlay below). With the
overlay, `ndisp page` stays low during playback and `video` ≈ the video fps.

### SDL present path facts

- `SDL_ps5video.c` `PS5_CreateWindowFramebuffer` (l. 41-64) always creates an
  ABGR8888 surface; the video-out buffers are registered with a fixed
  attribute (`sceVideoOutSetBufferAttribute2(..., 0x8000000022000000, ...)`,
  l. 160 / 236). There is no ARGB variant without patching SDL, and
  `WKPagePaint` always writes N32 premultiplied (BGRA bytes,
  `WKPagePrivatePlayStation.cpp:288`), hence the swap in the compose copy.
- `PS5_UpdateWindowFramebuffer` (l. 67-109) honours the rects:
  `PS5_Tilemap_Blit` (`SDL_ps5tilemap.c` l. 195-246) de-tiles only the
  bounding box of the rects, unioned with the damage the other of the two
  scanout buffers missed since its last flip (aligned to 4 px x 8 rows).
  `SDL_UpdateWindowSurface` passes the full window, i.e. a full 1920x1080
  de-tile (8 MB) per frame.
- It then calls `sceVideoOutSubmitFlip` and blocks in `sceKernelWaitEqueue`
  until the flip event (l. 101-106), flip rate 0 = 60 Hz: up to one vblank
  per present. Before the presenter thread this wait ran on the main
  thread, i.e. inside WebKit's run loop.

### Video overlay (hole punch)

The per-frame path above (Skia scaling the frame into the backing store,
WebKit IPC, two more copies in the UI) capped YouTube 720p at about five
page updates per second on the console (`ndisp page 4.8/s 1889kpx`). In
overlay mode a frame never goes through WebKit painting:

```
WebProcess main thread (player tick)        overlay sender thread            UI overlay server thread         UI main thread
-------------------------------------       -----------------------------    ------------------------------   ----------------------------
geometry: video box in view coords   --->   "geometry" (on change)   --->    rect, page, visible
A/V clock picks the frame, submit()  --->   copy into shm slot k
  (no repaint; the element painted a        (seqlock), "present k"   --->    copy slot k out, scale to the
   transparent hole once)                                                    rect (bilinear), swap in,
                                                                             damage the rect          --->   request_frame(): compose
                                                                                                             video -> page -> chrome,
                                                                                                             present the rect
```

- **WebProcess** (`webkit-port/media/ffmpeg/FFmpegVideoOverlay.*`, both
  players): the tick reads the element's video box
  (`RenderVideo::videoBox()` mapped through `localToAbsoluteQuad` and
  `contentsToMainFrameView`: layout, scrolling, frames, fullscreen) and the
  page identifier. The hole is punched (`paint()` does `clearRect`) only
  while the overlay is connected, the box is rectilinear, page and element
  are visible and a frame exists; otherwise the frame is painted as before.
  Canvas `drawImage()` (`paintCurrentFrameInContext`) always gets pixels.
  The A/V decision stays in the WebProcess: only the frame it presents is
  sent, a newer one replaces one still queued.
- **Transport** (`FFmpegVideoOverlayWire.h`, `FFmpegVideoOverlayClient.h`):
  a loopback TCP control connection per player to `PROSPEROWEB_VIDEO_PORT`
  (17773). On the first frame the client offers, in order, a named POSIX
  shm object (`/prosperoweb-video-<pid>-<player>-<n>`, three BGRA slots), a
  file under `PROSPEROWEB_VIDEO_SHM_DIR` (`/data/prosperoweb/tmp`) mapped
  `MAP_SHARED|MAP_NOSYNC`, then pixels inline over TCP; the first one the
  UI maps wins, and the name is unlinked as soon as both sides mapped it.
  Each slot has a sequence counter (seqlock); the UI drops a copy that a
  rewrite tore (`torn` in the stats).
- **UI** (`src/ps5/video`, `src/frontend/ps5/ps5_video_overlay.*`): the
  server keeps per player the newest frame scaled to its rect
  (`src/ps5/display/scale.*`, bilinear; `PROSPEROWEB_VIDEO_SCALE=nearest`)
  and the damage. The frame loop takes the active view's identity
  (`WKPageGetProcessIdentifier` + `WKPageGetWebPageIdentifierPlayStation`,
  patch 0011), maps rects by `content_top()` and clips them to the content
  area, and composes video under the page (`compose_rect`): pixels the page
  left transparent show the video, captions and controls blend over it,
  the chrome over everything. Before `WKPagePaint` the dirty rects of the
  page cache are cleared, since WebKit draws source-over and a hole would
  otherwise keep old pixels. Only players of the active page are drawn
  (hidden tabs report `hidden`).

Expected log lines:

```
[UI] video overlay: listening on tcp 127.0.0.1:17773 (bilinear scaling)
[WebProcess] media: video overlay: player 1: on (pid N, port 17773, transport auto: shm, file, tcp)
[WebProcess] media: video overlay: player 1: connected to 127.0.0.1:17773 (server key 1)
[UI] video overlay: player key 1 connected (pid N player 1)
[WebProcess] media: video overlay: player 1: rect 320,152 1280x720 page 5
[UI] video overlay: player key 1: shm /prosperoweb-video-N-1-1 mapped
[WebProcess] media: video overlay: player 1: transport shm (/prosperoweb-video-N-1-1, 3 x 3686400 bytes)
[WebProcess] media: video overlay: player 1: hole punch on
[WebProcess] media: video overlay: player 1: shm sent 29.9/s replaced 0 copy 0.60 ms | hole on rect 320,152 1280x720
[UI] video overlay 5.0s: frames in 29.9/s rendered 29.9/s torn 0 | scale 1280x720 -> 1280x720 1.10/2.40 ms | tcp 0.0 MB/s | players 1
[UI] perf 5.0s: ... | video 29.8/s 922kpx
```

Fallbacks: `shm ... refused: <errno> (the client falls back)` then
`transport file`, or `transport tcp (pixels inline ...)`; no server at all:
`no overlay server, painting video into the page`. `PROSPEROWEB_VIDEO_OVERLAY=0`
turns the overlay off in the WebProcess, `PROSPEROWEB_VIDEO_PORT=off` in both
processes, `PROSPEROWEB_VIDEO_TRANSPORT=shm|file|tcp` forces a transport.

Limits: the hole moves with the page repaint, the video follows at the
next geometry tick (8 ms while playing, 33 ms paused), so fast scrolling
can show a black edge for a frame; rotated or skewed video boxes are
painted into the page as before; CSS opacity and filters on the video do
not apply to the overlay (the hole is transparent whatever the opacity);
page snapshots show the hole.

### Hardware video decoding

Implemented, **not yet run on a console**. The console's video decoder
(`libSceVideodec2`) needs the big app, so the UI process decodes and the
WebProcess keeps demuxing, MSE and the A/V clock; details, the API as far as
open-source PS5 homebrew documents it, and what is uncertain are in
[webkit-port/media/README.md](../webkit-port/media/README.md#hardware-decoding-in-the-ui-process).

```
WebProcess decoder thread        overlay connection        UI HwStream threads                   UI main thread
-------------------------        ------------------        ----------------------------------    --------------
stream-open, packets       --->  (credit-bounded)   --->   decode: Annex-B, sceVideodec2Decode
pixel-less frames queued   <---  decoded {pts}      <---   picture slots (NV12, direct memory)
presenter: show {pts}      --->                     --->   present: NV12 -> ARGB (AVX2), scale,
                                                           swap into the player's frame     --->  compose under the page
```

- At startup `main()` calls `hwdec_prepare()` before anything else of the
  UI: `sceSysmoduleLoadModule(207)`, `dlopen("libSceVideodec2.sprx")`, one
  compute queue for the process, and the decoder memory for 1080p/4K H.264
  and HEVC (other homebrew saw these refused after the process got full
  access; hbldr may already give it, which only a console run will show).
- Each accepted stream has a decode thread and a present thread in the UI;
  the overlay server's poll thread only queues. At most two streams decode at
  once. Everything refused or failed decodes in the WebProcess as before.

Expected log lines (UI, then WebProcess):

```
[UI] hwdec: libSceVideodec2 loaded (sysmodule 207: 0), functions resolved
[UI] hwdec: compute queue ready (0, N KiB)
[UI] video overlay: remote decoding on (sceVideodec2: h264, hevc)
[WebProcess] media: video decoder h264: 1920x1080, asking the UI to decode it (stream 1)
[UI] hwdec: open h264 1920x1080 -> hw ok (decoder mem N MiB: ...)
[WebProcess] media: video decoder: stream 1 decoded by the UI (hardware, 16 frames held there)
[UI] hwdec: stats 5.0s: decoded 30.0 fps (1920x1080, decode call N ms), convert N/N ms -> 1920x1080, queue N, held N, in decoder N, presented N, shows N, late N, dropped N, evicted N
```

Fallbacks: `hwdec: unavailable: ...` or `hwdec: compute queue FAILED (0x...)`
at startup (then `video overlay: remote decoding off`), `hwdec: player key N:
reject vp9 ... (unsupported)` per stream, `hwdec: stream N ...: <why>; the
WebProcess decodes in software` at run time.
`PROSPEROWEB_VIDEO_DECODE=webprocess` restores software decoding.

Console status (runs 13 and 14): in the UI started by hbldr the first
decoder call, `sceVideodec2QueryComputeMemoryInfo`, stops the whole process
(`mDBG: Sending signal(... signo: 0xa0020101)` in klog), with
`sceSysmoduleLoadModule(207)` returning 0 (run 13) or `0x80020064` (run 14).
The payload build therefore keeps hardware decoding off unless
`PROSPEROWEB_HWDEC=1`; the native title (docs/native-title.md) creates the
decoder's library and compute queue as the first thing in `main()`, inside
its own sandbox and with the application credentials, as the references do. It is not a
linking problem: the payload SDK loads a `DT_NEEDED` `.sprx` and a `dlopen`ed
one through the same code (`crt/rtld_payload.c` `dt_needed()` and
`crt/rtld_dlfcn.c` `__dlopen()` both call `__rtld_lib_new()` and
`__rtld_lib_open()`, which end in `sceKernelLoadStartModule`), and `dlsym`
and import relocation both return the module's own symbol address
(`crt/rtld_sprx.c` `sprx_sym2addr()`), so a link stub as the native titles
use would change nothing, while a missing `NEEDED` module is fatal to the
whole multi-call ELF. The difference from the references is the process:
they are signed titles that make the decoder's library and compute queue
inside their sandbox, and VLC-PS5 saw both refused once its title had full
access (`0x80020063`, `0x811D0111`, `src/platform_ps5.c`); hbldr gives this
process uid 0, every capability and no jail before `main()` runs. The probe
logs those credentials (`hwdec: process uid ...`) before its first call.

Limits: no pixels in the WebProcess for hardware-decoded video, so canvas
`drawImage(video)`, snapshots and a video box the overlay cannot show (rotated,
skewed) stay empty; 8-bit 4:2:0 H.264 and HEVC Main only.

### CPU allocation (WebProcess and UI)

`src/frontend/ps5/ps5_cpu_monitor.cpp` logs at start and every 30 s:

```
[WebProcess] cpu: 16 cpus online, root set 0-12 (13), process may run on 0-12 (13)
[WebProcess] cpu: affinity 0-12 (13 of 13) | threads 31: 0-12 x31 | video decoders 1, 6 codec threads | user 41.2s sys 3.0s over 30s = 1.47 cpus busy | top FFmpegVideoDecoder 12.1s@cpu4, ...
```

If the process may run on fewer CPUs than the root set it tries once to
widen its affinity (`cpuset_setaffinity`, CPU_LEVEL_WHICH; the SDK has no
`sceKernelSetCpumask`): `cpu: widened ...` or `cpu: widening ... failed`.
`PROSPEROWEB_CPU_WIDEN=0` skips that, `PROSPEROWEB_CPU_MONITOR=0` disables
the monitor. A per-thread list needs the kernel's `kinfo_proc` to match the
SDK's; otherwise the line says `threads 0: unavailable (kinfo_proc size ...)`.

## Touchpad

The DualSense touchpad is a trackpad (`core::TouchpadModel`): one finger
moves the pointer relatively with acceleration, a quick still tap clicks
(under 180 ms and 2% of the pad width), two fingers scroll the page with the
content following the fingers, and the pad's button is the mouse button,
held while pressed (press and slide drags). SDL's PS5 joystick driver does
not report the touchpad (`SDL_ps5joystick.c` sets no touchpads and never
calls `SDL_PrivateJoystickTouchpad`), so `Ps5Touchpad`
(`src/frontend/ps5/ps5_touchpad.*`) reads the fingers with
`scePadReadState()` on the pad handle SDL opened (`scePadGetHandle()`),
in the `PS5_PadData` layout SDL passes to the same call. The resolution
comes from `scePadGetControllerInformation()` when it returns a sane value,
else 1920x1080, and grows if a finger reports a larger coordinate.
`log.txt` says which source is used once: `touchpad: source scePad,
resolution WxH (controller info|default)`, or `source sdl` should SDL ever
report a touchpad.

## Pointer visibility

The UI draws the pointer ring itself, over the composed frame. It is hidden
when the active page asks for `cursor: none`: the PlayStation port reports
the cursor through `WKViewClient.setCursor` (`kWKCursorTypePointer`, `Hand`
or `None`; every other CSS cursor arrives as `Pointer`), and the shell
keeps the last one per tab (`ShellController::page_cursor()`). As in a
desktop browser the page decides; moving the stick or the touchpad sends it
mouse moves, and pages that hide the cursor while idle (video players) show
it again on such a move. `log.txt` notes the changes: `view: page cursor
none (pointer hidden)` and `view: page cursor shown`.

In element fullscreen (the page called `requestFullscreen()`) the pointer
also hides after 3 s without pointer input (stick, touchpad or any button)
and comes back with the next one (`kFullscreenPointerHideSeconds`).

## Text input

Text entry goes through `shell::TextInputProvider`
(`src/shell/include/prosperoweb/shell/text_input.h`). On the PS5 the provider
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

Selection: `PROSPEROWEB_OSK=system|builtin` (environment, wins) or
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

Both are **on by default**: `setup_runtime_env()`
(`src/frontend/ps5/ps5_runtime_env.cpp`, run by `main()` for every role)
sets `JSC_useJIT=true` and `JSC_useWasm=true` unless already set.
JavaScriptCore reads every `JSC_<option>` with `getenv()` when it
initialises its options (`Options.cpp:1120` → `overrideOptionWithHeuristic`,
`Options.cpp:400`).

Verified on a console: `src/chrome-ui/bench.html` (packaged as
`chrome/bench.html`; recursive calls, a tight loop, object allocation and
sorting, JSON round trips) took 645 ms in total with the JIT and
WebAssembly on, against 1107 ms with both off, about 1.7x faster; browsing
YouTube, rezka and Google with the JIT on showed no crashes. The page also
reports whether `WebAssembly` is exposed.

To fall back to the interpreter, set them to `false` (see below). With
`useJIT=false`:

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

To turn them off, add to `/data/prosperoweb/config.json` (the values are
exported in every process; the UI's environment also reaches the children):

```json
{ "env": { "JSC_useJIT": "false", "JSC_useWasm": "false" } }
```

(`JSC_useJIT=false` with `JSC_useWasm=true` leaves only the IPInt
interpreter for WebAssembly; JSC allows it, but it is untested here). Each
process logs `env JSC_useJIT=false (config.json)` at start. If JIT memory
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

## Verified on a console

From the console runs on 2026-10-31 (`log.txt` and klog; the spawn log
reports `fw 0x13420000`):

- The broker, started through websrv, spawns long-lived WebProcess and
  NetworkProcess children over loopback TCP with the IPC socket rdup'ed from
  the UI (`spawn: role=WebProcess pid=N via broker (tcp, rdup)`), and they
  survive for whole browsing sessions.
- The environment reaches the children: the WebProcess reads the
  `PROSPEROWEB_*` media variables and logs its `media:` lines.
- Pages render on the TV through the SDL window surface; the chrome, the
  pointer and page input work with the DualSense.
- The system keyboard dialog opens from the big app, shows the pre-filled
  text and returns the result (`ime: status finished`, `ime: result ...`).
- Element fullscreen (`fullscreen: entered`, `fullscreen: exited`).
- Audio from the WebProcess through the UI's audio server
  (`audio server: stream N connected`), and YouTube playback through MSE
  with H.264.

## Still to verify on a console

- Hardware video decoding ([Hardware video decoding](#hardware-video-decoding)):
  that `libSceVideodec2` loads and its compute queue is created in the UI
  started by hbldr (run 13 stopped inside the first decoder calls; they now
  run in a watched background probe, the GPU libraries loaded first), the structure layouts, NV12 output and pts matching, the
  conversion cost, and the fallback to software.

- `WKPagePaint` output alpha/premultiplication composites correctly against
  the chrome overlay in every state (closed overlays leaving no ghosts).
- USB-keyboard key codes and text input into page fields.
- The option bits and the URL / mail / number types of the system keyboard
  dialog (PS4 values).
- The WebKit run loop: the public PlayStation API exposes only
  `WKRunLoopRunMain`/`WKRunLoopCallOnMainThread` (no "run once"), so the frame
  loop is a self-re-arming `WKRunLoopCallOnMainThread` callback that paces
  itself with `SDL_Delay`. Confirm this services IPC promptly and does not spin
  the CPU.

## Known risks

- **Spawn memory**: the broker maps the whole ELF image (176 MB, read-only),
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
  errno N` and the spawn fails at stage `rdup`. Works on the console used so
  far.
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
