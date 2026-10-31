// SPDX-License-Identifier: GPL-3.0-or-later
//
// Memory for the native title (PRWB00001/eboot.bin).
//
// A title's malloc comes from the system heap, which grows out of its small
// *flexible* memory (a few hundred MB, most of it taken before main() by the
// image itself); the Stremio PS5 port measured 8 MB working and 8 MB + 16
// bytes failing (native/heap.c). Titles have far more *direct* memory, so,
// like that port and the Kodi PS5 port before it, the UI reserves one large
// block of direct memory on first use and runs dlmalloc (third_party/dlmalloc,
// MIT-0) inside it. The link wraps the allocation functions
// (src/frontend/ps5/native/CMakeLists.txt), so every call from the app and
// its static libraries comes here; a pointer outside the block was allocated
// inside a system module (strdup in libSceLibcInternal, ...) and goes back
// to the system.
//
// Anonymous mmap: a title's anonymous memory is flexible memory, and VLC for
// PS5 found plain anonymous mmap refused in a title (ps5/compat/mmap.c). The
// wrapped mmap tries libkernel's mmap, then flexible memory, then direct
// memory (tracked, so munmap releases it).

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "native_title.h"

// dlmalloc, built with ONLY_MSPACES (src/frontend/ps5/native/CMakeLists.txt).
typedef void* mspace;
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
void* mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void* mem);
void* mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
void* mspace_realloc(mspace msp, void* mem, size_t newsize);
void* mspace_memalign(mspace msp, size_t alignment, size_t bytes);
size_t mspace_usable_size(const void* mem);
size_t mspace_footprint(mspace msp);

int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t* start);
int sceKernelMapDirectMemory(void** address, size_t length, int protection, int flags,
                             int64_t start, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t length);
int sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end,
                                       size_t alignment, int64_t* start_out,
                                       size_t* size_out);
int sceKernelMapFlexibleMemory(void** address, size_t length, int protection, int flags);
int sceKernelAvailableFlexibleMemorySize(size_t* available);
int sceKernelMunmap(void* address, size_t length);

void* __real_malloc(size_t size);
void __real_free(void* p);
void* __real_calloc(size_t n, size_t size);
void* __real_realloc(void* p, size_t size);
int __real_posix_memalign(void** out, size_t alignment, size_t size);
void* __real_aligned_alloc(size_t alignment, size_t size);
size_t __real_malloc_usable_size(const void* p);
void* __real_mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset);
int __real_munmap(void* address, size_t length);
int __real_pipe(int fds[2]);

enum {
  kDirectTypeCpu = 12,  // CPU-only direct memory (the type the references map for heaps)
  kProtCpuRead = 0x01,
  kProtCpuWrite = 0x02,
};
#define kDirectAlign ((size_t)0x10000)
#define kMiB ((size_t)1 << 20)

// Direct memory left for everything that is not the heap: the screen
// buffers SDL maps, the video decoder (about 550 MB for a 4K stream, Nuvio
// PS5's measurement) and its compute queue.
#define kReserveForOthers ((size_t)1536 * kMiB)
#define kHeapMax ((size_t)4096 * kMiB)
#define kHeapMin ((size_t)256 * kMiB)

static mspace g_heap;
static uintptr_t g_lo, g_hi;
static size_t g_heap_size;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void* map_direct(size_t size, int64_t* start_out) {
  const int64_t limit = sceKernelGetDirectMemorySize();
  int64_t start = -1;
  if (sceKernelAllocateDirectMemory(0, limit, size, kDirectAlign, kDirectTypeCpu, &start) != 0) {
    return NULL;
  }
  void* address = NULL;
  if (sceKernelMapDirectMemory(&address, size, kProtCpuRead | kProtCpuWrite, 0, start,
                               kDirectAlign) != 0 ||
      !address) {
    sceKernelReleaseDirectMemory(start, size);
    return NULL;
  }
  if (start_out) {
    *start_out = start;
  }
  return address;
}

static void heap_init(void) {
  const int64_t total = sceKernelGetDirectMemorySize();
  size_t cap = total > (int64_t)(kReserveForOthers + kHeapMin)
                   ? (size_t)total - kReserveForOthers
                   : kHeapMin;
  if (cap > kHeapMax) {
    cap = kHeapMax;
  }
  for (size_t size = cap & ~(kDirectAlign - 1); size >= kHeapMin; size -= 256 * kMiB) {
    void* base = map_direct(size, NULL);
    if (!base) {
      continue;
    }
    g_heap = create_mspace_with_base(base, size, 1);
    if (!g_heap) {
      continue;
    }
    g_lo = (uintptr_t)base;
    g_hi = g_lo + size;
    g_heap_size = size;
    native_debug_printf("[prosperoweb] native heap: %zu MiB of direct memory at %p (direct "
                        "memory %lld MiB)\n",
                        size >> 20, base, (long long)(total >> 20));
    return;
  }
  native_debug_printf("[prosperoweb] native heap: no direct memory (total %lld MiB); using "
                      "the system heap\n",
                      (long long)(total >> 20));
}

static inline int heap_ready(void) {
  pthread_once(&g_once, heap_init);
  return g_heap != NULL;
}

