// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal interface between the spawn front (spawn_glue.cpp: mode
// selection, broker client, broker main) and the in-process elfldr spawner
// (spawn.c). PS5 target only.
#ifndef PS5BROWSER_SPAWN_BACKEND_H
#define PS5BROWSER_SPAWN_BACKEND_H

#include "ps5browser/spawn/spawn.h"

#ifdef __cplusplus
extern "C" {
#endif

// Why a spawn failed: a static stage name and its errno (0 if none applies).
struct ps5_spawn_failure {
  const char* stage;  // NULL on success
  int err;
  int budget_errno;   // 0 if sys_budget_set(0) succeeded in the child
};

// Spawns elf_path (mapped once per path) with argv/envp, placing fds[i] at
// child fd 3+i. Logs the outcome. Returns the pid, or -1 with *failure set.
// The caller tracks and reaps the child.
int ps5_spawn_elfldr(const char* elf_path, char* const* argv, char* const* envp,
                     const int* fds, int nfds, struct ps5_spawn_failure* failure);

// The in-process spawn mode: ps5_spawn_self()/ps5_terminate_process() as
// they were before the broker (own environ, own reaper thread). Only safe
// when this process is a plain payload, not the hbldr big app.
int ps5_spawn_direct(const char* const* argv, const int* fds, int nfds);
int ps5_terminate_direct(int pid);

#ifdef __cplusplus
}
#endif

#endif  // PS5BROWSER_SPAWN_BACKEND_H
