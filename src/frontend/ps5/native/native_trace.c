// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native title: file access and IPC diagnostics (console runs 16-17).
//
// - access(): in the title the kernel answered EPERM for files the title
//   can stat and read (/app0/data/chrome/chrome.html, run 17). The wrapped
//   access() falls back to stat() and open() on EPERM, so existence and
//   readability checks (ours, sqlite's, WebKit's) see the truth.
// - The UI's WebKit spawned its children but never sent them a message
//   (run 17: "fd 3 ... nothing waiting"). The link wraps socketpair,
//   sendmsg, recvmsg and select (src/frontend/ps5/CMakeLists.txt) and logs
//   the first calls and every failure, so the log shows whether WebKit's
//   IPC reaches the kernel and what it answers.
// - native_file_selftest() reads the title's own files every way the UI
//   might, and logs each result.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "native_title.h"

void ps5_log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
_Bool ps5_trace_enabled(void);

int sceKernelOpen(const char* path, int flags, int mode);
long sceKernelRead(int fd, void* buffer, size_t length);
int sceKernelClose(int fd);
int sceKernelStat(const char* path, struct stat* st);

int __real_access(const char* path, int mode);
int __real_socketpair(int domain, int type, int protocol, int fds[2]);
ssize_t __real_sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t __real_recvmsg(int fd, struct msghdr* msg, int flags);
int __real_select(int n, fd_set* r, fd_set* w, fd_set* e, struct timeval* t);
int __real_fcntl(int fd, int command, ...);
int __real_dup(int fd);
int sceKernelRdup(int pid, int fd);

// ---- access() ------------------------------------------------------------------

int __wrap_access(const char* path, int mode) {
  const int rc = __real_access(path, mode);
  if (rc == 0 || errno != EPERM) {
    return rc;
  }
  struct stat st;
  if (stat(path, &st) != 0) {
    return -1;  // errno from stat
  }
  if (mode & W_OK) {
    if (S_ISDIR(st.st_mode)) {
      return 0;  // tried for real by whoever creates files in it
    }
    const int fd = open(path, O_WRONLY);
    if (fd < 0) {
      return -1;
    }
    close(fd);
  }
  if (mode & R_OK) {
    const int fd = open(path, S_ISDIR(st.st_mode) ? O_RDONLY | O_DIRECTORY : O_RDONLY);
    if (fd < 0) {
      return -1;
    }
    close(fd);
  }
  return 0;
}

// ---- fcntl() on sockets ------------------------------------------------------------
//
// A title's libc refuses fcntl on sockets with EINVAL (syobon-action-ps5
// docs/UPDATE_CHECK.md, the Stremio port's native/console_curl.c; console
// run 23: WebKit's IPC connections never opened). On a refusal:
// close-on-exec is accepted (a title never execs), and O_NONBLOCK goes
// through the console's own SO_NBIO socket option (0x1200).

#define kSoNbio 0x1200
static atomic_int g_fcntl_logged;

int __wrap_fcntl(int fd, int command, ...) {
  va_list args;
  va_start(args, command);
  const intptr_t argument = va_arg(args, intptr_t);
  va_end(args);
  const int rc = __real_fcntl(fd, command, argument);
  if (rc >= 0 || errno != EINVAL) {
    return rc;
  }
  int result = rc;
  switch (command) {
    case F_GETFD:
    case F_SETFD:
      result = 0;
      break;
    case F_DUPFD:
#ifdef F_DUPFD_CLOEXEC
    case F_DUPFD_CLOEXEC:
#endif
    {
      // The lowest free descriptor at or above the argument, like F_DUPFD:
      // dup() until one is high enough, closing the ones below.
      int below[64];
      int count = 0;
      result = dup(fd);
      while (result >= 0 && result < (int)argument && count < 64) {
        below[count++] = result;
        result = dup(fd);
      }
      for (int i = 0; i < count; i++) {
        close(below[i]);
      }
      break;
    }
    case F_SETFL: {
      const int on = (argument & O_NONBLOCK) != 0;
      result = setsockopt(fd, SOL_SOCKET, kSoNbio, &on, sizeof(on));
      break;
    }
    case F_GETFL: {
      int on = 0;
      socklen_t length = sizeof(on);
      result = getsockopt(fd, SOL_SOCKET, kSoNbio, &on, &length) < 0
                   ? O_RDWR
                   : (O_RDWR | (on ? O_NONBLOCK : 0));
      break;
    }
    default:
      errno = EINVAL;
      return -1;
  }
  if (atomic_fetch_add(&g_fcntl_logged, 1) < (ps5_trace_enabled() ? 16 : 2)) {
    ps5_log_printf("trace: fcntl(fd %d, cmd %d) refused (EINVAL); emulated -> %d", fd, command,
                   result);
  }
  return result;
}