static inline int ours(const void* p) {
  return (uintptr_t)p >= g_lo && (uintptr_t)p < g_hi;
}

size_t native_heap_size(void) { return g_heap_size; }

size_t native_heap_in_use(void) { return g_heap ? mspace_footprint(g_heap) : 0; }

void* __wrap_malloc(size_t size) {
  return heap_ready() ? mspace_malloc(g_heap, size) : __real_malloc(size);
}

void __wrap_free(void* p) {
  if (!p) {
    return;
  }
  if (ours(p)) {
    mspace_free(g_heap, p);
  } else {
    __real_free(p);
  }
}

void* __wrap_calloc(size_t n, size_t size) {
  return heap_ready() ? mspace_calloc(g_heap, n, size) : __real_calloc(n, size);
}

void* __wrap_realloc(void* p, size_t size) {
  if (!p) {
    return __wrap_malloc(size);
  }
  if (ours(p)) {
    return mspace_realloc(g_heap, p, size);
  }
  return __real_realloc(p, size);
}

int __wrap_posix_memalign(void** out, size_t alignment, size_t size) {
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  if (!heap_ready()) {
    return __real_posix_memalign(out, alignment, size);
  }
  void* p = mspace_memalign(g_heap, alignment, size);
  if (!p) {
    return ENOMEM;
  }
  *out = p;
  return 0;
}

void* __wrap_aligned_alloc(size_t alignment, size_t size) {
  if (!heap_ready()) {
    return __real_aligned_alloc(alignment, size);
  }
  return mspace_memalign(g_heap, alignment, size);
}

size_t __wrap_malloc_usable_size(const void* p) {
  if (!p) {
    return 0;
  }
  return ours(p) ? mspace_usable_size(p) : __real_malloc_usable_size(p);
}

// ---- anonymous mmap ------------------------------------------------------------

#define kDirectMaps 64
static struct {
  void* address;
  size_t length;
  int64_t start;
} g_direct_maps[kDirectMaps];
static pthread_mutex_t g_direct_lock = PTHREAD_MUTEX_INITIALIZER;

static int remember_direct(void* address, size_t length, int64_t start) {
  int stored = 0;
  pthread_mutex_lock(&g_direct_lock);
  for (int i = 0; i < kDirectMaps && !stored; i++) {
    if (!g_direct_maps[i].address) {
      g_direct_maps[i].address = address;
      g_direct_maps[i].length = length;
      g_direct_maps[i].start = start;
      stored = 1;
    }
  }
  pthread_mutex_unlock(&g_direct_lock);
  return stored;
}

static int forget_direct(void* address, size_t length) {
  int found = 0;
  pthread_mutex_lock(&g_direct_lock);
  for (int i = 0; i < kDirectMaps && !found; i++) {
    if (g_direct_maps[i].address == address && g_direct_maps[i].length == length) {
      sceKernelMunmap(address, length);
      sceKernelReleaseDirectMemory(g_direct_maps[i].start, length);
      g_direct_maps[i].address = NULL;
      found = 1;
    }
  }
  pthread_mutex_unlock(&g_direct_lock);
  return found;
}

void* __wrap_mmap(void* address, size_t length, int protection, int flags, int fd,
                  off_t offset) {
  void* result = __real_mmap(address, length, protection, flags, fd, offset);
  if (result != MAP_FAILED || !(flags & MAP_ANON) || (flags & MAP_FIXED) ||
      protection == PROT_NONE) {
    return result;
  }
  const int saved = errno;
  void* flexible = NULL;
  if (sceKernelMapFlexibleMemory(&flexible, length, protection & (PROT_READ | PROT_WRITE),
                                 0) == 0 &&
      flexible) {
    return flexible;
  }
  if ((protection & PROT_EXEC) == 0) {
    const size_t rounded = (length + kDirectAlign - 1) & ~(kDirectAlign - 1);
    int64_t start = -1;
    void* direct = map_direct(rounded, &start);
    if (direct && remember_direct(direct, length, start)) {
      memset(direct, 0, length);  // mmap's anonymous memory is zeroed
      return direct;
    }
    if (direct) {
      sceKernelMunmap(direct, rounded);
      sceKernelReleaseDirectMemory(start, rounded);
    }
  }
  size_t flexible_left = 0;
  sceKernelAvailableFlexibleMemorySize(&flexible_left);
  native_debug_printf("[prosperoweb] mmap: anonymous %zu KiB (prot %#x) refused: %s; flexible "
                      "memory left %zu MiB\n",
                      length >> 10, (unsigned)protection, strerror(saved), flexible_left >> 20);
  errno = saved;
  return MAP_FAILED;
}

int __wrap_munmap(void* address, size_t length) {
  if (forget_direct(address, length)) {
    return 0;
  }
  return __real_munmap(address, length);
}

// ---- pipe() --------------------------------------------------------------------
//
// The kernel refuses pipe() inside a title's sandbox (Kodi and Stremio PS5,
// native/posix_fixes.c); a local socket pair does the same job.

int __wrap_pipe(int fds[2]) {
  if (__real_pipe(fds) == 0) {
    return 0;
  }
  return socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}
