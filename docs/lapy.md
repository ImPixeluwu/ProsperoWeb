# PS5-Lapy-JB-Daemon: what it offers a native title, and whether ProsperoWeb can use it

This answers the question raised by `docs/no-elfldr.md` "Option D — PS5-Lapy-JB-Daemon": the operator reports that **ProsperoEden** (`PPSA99008`, a single-process emulator title) launches without the user starting elfldr because it uses a resident Lapy service, while ProsperoWeb hangs because it needs a loader to spawn WebKit's helper processes every launch. The short answer is that **Lapy does not help ProsperoWeb's launch problem**: it elevates an already-running process, it does not spawn one, and it is gated to `PPSA` title IDs. Full reasoning and citations below.

Claims are **[code]** when read from source in a pinned clone and **[authors]** when they rest on a project's own docs.

## What was read

- `mpereiraesaa/PS5-Lapy-JB-Daemon@b48b7d7` — `README.md`, `source/lapy_elevation_protocol.h`, `source/owned_root_daemon.c`, `source/main.cpp`, `tools/build_owned_daemon.py`, `Makefile`, `LICENSE`. This is the real upstream of the cooperative helper. **Pin note:** ProsperoEden pins `blackbearreloaded/PS5-Lapy-JB-Daemon@57fcc19` (`tools/deps.json` item `lapy-owned-helper`), but that fork is now `404 / Repository not found`, so the pinned tree could not be fetched. ProsperoEden states the fork is "based on mpereiraesaa's cooperative helper" and "does not copy or modify Lapy's privileged source" (`headless/elevation/README.md:3-9`), so `b48b7d7` is a faithful reading of the same mechanism; treat exact line numbers as upstream-HEAD, not the pin.
- `blackbearreloaded/ProsperoEden@a26ba78` — `headless/elevation/README.md`, `headless/elevation/protocol.hpp`, `headless/elevation/elevation.cpp`, `tools/build-lapy-helper.py`, `tools/deps.json`.

## 1. What the resident daemon is and how a title reaches it

Lapy is an **elevation** daemon, not a loader. It is built as a single-request helper, a bounded or unbounded resident service, or a one-shot elfldr helper, all from `source/owned_root_daemon.c` with compile-time switches (`tools/build_owned_daemon.py:27-49`, `Makefile:13-26`: targets `owned-service`, `owned-one-shot`, `owned-helper`, `legacy`). It is loaded once via an ELF loader and then stays resident (`README.md:64-68`). A separate legacy backend (`source/main.cpp`, `make legacy`) is a libhijacker etaHEN-style full jailbreak; the fork ProsperoEden uses is the cooperative owned-root backend, so the owned-root path is what matters here.

Transport — there are two, and the resident one needs **no socket and no elfldr**:

- **Resident / one-shot (file marker).** The title writes `{"PID":<getpid()>}` into **`/download0/elevate_proc`** inside its own sandbox; the daemon (root, unsandboxed) polls `/mnt/sandbox/<TID>_<NNN>/download0/elevate_proc` every 100 ms, elevates, and the title reads the verdict back from `/download0/lapy_owned_result` as `DATA_OK=%d OPEN_ERRNO=%d` **[code]** (`owned_root_daemon.c:45-46,205-249,1049-1074`; `README.md:30-54`). This is strongly reachable from a uid-1 sandbox because the title only touches files in **its own** `/download0` (requires `downloadDataSize` in `param.json`) — unlike etaHEN's `AF_UNIX /system_tmp/etaHEN_util_service`, which `docs/no-elfldr.md` flagged as unverified and which our console found absent (errno 2). No loopback TCP is involved at all.
- **One-shot elfldr helper (socket).** The title streams the helper ELF to elfldr on TCP `127.0.0.1:9021`, holds the connection open, and exchanges the fixed 24-byte `struct lapy_elevation_message` (`magic 0x31564c45 "ELV1"`, kinds `REQUEST/PREPARE/PREPARED/RESPONSE`, capability `FILESYSTEM`, a `pid`) over the helper's stdin/stdout **[code]** (`source/lapy_elevation_protocol.h:7-47`; `owned_root_daemon.c:895-957`, which reads/writes `STDIN_FILENO`/`STDOUT_FILENO`). **This path still requires elfldr** and is only the fallback.

