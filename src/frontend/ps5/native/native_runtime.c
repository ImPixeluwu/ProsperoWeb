// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the payload SDK's crt1.o and libc.a give a payload, for the native
// title (PRWB00001/eboot.bin), which starts from the boilerplate's app_crt
// instead (docs/native-title.md, "Native link").
//
// - The payload crt's kernel R/W helpers (kernel_*), klog and the SDK rtld
//   behind dlopen() do not exist in a title: the title runs with ordinary
//   application credentials and no kernel access. The frontend still links
//   the code that calls them (the direct spawner, the cpuset helper, the
//   decoder's credential log); here every kernel_* call fails cleanly, klog
//   goes to the system debug output, and dlopen() uses libkernel's own
//   module loader.
// - libc.a implements a few calls with the syscall instruction itself. A
//   title may not do that (the console kills it: "directly issued a
//   syscall", Stremio PS5's native/kernel_mem.c). The native link drops
//   libc.a's syscall objects (src/frontend/ps5/native/CMakeLists.txt); most
//   of the calls then come from libkernel, and the rest are here, failing
//   with ENOSYS. scripts/ps5/native/check-eboot.sh verifies that no syscall
//   instruction is left in the image.

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

int sceKernelDebugOutText(int channel, const char* text);
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags,
                             void* option, int* result);
int sceKernelStopUnloadModule(int handle, size_t args, const void* argp, uint32_t flags,
                              void* option, int* result);
int sceKernelDlsym(int handle, const char* symbol, void** address);

// ---- klog --------------------------------------------------------------------

int klog_puts(const char* s) {
  char line[512];
  snprintf(line, sizeof(line), "%s\n", s ? s : "");
  return sceKernelDebugOutText(0, line);
}

int klog_printf(const char* fmt, ...) {
  char line[1024];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  sceKernelDebugOutText(0, line);
  return n;
}

int klog_perror(const char* s) {
  return klog_printf("%s: %s\n", s ? s : "", strerror(errno));
}

// ---- kernel R/W: not available to a title -------------------------------------

const intptr_t KERNEL_ADDRESS_DATA_BASE = 0;

uint32_t kernel_get_fw_version(void) { return 0; }
intptr_t kernel_get_proc_file(pid_t pid, int fd) {
  (void)pid;
  (void)fd;
  return 0;
}
int kernel_mprotect(pid_t pid, intptr_t addr, size_t size, int prot) {
  (void)pid;
  (void)addr;
  (void)size;
  (void)prot;
  return -1;
}
int kernel_overlap_sockets(pid_t pid, int master_sock, int victim_sock) {
  (void)pid;
  (void)master_sock;
  (void)victim_sock;
  return -1;
}
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char* nid) {
  (void)pid;
  (void)handle;
  (void)nid;
  return 0;
}
intptr_t kernel_dynlib_entry_addr(pid_t pid, uint32_t handle) {
  (void)pid;
  (void)handle;
  return 0;
}
uint64_t kernel_get_ucred_authid(pid_t pid) {
  (void)pid;
  return 0;
}
int32_t kernel_set_ucred_authid(pid_t pid, uint64_t authid) {
  (void)pid;
  (void)authid;
  return -1;
}
int32_t kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]) {
  (void)pid;
  (void)caps;
  return -1;
}
int32_t kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]) {
  (void)pid;
  (void)caps;
  return -1;
}
intptr_t kernel_get_root_vnode(void) { return 0; }
intptr_t kernel_get_proc_rootdir(pid_t pid) {
  (void)pid;
  return 0;
}
int32_t kernel_set_proc_rootdir(pid_t pid, intptr_t vnode) {
  (void)pid;
  (void)vnode;
  return -1;
}
intptr_t kernel_get_proc_jaildir(pid_t pid) {
  (void)pid;
  return 0;
}
int32_t kernel_set_proc_jaildir(pid_t pid, intptr_t vnode) {
  (void)pid;
  (void)vnode;
  return -1;
}
uid_t kernel_get_ucred_uid(pid_t pid) {
  (void)pid;
  return (uid_t)-1;
}
int32_t kernel_set_ucred_uid(pid_t pid, uid_t uid) {
  (void)pid;
  (void)uid;
  return -1;
}

// ---- dlopen (libc.a's dlfcn.o calls these weak hooks) ----------------------------
//
// A handle is libkernel's module id + 1 (0 stays "failed"). Names without a
// slash are system modules: libkernel resolves them itself.

static __thread char g_dlerror[160];
static __thread int g_dlerror_set;

static void set_dlerror(const char* what, const char* name, int rc) {
  snprintf(g_dlerror, sizeof(g_dlerror), "%s %s: 0x%08x", what, name ? name : "(null)",
           (unsigned)rc);
  g_dlerror_set = 1;
}

