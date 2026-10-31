# ps5browser_spawn

The process spawner for the PS5 frontend. It defines the WebKit spawn contract
(`webkit-port/include/ps5webkit/spawn.h`): `ps5_spawn_self()` starts another
copy of our own multi-call ELF with a role name in `argv[0]` and the WebKit IPC
socket at child fd 3, and `ps5_terminate_process()` kills and reaps it.

## Origin and NOTICE

`elfldr.c`, `elfldr.h`, `pt.c`, `pt.h` and `log.h` are **derived from
[ps5-payload-dev/elfldr](https://github.com/ps5-payload-dev/elfldr)**, commit
`2a8da002e5292481ea964daa7342bbf4855f4c33`, Copyright (C) 2024 John Törnblom,
GPL-3.0-or-later. The original copyright headers are kept verbatim.

Changes made for ps5-browser (also noted in each file's header):

- `pt.c`, `pt.h`, `log.h`: copied verbatim.
- `elfldr.c` / `elfldr.h`: trimmed to the spawn path and extended for
  descriptor passing. `elfldr_exec()` became `elfldr_exec_fds()` and
  `elfldr_spawn()` became `elfldr_spawn_fds()`: instead of duplicating a single
  `stdio` descriptor onto 0/1/2, the loader keeps the `deci_std*` descriptors
  the rfork entry opens as stdio and injects a caller-supplied table of extra
  descriptors at child fd `3+i` (`pt_rdup` + `pt_dup2`). The current
  environment is forwarded to the child (`elfldr_set_environ`, taken from the
  shsrv variant of elfldr) so the role process sees the `PS5BROWSER_*`
  variables. `elfldr_read()` and `elfldr_find_pid()` were dropped as unused.
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

`spawn.c` and `spawn_util.c` are original to ps5-browser (GPL-3.0-or-later).
`spawn.c` maps the self ELF once (read-only `MAP_PRIVATE`, read into memory
only if `mmap` fails), tracks every child until it is reaped: by
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
fails, keeps the budget the child inherited from the UI (the big app's).
The result is logged for every spawn, e.g.

    spawn: role=WebProcess pid=1120 fds=[25->3] ok in 41ms, budget_set(0) failed: ENOENT (continuing with inherited budget)

Whether the inherited big-app budget gives WebProcess enough memory is
still to be confirmed on the console.

## Logging

`spawn.c` writes through `ps5_log_line()`/`ps5_log_printf()` (declared in
`ps5browser/spawn/spawn.h`, defined by the frontend's `ps5_log.cpp`), so
spawn results, failed stages and child exits reach
`/data/ps5browser/log.txt`. A spawn that fails makes WebKit's launcher
(patch 0005) print its own line to stderr only; the `spawn: ... FAILED`
line written just before it carries the same role and result.