## 2. What it actually does for the title: ELEVATE, not spawn, not run-an-arbitrary-ELF

It **elevates the calling process in place**. `run_one()` ptrace-stops the target and writes its ucred to full root: `uid=ruid=svuid=0`, `rgid=svgid=0`, `ngroups=0`, `authid = 0x4801000000000013` (SYSTEM_AUTHID), `caps = 0xff…ff`, and the unsandboxed attribute bit (`attrs[3] |= 0x80`) **[code]** (`owned_root_daemon.c:651-672`). It then performs the daemon's distinctive safe step — it transfers **two root-vnode references** obtained from native `rfork(RFPROC|RFFDG)` donor children into the target's `fd_rdir`/`fd_jdir` (the sandbox-directory escape), and releases the donors so the reference counts stay balanced (`owned_root_daemon.c:701-760`, `lapy_replace_two_roots`). The README explains why: the legacy backend that directly overwrote root/jail pointers was associated with a panic after ~20-29 elevations per boot; the reference transfer avoids that (`README.md:14-20`).

So, against the three choices in the task:

- **(a) ELEVATE the calling title — YES.** This is exactly what it does: root ucred + authid + full caps + sandbox-directory escape, giving the title root `/data` and system-path access. The protocol's only capability is `LAPY_ELEVATION_FILESYSTEM` (`lapy_elevation_protocol.h:17-19`), and the helper rejects anything else (`owned_root_daemon.c:910-913`), but the credential change applied is a full root elevation, not a narrow filesystem grant.
- **(b) RUN an arbitrary ELF/payload like elfldr — NO.** There is no loader, no `elfldr_spawn`, no `sceKernelSpawn`, no path-to-run command anywhere in the daemon. The only ELF it loads is **itself** (via elfldr, in helper mode). It cannot start our broker.
- **(c) Per-title "owned helper" — this is the packaging, not a capability.** The "owned helper" is the daemon ELF compiled for one exact title; it elevates that title and runs no other code. ProsperoEden's `tools/build-lapy-helper.py` builds `owned_root_helper-PPSA99008` with `mode: elf-helper, max_requests: 1, service: false` and then verifies the manifest's `target_title`, `mode`, `elf_sha256`, `protocol_sha256`, and the `root_layout_probe_retry` feature before packaging (`build-lapy-helper.py:14-55`). Consumers are told to reject assets whose title/ELF/protocol digest does not match (`README.md:123-127`).

**Cooperation the title must perform (non-negotiable):** in the same process that will be elevated, call `seteuid(geteuid())` **before** creating any other thread and before writing the request, so the kernel gives that process a private (unshared) credential; the daemon refuses a target unless it sees exactly one thread, `fd_refs == 1`, a private credential (`cred_refs == 2`), and `prison == PRISON0` (`README.md:30-54`; `owned_root_daemon.c:620-647`, preflight gate; helper handshake drives the `seteuid` between `PREPARE` and `PREPARED`, `owned_root_daemon.c:918-940`). ProsperoEden does this at single-threaded startup (`elevation.cpp:219,294`).

## 3. Can `PRWB00001` use it? Title-ID gate and hashes

**No, not with a stock Lapy.** The title ID is a hard gate in two places:

- The resident service matches sandbox directories by the compiled `TARGET_TITLE` prefix, and **even the wildcard `*` build only accepts `PPSA`-prefixed, `PPSA[0-9]{5}_` directories** (`owned_root_daemon.c:210-226`). `PRWB00001_000` starts with `PRWB`, so a generic resident Lapy ignores it outright. This is why ProsperoEden (`PPSA99008`) is served by a wildcard resident daemon and ProsperoWeb would not be.
- The build tool refuses to compile for a non-PPSA title at all: `--title` must match `re.fullmatch(r"PPSA[0-9]{5}", title)` or be `*` (`build_owned_daemon.py:43-47`). The helper also checks the live title with `sceKernelGetAppInfo(pid)` against the compiled `TARGET_TITLE` (`owned_root_daemon.c:962-968`).

To use Lapy at all, ProsperoWeb would have to **ship its own Lapy helper built for `PRWB00001`** (patching the `PPSA` regex in the build tool, or a resident daemon specially compiled for `PRWB00001`), plus the cooperative client (the `seteuid` + marker/handshake protocol) and the `lapy-manifest.json` hash scheme. `max_requests: 1` is **not** the blocker for us — a title needs elevating only once per launch, not once per WebProcess — the blocker is that elevation does not spawn anything (see §4).

## 4. The key question: does elevation let ProsperoWeb spawn its own children?

**No.** This is the crux and it is a clear negative. Our console finding was that `rfork_thread(RFPROC|RFCFDG|RFMEM)` from the foreground big app returned EPERM at uid 1. Lapy would remove the EPERM (root + full caps), but `docs/no-elfldr.md:120-125` and `docs/ps5-runtime.md` establish that the in-process-spawn problem is **the big-app context, not credentials**: `rfork`+`ptrace`+`execve` from inside the foreground big app panics the console regardless of privilege (hbldr never rforks from the big app either). etaHEN's own jailbreak was already assessed as "does not make an in-process rfork safe" for exactly this reason (Option C). Lapy grants the *same kind* of elevation (arguably more: it also moves `fd_rdir`/`fd_jdir`), so the same conclusion holds: an elevated ProsperoWeb UI still cannot safely `rfork` its WebProcess/NetworkProcess, and Lapy offers no out-of-process loader to do it for us.

Compare the two possible designs:

- **Lapy elevates the UI, UI spawns children itself** — rejected. Removes EPERM but not the big-app panic; turns a clean EPERM-and-fallback into a console panic risk. Lapy's value (root `/data`) is real but irrelevant to spawning; our title already runs with application credentials and reaches `/data` through ShadowMountPlus's nullfs mount (`docs/native-title.md:79`).
- **A loader launches our broker ELF (the elfldr/websrv model we already have)** — retained. The broker is a separate plain payload; spawning from it is the proven-safe context (`docs/ps5-runtime.md`, `docs/native-title.md:47-57`). Lapy cannot play this role because it is not a loader. Only an elfldr-class loader (John's elfldr on 9021, or etaHEN's `BREW_UTIL_LAUNCH_PLUGIN`, or websrv) can.

## Options

| Option | How | Pros | Cons / Risk |
|---|---|---|---|
| **A. Ignore Lapy for spawning; keep the elfldr/websrv broker chain** | Unchanged from `docs/native-title.md`: UI asks elfldr `9021` (etaHEN auto-runs it) or websrv to start `prosperoweb-broker.elf`. | No new dependency; proven-safe spawn context; works on any etaHEN console with no user step. | Still needs a loader (that is inherent to multi-process). **Low risk — recommended.** |
| **B. Ship a `PRWB00001` Lapy helper to elevate the UI, then spawn in-process** | Patch the PPSA gate, build `owned_root_helper-PRWB00001`, run the cooperative client, drop `rfork` in the UI. | Removes the loader *if* in-process spawn were safe. | In-process spawn is **not** safe (big-app panic); also still needs elfldr to load the one-shot helper. **High risk — rejected.** |
| **C. Use resident Lapy only for `/data`/system-font access, keep the broker for spawning** | Elevate the UI for filesystem, spawn children via the broker as today. | Could grant system-font/`/data` paths without ShadowMountPlus. | We already get `/data` via ShadowMountPlus nullfs; gate forbids `PRWB` on stock Lapy; adds a dependency for a problem we do not have. **Not worth it.** |
| **D. Lapy as a general no-loader path (what ProsperoEden gets)** | Resident Lapy file-marker elevation, no elfldr. | Real and elfldr-free **for a single-process title**. | Only elevates; gives a multi-process title nothing for spawning. **Does not apply to us.** |