// ---- dup() -----------------------------------------------------------------------
//
// A title may not duplicate a descriptor with dup() (EPERM, console run 28:
// WebKit's shared-memory handles failed). On a refusal: dup2() onto a
// descriptor number reserved with open("/dev/null"), then rdup of our own
// process. The first attempts are logged.

static atomic_int g_dup_logged;

int __wrap_dup(int fd) {
  const int rc = __real_dup(fd);
  if (rc >= 0 || errno != EPERM) {
    return rc;
  }
  int result = -1;
  const char* how = "nothing worked";
  const int slot = open("/dev/null", O_RDONLY);
  if (slot >= 0) {
    if (dup2(fd, slot) == slot) {
      result = slot;
      how = "dup2 onto a reserved number";
    } else {
      close(slot);
    }
  }
  if (result < 0) {
    const int r = sceKernelRdup(getpid(), fd);
    if (r >= 0) {
      result = r;
      how = "sceKernelRdup";
    }
  }
  if (atomic_fetch_add(&g_dup_logged, 1) < (ps5_trace_enabled() ? 16 : 2)) {
    // The callers, as image offsets for prosperoweb-native.unstripped.elf.
    extern char __prwb_image_base[];
    const uintptr_t base = (uintptr_t)__prwb_image_base;
    ps5_log_printf("trace: dup(%d) refused (EPERM); %s -> %d; called from image+%#lx", fd, how,
                   result, (unsigned long)((uintptr_t)__builtin_return_address(0) - base));
  }
  if (result < 0) {
    errno = EPERM;
  }
  return result;
}

// ---- IPC tracing ----------------------------------------------------------------

#define kTraceFirst 64
#define kTraceFds 256
// Messages and bytes per descriptor, both directions, for the periodic
// summary (native_trace_io_summary): the first-64 log lines do not show
// which connection carries the traffic later on.
static atomic_uint g_fd_sent[kTraceFds];
static atomic_uint g_fd_received[kTraceFds];
static atomic_uint g_fd_fds_in[kTraceFds];
static atomic_uint g_fd_fds_out[kTraceFds];
static atomic_int g_send_logged;
static atomic_int g_recv_logged;
static atomic_int g_select_logged;

