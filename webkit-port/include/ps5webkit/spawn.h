// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process spawning contract between libWebKit (PS5_HOMEBREW build) and the
// shell. libWebKit only declares these functions; the shell must define them
// (with C linkage) in the final executable, otherwise the link fails with
// undefined references from ProcessLauncherPlayStation.cpp.
//
// Caller: Source/WebKit/UIProcess/Launcher/playstation/ProcessLauncherPlayStation.cpp
// (patched). It creates a SOCK_SEQPACKET socketpair, keeps the server end and
// calls
//
//     const char* argv[] = { "WebProcess", "<processIdentifier>", "3", NULL };
//     int fds[] = { clientFd };
//     int pid = ps5_spawn_self(argv, fds, 1);
//
// and later ps5_terminate_process(pid) when the process is no longer needed.
// The launcher may run on a WebKit work queue thread, not on the main thread.

#ifndef PS5WEBKIT_SPAWN_H
#define PS5WEBKIT_SPAWN_H

#ifdef __cplusplus
extern "C" {
#endif

// Starts a new process running the current executable (the same multi-call
// ELF) and returns its pid, or -1 on failure.
//
//  - argv: NULL-terminated; argv[0] is the role name ("WebProcess" or
//    "NetworkProcess") and must become the child's argv[0], which is what the
//    shell's main() dispatches on. The remaining elements are passed through
//    unchanged (see entry.h for the format). The strings are only valid for
//    the duration of the call; the implementation must copy them.
//  - fds/nfds: descriptors of the caller to pass to the child. fds[i] must be
//    available in the child as fd 3+i (open, blocking mode unchanged,
//    close-on-exec cleared). The child must not inherit any other descriptor
//    besides 0/1/2 (stdio; may point to a log or /dev/null).
//  - The caller keeps ownership of fds[] and closes its copies after the call
//    returns, so the child must hold independent duplicates by then.
//  - The child gets an environment suitable for WebKit (at least the
//    PROSPEROWEB_* variables the UI process has set, if the spawner supports
//    passing an environment).
int ps5_spawn_self(const char* const* argv, const int* fds, int nfds);

// Forcefully terminates a process created by ps5_spawn_self() and reaps it.
// Returns 0 on success, -1 on failure (e.g. unknown or already dead pid).
// Must be safe to call for a process that already exited.
int ps5_terminate_process(int pid);

#ifdef __cplusplus
}
#endif

#endif // PS5WEBKIT_SPAWN_H
