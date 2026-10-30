// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entry points of the WebKit auxiliary processes, as built for the PS5
// homebrew port (PS5_HOMEBREW). Implemented inside libWebKit
// (Source/WebKit/{WebProcess,NetworkProcess}/EntryPoint/playstation/*Main.cpp
// with the webkit-port patches applied). Called by the shell.
//
// Process model: the browser is one static multi-call ELF. The UI process is
// the normal `main()` of the shell. When the WebKit process launcher needs a
// WebProcess or NetworkProcess it calls ps5_spawn_self() (see spawn.h), which
// starts another copy of the same ELF with argv[0] set to the role name. The
// shell's `main()` dispatches on argv[0]:
//
//     if (!strcmp(argv[0], "WebProcess"))     return ps5_webkit_web_process_main(argc, argv);
//     if (!strcmp(argv[0], "NetworkProcess")) return ps5_webkit_network_process_main(argc, argv);
//     ... otherwise run the UI process ...
//
// argv format (identical to WebKit's AuxiliaryProcessMain):
//
//     argv[0] = "WebProcess" | "NetworkProcess"   role name
//     argv[1] = "<processIdentifier>"              decimal uint64, WebCore::ProcessIdentifier
//     argv[2] = "<fd>"                             decimal fd of the IPC socket (always "3")
//     argc    = 3
//
// The IPC socket is one end of a SOCK_SEQPACKET socketpair created by the UI
// process; ps5_spawn_self() places it at fd 3 in the child.
//
// Both functions block for the lifetime of the auxiliary process and return
// its exit status. They must be called on the main thread, before any other
// WebKit API is used in that process, and at most once per process.

#ifndef PS5WEBKIT_ENTRY_H
#define PS5WEBKIT_ENTRY_H

#ifdef __cplusplus
extern "C" {
#endif

int ps5_webkit_web_process_main(int argc, char** argv);
int ps5_webkit_network_process_main(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // PS5WEBKIT_ENTRY_H
