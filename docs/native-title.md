# Native PS5 title (`PRWB00001`)

How ProsperoWeb becomes a native PS5 title: a home-screen tile with icon and backgrounds, launched like a game, registered by ShadowMountPlus, and shaped as the ProsperoStore catalog artifact (`docs/store-submission.md`: a `.zip` with `PRWB00001/eboot.bin` + `sce_sys/`). This document records what was verified, the launch-chain design, how to build and install the title, and the exact frontend/spawner code changes still needed. The packaging is implemented in `scripts/ps5/native/make-title.sh`; the listed code changes are **not** implemented yet, so the title's `eboot.bin` is still a loader stub and the browser itself runs only through the websrv tile.

Claims are marked **[code]** when read from source in this tree or a pinned clone, and **[authors]** when they rest on another project's documentation or a homebrew author's statement that was not independently verified on hardware.

## The central finding: a plain ELF does not work

A native title's `eboot.bin` is a **signed development FSELF wrapping a PS5 native ELF** (`e_type=0xFE10`), not the payload-SDK ELF the browser is built as today. The two are incompatible in three ways, all **[code]**:

- **Entry ABI.** A native app's `_start(process_parameters, loader_teardown)` is called by the system loader with a process-parameter block; the payload-SDK ELF expects elfldr to jump to its entry with a `payload_args_t*` in `rdi` (`third_party/ps5-native-tool/tooling/native/app_crt.cpp`; elfldr's `elfldr_prepare_exec` sets `r.r_rdi = args` in the pinned `ps5-payload-dev/elfldr` clone).
- **Layout.** The converter needs the native PIE layout from `ps5-pie.ld` (a gap reserved for the process-parameter block, RELRO anchored to `.data.rel.ro`, every mapped `PT_LOAD` satisfying `p_offset % 0x4000 == p_vaddr % 0x4000`). Feeding it the current browser PIE fails immediately: `ps5-native-tool link` prints `error: LLVM layout leaves no room for PS5 process parameters` (reproduced in `build-native/`).
- **Runtime.** A native title loads a `sce_module/libc.prx` companion and resolves its `DT_NEEDED` `.sprx` by module/library NID metadata the converter writes; the payload ELF is self-relocating and resolves `.sprx` through the payload SDK's own `rtld` at runtime.

The browser ELF already lists `.sprx` dependencies (`libScePad.sprx`, `libkernel_web.sprx`, `libSceLibcInternal.sprx`, …), so it is closer to a native layout than a fully static payload, but it still cannot be converted as-is. **The browser must be relinked as a native application** — the same path VLC-PS5 and the unofficial Stremio PS5 port take (both compile against a native CRT and the boilerplate converter, `ps5/build-title.sh` and `gl/build.sh` in their pinned clones) **[authors]**. VLC and Stremio are single-process; the browser's multi-process model needs the extra work in the launch chain below.

## Producing `eboot.bin` (the toolchain)

The open, Linux-only toolchain is BlackBearReloaded's `ps5-native-app-boilerplate`, vendored from the ProsperoStore repository at commit `22d5a44` under `third_party/ps5-native-tool/` (GPL-3.0; provenance in its `README.md`). `make-title.sh` builds two host binaries from it with the system `clang++` and host `zlib`, no Sony SDK and no ISPC:

- `ps5-native-tool link` converts an LLVM PIE to the PS5 ELF layout, then `ps5-native-tool self --sign --magic 0x1D3D154F` wraps and signs it as a development FSELF. Constants, all **[code]** (verified by building a minimal native app and inspecting the result): FSELF magic `0x1D3D154F`, authority `0x3100000000000002`, program type `1`, module SDK `0x02000009`, companion SDK `0x08050001`.
- `libc-builder` emits the clean-room `libc.prx` from the two text manifests under `tooling/native/runtime/`. **[code]** The build here reproduced the raw ELF (`8ee6e124…`) and the signed `libc.prx` (`e6ff45d16adf…`, 1284674 bytes) **byte-for-byte** against the digests ProsperoStore ships.

Program authority / attributes: the title does not need kernel R/W or elfldr payload args to *run as the foreground app* — the system launches it with the normal application credentials of a homebrew title. It needs elevated privileges only to spawn its helper processes, which is done out-of-process (see the launch chain). This matches VLC/Stremio, which run as ordinary native titles and reach for etaHEN/kstuff/Lapy only for optional sandbox escape **[authors]**.

## `sce_sys` requirements

