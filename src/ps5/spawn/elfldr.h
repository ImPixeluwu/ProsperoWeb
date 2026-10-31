/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

/* Derived from ps5-payload-dev/elfldr (commit 2a8da002), elfldr.h.
   Changes for ps5-browser: the spawn/exec entry points take a table of
   extra descriptors (possibly of another process) to inject at child fd 3+i
   (see elfldr.c), and the spawn
   reports what happened in the child through elfldr_spawn_report_t, and the
   child's environment is a parameter instead of the caller's environ. */

#pragma once

#include <stdint.h>
#include <unistd.h>

#include "ps5browser/spawn/spawn.h"


/**
 * What a spawn did, for the caller's log. The rfork child shares the
 * parent's address space (RFMEM) and writes the child-side fields itself.
 **/
typedef struct elfldr_spawn_report {
  int budget_errno;          /* 0 if sys_budget_set(0) succeeded in the child */
  const char* failed_stage;  /* NULL on success, else a static stage name */
  int failed_errno;          /* errno of the failed stage, 0 if none applies */
  int child_status;          /* waitpid status if the child died before exec */
} elfldr_spawn_report_t;


/**
 * Spawn a new process that executes the given ELF image, forwarding the
 * descriptors described by fds[0..nfds) so that fds[i].parent_fd appears in
 * the child as fds[i].child_fd. argv is NUL-terminated; argv[0] becomes the
 * child's process name; envp (NUL-terminated) becomes its environment.
 * Returns the child pid, or -1 on failure. report
 * (required) is filled in on success and on failure.
 **/
pid_t elfldr_spawn_fds(char* const argv[], char* const envp[],
                       const struct ps5_spawn_fd_entry* fds,
                       int nfds, const uint8_t* elf, size_t elf_size,
                       elfldr_spawn_report_t* report);


/**
 * Execute an ELF image in a process that is already stopped under ptrace,
 * injecting the given extra descriptors (each rdup'ed from its src_pid, 0
 * meaning our own process). Detaches the process on return. A failed
 * injection sets report->failed_stage/failed_errno.
 **/
int elfldr_exec_fds(pid_t pid, const struct ps5_spawn_fd_entry* fds, int nfds,
                    const uint8_t* elf, elfldr_spawn_report_t* report);


int elfldr_sanity_check(const uint8_t* elf, size_t elf_size);

int elfldr_raise_privileges(pid_t pid);
