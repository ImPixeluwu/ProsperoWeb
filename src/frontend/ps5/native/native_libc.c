// SPDX-License-Identifier: GPL-3.0-or-later
//
// The libSceLibcInternal functions the clean-room sce_module/libc.prx does
// NOT export (docs/native-title.md). The native title binds its libc
// imports across the loaded modules by NID; these 21 ended up at a wrong
// address, and the first one called (getcwd) jumped into garbage (console
// run 18, klog SIGSEGV rip inside a module, not our image). Defined here
// they are resolved inside the image (app-symbols.map keeps them local) and
// are no longer imports; scripts/ps5/native/check-eboot.py fails the build
// if any of them is left bound to a module.
//
// Each is a full, self-contained implementation on libkernel, except
// getenv/setenv, which forward to the real libSceLibcInternal so the shared
// environ the children inherit stays consistent, and the stdio data symbols,
// which point at the real FILE objects (native_libc_init, called from the
// CRT after _init_env).

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>

// libkernel.
int getdents(int fd, char* buf, int nbytes);
int sceKernelOpen(const char* path, int flags, int mode);
int sceKernelClose(int fd);
int utimes(const char* path, const struct timeval* times);
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags,
                             void* option, int* result);
int sceKernelDlsym(int handle, const char* symbol, void** address);

// ---- directories (opendir/readdir/closedir) ------------------------------------
//
// FreeBSD getdents fills a buffer with struct dirent records (the layout in
// <sys/dirent.h>): d_fileno u32, d_reclen u16, d_type u8, d_namlen u8, name.
// A record is returned straight from the buffer, so its fields match what
// callers (fontconfig, WebKit) expect.

#define O_RDONLY 0x0
#define O_DIRECTORY 0x20000
#define DIR_BUF 0x2000

typedef struct {
  int fd;
  int used;
  int pos;
  int records;
  char buf[DIR_BUF] __attribute__((aligned(16)));
} NativeDir;

void native_boot_say(const char* text);  // native_crt.cpp

// The first calls of each directory function leave a breadcrumb (console
// run 19 died in this code without klog).
static int g_dir_trace = 24;

static void dir_say(const char* what, const char* detail, long value) {
  if (g_dir_trace <= 0) {
    return;
  }
  g_dir_trace--;
  char line[256];
  snprintf(line, sizeof(line), "[prosperoweb] dir: %s %s %ld\n", what, detail ? detail : "", value);
  native_boot_say(line);
}

void* opendir(const char* path) {
  dir_say("opendir", path, 0);
  const int fd = sceKernelOpen(path, O_RDONLY | O_DIRECTORY, 0);
  dir_say("opendir sceKernelOpen ->", path, fd);
  if (fd < 0) {
    errno = fd ? (fd & 0xffff) : ENOENT;
    return NULL;
  }
  NativeDir* d = (NativeDir*)malloc(sizeof(NativeDir));
  if (!d) {
    sceKernelClose(fd);
    errno = ENOMEM;
    return NULL;
  }
  d->fd = fd;
  d->used = 0;
  d->pos = 0;
  d->records = 0;
  return d;
}

struct dirent* readdir(void* dirp) {
  NativeDir* d = (NativeDir*)dirp;
  if (!d) {
    errno = EBADF;
    return NULL;
  }
  if (d->pos >= d->used) {
    const int n = getdents(d->fd, d->buf, (int)sizeof(d->buf));
    dir_say("readdir getdents ->", "", n);
    if (n <= 0) {
      return NULL;  // end of directory, or errno from getdents
    }
    d->used = n;
    d->pos = 0;
  }
  // FreeBSD 11 record: u32 d_fileno, u16 d_reclen, u8 d_type, u8 d_namlen,
  // then the name. A record that does not fit ends the listing instead of
  // handing callers garbage.
  if (d->used - d->pos < 8) {
    dir_say("readdir short tail", "", d->used - d->pos);
    d->pos = d->used;
    return NULL;
  }
  struct dirent* entry = (struct dirent*)(d->buf + d->pos);
  uint16_t reclen;
  memcpy(&reclen, d->buf + d->pos + 4, sizeof(reclen));
  const uint8_t namlen = (uint8_t)d->buf[d->pos + 7];
  if (reclen < 8 || d->pos + reclen > d->used || 8 + (int)namlen >= (int)reclen) {
    dir_say("readdir bad record, reclen", "", reclen);
    d->pos = d->used;
    errno = EIO;
    return NULL;
  }
  if (d->records++ == 0) {
    char name[64] = {0};
    memcpy(name, d->buf + d->pos + 8, namlen < sizeof(name) - 1 ? namlen : sizeof(name) - 1);
    dir_say("readdir first entry", name, reclen);
  }
  d->pos += reclen;
  return entry;
}