**[code]** (ProsperoStore `tools/validate-assets.sh` and `sce_sys/param.json` at commit `22d5a44`, and `make-title.sh`'s own self-contained validator, which passes and also passes the upstream `validate-assets.sh`):

- `sce_sys/icon0.png` — 512x512 PNG, required.
- `sce_sys/pic0.dds` (selection background) and `pic1.dds` (launch background) — both or neither, each a single `3840x2160` `BC7_UNORM` DX10 2D DDS **without mipmaps** (header `dwMipMapCount = 1`, `dxgiFormat = 98`, `resourceDimension = 2D`, `arraySize = 1`, file size `148 + (W/4)·(H/4)·16`). Generated from `build-dist/assets/background-3840x2160.png` (from `scripts/assets/render.sh`) by `bc7enc` — the CPU BC7 encoder `bc7enc_rdo`, vendored under `third_party/bc7enc_rdo/` (MIT, commit `b9438627…`), built with host `g++`/OpenMP. bc7enc writes `dwMipMapCount = 0`; the script patches the one header dword to `1`.
- `sce_sys/param.json` — generated from `assets/app.json` (the single source), fields: `titleId` (`PRWB00001`), `contentId` (`IV9999-PRWB00001_00-PROSPEROWEB00000`), `conceptId` (`00001`), `contentVersion` (`01.001.000`), `masterVersion` (`01.00`), `applicationCategoryType` `0` + `contentBadgeType` `1` (the Games shelf), `applicationDrmType` `free`, `downloadDataSize` `256`, `localizedParameters`.
- `sce_module/libc.prx` — the clean-room runtime companion, required by the title layout.

Optional `snd0.at9` selection audio (looped 48 kHz ATRAC9, ≤ 2 MiB) is supported by the shell but not shipped — no free Linux ATRAC9 encoder is vendored **[authors]**. `about/right.sprx`, `nptitle.dat` and `keystone` are retail/NP-store files and are **not** required for a homebrew title that ShadowMountPlus registers **[authors]** (ShadowMountPlus registers a folder on `eboot.bin` + `sce_sys/param.json` alone; its README at `30c5a2b7`).

## Memory budget

A native title launched as the foreground app gets the **game memory budget**, far larger than the fixed homebrew payload budget the browser fights today (`docs/ps5-runtime.md`, "Why a broker": three WebKit processes on `sys_budget_set(0)` risk OOM). **[authors]** The Stremio port sets `applicationCategoryType` to the Games shelf specifically "so that it gets the console's whole direct memory" (`gl/build.sh` in its clone). `make-title.sh` therefore writes `applicationCategoryType: 0` + `contentBadgeType: 1`. No `param.json` field grows the budget beyond the category; `downloadDataSize` only sizes the `/download0` scratch mount **[code]** (boilerplate `docs/PLATFORM_NOTES.md`).

## Launch-chain design

As a native title, the browser UI *is* the foreground big app the moment the tile is launched — the system loader runs `PRWB00001/eboot.bin` directly, so **websrv is no longer needed to obtain the screen and the controller** (today websrv's `hbldr` takes over the `FAKE00000` big-app slot). The broker problem is unchanged: spawning `rfork`+`ptrace`+`execve` from inside the foreground big app panics the console (`docs/ps5-runtime.md`), so the WebProcess and NetworkProcess must still be spawned by a **separate plain-payload broker**.

Recommended chain, with fallbacks:

1. The system launches `PRWB00001/eboot.bin` (native UI, foreground). It gets the screen, the controller and the game memory budget.
2. On the first `ps5_spawn_self()`, the UI tries the existing broker endpoints (AF_UNIX, then TCP `127.0.0.1:17771`) exactly as now.
3. If no broker answers, the UI starts one. The broker is the browser's **payload ELF** (not the native eboot) streamed to **elfldr on `127.0.0.1:9021`**, replacing today's websrv `GET /hbldr?...&daemon=1`. elfldr accepts a raw ELF, a `file:` URI line, or an HTTP `GET ?uri=&args=&env=` — **[code]** (pinned `elfldr/socksrv.c`: `payload_readhttp`/`payload_readuri`, `args_split`, `elfldr_spawn`). Sending `file:/app0/prosperoweb.elf?args=spawn-broker%20--role%3Dspawn-broker&env=…` makes elfldr load it from the title's own folder with the broker argv/env, so nothing has to be uploaded over the network. elfldr copies the parent `environ` into the child, so the broker and its children inherit `FONTCONFIG_FILE`, `PROSPEROWEB_CA_BUNDLE`, … **[code]**.
4. Fallback order when elfldr is absent: try websrv (`GET /hbldr?...&daemon=1`) as today; this keeps the websrv-tile launch working unchanged. Direct in-process spawn stays forbidden for the foreground app (`PROSPEROWEB_SPAWN_FALLBACK=direct` is debug-only).
5. The broker spawns the WebProcess/NetworkProcess as plain payloads and lives as long as the UI, exactly as documented in `docs/ps5-runtime.md`.

This is the pattern OpenNOW-PS5 uses for its tile installer and Stremio for its updater: a native title streams a helper ELF to elfldr on `127.0.0.1:9021` for the privileged work a foreground app cannot do itself **[authors]** (`docs/TITLE_FORWARDING.md`, `docs/SANDBOX_ELEVATION.md` in the pinned clones).

Consequence for the build: the title must ship **both** the native `eboot.bin` (the foreground UI) **and** the payload ELF (what the broker spawns for the children). They are the same multi-call `main()` compiled/linked two ways — natively for the UI, and as the payload SDK ELF for the broker and its children. The payload ELF lives in the title folder (e.g. `PRWB00001/prosperoweb.elf`), and `PROSPEROWEB_SELF_PATH` points at it.

**What the user must have running:** a jailbroken PS5 with ShadowMountPlus (to register the tile) and an elfldr on `127.0.0.1:9021` (to start the broker); kstuff/etaHEN as required by those. websrv is optional (fallback only).

## What this repository builds now

`scripts/ps5/native/make-title.sh [elf]` produces `build-dist/native/PRWB00001/` and the catalog artifact `build-dist/PRWB00001.zip` (`PRWB00001/` at the archive top, `eboot.bin` kept `0755`):

- `eboot.bin` — if the input ELF is a native-app PIE it is converted and signed; the current payload-SDK ELF is rejected by the converter, so the script builds a small **native loader stub** from `scripts/ps5/native/stub/stub_main.c` instead, so the tile is complete, installable and validatable today. The stub is a real native app (verified: FSELF, authority `0x3100000000000002`, program type `1`, integrity valid) that logs one line and raises a notification explaining it is a placeholder, then exits cleanly. Replace it with the real browser eboot once the frontend is relinked natively.
- `sce_module/libc.prx`, `sce_sys/{param.json,icon0.png,pic0.dds,pic1.dds}`.
- `PRWB00001/data/` — read-only runtime data (`chrome/`, `fonts/`, `cacert.pem`, `config.json.sample`, `LICENSES/`) copied from `dist/prosperoweb/`, served as `/app0/data/…`. Skip with `NATIVE_SKIP_DATA=1` for a format-only check.

Data layout decision: **read-only** assets live inside the title at `/app0/data/` (immutable, travels in the zip); the **writable** profile stays at `/data/prosperoweb/`, which ShadowMountPlus mounts writable via `nullfs` for a homebrew title **[code]** (ShadowMountPlus README `30c5a2b7`: "`/data` is mounted through writable `nullfs` for … homebrew titles"). So no `SaveData` setup and no large `downloadDataSize` are needed for the profile.

Light build outputs land in `build-native/` (host tools, intermediate ELFs); `build-dist/` and `build-native/` are git-ignored.

## Install

1. Build: `bash scripts/ps5/native/make-title.sh` (optionally pass a native-app ELF).
2. Copy `build-dist/native/PRWB00001/` to `/data/homebrew/PRWB00001/` on the console (FTP), **or** install `build-dist/PRWB00001.zip` through ProsperoStore. ShadowMountPlus scans `/data/homebrew` and registers a folder that holds `eboot.bin` + `sce_sys/param.json`; `PRWB` is classified as a homebrew prefix **[code]** (ShadowMountPlus README). Keep the `eboot.bin` execute bit: prefer the zip or an image, as loose-file copy tools drop it **[authors]**.
3. Ensure elfldr is listening on `127.0.0.1:9021` (and ShadowMountPlus is running). Launch "ProsperoWeb" from the home screen.
4. Remove the old websrv tile at `/data/homebrew/prosperoweb/` if present, so only the native tile shows (once the title runs the browser; until then the stub only shows a notification and the websrv tile is still the way to start it).

## What the frontend/spawner must change (not implemented yet)

The remaining work before the native title runs the browser. None of it is done yet.

1. **Native second linkage of the frontend.** Add a native-app link of the same `src/frontend/ps5/main.cpp` multi-call program, using `third_party/ps5-native-tool/tooling/native/{app_crt.cpp,app_cpp_runtime.cpp,ps5-pie.ld,app-symbols.map}` and the vendored converter, to produce `PRWB00001/eboot.bin` (the foreground UI). Keep the existing payload-SDK link for the broker/children. This is the real-eboot replacement for the stub and belongs in the PS5 frontend build (`src/frontend/ps5/CMakeLists.txt` / the build under `build-wk-ps5`), feeding its output to `make-title.sh`.

2. **Resolve read-only data from `/app0` when running as a title.** In `src/frontend/ps5/ps5_runtime_env.cpp`, `setup_runtime_env(base_dir)` and `apply_config_env(base_dir)` derive `fonts`, `cacert.pem` and `config.json` from a single `base_dir` (today the install dir). Split it: read-only assets (`fonts/`, `cacert.pem`, `chrome/`) from `/app0/data` when the process image is under `/app0` (detect via the self path or a new `PROSPEROWEB_APP0`), while `HOME`, `TMPDIR` and the profile stay at `/data/prosperoweb`. The default `chrome_dir` in `src/frontend/ps5/ps5_config.h` (`/data/prosperoweb/chrome`) needs the same `/app0/data/chrome` resolution.

3. **Self-path for the children.** `export_self_path()` in `src/frontend/ps5/main.cpp` sets `PROSPEROWEB_SELF_PATH` from `argv[0]` only when it ends in `.elf`; a native title's `argv[0]` is `/app0/eboot.bin`, so it would fall back to `/data/prosperoweb/prosperoweb.elf`. Point it at the payload ELF shipped in the title (`/app0/prosperoweb.elf`) when running under `/app0`.

4. **Broker launch via elfldr.** In `src/ps5/spawn/spawn_glue.cpp`, add `start_broker_via_elfldr(self_path)` beside `start_broker_via_websrv(self_path)`: open `127.0.0.1:9021`, send `file:<self_path>?args=spawn-broker%20--role%3Dspawn-broker&env=<PROSPEROWEB_ROLE/SPAWN_PORT/SELF_PATH…>` (reuse `websrv_broker_target`/`websrv_escape_token` for the query building, and the elfldr line protocol from `src/ps5/spawn/elfldr.c`), then `shutdown(SHUT_WR)` so elfldr runs it. In `ensure_broker()`, try elfldr first (or when websrv is absent) before `start_broker_via_websrv()`; keep websrv as the fallback. A `PROSPEROWEB_BROKER_LOADER=elfldr|websrv` override and an elfldr address env (default `127.0.0.1:9021`) mirror the existing `PROSPEROWEB_WEBSRV` knob.

## Sources

Verified in code in this tree or pinned clones:

- `third_party/ps5-native-tool` ← ProsperoStore `tooling/`, commit `22d5a44a053b4dad4f3a5867fe39d262a80a1b8c` (converter, `libc.prx`, `ps5-pie.ld`, `app_crt.cpp`, `app-symbols.map`, `tools/validate-assets.sh`, `sce_sys/param.json`, `docs/PLATFORM_NOTES.md`, `docs/RUNTIME_SHIM.md`, `docs/SANDBOX_ELEVATION.md`).
- `third_party/bc7enc_rdo`, commit `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`.
- `ps5-payload-dev/elfldr` at `2a8da002…` (`socksrv.c`, `elfldr.c`, `uri.c`): line/URI/HTTP protocol on port 9021, `args_split`, `elfldr_set_environ`, `elfldr_prepare_exec`.
- `ps5-payload-dev/websrv` at `7748df9d…` (`src/ps5/hbldr.c`, `src/ps5/elfldr.c`): the big-app takeover and daemon launch today.
- `drakmor/ShadowMountPlus` at `30c5a2b7…` (`README.md`): homebrew-prefix classification, `/data/homebrew` scan, `eboot.bin` + `param.json` registration, writable `nullfs` for `/data`.

Per authors (documentation, not independently verified on hardware here):

- `ZiZc3/VLC` (`ps5/build-title.sh`, `README.md`) and `Sp9nky/unofficial-stremio-ps5-port` (`gl/build.sh`, `README.md`): native titles built with a native CRT + the boilerplate converter; Games category for the memory budget; kstuff/etaHEN/ShadowMountPlus prerequisites.
- `OpenCloudGaming/OpenNOW-PS5` (`docs/TITLE_FORWARDING.md`, `docs/NATIVE_TOOLING.md`): a native title streaming a helper ELF to elfldr on `127.0.0.1:9021` for privileged work.