## Recommendation

**Do not adopt Lapy to solve ProsperoWeb's launch.** Lapy is the right tool for ProsperoEden because that title is single-process and only needs root `/data`; our title is multi-process and needs an out-of-process *loader*, which Lapy is not. Keep the loader-based broker chain from `docs/no-elfldr.md` and `docs/native-title.md` and add the friendly no-loader screen.

Recommended `PROSPEROWEB_BROKER_LOADER` fallback order, unchanged in substance from `docs/no-elfldr.md` (Lapy does not enter it):

1. **etaHEN `BREW_UTIL_LAUNCH_PLUGIN`** (if the util socket is reachable — the one hardware test from `docs/no-elfldr.md` Option B); removes elfldr entirely by letting etaHEN spawn the broker.
2. **elfldr `127.0.0.1:9021`** — already up under etaHEN with no user step; optionally bootstrapped via `BREW_UTIL_LAUNCH_ELFLDR`.
3. **websrv `127.0.0.1:8080`** (`/hbldr?...&daemon=1`) — the existing fallback.
4. **Friendly no-loader screen** — "This browser needs a jailbroken PS5 with etaHEN running (it provides the loader used to start the browser's helper process). Could not reach a loader — make sure etaHEN is running, then relaunch."

**Optional, low priority:** if a future need for root `/data`/system paths in the UI itself appears (it has not — ShadowMountPlus covers `/data`), the resident Lapy file-marker path (`/download0/elevate_proc`, no elfldr, MIT-licensed) is the cleanest elevation primitive available and worth revisiting *then*, gated on building a `PRWB00001` helper past the PPSA check. It still would not spawn our children.

## What ProsperoWeb must build (if §4 ever changes and Lapy is wanted)

Nothing for the launch problem today. For completeness, a Lapy filesystem-elevation integration would require: a `PRWB00001` Lapy helper (upstream build tool patched past `PPSA[0-9]{5}`), the cooperative client in a single-threaded startup path (`seteuid(geteuid())` before any thread, write `/download0/elevate_proc`, poll `/download0/lapy_owned_result`, with the elfldr-9021 one-shot handshake as fallback), `downloadDataSize` already set in `param.json`, and a `lapy-manifest.json` hash-verification step in `make-title.sh`. These would touch `src/ps5/spawn/` (a new elevation client), `assets/app.json`/`param.json`, and `scripts/ps5/native/make-title.sh`. None of this is recommended now.

## Licensing (correction to `docs/no-elfldr.md`)

`docs/no-elfldr.md` Option D called Lapy "GPL-3.0-or-later per ProsperoEden". That is wrong. **Lapy is MIT** (Arksama / Team PHU; `LICENSE`), and `source/lapy_elevation_protocol.h` is LGPL-2.1-or-later. ProsperoEden itself is GPL-3.0-or-later and its client code is GPL, but the Lapy helper it ships is MIT (`ProsperoEden headless/elevation/README.md:23` "Lapy's MIT license"). So re-declaring Lapy's 24-byte wire ABI or even vendoring the protocol header is low-burden — but moot, since the recommendation is not to integrate Lapy.

## Answer in one line

ProsperoWeb **cannot** launch without a loader by using Lapy: Lapy only elevates an already-running process (it does not spawn one), it is hard-gated to `PPSA` title IDs, and even a fully-elevated UI still cannot safely `rfork` its children from the foreground big-app context. The no-loader goal stays with etaHEN's `LAUNCH_PLUGIN` / elfldr-9021 / websrv chain plus a friendly fallback screen.
