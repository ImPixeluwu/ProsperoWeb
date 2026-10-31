// SPDX-License-Identifier: MIT-0
//
// dlmalloc (third_party/dlmalloc, Doug Lea, MIT-0) configured for the
// native title's direct-memory heap (native_memory.c): mspaces only, no
// system memory of its own (the block is handed in), locks, and 32-byte
// alignment. The PS5 target assumes 32-byte aligned allocations
// (__STDCPP_DEFAULT_NEW_ALIGNMENT__ is 32; clang stores ymm registers into
// fresh blocks, which faulted on dlmalloc's default 16 in the Stremio PS5
// port, native/heap.c).
#include <errno.h>

#define ONLY_MSPACES 1
#define MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_LOCKS 1
#define MALLOC_ALIGNMENT 32
#define MALLOC_FAILURE_ACTION errno = ENOMEM;
#define ABORT __builtin_trap()
#include "dlmalloc.c"
