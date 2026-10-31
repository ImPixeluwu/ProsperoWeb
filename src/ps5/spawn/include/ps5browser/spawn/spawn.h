// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process spawning contract implemented for the PS5 homebrew port. The WebKit
// static library declares ps5_spawn_self()/ps5_terminate_process() (see
// webkit-port/include/ps5webkit/spawn.h); this library defines them on top of
// an elfldr-derived loader (src/ps5/spawn/elfldr.c, GPL-3.0, see README.md).
//
// The pure (platform-independent) helpers in this header are compiled on the
// host as well, so they can be unit-tested without ptrace or the PS5 kernel.

#ifndef PS5BROWSER_SPAWN_SPAWN_H
#define PS5BROWSER_SPAWN_SPAWN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- The spawn contract (defined only on the PS5 target). -----------------

// Starts a new process running the current executable (the same multi-call
// ELF) with argv[0] as the role name, and places fds[i] at child fd 3+i.
// Returns the child pid, or -1 on failure. See webkit-port spawn.h.
int ps5_spawn_self(const char* const* argv, const int* fds, int nfds);

// Forcefully terminates and reaps a process created by ps5_spawn_self().
// Returns 0 on success, -1 on failure. Safe for an already-dead pid.
int ps5_terminate_process(int pid);

// Runs the spawn broker role: the plain-payload process that performs the
// spawns for the UI (see src/ps5/spawn/README.md, "Spawn broker"). main()
// dispatches the "spawn-broker" role here. Returns the exit code.
int ps5_spawn_broker_main(void);

// --- Logging, provided by the executable (src/frontend/ps5/ps5_log.cpp). ---
//
// The spawner reports every spawn and every child-side failure through these,
// so they reach /data/ps5browser/log.txt and not only stderr. Thread-safe:
// WebKit launches processes from a work queue thread.

void ps5_log_line(const char* line);
void ps5_log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// --- Pure helpers (compiled on host and target; unit-tested on host). ------

// Maximum number of extra descriptors ps5_spawn_self() will forward. The IPC
// model only needs one (the SOCK_SEQPACKET end), but allow a small margin.
#define PS5_SPAWN_MAX_FDS 8

// Resolves the path of our own ELF image, used as the program to re-exec.
// Strategy, in order:
//   1. the PS5BROWSER_SELF_PATH environment variable, if set and non-empty;
//   2. the fixed install path /data/ps5browser/ps5browser.elf;
//   3. argv0, but only if it is an absolute path (begins with '/').
// getenv_fn is the environment lookup (pass the real getenv; tests inject a
// stub). The result is copied into out_path (always NUL-terminated when the
// return value is 0). Returns 0 on success, -1 if nothing resolved or the
// path did not fit in out_len.
int ps5_spawn_resolve_self_path(const char* argv0,
                                const char* (*getenv_fn)(const char*),
                                char* out_path, size_t out_len);

// One entry of the child's extra fd table: the caller's descriptor and the
// descriptor number it must appear as in the child.
struct ps5_spawn_fd_entry {
  int parent_fd;  // descriptor in the calling (UI) process
  int child_fd;   // descriptor number in the spawned child (== 3 + index)
};

// Builds the child fd table from the fds[]/nfds the launcher passed: each
// fds[i] maps to child fd 3+i. Validates nfds against PS5_SPAWN_MAX_FDS and
// rejects negative descriptors. Writes up to out_max entries into out and
// returns their count, or -1 on error.
int ps5_spawn_build_fd_table(const int* fds, int nfds,
                             struct ps5_spawn_fd_entry* out, int out_max);

#ifdef __cplusplus
}
#endif

#endif  // PS5BROWSER_SPAWN_SPAWN_H
