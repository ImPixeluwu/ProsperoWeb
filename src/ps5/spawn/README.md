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

Kernel read/write comes from the payload arguments parsed by the ps5-payload
SDK crt; the loader uses `kernel_*` from `<ps5/kernel.h>` where upstream elfldr
used its own kernel helpers.

`spawn.c` and `spawn_util.c` are original to ps5-browser (GPL-3.0-or-later).
`spawn_util.c` is pure (no ptrace/kernel) and is unit-tested on the host
(`tests/ps5`).