int closedir(void* dirp) {
  NativeDir* d = (NativeDir*)dirp;
  if (!d) {
    errno = EBADF;
    return -1;
  }
  const int fd = d->fd;
  dir_say("closedir records", "", d->records);
  free(d);
  return sceKernelClose(fd) < 0 ? -1 : 0;
}

// ---- getcwd / realpath ---------------------------------------------------------
//
// A title's working directory is not its folder and chdir is not used;
// getcwd reports "/". realpath normalises lexically (collapsing "." and
// "..") and confirms the result exists with stat, which is what callers
// (sqlite, WebKit's data store) rely on.

char* getcwd(char* buf, size_t size) {
  if (!buf || size < 2) {
    errno = ERANGE;
    return NULL;
  }
  buf[0] = '/';
  buf[1] = 0;
  return buf;
}

char* realpath(const char* path, char* resolved) {
  if (!path || !path[0]) {
    errno = ENOENT;
    return NULL;
  }
  char work[1024];
  if (path[0] == '/') {
    if (strlen(path) >= sizeof(work)) {
      errno = ENAMETOOLONG;
      return NULL;
    }
    strcpy(work, path);
  } else {
    if (strlen(path) + 2 > sizeof(work)) {
      errno = ENAMETOOLONG;
      return NULL;
    }
    work[0] = '/';
    strcpy(work + 1, path);
  }
  // Collapse components in place into out.
  char out[1024];
  size_t o = 0;
  out[o++] = '/';
  const char* p = work;
  while (*p) {
    while (*p == '/') {
      p++;
    }
    const char* start = p;
    while (*p && *p != '/') {
      p++;
    }
    const size_t len = (size_t)(p - start);
    if (len == 0) {
      break;
    }
    if (len == 1 && start[0] == '.') {
      continue;
    }
    if (len == 2 && start[0] == '.' && start[1] == '.') {
      while (o > 1 && out[o - 1] == '/') {
        o--;
      }
      while (o > 1 && out[o - 1] != '/') {
        o--;
      }
      continue;
    }
    if (o > 1) {
      out[o++] = '/';
    }
    if (o + len >= sizeof(out)) {
      errno = ENAMETOOLONG;
      return NULL;
    }
    memcpy(out + o, start, len);
    o += len;
  }
  out[o] = 0;
  struct stat st;
  if (stat(out, &st) != 0) {
    return NULL;  // errno from stat
  }
  char* result = resolved ? resolved : (char*)malloc(o + 1);
  if (!result) {
    errno = ENOMEM;
    return NULL;
  }
  memcpy(result, out, o + 1);
  return result;
}

// ---- random / initstate / setstate --------------------------------------------
//
// A small xorshift PRNG: random() returns a non-negative long under RAND_MAX
// (0x7fffffff). initstate/setstate manage the opaque state the way callers
// expect (return the previous state), without the BSD additive generator.

static uint64_t g_rng = 0x2545f4914f6cdd1dULL;

long random(void) {
  uint64_t x = g_rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  g_rng = x;
  return (long)((x >> 33) & 0x7fffffffULL);
}

char* initstate(unsigned long seed, char* state, long size) {
  (void)size;
  g_rng = seed ? seed : 1;
  return state;
}

char* setstate(char* state) { return state; }

// ---- gethostname / getifaddrs / freeifaddrs ------------------------------------

