# Native PS5 title (`PRWB00001`)

How ProsperoWeb becomes a native PS5 title: a home-screen tile with icon and backgrounds, launched like a game, registered by ShadowMountPlus, and shaped as the ProsperoStore catalog artifact (`docs/store-submission.md`: a `.zip` with `PRWB00001/eboot.bin` + `sce_sys/`). This document records what was verified, the launch-chain design, how the UI is linked as a native application, and how to build and install the title. The title's `eboot.bin` is the browser UI itself (CMake target `prosperoweb_native`, packaged by `scripts/ps5/native/make-title.sh`). **It builds, converts and signs, but has not been run on a console yet**; the risks are listed under "Not verified on hardware".

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

A native title launched as the foreground app gets the **game memory budget**, far larger than the fixed homebrew payload budget the browser fights today (`docs/ps5-runtime.md`, "Why a broker": three WebKit processes on `sys_budget_set(0)` risk OOM). **[authors]** The Stremio port sets `applicationCategoryType` to the Games shelf specifically "so that it gets the console's whole direct memory" (`gl/build.sh` in its clone). `make-title.sh` therefore writes `applicationCategoryType: 0` + `contentBadgeType: 1`. `downloadDataSize` only sizes the `/download0` scratch mount **[code]** (boilerplate `docs/PLATFORM_NOTES.md`). The budget has two parts: **direct memory** (gigabytes for a game; the UI's heap, the screen buffers and the video decoder live there) and **flexible memory** (the image, thread stacks, the system libc heap, anonymous `mmap`), which `param.json` `kernel.flexibleMemorySize` sizes: syobon-action-ps5 asks for 256 MiB, this title for 512 MiB (see "Native link").

## Launch-chain design

As a native title, the browser UI *is* the foreground big app the moment the tile is launched — the system loader runs `PRWB00001/eboot.bin` directly, so **websrv is no longer needed to obtain the screen and the controller** (today websrv's `hbldr` takes over the `FAKE00000` big-app slot). The broker problem is unchanged: spawning `rfork`+`ptrace`+`execve` from inside the foreground big app panics the console (`docs/ps5-runtime.md`), so the WebProcess and NetworkProcess must still be spawned by a **separate plain-payload broker**.

Recommended chain, with fallbacks:

1. The system launches `PRWB00001/eboot.bin` (native UI, foreground). It gets the screen, the controller and the game memory budget.
2. On the first `ps5_spawn_self()`, the UI tries the existing broker endpoints (AF_UNIX, then TCP `127.0.0.1:17771`) exactly as now.
3. If no broker answers, the UI starts one. The broker is the browser's **payload ELF** (not the native eboot), loaded by **elfldr on `127.0.0.1:9021`** from the title folder: the UI sends one line, `file:<install folder>/prosperoweb.elf?args=<args>&pipe=0`, and half-closes the connection. **[code]** (pinned `elfldr` at `2a8da002`, `socksrv.c`): `on_connection` peeks the magic `file`, `payload_readuri` reads the line, `uri_get_content` loads the path from the console's file system (elfldr's own namespace, where `/app0` does not exist, hence the install folder), `uri_get_param(uri, "args")` is URI-decoded and split on spaces with a backslash escaping a space (`args_split`) into `argv[1..]` (`argv[0]` is the file name), and `pipe=0` keeps the connection from becoming the payload's stdout/stderr. elfldr writes only on failure (`[elfldr.elf] Error ...`). **elfldr passes no environment of ours** (the payload inherits elfldr's), so the broker's settings travel as `--env=NAME=VALUE` arguments that `main()` exports before anything reads the environment (`elfldr_broker_args()` in `src/ps5/spawn/broker_client.cpp`).
4. Fallback order when elfldr is absent: try websrv (`GET /hbldr?...&daemon=1`) as today; this keeps the websrv-tile launch working unchanged. Direct in-process spawn stays forbidden for the foreground app (`PROSPEROWEB_SPAWN_FALLBACK=direct` is debug-only).
5. The broker spawns the WebProcess/NetworkProcess as plain payloads and lives as long as the UI, exactly as documented in `docs/ps5-runtime.md`.

This is the pattern OpenNOW-PS5 uses for its tile installer and Stremio for its updater: a native title streams a helper ELF to elfldr on `127.0.0.1:9021` for the privileged work a foreground app cannot do itself **[authors]** (`docs/TITLE_FORWARDING.md`, `docs/SANDBOX_ELEVATION.md` in the pinned clones).

Consequence for the build: the title must ship **both** the native `eboot.bin` (the foreground UI) **and** the payload ELF (what the broker spawns for the children). They are the same multi-call `main()` compiled/linked two ways — natively for the UI, and as the payload SDK ELF for the broker and its children. The payload ELF lives in the title folder (e.g. `PRWB00001/prosperoweb.elf`), and `PROSPEROWEB_SELF_PATH` points at it.

**What the user must have running:** a jailbroken PS5 with ShadowMountPlus (to register the tile) and an elfldr on `127.0.0.1:9021` (to start the broker); kstuff/etaHEN as required by those. websrv is optional (fallback only).

## Native link (implemented)

The frontend sources are compiled a second time with `PROSPEROWEB_NATIVE_TITLE=1` and linked by the CMake target `prosperoweb_native` (`src/frontend/ps5/CMakeLists.txt`) into `build-ps5/src/frontend/ps5/prosperoweb-native.elf`. The payload target `prosperoweb` is unchanged and remains what the broker and the children run.

The template is the same as the working native titles built from the **same payload SDK** (`/opt/ps5-payload-sdk`, its `libc.a`, `libc++.a`, `libSceLibcInternal`, PacBrew SDL2): [kvaps/syobon-action-ps5](https://github.com/kvaps/syobon-action-ps5) (commit `9469362`, verified on fw 13.42 with kstuff-lite and ShadowMountPlus: `Makefile` `APP_WRAP_SYMBOLS`, README "Port notes", `src/ps5_compat.cpp`, `sce_sys/param.json`) and the Stremio PS5 port (`gl/build.sh`, `native/*.c`). VLC for PS5 (`ps5/build-title.sh`) uses the same converter. The pieces:

- **CRT and layout.** `third_party/ps5-native-tool/tooling/native/app_crt.cpp` (`_start(process_parameters, teardown)`) replaces the payload `crt1.o` (`-nostartfiles`). `src/frontend/ps5/native/ps5-native-app.ld` is the boilerplate's `ps5-pie.ld` plus one change: WebKit's page-aligned writable sections `__DATA,__wtf_config` and `__DATA,__jsc_opcodes` go in the data segment. As orphans lld put them inside the RELRO range and the converter failed with "LLVM layout leaves no room for PS5 process parameters". `app-symbols.map` keeps every symbol local (the converter publishes no exports); `--no-dynamic-linker` keeps undefined weak references (`__cxa_thread_atexit_impl`, a TLS init hook) out of the imports; `--as-needed` drops modules nothing calls.
- **libc.** The same SDK `libc.a` works in a title, except for objects that execute the `syscall` instruction themselves, which a title may not do (Stremio PS5 `native/kernel_mem.c`: "directly issued a syscall"). The build makes `libc-native.a` = `libc.a` without `syscalls.o` and `mman.o`; `mmap`, `mprotect`, `dup`, … then resolve to libkernel, and `native/native_runtime.c` makes the rest (`ptrace`, `syscall`, `cpuset_setid`, `readlink`, `fstatfs`, …) fail with `ENOSYS`. `scripts/ps5/native/check-eboot.py` disassembles the linked image and fails the build if any `syscall` instruction is left.
- **Payload-only pieces.** The payload crt provides the kernel R/W helpers (`kernel_*`), `klog_*` and the SDK runtime loader behind `dlopen`. In the title, `kernel_*` fail cleanly (the title runs with application credentials; only the broker raises privileges), `klog_*` go to `sceKernelDebugOutText`, and `__dlopen`/`__dlsym` use `sceKernelLoadStartModule`/`sceKernelDlsym` (`native/native_runtime.c`).
- **Heap.** A title's libc heap is tiny: syobon measured it stopping at about 16 MiB, Stremio 8 MiB. The link wraps `malloc`, `free`, `calloc`, `realloc`, `posix_memalign`, `aligned_alloc`, `malloc_usable_size` and runs dlmalloc mspaces (`third_party/dlmalloc`, MIT-0) in one block of direct memory, as large as the title's direct memory minus 1.5 GiB (the screen, the decoder), at most 4 GiB (`native/native_memory.c`, after Stremio's `native/heap.c`). Pointers outside the block belong to a system module and go back to the system heap.
- **Other title limits.** Anonymous `mmap` falls back to flexible, then direct memory (VLC `ps5/compat/mmap.c` found it refused); `pipe()` falls back to `socketpair` (Kodi/Stremio); threads get 2 MiB stacks by default instead of 64 KiB (`ps5_cpu_affinity.cpp`); `sceKeyboard*` are stand-ins reporting no keyboard (in a title `sceKeyboardInit` crashes, syobon); the IME dialog calls SDL polls every frame are wrapped and only reach the module once `sceSysmoduleLoadModule(0x96)` loaded it; returning from `main()` waits for the system to close the title instead of crashing it; the UI runs JSC without JIT (no executable memory for a title), while the children keep their JIT (`PROSPEROWEB_UI_ONLY_ENV`).
- **Imports.** `check-eboot.py` maps every import to its module: all come from modules a title gets at start-up (the list Stremio's `native/check_imports.py` took from a crash log) except `libSceImeDialog` and `libSceVideodec2`, which the UI loads itself. `libSceVideodec2` has no SDK stub: `native/videodec2_stub.c` is built as `libSceVideodec2.so` (SONAME `libSceVideodec2.sprx`) and passed to the converter as an extra `--stub`.
- **param.json.** `kernel.flexibleMemorySize` is 512 MiB (`NATIVE_FLEXIBLE_MB`); syobon runs with 256 MiB, and this image alone is about 130 MiB. The largest value the system accepts is not known. If the title does not start, edit `sce_sys/param.json` on the console to `268435456`.

## Runtime paths

A title sees its folder as `/app0`; the broker and the children are payloads and see the real file system, where `/app0` does not exist. ShadowMountPlus mounts `/data`, `/mnt/usbN` and `/mnt/extN` into a homebrew title's sandbox with writable nullfs (README at `30c5a2b7`, line 287), so a title installed as a folder under one of its scan paths (README "Scan paths": `/data/homebrew`, `/data/etaHEN/games`, `/mnt/ext0/homebrew`, …) has the same path on both sides. At start-up the UI looks for `<scan path>/PRWB00001/eboot.bin` with the same size and modification time as `/app0/eboot.bin` (`src/ps5/spawn/title_paths.cpp`, `PROSPEROWEB_TITLE_DIR` overrides) and then uses that folder for everything it hands to its helpers:

- `PROSPEROWEB_SELF_PATH=<folder>/prosperoweb.elf`, the payload the broker runs as itself and for the children;
- `FONTCONFIG_FILE`, `FONTCONFIG_PATH`, `PROSPEROWEB_CA_BUNDLE` under `<folder>/data/` (`setup_runtime_env(profile, assets)` in `ps5_runtime_env.cpp`); the title's `fonts.conf` names its fonts relative to itself (`<dir prefix="relative">.</dir>`), so it works from any folder;
- the chrome page `file://<folder>/data/chrome/chrome.html` (`PROSPEROWEB_CHROME_DIR`; a `config.json` `chrome_dir` still wins when that directory exists).

If the folder is not found (an image mount, an unusual folder name), the UI reads `/app0` and the children get `/mnt/sandbox/PRWB00001_000/app0`: the spawner rewrites the forwarded environment from one root to the other (`rebase_env`). That fallback path is the usual sandbox layout and is **not verified**.

The writable profile stays `/data/prosperoweb/` (created on first start): `config.json`, `log.txt`, `tmp/`, the web data, the fontconfig cache.

## Hardware decoding in the title

`main()` of the native build first opens the log, then calls `hwdec_native_startup()` before anything else starts a thread or a helper: `sceSysmoduleLoadModule(207)`, the decoder functions imported by NID, `sceVideodec2QueryComputeMemoryInfo`, direct memory for the queue, `sceVideodec2AllocateComputeQueue`, and the memory answers for H.264/HEVC 1080p/4K. That is VLC for PS5's order (library and compute queue before anything raises privileges; `src/platform_ps5.c:168-174, 528-530`). The title never raises its own privileges; only the broker (a separate payload) does. Every call still goes through the watched probe (3 s per call), and `main()` waits at most 8 s for it, so a hang turns hardware decoding off instead of stopping the UI. The queue is kept for the session and shared by every decoder. `PROSPEROWEB_HWDEC=0` turns it off. In the payload build hardware decoding stays off unless `PROSPEROWEB_HWDEC=1` (docs/ps5-runtime.md).

## Building

```
cmake --build build-ps5 -j4 --target prosperoweb prosperoweb_native
scripts/ps5/package.sh                      # dist/prosperoweb/prosperoweb.elf (stripped payload)
scripts/ps5/native/make-title.sh            # build-dist/native/PRWB00001/, build-dist/PRWB00001.zip
```

`make-title.sh` runs `check-eboot.py`, converts the native ELF with `ps5-native-tool link` (plus the `libSceVideodec2` stub), prints the converted ELF's header and dynamic section (`e_type 0xFE10`, `NEEDED *.prx`), signs it (`self --sign --magic 0x1D3D154F`) and inspects the result (authority `0x3100000000000002`, program type `1`, integrity valid). The folder holds `eboot.bin`, `prosperoweb.elf`, `sce_module/libc.prx`, `sce_sys/{param.json,icon0.png,pic0.dds,pic1.dds}` and `data/{chrome,fonts,cacert.pem,LICENSES,config.json.sample}`; the zip has `PRWB00001/` at the top and `eboot.bin` at mode 0755. With `PROSPEROSTORE_DIR` pointing at a ProsperoStore clone, its `tools/validate-assets.sh` is run as well. Without a native ELF the script still builds the old loader stub (`scripts/ps5/native/stub/`) so the format can be checked; `NATIVE_ALLOW_STUB=0` (CI) makes that an error.

## Install

Prerequisites: a jailbroken PS5 with etaHEN or kstuff, ShadowMountPlus running, and elfldr listening on `127.0.0.1:9021` (websrv on `127.0.0.1:8080` is the fallback loader for the broker).

1. Put the folder at `/data/homebrew/PRWB00001/` (ShadowMountPlus's first default scan path; it registers a folder holding `eboot.bin` + `sce_sys/param.json`, `PRWB` being a homebrew prefix, README "Scan paths"). Copy `PRWB00001.zip` to the console and unpack it there, or copy the folder over FTP and make sure `eboot.bin` keeps its execute bit (loose-file copy tools drop it).
2. Wait for ShadowMountPlus's next scan (`scan_interval_seconds`, 15 s by default) and start "ProsperoWeb" from the home screen.
3. The websrv tile (`/data/prosperoweb/`, `/data/homebrew/prosperoweb/`) can stay installed; both share the profile in `/data/prosperoweb/`. Do not run both at once.

What `/data/prosperoweb/log.txt` should show on the first launch, in order:

```
[UI] main: native title PRWB00001 pid=... argv0=...
[UI] hwdec: process uid ... (native title, application credentials)
[UI] hwdec: libSceVideodec2 functions imported (native title)
[UI] hwdec: compute queue ready (N KiB)
[UI] hwdec: native startup probe ready
[UI] main: system modules: libSceImeDialog (sysmodule 0x96): loaded 0
[UI] main: heap N MiB of direct memory
[UI] title: install folder /data/homebrew/PRWB00001 (same eboot.bin as /app0); UI reads ..., helpers ...
[UI] title: payload ELF for the broker and children /data/homebrew/PRWB00001/prosperoweb.elf (present here)
[UI] chrome: file:///data/homebrew/PRWB00001/data/chrome/chrome.html
[UI] spawn: starting the broker /data/homebrew/PRWB00001/prosperoweb.elf via elfldr (loader auto, attempt 1/2)
[UI] elfldr: 127.0.0.1:9021 <- file:/data/homebrew/PRWB00001/prosperoweb.elf?args=... -> accepted in Nms
[UI] spawn: broker ready pid=... in Nms
```

The `[prosperoweb] native heap: ...` line goes to klog (port 3232) only, as it is written before the log file is open.

Fallback: if the title shows nothing or closes, read `log.txt` and klog. `PROSPEROWEB_HWDEC=0` in `/data/prosperoweb/config.json` `"env"` skips the decoder; `PROSPEROWEB_BROKER_LOADER=websrv` uses websrv instead of elfldr. If the title does not start at all, lower `kernel.flexibleMemorySize` in `sce_sys/param.json` to `268435456`. The websrv tile keeps working independently.

## Not verified on hardware

Everything above links, converts, signs and passes the import and syscall checks; none of it has run on a console yet. The open questions, most likely first:

- whether the UI's WebKit and SDL start under the title's memory limits (direct-memory heap, 512 MiB flexible memory);
- whether imports of `libSceImeDialog`/`libSceVideodec2` bind after `sceSysmoduleLoadModule` (the Stremio port and VLC rely on it with the same `-fno-plt` code);
- IPC between the sandboxed UI and payload children: AF_UNIX sockets, descriptors passed by the broker (`rdup` from the UI's pid over TCP), and the shared-memory video/audio transports;
- whether `/data/homebrew/PRWB00001` is the folder ShadowMountPlus mounts as `/app0` (detected at run time, logged) and the `/mnt/sandbox/PRWB00001_000/app0` fallback;
- whether the hardware decoder answers in this process (VLC: yes, in a sandboxed title with application credentials).

## Sources

Verified in code in this tree or pinned clones:

- `third_party/ps5-native-tool` ← ProsperoStore `tooling/`, commit `22d5a44a053b4dad4f3a5867fe39d262a80a1b8c` (converter, `libc.prx`, `ps5-pie.ld`, `app_crt.cpp`, `app-symbols.map`, `tools/validate-assets.sh`, `sce_sys/param.json`, `docs/PLATFORM_NOTES.md`, `docs/RUNTIME_SHIM.md`, `docs/SANDBOX_ELEVATION.md`).
- `third_party/bc7enc_rdo`, commit `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`.
- `ps5-payload-dev/elfldr` at `2a8da002…` (`socksrv.c`, `elfldr.c`, `uri.c`): line/URI/HTTP protocol on port 9021, `args_split`, `elfldr_set_environ`, `elfldr_prepare_exec`.
- `ps5-payload-dev/websrv` at `7748df9d…` (`src/ps5/hbldr.c`, `src/ps5/elfldr.c`): the big-app takeover and daemon launch today.
- `drakmor/ShadowMountPlus` at `30c5a2b7…` (`README.md`): homebrew-prefix classification, `/data/homebrew` scan, `eboot.bin` + `param.json` registration, writable `nullfs` for `/data`.

- `kvaps/syobon-action-ps5` at `9469362` (`Makefile`, `README.md` "Port notes", `src/ps5_compat.cpp`, `sce_sys/param.json`): a native title from the payload SDK, PacBrew SDL2 and the same boilerplate, verified on fw 13.42 by its author.
- `Sp9nky/unofficial-stremio-ps5-port` at `b8444a66` (`gl/build.sh`, `native/heap.c`, `native/kernel_mem.c`, `native/posix_fixes.c`, `native/ps5_modules.c`, `native/check_imports.py`, `native/dlmalloc.c`).

Per authors (documentation, not independently verified on hardware here):

- `ZiZc3/VLC` (`ps5/build-title.sh`, `README.md`) and `Sp9nky/unofficial-stremio-ps5-port` (`gl/build.sh`, `README.md`): native titles built with a native CRT + the boilerplate converter; Games category for the memory budget; kstuff/etaHEN/ShadowMountPlus prerequisites.
- `OpenCloudGaming/OpenNOW-PS5` (`docs/TITLE_FORWARDING.md`, `docs/NATIVE_TOOLING.md`): a native title streaming a helper ELF to elfldr on `127.0.0.1:9021` for privileged work.