void* __dlopen(const char* path, int mode) {
  (void)mode;
  if (!path) {
    set_dlerror("dlopen", "(main program)", 0);
    return NULL;
  }
  int result = 0;
  const int handle = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &result);
  if (handle < 0) {
    set_dlerror("sceKernelLoadStartModule", path, handle);
    return NULL;
  }
  return (void*)(intptr_t)(handle + 1);
}

void* __dlsym(void* handle, const char* name) {
  if (!handle) {
    set_dlerror("dlsym", name, 0);
    return NULL;
  }
  void* address = NULL;
  const int rc = sceKernelDlsym((int)(intptr_t)handle - 1, name, &address);
  if (rc != 0 || !address) {
    set_dlerror("sceKernelDlsym", name, rc);
    return NULL;
  }
  return address;
}

int __dlclose(void* handle) {
  if (!handle) {
    return -1;
  }
  int result = 0;
  return sceKernelStopUnloadModule((int)(intptr_t)handle - 1, 0, NULL, 0, NULL, &result) == 0
             ? 0
             : -1;
}

char* __dlerror(void) {
  if (!g_dlerror_set) {
    return NULL;
  }
  g_dlerror_set = 0;
  return g_dlerror;
}

int __dladdr(const void* address, void* info) {
  (void)address;
  (void)info;
  return 0;
}

// ---- __cxa_atexit -----------------------------------------------------------------
//
// The first console run of the title died in its first static constructor:
// the import of __cxa_atexit (libSceLibcInternal) was left at address 0 by
// the system, while _init_env and atexit from the same module resolved
// (klog run 14; docs/native-title.md, "First console run"). The title has
// its own: the destructors are recorded and run by __cxa_finalize, which
// nothing calls in practice (a title is closed by the system, it does not
// return from main()).

#define kAtexitMax 4096
static struct {
  void (*fn)(void*);
  void* arg;
  void* dso;
} g_atexit[kAtexitMax];
static int g_atexit_count;
static int g_atexit_lock;

int __cxa_atexit(void (*fn)(void*), void* arg, void* dso) {
  while (__atomic_exchange_n(&g_atexit_lock, 1, __ATOMIC_ACQUIRE)) {
  }
  int rc = -1;
  if (g_atexit_count < kAtexitMax) {
    g_atexit[g_atexit_count].fn = fn;
    g_atexit[g_atexit_count].arg = arg;
    g_atexit[g_atexit_count].dso = dso;
    g_atexit_count++;
    rc = 0;
  }
  __atomic_store_n(&g_atexit_lock, 0, __ATOMIC_RELEASE);
  return rc;
}

void __cxa_finalize(void* dso) {
  for (int i = g_atexit_count - 1; i >= 0; i--) {
    if (g_atexit[i].fn && (!dso || g_atexit[i].dso == dso)) {
      void (*fn)(void*) = g_atexit[i].fn;
      g_atexit[i].fn = NULL;
      fn(g_atexit[i].arg);
    }
  }
}

// ---- libc.a's raw-syscall calls that libkernel does not export ---------------------

static int no_syscall(void) {
  errno = ENOSYS;
  return -1;
}

int syscall(int number, ...) {
  (void)number;
  return no_syscall();
}

off_t __syscall(quad_t number, ...) {
  (void)number;
  return no_syscall();
}

// The title may not set its file mode mask (libc.a's umask is a raw
// syscall and libkernel has none); what it creates is used by the root
// payloads, which can access it anyway. Reports the conventional mask.
mode_t umask(mode_t mask) {
  (void)mask;
  return 022;
}

int ptrace(int request, pid_t pid, caddr_t addr, int data) {
  (void)request;
  (void)pid;
  (void)addr;
  (void)data;
  return no_syscall();
}

int cpuset_setid(int which, id_t id, int setid) {
  (void)which;
  (void)id;
  (void)setid;
  return no_syscall();
}

int fchown(int fd, uid_t owner, gid_t group) {
  (void)fd;
  (void)owner;
  (void)group;
  return no_syscall();
}

int fstatfs(int fd, struct statfs* buf) {
  (void)fd;
  (void)buf;
  return no_syscall();
}

int link(const char* from, const char* to) {
  (void)from;
  (void)to;
  return no_syscall();
}

int symlink(const char* target, const char* path) {
  (void)target;
  (void)path;
  return no_syscall();
}

ssize_t readlink(const char* restrict path, char* restrict buf, size_t size) {
  (void)path;
  (void)buf;
  (void)size;
  errno = EINVAL;  // "not a symbolic link": callers then use the path as is
  return -1;
}

int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags) {
  (void)dirfd;
  (void)path;
  (void)times;
  (void)flags;
  return no_syscall();
}