int gethostname(char* name, size_t len) {
  if (!name || len == 0) {
    errno = EINVAL;
    return -1;
  }
  const char host[] = "ps5";
  if (len < sizeof(host)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(name, host, sizeof(host));
  return 0;
}

// No per-interface addresses: callers (curl, WebRTC probing) treat an empty
// list as "no local interface info", which is harmless here.
int getifaddrs(void** ifap) {
  if (ifap) {
    *ifap = NULL;
  }
  return 0;
}

void freeifaddrs(void* ifa) { (void)ifa; }

// ---- utime ---------------------------------------------------------------------

int utime(const char* path, const struct utimbuf* times) {
  if (!times) {
    return utimes(path, NULL);
  }
  struct timeval tv[2];
  tv[0].tv_sec = times->actime;
  tv[0].tv_usec = 0;
  tv[1].tv_sec = times->modtime;
  tv[1].tv_usec = 0;
  return utimes(path, tv);
}

// ---- __inet_aton ---------------------------------------------------------------

int __inet_aton(const char* text, void* addr) {
  uint32_t parts[4];
  int count = 0;
  const char* p = text;
  while (count < 4) {
    if (*p < '0' || *p > '9') {
      return 0;
    }
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9') {
      value = value * 10 + (uint32_t)(*p - '0');
      if (value > 255) {
        return 0;
      }
      p++;
    }
    parts[count++] = value;
    if (*p == '.') {
      p++;
    } else {
      break;
    }
  }
  if (*p != 0 || count != 4) {
    return 0;
  }
  const uint32_t packed = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
  const uint32_t net = __builtin_bswap32(packed);  // network byte order
  memcpy(addr, &net, 4);
  return 1;
}

int inet_aton(const char* text, void* addr) { return __inet_aton(text, addr); }

// ---- makecontext ---------------------------------------------------------------
//
// No caller reaches it (WebKit's "makeContextCurrent" is unrelated GL); it
// is pulled in by a libc.a object. Trap if it is ever called.

int sceKernelDebugOutText(int channel, const char* text);

void makecontext(void* ucp, void (*func)(void), int argc, ...) {
  (void)ucp;
  (void)func;
  (void)argc;
  sceKernelDebugOutText(0, "[prosperoweb] makecontext is not supported in the native title\n");
  __builtin_trap();
}

// ---- getenv / setenv: forward to the real libSceLibcInternal -------------------
//
// These work through the real module (the UI's whole environment is set with
// setenv and the children inherit it). Forwarding keeps that path and the
// shared environ, while removing the direct import that libc.prx shadowed.

typedef char* (*getenv_fn)(const char*);
typedef int (*setenv_fn)(const char*, const char*, int);
static getenv_fn g_real_getenv;
static setenv_fn g_real_setenv;
static int g_libc_handle = -1;

static int libc_handle(void) {
  if (g_libc_handle < 0) {
    int result = 0;
    g_libc_handle =
        sceKernelLoadStartModule("libSceLibcInternal.sprx", 0, NULL, 0, NULL, &result);
  }
  return g_libc_handle;
}

static void* libc_sym(const char* name) {
  const int handle = libc_handle();
  void* address = NULL;
  if (handle >= 0 && sceKernelDlsym(handle, name, &address) == 0) {
    return address;
  }
  return NULL;
}

char* getenv(const char* name) {
  if (!g_real_getenv) {
    g_real_getenv = (getenv_fn)libc_sym("getenv");
  }
  return g_real_getenv ? g_real_getenv(name) : NULL;
}

int setenv(const char* name, const char* value, int overwrite) {
  if (!g_real_setenv) {
    g_real_setenv = (setenv_fn)libc_sym("setenv");
  }
  if (!g_real_setenv) {
    errno = ENOSYS;
    return -1;
  }
  return g_real_setenv(name, value, overwrite);
}

// ---- stdio data symbols --------------------------------------------------------
//
// __stdinp/__stdoutp/__stderrp (FILE*), __mb_cur_max (int) and tzname
// (char*[2]) are data the app reads directly (stderr is __stderrp).
// native_libc_init, called from the CRT after _init_env, points the three
// streams at the real libc FILE objects.

FILE* __stdinp;
FILE* __stdoutp;
FILE* __stderrp;
int __mb_cur_max = 1;
char* tzname[2] = {(char*)"UTC", (char*)"UTC"};

void native_libc_init(void) {
  FILE** in = (FILE**)libc_sym("__stdinp");
  FILE** out = (FILE**)libc_sym("__stdoutp");
  FILE** err = (FILE**)libc_sym("__stderrp");
  __stdinp = in ? *in : NULL;
  __stdoutp = out ? *out : NULL;
  __stderrp = err ? *err : NULL;
  int* mb = (int*)libc_sym("__mb_cur_max");
  if (mb) {
    __mb_cur_max = *mb;
  }
}
