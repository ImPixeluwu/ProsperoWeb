// SPDX-License-Identifier: GPL-3.0-or-later
//
// Support for the native title build of the UI (PRWB00001/eboot.bin,
// PROSPEROWEB_NATIVE_TITLE). See docs/native-title.md.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One line in /data/prosperoweb/boot.txt and klog, written with libkernel
// calls only (native_crt.cpp); text should end in a newline.
void native_boot_say(const char* text);

// The system debug output (klog), printf-style.
void native_debug_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Loads the system modules a title does not get at start-up but the UI
// imports: libSceImeDialog (the system keyboard). Writes one line per
// module into report. Returns the number of modules that failed.
int native_load_system_modules(char* report, size_t report_len);

// Logs how the title's own files can be read (stat, access, open, fopen,
// sceKernelOpen/Stat, opendir) under ui_root and child_root
// (native_trace.c).
void native_file_selftest(const char* ui_root, const char* child_root);

// Moves stdout and stderr (descriptors 1 and 2) to path and gives them
// fresh unbuffered FILEs (native_libc.c). Returns 0 on success.
int native_stdio_redirect(const char* path);

// "fdN sent/received+fds ..." for every descriptor sendmsg/recvmsg used.
void native_trace_io_summary(char* out, size_t len);

// Logs whether the UI can create files in the profile's directories.
void native_profile_probe(const char* base);

// A loopback TCP round trip with this process's own sockets (as the audio
// and video servers use them); switches poll() to select() if it misses data.
void native_socket_selftest(void);
// One log line: can the title reach etaHEN's util daemon command socket
// (AF_UNIX /system_tmp/etaHEN_util_service, docs/no-elfldr.md)? Connects
// and closes at once (nothing is sent), and tries opendir("/system_tmp").
void native_etahen_probe(void);
// config.json "probe_fork" (level 0: nothing): whether the title can create
// a child process itself and the child inherits descriptors. Levels 1/2
// resolve fork/rfork_thread/vfork/pdfork by name (inconclusive on hardware,
// SCE functions are keyed by NID); level 2 adds a WebProcess-like IPC step in
// the child. Levels 3/4 call rfork_thread as a real libkernel NID import:
// level 3 RFPROC|RFCFDG|RFMEM (shared address space, lowest risk), level 4
// RFPROC|RFFDG (copied fd table and address space, a panic candidate). Each
// level is run on its own (native_fork_probe.c). Waits for it 15 s at most.
void native_fork_probe(int level);

// The direct-memory heap (native_memory.c): its size, 0 when the system
// heap is in use, and how much of it is in use.
size_t native_heap_size(void);
size_t native_heap_in_use(void);

#ifdef __cplusplus
}
#endif
