// SPDX-License-Identifier: GPL-3.0-or-later
//
// Support for the native title build of the UI (PRWB00001/eboot.bin,
// PROSPEROWEB_NATIVE_TITLE). See docs/native-title.md.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The system debug output (klog), printf-style.
void native_debug_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Loads the system modules a title does not get at start-up but the UI
// imports: libSceImeDialog (the system keyboard). Writes one line per
// module into report. Returns the number of modules that failed.
int native_load_system_modules(char* report, size_t report_len);

// The direct-memory heap (native_memory.c): its size, 0 when the system
// heap is in use, and how much of it is in use.
size_t native_heap_size(void);
size_t native_heap_in_use(void);

#ifdef __cplusplus
}
#endif