static int attached_fds(const struct msghdr* msg) {
  int n = 0;
  if (!msg || !msg->msg_control) {
    return 0;
  }
  for (struct cmsghdr* c = CMSG_FIRSTHDR(msg); c; c = CMSG_NXTHDR(msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      n += (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    }
  }
  return n;
}

static size_t iov_bytes(const struct msghdr* msg) {
  size_t n = 0;
  for (int i = 0; msg && i < (int)msg->msg_iovlen; i++) {
    n += msg->msg_iov[i].iov_len;
  }
  return n;
}

int __wrap_socketpair(int domain, int type, int protocol, int fds[2]) {
  const int rc = __real_socketpair(domain, type, protocol, fds);
  const int saved = errno;
  if (rc == 0 && !ps5_trace_enabled()) {
    return rc;
  }
  ps5_log_printf("trace: socketpair(%d, %d) -> %d [%d, %d]%s%s", domain, type, rc,
                 rc == 0 ? fds[0] : -1, rc == 0 ? fds[1] : -1, rc ? ": " : "",
                 rc ? strerror(saved) : "");
  errno = saved;
  return rc;
}

ssize_t __wrap_sendmsg(int fd, const struct msghdr* msg, int flags) {
  const ssize_t rc = __real_sendmsg(fd, msg, flags);
  const int saved = errno;
  if (rc >= 0 && fd >= 0 && fd < kTraceFds) {
    atomic_fetch_add(&g_fd_sent[fd], 1);
    atomic_fetch_add(&g_fd_fds_out[fd], (unsigned)attached_fds(msg));
  }
  if (rc < 0 || (ps5_trace_enabled() && atomic_fetch_add(&g_send_logged, 1) < kTraceFirst)) {
    ps5_log_printf("trace: sendmsg(fd %d, %zu bytes, %d fds) -> %zd%s%s", fd, iov_bytes(msg),
                   attached_fds(msg), rc, rc < 0 ? ": " : "", rc < 0 ? strerror(saved) : "");
  }
  errno = saved;
  return rc;
}

ssize_t __wrap_recvmsg(int fd, struct msghdr* msg, int flags) {
  const ssize_t rc = __real_recvmsg(fd, msg, flags);
  const int saved = errno;
  if (rc > 0 && fd >= 0 && fd < kTraceFds) {
    atomic_fetch_add(&g_fd_received[fd], 1);
    atomic_fetch_add(&g_fd_fds_in[fd], (unsigned)attached_fds(msg));
  }
  if ((rc < 0 && saved != EAGAIN && saved != EINTR) ||
      (ps5_trace_enabled() && atomic_fetch_add(&g_recv_logged, 1) < kTraceFirst)) {
    ps5_log_printf("trace: recvmsg(fd %d) -> %zd, %d fds%s%s%s", fd, rc,
                   rc > 0 ? attached_fds(msg) : 0,
                   rc >= 0 && (msg->msg_flags & MSG_CTRUNC) ? " CTRUNC" : "", rc < 0 ? ": " : "",
                   rc < 0 ? strerror(saved) : "");
  }
  errno = saved;
  return rc;
}

int __wrap_select(int n, fd_set* r, fd_set* w, fd_set* e, struct timeval* t) {
  const int rc = __real_select(n, r, w, e, t);
  const int saved = errno;
  if ((rc < 0 && saved != EINTR) ||
      (ps5_trace_enabled() && atomic_fetch_add(&g_select_logged, 1) < kTraceFirst)) {
    ps5_log_printf("trace: select(nfds %d) -> %d%s%s", n, rc, rc < 0 ? ": " : "",
                   rc < 0 ? strerror(saved) : "");
  }
  errno = saved;
  return rc;
}

void native_trace_io_summary(char* out, size_t len) {
  size_t used = 0;
  out[0] = 0;
  for (int fd = 0; fd < kTraceFds && used + 48 < len; fd++) {
    const unsigned sent = atomic_load(&g_fd_sent[fd]);
    const unsigned received = atomic_load(&g_fd_received[fd]);
    if (!sent && !received) {
      continue;
    }
    const int n = snprintf(out + used, len - used, "%sfd%d sent %u (%u fds) recv %u (%u fds)",
                           used ? "; " : "", fd, sent, atomic_load(&g_fd_fds_out[fd]), received,
                           atomic_load(&g_fd_fds_in[fd]));
    used += n > 0 ? (size_t)n : 0;
  }
}

// ---- file self-test ---------------------------------------------------------------

static const char* result(int ok, int saved) { return ok ? "ok" : strerror(saved); }

static void step(const char* what, const char* path);

static void test_path(const char* path) {
  step("path", path);
  struct stat st;
  int rc = stat(path, &st);
  const int stat_errno = errno;
  int saved;
  const int acc = __real_access(path, R_OK);
  saved = errno;
  char head[8] = {0};
  const int fd = open(path, O_RDONLY);
  const int open_errno = errno;
  long got = -1;
  if (fd >= 0) {
    got = read(fd, head, 4);
    close(fd);
  }
  FILE* f = fopen(path, "rb");
  const int fopen_errno = errno;
  if (f) {
    fclose(f);
  }
  const int kfd = sceKernelOpen(path, O_RDONLY, 0);
  long kgot = -1;
  if (kfd >= 0) {
    char kh[4];
    kgot = sceKernelRead(kfd, kh, 4);
    sceKernelClose(kfd);
  }
  struct stat kst;
  const int kstat = sceKernelStat(path, &kst);
  ps5_log_printf("file selftest: %s: stat %s (size %lld) | access(R_OK) %s | open %s, read %ld | "
                 "fopen %s | sceKernelOpen %#x, read %ld | sceKernelStat %#x",
                 path, result(rc == 0, stat_errno), rc == 0 ? (long long)st.st_size : -1LL,
                 result(acc == 0, saved), result(fd >= 0, open_errno), got,
                 result(f != NULL, fopen_errno), (unsigned)kfd, kgot, (unsigned)kstat);
}

static void step(const char* what, const char* path) {
  char line[320];
  snprintf(line, sizeof(line), "[prosperoweb] file selftest step: %s %s\n", what, path);
  native_boot_say(line);
}

static void test_dir(const char* path) {
  step("opendir", path);
  DIR* d = opendir(path);
  const int saved = errno;
  int entries = 0;
  if (d) {
    step("readdir loop", path);
    while (entries < 10000 && readdir(d)) {
      entries++;
    }
    step("closedir", path);
    closedir(d);
  }
  step("getcwd", path);
  char cwd[256] = "?";
  if (!getcwd(cwd, sizeof(cwd))) {
    snprintf(cwd, sizeof(cwd), "getcwd: %s", strerror(errno));
  }
  step("done", path);
  ps5_log_printf("file selftest: opendir %s: %s (%d entries); cwd %s", path,
                 result(d != NULL, saved), entries, cwd);
}

void native_profile_probe(const char* base) {
  static const char* const kDirs[] = {"", "/tmp", "/cache", "/storage", "/localstorage",
                                      "/indexeddb"};
  char line[1024];
  size_t used = 0;
  line[0] = 0;
  for (size_t i = 0; i < sizeof(kDirs) / sizeof(kDirs[0]); i++) {
    char dir[256];
    char probe[300];
    snprintf(dir, sizeof(dir), "%s%s", base, kDirs[i]);
    snprintf(probe, sizeof(probe), "%s/.ui-write-probe", dir);
    struct stat st;
    const int exists = stat(dir, &st) == 0;
    const int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    const int saved = errno;
    if (fd >= 0) {
      close(fd);
      unlink(probe);
    }
    const int n = snprintf(line + used, sizeof(line) - used, "%s%s %s%s", used ? "; " : "",
                           dir, !exists ? "missing" : (fd >= 0 ? "writable" : "NOT writable: "),
                           exists && fd < 0 ? strerror(saved) : "");
    used += n > 0 ? (size_t)n : 0;
    if (used >= sizeof(line)) {
      break;
    }
  }
  ps5_log_printf("profile: UI write access: %s", line);
}

void native_file_selftest(const char* ui_root, const char* child_root) {
  char path[512];
  test_path("/app0/eboot.bin");
  snprintf(path, sizeof(path), "%s/data/chrome/chrome.html", ui_root);
  test_path(path);
  snprintf(path, sizeof(path), "%s/data/fonts/fonts.conf", ui_root);
  test_path(path);
  snprintf(path, sizeof(path), "%s/prosperoweb.elf", child_root);
  test_path(path);
  test_path("/data/prosperoweb/config.json");
  snprintf(path, sizeof(path), "%s/data", ui_root);
  test_dir(path);
  test_dir("/data/prosperoweb");
}
