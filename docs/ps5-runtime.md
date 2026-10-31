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
  fonts/                DejaVu TTFs + fonts.conf (FONTCONFIG_FILE)
  config.json           home url, user agent, language (see below);
                        shipped as config.json.sample
  LICENSES/             GPL-3.0 (the ELF), MPL-2.0 notice (cacert.pem), DejaVu
  README-install.txt
  tabs.json             persisted session (written by core::Session)
  history.json          persisted history
  bookmarks.json        persisted bookmarks
  cache/                network cache (WKWebsiteDataStore)
  storage/ localstorage/ indexeddb/   web storage (WKWebsiteDataStore)
  tmp/                  TMPDIR (created at startup)
  log.txt               UI-process log (also echoed to stderr)
```

`config.json` keys (all optional; defaults in `src/frontend/ps5/ps5_config.h`):

```json
{
  "home_url":   "https://duckduckgo.com/",
  "user_agent": "Mozilla/5.0 (PlayStation; PlayStation 5) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15 ps5browser/0.1",
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
the same values. `fonts.conf` maps the generic families (`sans-serif`,
`serif`, `monospace`, `system-ui`, ...) and common web fonts to DejaVu; no
CJK font is shipped (Noto Sans CJK is ~100 MB per weight).

Sizes of the current build (Release, `--gc-sections`, ffmpeg engine on):
`ps5browser.elf` 169 MB as linked, **162 MB shipped** (`llvm-strip
--strip-debug`, symbol table kept for backtraces), 129 MB with
`--strip-all` (not shipped: elfldr and the spawner only need program
headers, section headers and `SHT_RELA`, so `--strip-all` should also load;
untested). About 87 MB `.text`, 19 MB `.rodata`, 22 MB `.bss`, no `PT_TLS`.
`NEEDED`: `libkernel_web`, `libSceLibcInternal`, `libSceNet` plus, through
SDL2, `libSceSystemService`, `libSceUserService`, `libScePad`,
`libSceAudioOut`, `libSceVideoOut`, `libSceKeyboard`, `libSceImeDialog`
(all `.sprx` system modules). The whole package is about 175 MB.

The ELF path used when re-spawning the auxiliary processes is resolved by
`ps5_spawn_resolve_self_path()`: `PS5BROWSER_SELF_PATH` env →
`/data/ps5browser/ps5browser.elf` → an absolute `argv[0]`.

## Process model

The payload is one multi-call ELF. `main()`
(`src/frontend/ps5/main.cpp`) dispatches on the role name:

- `argv[0]` basename `WebProcess` → `ps5_webkit_web_process_main()`
- `argv[0]` basename `NetworkProcess` → `ps5_webkit_network_process_main()`
- otherwise → the UI process (`Ps5App`)

`PS5BROWSER_ROLE` is honoured as a fallback when `argv[0]` is not a role name.

The three roles:

- **UI** — our `main()`. Owns the SDL2 window and the WebKit UI process
  (`WKView`/`WKPage`), composites content + chrome, feeds gamepad/keyboard
  input. Started by the user (launchpad/etaHEN).
- **WebProcess** — WebKit web content process. One per web process (the number
  is bounded by WebKit; see risks). Started by the UI process via the spawner.
- **NetworkProcess** — WebKit networking process. Started the same way.

WebKit's `ProcessLauncherPlayStation.cpp` (patched for the homebrew port)
creates a `SOCK_SEQPACKET` socketpair, keeps the server end, and calls our
spawn contract.

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

`ps5_spawn_self`:

1. Builds the child fd table: `fds[i]` must appear in the child as fd `3+i`
   (`ps5_spawn_build_fd_table`). The WebKit IPC socket therefore lands at fd 3,
   which is exactly where `AuxiliaryProcessMain` reads it (`argv[2] == "3"`).
2. Reads our own ELF image once and caches the bytes.
3. Runs the elfldr-derived loader (`elfldr_spawn_fds`):
   `rfork_thread(RFPROC|RFCFDG|RFMEM)` → the child opens `/dev/deci_std*` as
   fds 0/1/2, `PT_TRACE_ME`, and `execve` of the `SceSpZeroConf` system
   trampoline; the parent breakpoints to just before `main`, sets the libc
   heap size, the process name (`argv[0]`) and the environment, maps our ELF,
   then — because `RFCFDG` wiped the fd table — injects each extra descriptor
   with `pt_rdup(child, self, fds[i].parent_fd)` + `pt_dup2(child, tmp, 3+i)`,
   and detaches so `main()` runs our dispatcher.

Descriptor rules (from the contract):

- `fds[i]` → child fd `3+i`; open, blocking mode unchanged, close-on-exec
  cleared.
- The child inherits only 0/1/2 (the `deci_std*` devices, so stderr logging is
  forwarded to the launcher) plus the injected extras.
- The caller keeps ownership of `fds[]` and closes its copies after the call;
  the child holds independent duplicates.
- The current environment (at least the `PS5BROWSER_*` variables) is copied
  into the child.

`ps5_terminate_process` = `kill(pid, SIGKILL)` + non-blocking `waitpid` reap;
safe for an already-dead pid.

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
- The HTML5 Fullscreen API path (`WKViewClient.enterFullScreen/exitFullScreen`)
  and `WKViewRequestExitFullScreen`.
- The WebKit run loop: the public PlayStation API exposes only
  `WKRunLoopRunMain`/`WKRunLoopCallOnMainThread` (no "run once"), so the frame
  loop is a self-re-arming `WKRunLoopCallOnMainThread` callback that paces
  itself with `SDL_Delay`. Confirm this services IPC promptly and does not spin
  the CPU.

## Known risks

- **Spawn memory**: `ps5_spawn_self` caches the whole ELF image (162 MB) in
  the UI process, and the loader mallocs a mirror of the mapped image
  (~130 MB) per spawn while copying it into the child. Shipping
  `--strip-all` saves 33 MB of the cached copy; mapping the file instead of
  reading it would save the rest.
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
- **`text_input_focused`** is not wired: the public PlayStation C API has no
  editor-state/focus callback, so the OSK auto-open on field focus is not
  available yet (see "Could not be mapped" in the handoff).
