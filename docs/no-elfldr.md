# Starting the broker without a user-run elfldr or websrv

The native title (`docs/native-title.md`) still cannot spawn WebKit's helper
processes itself: `rfork`+`ptrace`+`execve` from inside the foreground big app
panics the console (`docs/ps5-runtime.md`, "Why a broker"). A separate
plain-payload broker must do the spawning, and today the title asks the user's
**elfldr on `127.0.0.1:9021`** (fallback **websrv**) to start that broker. This
document asks whether a common jailbreak setup can start the broker (or spawn
the WebKit processes) with no user-visible elfldr/websrv step.

Short answer: **yes, and in the common case it already does** — etaHEN bundles
and auto-starts John's elfldr on `9021`, so our existing path needs no manual
step on an etaHEN console. etaHEN also exposes a richer private API (a daemon
that is itself a spawn broker) that can remove the elfldr dependency entirely,
subject to one unverified reachability question (`/system_tmp` from a sandboxed
title).

Claims are **[code]** when read from source in a pinned clone, **[authors]**
when they rest on a project's own docs/README. Commits are the clones read for
this note.

## ProsperoDen / ProsperoEden — does it already solve this?

The operator's "ProsperoDen" is **ProsperoEden** (`blackbearreloaded/ProsperoEden`,
a Switch emulator native title; there is no repo named "ProsperoDen"). It does
**not** introduce a new no-elfldr mechanism — it uses exactly our two
dependencies:

- It spawns its exact-title one-shot helper by **streaming it to the ELF loader
  on TCP `9021`** — "sends the packaged helper to the local ELF loader on TCP
  port 9021. Without that loader …" **[authors]** (`blackbearreloaded/ProsperoEden@a26ba78`,
  `docs/BUILDING.md:19`).
- It escapes the sandbox through **etaHEN**: "Jailbreaking the app (etaHEN's
  app-jailbreak list, or its legacy command server's `JAILBREAK_CMD`)"
  **[authors]** (`@a26ba78`, `docs/FW740.md:29,4`).
- Its "runtime launcher packs" are **Lua presentation packs, not native payload
  loaders** — "It does not replace the emulator or load native plugins"
  **[authors]** (`@a26ba78`, `docs/RUNTIME_LAUNCHERS.md`).

So ProsperoEden corroborates the etaHEN approach below but gives us nothing that
removes elfldr. It is a second witness, not a solution.

## etaHEN is the key (`LightningMods/etaHEN@dafa13b`)

etaHEN runs a persistent root daemon that is, in effect, a general-purpose
spawn broker with several front doors.

### 1. It already runs elfldr on 9021 for us

