# prosperobrowser_spawn

The process spawner for the PS5 frontend. It defines the WebKit spawn contract
(`webkit-port/include/ps5webkit/spawn.h`): `ps5_spawn_self()` starts another
copy of our own multi-call ELF with a role name in `argv[0]` and the WebKit IPC
socket at child fd 3, and `ps5_terminate_process()` kills and reaps it.

## Spawn broker

The UI runs as the hbldr big app, and `rfork` + `ptrace` + `execve` from a
big app panics the console kernel (console run of 2026-10-31: panic right
after `spawn: role=WebProcess starting`). The same code works from a plain
payload. So by default (`PROSPEROBROWSER_SPAWN_MODE=broker`) the UI does not
spawn itself: it asks the **spawn broker**, the same ELF in the
`spawn-broker` role, which websrv starts as a daemon payload on the first
spawn (`GET /hbldr?...&daemon=1`, see `docs/ps5-runtime.md`, "Launch
sequence").

Protocol (`broker_proto.{h,cpp}`): length-prefixed JSON frames (4-byte
big-endian length, then a JSON object) over a stream socket.

Transport selection. The console refuses to bind an AF_UNIX socket under
`/data` (`bind: Operation not supported`, console run 3 of 2026-10-31), so
both sides walk the same list:

1. AF_UNIX `$PROSPEROBROWSER_SPAWN_SOCK` (if set),
2. AF_UNIX `/tmp/prosperobrowser-spawn.sock`,
3. AF_UNIX `/data/prosperobrowser/tmp/spawn.sock`,
4. TCP `127.0.0.1:$PROSPEROBROWSER_SPAWN_PORT` (default `17771`).

The broker binds the first AF_UNIX path that works (mode `0666`; every
failed bind is logged with its errno) and **also** listens on loopback TCP,
because the UI, a big app, may not see the broker's filesystem the same way;
it exits only if it can listen on nothing. The UI connects to the first
endpoint that answers, in the same order.

Descriptors. Over AF_UNIX they are `SCM_RIGHTS` ancillary data on the frame
that announces them (`nfds`); the broker then owns copies and the loader
rdups them from the broker itself. Over TCP that is impossible, so the
frame names them instead: `"src_pid":<UI pid>,"src_fds":[13]`, `nfds` 0.
The loader rdups them straight out of the UI process into the stopped
child (`pt_rdup(child, src_pid, fd)` + `pt_dup2(child, tmp, 3+i)`, see
`elfldr_inject_fds`). The UI keeps the descriptors open until the broker's
reply arrives: `ps5_spawn_self()` is synchronous and WebKit's launcher
(patch 0005) closes the client end only after it returns.

| Direction | Frame | Reply |
|---|---|---|
| UI → broker (AF_UNIX) | `{"op":"spawn","elf":"/data/prosperobrowser/prosperobrowser.elf","argv":[...],"env":[...],"nfds":1}` + fds | `{"ok":true,"pid":N}` or `{"ok":false,"stage":"...","errno":N,"error":"..."}` |
| UI → broker (TCP) | `{"op":"spawn","elf":...,"argv":[...],"env":[...],"nfds":0,"src_pid":93,"src_fds":[13]}` | same; a refused rdup is `"stage":"rdup"` with its errno |
| UI → broker | `{"op":"terminate","pid":N}` | `{"ok":true}` (also for an already reaped pid) |
| UI → broker | `{"op":"ping"}` | `{"ok":true,"pid":<broker pid>}` |
| broker → UI | `{"ev":"exited","pid":N,"status":<wait status>}` | — |

`env` is the UI's whole `environ`, so the children see the UI's
environment, not the broker's. The descriptors (received or named) land at
child fd 3, 4, … in order.

Broker (`broker_server.{h,cpp}`, run by `ps5_spawn_broker_main()` in
`spawn_glue.cpp`): a single-threaded accept/request loop plus a reaper
thread that polls the tracked children and sends `exited`. One client at a
time; a new connection replaces the old one. The UI keeps its connection
for its whole lifetime (`g_client` in `spawn_glue.cpp` is never closed, only
replaced after a failed request), so the broker lives exactly as long as
that connection: when it closes, the broker logs `broker: client
disconnected, terminating N children and exiting`, kills the children and
exits. websrv, which reads the whole ELF to start a broker, is therefore
asked at most once per UI run. It also exits on `SIGTERM`, and 10 s after
start if no client ever connected. The
actual spawn is a hook (`broker_set_spawn_hook`): `ps5_spawn_elfldr()` from
`spawn.c` on the console, a `fork()` fake in `tests/ps5`.

Client (`broker_client.{h,cpp}`, used by `spawn_glue.cpp`): one connection
with a reader thread that hands replies to the waiting request and logs
`exited` events; the raw HTTP/1.0 GET that asks websrv to start the broker
(`PROSPEROBROWSER_WEBSRV`, default `127.0.0.1:8080`; it forwards
`PROSPEROBROWSER_SPAWN_SOCK` if set and `PROSPEROBROWSER_SPAWN_PORT`). After asking
websrv the UI polls every endpoint for up to 5 s. If the broker cannot be
reached the spawn fails, unless `PROSPEROBROWSER_SPAWN_FALLBACK=direct`. After two
such failures in a row the UI waits 30 s before the next attempt (WebKit
relaunches a failed process immediately and forever, and each attempt would
ask websrv for another broker); a success clears the back-off.

`PROSPEROBROWSER_SPAWN_MODE=direct` keeps the in-process spawner
(`ps5_spawn_direct()` in `spawn.c`, with its own reaper thread). Use it only
when the UI itself is a plain payload (an elfldr launch).

Expected `log.txt` lines on the first spawn when no AF_UNIX path binds
(the console as of run 3; UI pid 92):

    [ 95 UI] spawn: mode broker
    [ 95 UI] spawn: broker connecting, trying unix /tmp/prosperobrowser-spawn.sock, unix /data/prosperobrowser/tmp/spawn.sock, tcp 127.0.0.1:17771
    [ 96 UI] spawn: broker not running (unix /tmp/prosperobrowser-spawn.sock: No such file or directory; unix /data/prosperobrowser/tmp/spawn.sock: No such file or directory; tcp 127.0.0.1:17771: Connection refused), asking websrv
    [ 12 broker] main: pid=101 ppid=93 argv0=spawn-broker
    [ 14 broker] broker: bind unix /tmp/prosperobrowser-spawn.sock: errno 45 (Operation not supported)
    [ 15 broker] broker: bind unix /data/prosperobrowser/tmp/spawn.sock: errno 45 (Operation not supported)
    [ 16 broker] broker: listening on tcp 127.0.0.1:17771
    [ 16 broker] broker: ready, pid 101
    [900 UI] websrv: GET http://127.0.0.1:8080/hbldr?path=... -> HTTP 200 in 800ms
    [905 UI] spawn: broker connected via tcp 127.0.0.1:17771 (descriptors passed by rdup from this pid)
    [ 20 broker] broker: client connected over tcp, 0 children alive
    [906 UI] spawn: broker ready pid=101 in 810ms
    [ 21 broker] broker: role=WebProcess takes fds [13] from pid 92 (rdup)
    [ 21 broker] spawn: self ELF /data/prosperobrowser/prosperobrowser.elf (... bytes, mapped), fw 0x..., pid 101
    [ 22 broker] spawn: role=WebProcess starting, fds=[92:13->3]
    [ 58 broker] spawn: rdup(src_pid=92, fd=13) -> child fd 3
    [ 60 broker] spawn: role=WebProcess pid=102 fds=[92:13->3] ok in 38ms, budget_set(0) ok
    [ 60 broker] broker: role=WebProcess pid=102 spawned in 39ms
    [950 UI] spawn: role=WebProcess pid=102 via broker (tcp, rdup) in 45ms

If the kernel refuses the cross-process rdup, the broker logs
`spawn: rdup(src_pid=92, fd=13) FAILED: errno N (...)` and
`spawn: role=WebProcess FAILED at rdup: ...`, and the UI
`spawn: role=WebProcess FAILED via broker at rdup: errno N (...)`.
With a working AF_UNIX path the broker logs `listening on unix <path>`, the
UI `connected via unix <path>`, the fd table reads `fds=[5->3]` and the
rdup source is the broker's own pid.

## Origin and NOTICE

`elfldr.c`, `elfldr.h`, `pt.c`, `pt.h` and `log.h` are **derived from
[ps5-payload-dev/elfldr](https://github.com/ps5-payload-dev/elfldr)**, commit
`2a8da002e5292481ea964daa7342bbf4855f4c33`, Copyright (C) 2024 John Törnblom,
GPL-3.0-or-later. The original copyright headers are kept verbatim.

Changes made for ProsperoBrowser (also noted in each file's header):

- `pt.c`, `pt.h`, `log.h`: copied verbatim.
- `elfldr.c` / `elfldr.h`: trimmed to the spawn path and extended for
  descriptor passing. `elfldr_exec()` became `elfldr_exec_fds()` and
  `elfldr_spawn()` became `elfldr_spawn_fds()`: instead of duplicating a single
  `stdio` descriptor onto 0/1/2, the loader keeps the `deci_std*` descriptors
  the rfork entry opens as stdio and injects a caller-supplied table of extra
  descriptors at child fd `3+i` (`pt_rdup` + `pt_dup2`), each copied from
  the process named in its table entry (`src_pid`: the broker itself, or
  the UI when the broker runs over TCP); every rdup/dup2 is logged with
  its errno and a failed one is reported as stage `rdup`/`dup2`. The child's
  environment is an `envp` argument (`elfldr_set_environ`, taken from the
  shsrv variant of elfldr): the UI's environment, forwarded by the broker,
  so the role process sees the `PROSPEROBROWSER_*` variables. `elfldr_read()` and `elfldr_find_pid()` were dropped as unused.
  The load mirror is `calloc`ed so the `p_filesz`..`p_memsz` gaps reach the
  child as zeros, an already open child descriptor replaced by `pt_dup2` is
  logged, and a child that fails during setup is reaped after `SIGKILL`.
  The `EVFILT_PROC`/`NOTE_EXEC` registration after `rfork_thread` is kept in
  upstream's order (elfldr, shsrv and websrv all do the same); a failed
  registration (a child that already exited) no longer skips the `waitpid`.
  A failing `sys_budget_set(0)` in the rfork child is not fatal (see
  "Budget"), and every child-side failure (`open /dev/deci_std*`,
  `ptrace(PT_TRACE_ME)`, `execve`) and parent-side failure is reported to
  the caller through `elfldr_spawn_report_t`.

Kernel read/write comes from the payload arguments parsed by the ps5-payload
SDK crt; the loader uses `kernel_*` from `<ps5/kernel.h>` where upstream elfldr
used its own kernel helpers.

`spawn.c`, `spawn_util.c`, `spawn_glue.cpp`, `spawn_backend.h` and
`broker_*` are original to ProsperoBrowser (GPL-3.0-or-later).
`spawn.c` maps the ELF once (read-only `MAP_PRIVATE`, read into memory
only if `mmap` fails), and, in direct mode, tracks every child until it is reaped: by
`ps5_terminate_process()` (`SIGKILL`, then a bounded wait) or, for children
that exit on their own, by a reaper thread polling the tracked pids once a
second (never `waitpid(-1)`, which would also consume the loader's ptrace
stops).
`spawn_util.c` is pure (no ptrace/kernel) and is unit-tested on the host
(`tests/ps5`).

## Budget

Upstream elfldr's rfork entry calls `sys_budget_set(0)` (syscall `0x23b`)
in the child and aborts the spawn if it fails (`elfldr.c`, rfork entry;
`selfldr.c` does the same). That is right for a payload started by elfldr,
which runs where budget 0 exists. Started from the websrv tile, the UI is
the hbldr big app (FAKE00000), and there the call fails with `ENOENT`: on
the first console run (firmware 10.00) every WebProcess/NetworkProcess
spawn died before `execve`, so nothing was ever painted.

hbldr does not know a better budget id for a big app either: it issues the
same `sys_budget_set(0)` on the big app itself through `pt_syscall` and
ignores the result, and only when `kernel_get_fw_version() > 0x10000000`
because on older firmware it can panic the kernel when the big app is
restarted (websrv `src/ps5/hbldr.c:390-396`, commit `7748df9`). websrv's
own elfldr copy keeps the fatal call in its rfork entry
(`src/ps5/elfldr.c:626-631`). So the spawner tries budget 0 and, when that
fails, keeps the budget the child inherited from its parent. With the spawn
broker the parent is a plain payload again, where budget 0 exists; the
non-fatal handling stays for direct mode.
The result is logged for every spawn, e.g.

    spawn: role=WebProcess pid=1120 fds=[25->3] ok in 41ms, budget_set(0) failed: ENOENT (continuing with inherited budget)

Whether the inherited big-app budget gives WebProcess enough memory is
still to be confirmed on the console.

## CPU placement

Children spawned elfldr-style (the broker, and from it WebProcess and
NetworkProcess) land in the console's system cpuset `9,11,13-15` (5 logical
CPUs), while the UI big app runs on the app cores `0-12`. An unprivileged
process cannot move itself out: `cpuset_setaffinity` returns `EPERM` at the
kernel's `priv_check(PRIV_SCHED_CPUSET)`, and `cpuset_setid` fails likewise.
This starves the WebProcess media decoder (ffmpeg, 6 threads) and drops
frames.

The broker already holds kernel read/write (elfldr). After a child starts it
raises its *own* credentials to uid 0 with full capabilities
(`cpuset_assign_ps5.cpp`), which makes the priv check pass, then as a
privileged caller moves the child onto the app cores: it joins the child to
the UI's cpuset (`cpuset_setid`, the UI's id from
`cpuset_getid(CPU_LEVEL_CPUSET, …, ui_pid)`) and/or widens the child's mask
(`cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, …)`), then restores its
own credentials. The decision (which mechanism, which mask, clipped to the
root set) is portable code in `src/ps5/cpu/cpuset_assign.{h,cpp}`, covered by
the host tests; the syscalls are here. `PROSPEROBROWSER_CHILD_CPU` selects the
mechanism (`off`/`0`, `setid`, `mask`, default auto). The broker also widens
itself to the root set at start-up. Every attempt is logged with its errno:

    cpu: child pid 1120 ui pid 1000 -> cpuset id 3: ok; mask 0x1fff: skipped | before 9,11,13-15 (0xea00), after 0-12 (0x1fff) | join ui set

## Logging

`spawn.c` writes through `ps5_log_line()`/`ps5_log_printf()` (declared in
`prosperobrowser/spawn/spawn.h`, defined by the frontend's `ps5_log.cpp`), so
spawn results, failed stages and child exits reach
`/data/prosperobrowser/log.txt`. A spawn that fails makes WebKit's launcher
(patch 0005) print its own line to stderr only; the `spawn: ... FAILED`
line written just before it carries the same role and result.
