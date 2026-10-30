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
   extra descriptors to inject at child fd 3+i (see elfldr.c). */

#pragma once

#include <stdint.h>
#include <unistd.h>

#include "ps5browser/spawn/spawn.h"


/**
 * Spawn a new process that executes the given ELF image, forwarding the
 * descriptors described by fds[0..nfds) so that fds[i].parent_fd appears in
 * the child as fds[i].child_fd. argv is NUL-terminated; argv[0] becomes the
 * child's process name. Returns the child pid, or -1 on failure.
 **/
pid_t elfldr_spawn_fds(char* const argv[], const struct ps5_spawn_fd_entry* fds,
                       int nfds, const uint8_t* elf, size_t elf_size);


/**
 * Execute an ELF image in a process that is already stopped under ptrace,
 * injecting the given extra descriptors. Detaches the process on return.
 **/
int elfldr_exec_fds(pid_t pid, const struct ps5_spawn_fd_entry* fds, int nfds,
                    const uint8_t* elf);


int elfldr_sanity_check(const uint8_t* elf, size_t elf_size);

int elfldr_raise_privileges(pid_t pid);