The README lists, as standard resident services: "elf loader on port 9021 (use
Johns elfldr)" and "Direct PKG installer service on port 9090", plus a
klog server on `9081` and a command server **[authors]** (`Source Code/README.md:90-96`).
etaHEN bundles `elfldr.elf` (`Source Code/lib/elfldr.elf.h`) and spawns it with
its own in-daemon loader; the util daemon even re-launches it on demand via the
`BREW_UTIL_LAUNCH_ELFLDR = 0xE1F1D8` IPC ("Special command to launch Johns
elfldr for lite mode … not used if already running") **[code]**
(`Source Code/include/msg.hpp:58-59`; handler `Source Code/util/source/msg.cpp:535-542`:
`elfldr_spawn("/", STDOUT_FILENO, elfldr_start, "elfldr.elf")`).

Consequence: on a normal etaHEN console the loopback endpoint our title already
targets (`PROSPEROWEB_ELFLDR=127.0.0.1:9021`) is up with **no user action**.
Loopback TCP is reachable from the uid-1 sandbox — our broker already uses
loopback TCP `17771` successfully (`docs/ps5-runtime.md`). This is the
lowest-risk answer and needs no code change.

### 2. It can spawn an arbitrary ELF for us — no elfldr at all

The daemon's IPC command `BREW_UTIL_LAUNCH_PLUGIN` takes a JSON
`{"plugin_path","title_id"}` and calls `load_plugin(path)` **[code]**
(`Source Code/util/source/msg.cpp:433-448`). For a `.elf` file `load_plugin`
reads it and runs etaHEN's **own** loader: `pid = elfldr_spawn("/",
STDOUT_FILENO, buf, titleID)` **[code]** (`Source Code/util/source/common_utils.c:443,528-534`).

`elfldr_spawn` is the same privileged mechanism our `src/ps5/spawn/` broker
uses: `sceKernelSpawn` the SceSpZeroConf trampoline
(`/system/vsh/app/NPXS40112/eboot.bin`), `elfldr_raise_privileges` (root vnode
as rootdir, jaildir 0, uid 0, full `caps` 0xff…), then ptrace/mdbg ELF
injection **[code]** (`Source Code/bootstrapper/source/elfldr.c:690-702`, `77`,
`392-413`). Crucially this runs **inside the etaHEN daemon (a plain payload),
not inside our foreground big app**, so it is exactly the "spawn from a plain
payload works" context from `docs/ps5-runtime.md` — it cannot trigger the
big-app rfork panic.

So the title could hand etaHEN the path to `prosperoweb-broker.elf` and etaHEN
would start it, replacing both elfldr-9021 and websrv.

**The open question — transport reachability.** The daemon binds its command
sockets as **AF_UNIX** at `/system_tmp/etaHEN_crit_service` and
`/system_tmp/etaHEN_util_service` **[code]** (`Source Code/include/msg.hpp:10-12`;
bind `Source Code/daemon/source/msg.cpp:481-495`; message framing is
`struct IPCMessage{int magic=0xDEADBABE; DaemonCommands cmd; int error; char
msg[0x1000];}` at `msg.hpp:70-75`). etaHEN's own clients that use these sockets
(`shellui`, `fps_elf`) are code **injected into system processes**, not a uid-1
homebrew sandbox (`Source Code/shellui/include/ipc.hpp:84-102`). Whether a
sandboxed native title can `connect()` a socket under `/system_tmp` is **not
verified** — `/data` is mounted into app sandboxes by default
(`Allow_data_in_sandbox`, `README`), but `/system_tmp` is not documented as
mounted. There is **no TCP equivalent** of `LAUNCH_PLUGIN` (the TCP servers are
elfldr `9021`, jailbreak `9028`, PKG-install `9090` only), so this option lives
or dies on that one test.

### 3. It can jailbreak our title (self-elevation) — TCP, non-whitelist

etaHEN exposes a documented "Jailbreak IPC call (jailbreaks Homebrew apps)"
**[authors]** (`README:85`). Two routes **[code]**:

- **TCP `127.0.0.1:9028`, any app** — connect, send
  `struct HijackerCommand{int magic=0xDEADBEEF; Commands cmd=JAILBREAK_CMD; int
  PID=getpid(); …}`, read the reply (`README:140-205`, "non-whitelist method").
  The handler resolves the caller's pid and runs `Hijacker::jailbreak(true)`
  (root vnode rootdir + ucred) (`Source Code/daemon/source/commands.cpp` enum
  `:78-91`, handler `:662-733`, `jailbreak_proc` `:174-176`).
- **Whitelist fifo** — a whitelisted app drops JSON at
  `/mnt/sandbox/<TID>_000/download0/etahen_jailbreak` and the daemon polls it
  (`commands.cpp:662-733`). The whitelist is **hard-coded and not
  config-extendable**: `ITEM00001`, `NPXS39041`, `DUMP00000`, `PKGI13337`,
  `PKGI12345`, `TOOL00001`, and any TID containing `"LAPY"` (`commands.cpp:377-398`).
  Our `PRWB00001` is not on it, so only the TCP route applies to us.

Jailbreaking only grants the UI privileges; it does **not** make an in-process
rfork safe (the panic is about the big-app context, not credentials —
`docs/ps5-runtime.md`: hbldr never rforks from the big app either). So
self-elevation alone does not let the UI spawn its children; it would still need
a separate broker process. It is useful only if we later need kernel R/W in the
UI itself.

## Other setups surveyed

| Source (pinned) | Relevant capability | Verdict |
|---|---|---|
| **kstuff / kstuff-lite** (`ps5-payload-dev/kstuff@3962120`, `env-kstuff`) | Kernel toolkit: fself/fpkg, syscall hooks, sqlite triggers. **No title-facing API** — no IPC socket, no jailbreak-for-apps call, C payload only, no README surface. | Enabler only. It is what etaHEN/elfldr/ShadowMount sit on; a title talks to etaHEN, not kstuff. |
| **ShadowMountPlus** (`drakmor/shadowMountPlus@30c5a2b`) | Background **auto-mounter** for game dumps; registers the tile and mounts `/data`, `/mnt/usbN`, `/mnt/extN` into the sandbox (`README:11`, scan paths per `native-title.md`). | Orthogonal: it gets our tile mounted/launched, which we already rely on. **No "run payload on title start"/plugin/autoload** service. |
| **ProsperoStore** (`darkxex/ProsperoStore@22d5a44`) | Streams helper ELFs (installer worker, `self-updater.elf`) to the **loader on `9021`**, and gets elevation from **elfldr OR a resident Lapy JB daemon** (`README:39-42,93`; `PLAN.md:194-195`; `IMPLEMENTATION_NOTES.md:25,170`). | Same two dependencies as us + introduces **PS5-Lapy-JB-Daemon** (`mpereiraesaa/PS5-Lapy-JB-Daemon`) as an alternative resident jailbreak service (note the `"LAPY"` whitelist hook above). |
| **Itemzflow** (`illusion0001/Itemzflow@6744d42`) | `ITEM00001` is **hard-coded in etaHEN's whitelist**; etaHEN launches and jailbreaks it automatically. It carries no elfldr/jailbreak client of its own. | Model for the whitelist path — but that path is closed to our non-whitelisted TID. |
| **OpenNOW** (`OpenCloudGaming/OpenNOW-PS5@2a2349b`) | Native title that streams a helper ELF to elfldr `9021` (already cited in `native-title.md`). | Same pattern we use; confirms elfldr-9021 is the norm. |

## Options

| Option | How | Pros | Cons / Risk |
|---|---|---|---|
| **A. etaHEN bundled elfldr 9021 (already running)** | Keep current client; target `127.0.0.1:9021`. Optionally send `BREW_UTIL_LAUNCH_ELFLDR` first to guarantee it is up. | No code change; works today on any etaHEN console; TCP loopback is sandbox-reachable (proven by our `17771` broker). | Still "uses elfldr", just not a user-run one. Depends on the console running etaHEN. **Low risk.** |
| **B. etaHEN `BREW_UTIL_LAUNCH_PLUGIN`** | Title connects AF_UNIX `/system_tmp/etaHEN_util_service`, sends `IPCMessage{0xDEADBABE, BREW_UTIL_LAUNCH_PLUGIN, json:{plugin_path:<broker.elf>,title_id:"PRWB"}}`; etaHEN spawns the broker via its own loader. | **Removes elfldr and websrv entirely**; etaHEN does the privileged spawn in a safe plain-payload context. | **AF_UNIX under `/system_tmp` reachability from a uid-1 sandbox is UNVERIFIED**; no TCP fallback for this command; etaHEN-specific. **Med–High risk until a one-line connect test on hardware.** |
| **C. etaHEN jailbreak IPC (TCP 9028)** | Self-elevate the title (uid 0 + rootdir). | Non-whitelist, TCP, reachable; grants kernel R/W to the UI. | Does **not** make in-process spawn safe (big-app panic persists); still needs a separate broker. Useful only as a building block. **Not a standalone solution.** |
| **D. PS5-Lapy-JB-Daemon** | Resident JB daemon (used by ProsperoStore); a `"LAPY"`-named TID is auto-whitelisted. | Alternative to etaHEN for consoles that run Lapy. | Extra dependency, less common than etaHEN; still needs a loader to spawn our ELF. **Med risk.** |
| **E. websrv `/hbldr?daemon=1`** (today's fallback) | Unchanged. | Works where websrv is installed. | User-visible websrv tile; the thing we want to stop requiring. **Keep as fallback only.** |

## Recommendation

1. **Ship Option A as the baseline and state etaHEN as the requirement.** Our
   existing elfldr-9021 client already works unchanged on an etaHEN console
   because etaHEN auto-starts that loader. Before the first spawn, if `9021`
   does not answer, send etaHEN's `BREW_UTIL_LAUNCH_ELFLDR` (over the util
   socket if reachable) to bring it up; otherwise fall through.
2. **Pursue Option B as the "no elfldr" endgame, gated on one hardware test:**
   from the running native title, `connect()` an `AF_UNIX` socket to
   `/system_tmp/etaHEN_util_service` and read back a `BREW_UTIL_TEST_CONNECTION`
   reply. If it connects, implement the `LAUNCH_PLUGIN` client and make it the
   first loader; this deletes the elfldr/websrv requirement cleanly. If it does
   not connect, Option B is dead and A stands.
3. Keep **C** in reserve only if the UI later needs its own kernel R/W.

### Fallback order (the `PROSPEROWEB_BROKER_LOADER` chain)

`etahen-plugin` (Option B, if the util socket is reachable) → `elfldr`
(`127.0.0.1:9021`, already up under etaHEN; optionally bootstrapped via
`BREW_UTIL_LAUNCH_ELFLDR`) → `websrv` (`127.0.0.1:8080`) → **friendly error
screen** ("This browser needs a jailbroken PS5 running etaHEN. Could not reach
etaHEN's loader — make sure etaHEN is running, then relaunch.").

### Changes in our tree (if Option B is adopted)

- `src/ps5/spawn/broker_client.cpp` — add an etaHEN IPC client: open
  `UTIL_IPC_SOC`, frame `IPCMessage` (magic `0xDEADBABE`), send
  `BREW_UTIL_LAUNCH_PLUGIN` with `{plugin_path,title_id}`; add
  `BREW_UTIL_LAUNCH_ELFLDR` as the 9021 bootstrap.
- A small header for etaHEN's constants (socket paths, `DaemonCommands`,
  `IPCMessage`) under `src/ps5/spawn/` — these are GPLv3 in etaHEN, so
  re-declare the ABI rather than copy the file, or depend on the published
  **etaHEN-SDK** (`LightningMods/etaHEN-SDK`, `README:98-100`).
- `docs/ps5-runtime.md` env table — add `etahen` as a `PROSPEROWEB_BROKER_LOADER`
  value and document the order above; `broker_path` already resolves via
  `PROSPEROWEB_BROKER_PATH`/`PROSPEROWEB_SELF_PATH`.
- `docs/native-title.md` prerequisites — fold the new chain in.

### User-facing requirement (README / store listing)

> Requires a jailbroken PS5 running **etaHEN** and **ShadowMountPlus**. etaHEN
> provides the ELF loader (port 9021) the browser uses to start its helper
> process — **no manual payload, elfldr, or websrv step is needed**. (websrv is
> an optional fallback.)

## What was read

- `LightningMods/etaHEN@dafa13b` — `Source Code/include/msg.hpp`,
  `daemon/source/{msg,commands,main}.cpp`, `util/source/{msg.cpp,common_utils.c}`,
  `bootstrapper/source/elfldr.c`, `shellui/include/ipc.hpp`, `README.md`.
- `blackbearreloaded/ProsperoEden@a26ba78` — `docs/{BUILDING,FW740,RUNTIME_LAUNCHERS}.md`.
- `darkxex/ProsperoStore@22d5a44` — `README.md`, `PLAN.md`, `IMPLEMENTATION_NOTES.md`.
- `drakmor/shadowMountPlus@30c5a2b` — `README.md`.
- `illusion0001/Itemzflow@6744d42`; `OpenCloudGaming/OpenNOW-PS5@2a2349b`;
  `ps5-payload-dev/kstuff@3962120`.
- Context: `docs/native-title.md`, `docs/ps5-runtime.md`, `docs/spike-inprocess-webkit.md`.
