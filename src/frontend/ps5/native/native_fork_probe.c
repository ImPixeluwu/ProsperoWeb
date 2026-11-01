// SPDX-License-Identifier: GPL-3.0-or-later
//
// Start-up fork probe of the native title (config.json "probe_fork", off by
// default): can the sandboxed title (uid 1) create a child process itself,
// so the spawn supervisor could live in the title without elfldr
// (docs/no-elfldr.md)? Runs before the decoder, the broker and WebKit.
//
// A title may not issue the syscall instruction itself (the console ends
// it, see native_runtime.c), so every attempt goes through libkernel,
// resolved by name at run time: fork, then rfork_thread (rfork(RFPROC|RFFDG)
// with a copied address space, then with a shared one as elfldr does), then
// vfork and pdfork. The first one that creates a child is used; the rest are
// not tried. Each child writes one line into a socket pair made before the
// fork (descriptor inheritance without dup), one line through the logger,
// and exits at once. Level 2 also has the child do what a WebProcess does
// first: its pid, its inherited IPC socket, a new socket pair, one end of it
// sent over the IPC socket.
//
// Levels 3 and 4 call libkernel's rfork_thread as a REAL NID import (linked,
// not resolved by name): NID bSDxEpGzmUE, the only process-creation primitive
// the game libkernel exports, the one elfldr/etaHEN use. By-name lookup finds
// no SCE function (a previous console run resolved none of fork/rfork_thread/
// vfork/pdfork), because PS5 keys them by NID, so the real import is the only
// way to actually call it from the title. The signature is
//   pid_t rfork_thread(int flags, void* stack, int (*func)(void*), void* arg)
// (FreeBSD/PS5): in the PARENT it returns the child pid (or -1/errno); the
// CHILD runs func(arg) on the supplied stack top and must _exit(), never
// return. The stack is mmap'd (MAP_ANON|MAP_PRIVATE, grows down, passed
// top-aligned), as elfldr does with its malloc'd stack+size-8.
//   - Level 3 (proven shape, lowest risk): rfork_thread(RFPROC|RFCFDG|RFMEM),
//     the exact flags elfldr/etaHEN use. The address space is SHARED, so the
//     child reports by writing its pid and a magic marker into a volatile
//     struct the parent allocated before the call (RFCFDG cleared the fds, so
//     no socket); the parent spin-waits on the marker, then waitpid.
//   - Level 4 (the real supervisor shape, higher risk, a panic candidate):
//     rfork_thread(RFPROC|RFFDG) copies the fd table and COPIES the address
//     space (no RFMEM). A socketpair is made before the call; the child writes
//     one line to its inherited end and _exit(0); the parent reads it. This is
//     what a forked WebProcess supervisor needs, and copying a vmspace with
//     direct-memory/GPU mappings is the main panic risk.
// The level 3/4 child does nothing but report and _exit (no logger, no
// allocation): in level 3 it shares the parent's heap and locks, in level 4
// it holds a copy of them; all logging is done by the parent.
//
// The parent never blocks: it reads with MSG_DONTWAIT and polls the child
// with WNOHANG, kills it after a timeout, and the start-up waits for the
// whole probe (it runs on its own thread) for a bounded time only. Before
// every call that could take the console down, boot.txt gets a breadcrumb.

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include "native_title.h"

#ifndef RFFDG
#define RFFDG (1 << 2)  // copy the fd table
#endif
#ifndef RFPROC
#define RFPROC (1 << 4)  // create a child process
#endif
#ifndef RFMEM
#define RFMEM (1 << 5)  // share the address space
#endif
#ifndef RFCFDG
#define RFCFDG (1 << 12)  // give the child a clean (empty) fd table
#endif

#ifndef MAP_ANON
#ifdef MAP_ANONYMOUS
#define MAP_ANON MAP_ANONYMOUS
#else
#define MAP_ANON 0x1000
#endif
#endif

void ps5_log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags,
                             void* option, int* result);
int sceKernelDlsym(int handle, const char* symbol, void** address);

#if PROSPEROWEB_NATIVE_TITLE
// The real NID import of the game libkernel's rfork_thread (NID bSDxEpGzmUE).
// The native link binds this to libkernel.sprx (CMakeLists links -lkernel
// first, and libc.a defines no rfork_thread: only fork/vfork/pdfork, in the
// syscalls.o the native link drops). check-eboot.py confirms it resolves to a
// start-up module and leaves no raw syscall instruction in the image.
extern pid_t rfork_thread(int flags, void* stack, int (*func)(void*), void* arg);
#endif

typedef pid_t (*fork_fn)(void);
typedef pid_t (*rfork_thread_fn)(int flags, void* stack, int (*func)(void*), void* arg);
typedef pid_t (*pdfork_fn)(int* fdp, int flags);

enum { kReadTimeoutMs = 3000, kExitTimeoutMs = 2000, kKillTimeoutMs = 1000 };
enum { kStartupWaitMs = 15000 };

struct probe_ctx {
  const char* method;
  int level;
  int report_fd;  // the child's end of the report socket pair
  int ipc_fd;     // level 2: the child's end of the "IPC" socket pair, else -1
};

// The stack rfork_thread runs the child on (copied with the address space,
// or shared with RFMEM; the parent does not touch it while a child runs).
static _Alignas(64) char g_child_stack[64 * 1024];

static fork_fn g_fork;
static fork_fn g_vfork;
static rfork_thread_fn g_rfork_thread;
static pdfork_fn g_pdfork;

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

static void crumb(const char* step) {
  char line[160];
  snprintf(line, sizeof(line), "[prosperoweb] probe: fork: %s\n", step);
  native_boot_say(line);
}

// libkernel first (the module the title imports from), then every handle.
static void* resolve(const char* name, int* handle) {
  static int libkernel = -2;
  if (libkernel == -2) {
    int result = 0;
    libkernel = sceKernelLoadStartModule("libkernel.sprx", 0, NULL, 0, NULL, &result);
  }
  void* address = NULL;
  if (libkernel >= 0 && sceKernelDlsym(libkernel, name, &address) == 0 && address) {
    *handle = libkernel;
    return address;
  }
  for (int h = 0; h < 64; h++) {
    if (sceKernelDlsym(h, name, &address) == 0 && address) {
      *handle = h;
      return address;
    }
  }
  *handle = -1;
  return NULL;
}

static void describe_symbol(char* out, size_t len, const char* name, void* address, int handle) {
  if (address) {
    snprintf(out, len, "%s=%p (handle %d)", name, address, handle);
  } else {
    snprintf(out, len, "%s=absent", name);
  }
}

// ---- The child ---------------------------------------------------------------

// Level 2: as a WebProcess starts: its pid, its inherited IPC socket, a new
// socket pair (WebKit's IPC creates them), one end sent over the IPC socket
// with SCM_RIGHTS (how WebKit hands descriptors to the UI).
static void child_ipc_step(const struct probe_ctx* ctx, int pid) {
  struct stat st;
  const int stat_rc = fstat(ctx->ipc_fd, &st);
  const int stat_errno = stat_rc ? errno : 0;
  int pair[2] = {-1, -1};
  const int pair_rc = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
  const int pair_errno = pair_rc ? errno : 0;

  char text[96];
  const int text_len = snprintf(text, sizeof(text), "ipc hello from pid %d", pid);
  struct iovec iov = {text, (size_t)text_len};
  union {
    struct cmsghdr header;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control;
  memset(&control, 0, sizeof(control));
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  if (pair_rc == 0) {
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &pair[1], sizeof(int));
  }
  const ssize_t sent = sendmsg(ctx->ipc_fd, &msg, 0);
  const int send_errno = sent < 0 ? errno : 0;
  ps5_log_printf(
      "probe: fork child pid %d: ipc fd %d %s (errno %d); socketpair %s (errno %d %s); "
      "sendmsg with a descriptor -> %zd (errno %d %s)",
      pid, ctx->ipc_fd,
      stat_rc ? "fstat FAILED" : (S_ISSOCK(st.st_mode) ? "is a socket" : "is NOT a socket"),
      stat_errno, pair_rc ? "FAILED" : "ok", pair_errno, pair_errno ? strerror(pair_errno) : "",
      sent, send_errno, send_errno ? strerror(send_errno) : "");
  if (pair_rc == 0) {
    close(pair[0]);
    close(pair[1]);
  }
}

static __attribute__((noreturn)) void child_body(const struct probe_ctx* ctx) {
  const int pid = (int)getpid();
  char line[128];
  const int n = snprintf(line, sizeof(line), "child pid %d ppid %d via %s", pid, (int)getppid(),
                         ctx->method);
  const ssize_t written = write(ctx->report_fd, line, (size_t)n);
  ps5_log_printf("probe: fork child pid %d (%s): this line comes from the child through the "
                 "logger (report write %zd)",
                 pid, ctx->method, written);
  if (ctx->level >= 2 && ctx->ipc_fd >= 0) {
    child_ipc_step(ctx, pid);
  }
  _exit(0);
}

static int rfork_entry(void* arg) { child_body((const struct probe_ctx*)arg); }

// ---- The parent --------------------------------------------------------------

enum method { kFork, kRforkCopy, kRforkShared, kVfork, kPdfork, kMethods };

static const char* method_name(enum method m) {
  switch (m) {
    case kFork: return "fork()";
    case kRforkCopy: return "rfork_thread(RFPROC|RFFDG)";
    case kRforkShared: return "rfork_thread(RFPROC|RFFDG|RFMEM)";
    case kVfork: return "vfork()";
    case kPdfork: return "pdfork()";
    default: return "?";
  }
}

static int method_available(enum method m) {
  switch (m) {
    case kFork: return g_fork != NULL;
    case kRforkCopy:
    case kRforkShared: return g_rfork_thread != NULL;
    case kVfork: return g_vfork != NULL;
    case kPdfork: return g_pdfork != NULL;
    default: return 0;
  }
}

// Creates the child; returns its pid (the child never returns from here),
// or -1 with errno set. *pd receives pdfork's process descriptor.
static pid_t launch(enum method m, struct probe_ctx* ctx, int* pd) {
  pid_t pid = -1;
  char* stack_top = g_child_stack + sizeof(g_child_stack) - 8;  // as elfldr's rfork_thread call
  switch (m) {
    case kFork:
      pid = g_fork();
      break;
    case kRforkCopy:
      return g_rfork_thread(RFPROC | RFFDG, stack_top, rfork_entry, ctx);
    case kRforkShared:
      return g_rfork_thread(RFPROC | RFFDG | RFMEM, stack_top, rfork_entry, ctx);
    case kVfork:
      pid = g_vfork();
      break;
    case kPdfork:
      pid = g_pdfork(pd, 0);
      break;
    default:
      errno = EINVAL;
      return -1;
  }
  if (pid == 0) {
    child_body(ctx);
  }
  return pid;
}

static const char* describe_status(int status, char* buf, size_t len) {
  if (WIFEXITED(status)) {
    snprintf(buf, len, "exit %d", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    snprintf(buf, len, "signal %d", WTERMSIG(status));
  } else {
    snprintf(buf, len, "status 0x%x", status);
  }
  return buf;
}

// Reads what is waiting on fd without blocking; returns the byte count.
static ssize_t read_now(int fd, char* buf, size_t len, int* err) {
  const ssize_t n = recv(fd, buf, len - 1, MSG_DONTWAIT);
  *err = n < 0 ? errno : 0;
  buf[n > 0 ? n : 0] = '\0';
  return n;
}

// Level 2: the child's IPC message and the descriptor it carries.
static void read_ipc(int fd) {
  char text[128] = "";
  union {
    struct cmsghdr header;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control;
  memset(&control, 0, sizeof(control));
  struct iovec iov = {text, sizeof(text) - 1};
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control.bytes;
  msg.msg_controllen = sizeof(control.bytes);
  const ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
  const int err = n < 0 ? errno : 0;
  int received = -1;
  for (struct cmsghdr* c = n > 0 ? CMSG_FIRSTHDR(&msg) : NULL; c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      memcpy(&received, CMSG_DATA(c), sizeof(int));
    }
  }
  if (n > 0) {
    text[n] = '\0';
  }
  struct stat st;
  const int is_socket = received >= 0 && fstat(received, &st) == 0 && S_ISSOCK(st.st_mode);
  ps5_log_printf("probe: fork level 2 parent: ipc recvmsg -> %zd (errno %d %s) '%s'; "
                 "descriptor %d received%s",
                 n, err, err ? strerror(err) : "", n > 0 ? text : "", received,
                 received < 0 ? " (NONE)" : (is_socket ? ", a socket" : ", NOT a socket"));
  if (received >= 0) {
    close(received);
  }
}

// One child with method m; returns 1 when it ran and reported back.
static int try_method(enum method m, int level) {
  char step[128];
  int report[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, report) != 0) {
    const int err = errno;
    ps5_log_printf("probe: fork %s level %d: report socketpair FAILED: errno %d %s",
                   method_name(m), level, err, strerror(err));
    return 0;
  }
  int ipc[2] = {-1, -1};
  if (level >= 2 && socketpair(AF_UNIX, SOCK_STREAM, 0, ipc) != 0) {
    const int err = errno;
    ps5_log_printf("probe: fork %s level 2: ipc socketpair FAILED: errno %d %s", method_name(m),
                   err, strerror(err));
    ipc[0] = ipc[1] = -1;
  }
  struct probe_ctx ctx = {method_name(m), level, report[1], ipc[1]};

  snprintf(step, sizeof(step), "calling %s (level %d)", method_name(m), level);
  crumb(step);
  int pd = -1;
  const long long t0 = now_ms();
  const pid_t pid = launch(m, &ctx, &pd);
  const int launch_errno = pid < 0 ? errno : 0;
  const long long launch_ms = now_ms() - t0;
  snprintf(step, sizeof(step), "%s returned %d (errno %d) in %lld ms", method_name(m), (int)pid,
           launch_errno, launch_ms);
  crumb(step);
  close(report[1]);
  if (ipc[1] >= 0) {
    close(ipc[1]);
  }
  if (pid < 0) {
    ps5_log_printf("probe: fork %s level %d: REFUSED: errno %d %s (%lld ms)", method_name(m),
                   level, launch_errno, strerror(launch_errno), launch_ms);
    close(report[0]);
    if (ipc[0] >= 0) {
      close(ipc[0]);
    }
    return 0;
  }
  ps5_log_printf("probe: fork %s level %d: child pid %d created in %lld ms%s", method_name(m),
                 level, (int)pid, launch_ms, pd >= 0 ? " (process descriptor)" : "");

  // The child's line, then its exit; never a blocking call.
  char line[160] = "";
  ssize_t got = 0;
  int read_errno = 0;
  int reaped = 0;
  int status = 0;
  const long long start = now_ms();
  while (now_ms() - start < kReadTimeoutMs) {
    got = read_now(report[0], line, sizeof(line), &read_errno);
    if (got > 0) {
      break;
    }
    if (got == 0) {
      break;  // the child closed its end without writing
    }
    if (!reaped && waitpid(pid, &status, WNOHANG) == pid) {
      reaped = 1;
      got = read_now(report[0], line, sizeof(line), &read_errno);
      break;
    }
    sleep_ms(20);
  }
  const long long read_ms = now_ms() - start;
  ps5_log_printf("probe: fork %s level %d: parent read %zd bytes after %lld ms%s%s%s (errno %d)",
                 method_name(m), level, got, read_ms, got > 0 ? ": '" : "",
                 got > 0 ? line : (got == 0 ? ": EOF, nothing written" : ": NOTHING (timeout)"),
                 got > 0 ? "'" : "", read_errno);
  if (level >= 2 && ipc[0] >= 0) {
    // The child sends its IPC message after its report line.
    for (long long t = now_ms(); now_ms() - t < kReadTimeoutMs;) {
      char probe_byte;
      const ssize_t peek = recv(ipc[0], &probe_byte, 1, MSG_PEEK | MSG_DONTWAIT);
      if (peek >= 0 || errno != EAGAIN || reaped) {
        break;
      }
      if (waitpid(pid, &status, WNOHANG) == pid) {
        reaped = 1;
      }
      sleep_ms(20);
    }
    read_ipc(ipc[0]);
  }

  // Its exit status.
  const long long wait_start = now_ms();
  int killed = 0;
  int wait_errno = 0;
  while (!reaped) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = 1;
      break;
    }
    if (r < 0 && errno != EINTR) {
      wait_errno = errno;
      break;
    }
    const long long waited = now_ms() - wait_start;
    if (!killed && waited >= kExitTimeoutMs) {
      killed = 1;
      crumb("child did not exit, sending SIGKILL");
      if (kill(pid, SIGKILL) != 0) {
        const int err = errno;
        ps5_log_printf("probe: fork %s: kill(%d, SIGKILL) failed: errno %d %s", method_name(m),
                       (int)pid, err, strerror(err));
      }
    }
    if (waited >= kExitTimeoutMs + kKillTimeoutMs) {
      break;
    }
    sleep_ms(20);
  }
  char desc[32] = "";
  if (reaped) {
    describe_status(status, desc, sizeof(desc));
  }
  ps5_log_printf("probe: fork %s level %d: waitpid(%d) %s%s%s after %lld ms%s", method_name(m),
                 level, (int)pid, reaped ? "-> " : "FAILED: ", reaped ? desc : "",
                 reaped ? "" : (wait_errno ? strerror(wait_errno) : "not reaped (timeout)"),
                 now_ms() - wait_start, killed ? " (KILLED: it hung)" : "");
  if (pd >= 0) {
    close(pd);  // reaps a pdfork child that is still there
  }
  close(report[0]);
  if (ipc[0] >= 0) {
    close(ipc[0]);
  }
  crumb("child done");
  return got > 0;
}

// ---- Levels 3 and 4: the real rfork_thread NID import ------------------------
#if PROSPEROWEB_NATIVE_TITLE

enum { kRforkStackSize = 4 * 1024 * 1024 };  // a few MiB; the stack grows down
enum { kReportMarker = 0x50524F42 };          // 'PROB', the child-ran marker

// Level 3: the child shares the parent's address space (RFMEM), so it reports
// by writing into this struct (static, always mapped and shared). volatile so
// the parent's spin-wait re-reads it. No fd is needed (RFCFDG cleared them).
static volatile struct {
  int marker;  // kReportMarker once pid/ppid are written
  int pid;
  int ppid;
} g_rfork_report;

// Level 3 child: fill the shared struct, publish the marker last, _exit. It
// does nothing else (the heap and locks are the parent's, shared).
static int rfork_child_shared(void* arg) {
  (void)arg;
  g_rfork_report.pid = (int)getpid();
  g_rfork_report.ppid = (int)getppid();
  __sync_synchronize();  // pid/ppid are visible before the marker
  g_rfork_report.marker = kReportMarker;
  _exit(0);
}

// Level 4 child: its own copy of the address space; write one line to the
// inherited socket end (arg is the fd), then _exit. snprintf on the private
// stack is fine here.
static int rfork_child_copied(void* arg) {
  const int fd = (int)(intptr_t)arg;
  char line[96];
  const int n = snprintf(line, sizeof(line),
                         "l4 child pid %d ppid %d via rfork_thread(RFPROC|RFFDG)",
                         (int)getpid(), (int)getppid());
  write(fd, line, (size_t)(n > 0 ? n : 0));
  _exit(0);
}

// Calls the real rfork_thread import once for level 3 or 4, waits for the
// child's report with a timeout, and SIGKILLs + reaps a child that hangs.
// boot.txt gets a breadcrumb immediately before and after the call, with the
// flags, so a panic points at the exact flag combination. Returns 1 when the
// child reported back. Never blocks the start-up beyond the bounded waits.
static int rfork_probe(int level) {
  const int flags = (level == 3) ? (RFPROC | RFCFDG | RFMEM) : (RFPROC | RFFDG);
  const char* ftxt = (level == 3) ? "RFPROC|RFCFDG|RFMEM" : "RFPROC|RFFDG";
  char step[160];

  void* stack = mmap(NULL, (size_t)kRforkStackSize, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
  if (stack == MAP_FAILED) {
    const int err = errno;
    ps5_log_printf("probe: fork level %d: child stack mmap(%d bytes) FAILED: errno %d %s",
                   level, kRforkStackSize, err, strerror(err));
    return 0;
  }
  char* stack_top = (char*)stack + kRforkStackSize - 16;  // top-aligned (grows down)

  int pair[2] = {-1, -1};
  void* arg;
  int (*child_fn)(void*);
  if (level == 3) {
    g_rfork_report.marker = 0;
    g_rfork_report.pid = 0;
    g_rfork_report.ppid = 0;
    __sync_synchronize();
    arg = NULL;
    child_fn = rfork_child_shared;
  } else {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
      const int err = errno;
      ps5_log_printf("probe: fork level 4: report socketpair FAILED: errno %d %s", err,
                     strerror(err));
      munmap(stack, (size_t)kRforkStackSize);
      return 0;
    }
    arg = (void*)(intptr_t)pair[1];
    child_fn = rfork_child_copied;
  }

  ps5_log_printf("probe: fork level %d: calling the real rfork_thread import (NID bSDxEpGzmUE) "
                 "with %s; parent pid %d uid %d, child stack %p..%p",
                 level, ftxt, (int)getpid(), (int)getuid(), stack, (void*)stack_top);
  snprintf(step, sizeof(step), "level %d: about to call rfork_thread(%s)", level, ftxt);
  crumb(step);
  const long long t0 = now_ms();
  const pid_t pid = rfork_thread(flags, stack_top, child_fn, arg);
  const int launch_errno = pid < 0 ? errno : 0;
  const long long launch_ms = now_ms() - t0;
  snprintf(step, sizeof(step), "level %d: rfork_thread(%s) returned %d (errno %d) in %lld ms", level,
           ftxt, (int)pid, launch_errno, launch_ms);
  crumb(step);

  if (pair[1] >= 0) {
    close(pair[1]);  // the parent keeps only its own end
  }
  if (pid < 0) {
    ps5_log_printf("probe: fork level %d: rfork_thread(%s) REFUSED: errno %d %s (%lld ms)", level,
                   ftxt, launch_errno, strerror(launch_errno), launch_ms);
    if (pair[0] >= 0) {
      close(pair[0]);
    }
    munmap(stack, (size_t)kRforkStackSize);
    return 0;
  }
  ps5_log_printf("probe: fork level %d: child pid %d created in %lld ms (%s); waiting for its report",
                 level, (int)pid, launch_ms, ftxt);

  // Wait for the child's report: the shared marker (level 3) or a socket line
  // (level 4). Never a blocking call; also poll the child with WNOHANG.
  int observed = 0;
  int reaped = 0;
  int status = 0;
  char line[160] = "";
  const long long start = now_ms();
  while (now_ms() - start < kReadTimeoutMs) {
    if (level == 3) {
      __sync_synchronize();
      if (g_rfork_report.marker == kReportMarker) {
        observed = 1;
        break;
      }
    } else {
      int err = 0;
      const ssize_t got = read_now(pair[0], line, sizeof(line), &err);
      if (got > 0) {
        observed = 1;
        break;
      }
      if (got == 0) {
        break;  // the child closed its end without writing
      }
    }
    if (!reaped && waitpid(pid, &status, WNOHANG) == pid) {
      reaped = 1;
      if (level == 3) {
        __sync_synchronize();
        observed = g_rfork_report.marker == kReportMarker;
      } else {
        int err = 0;
        observed = read_now(pair[0], line, sizeof(line), &err) > 0;
      }
      break;
    }
    sleep_ms(20);
  }
  const long long report_ms = now_ms() - start;

  if (level == 3) {
    ps5_log_printf("probe: fork level 3: child report %s after %lld ms: marker 0x%x pid %d ppid %d "
                   "(parent pid %d)",
                   observed ? "OBSERVED" : "NOT SEEN (timeout)", report_ms, g_rfork_report.marker,
                   g_rfork_report.pid, g_rfork_report.ppid, (int)getpid());
  } else {
    ps5_log_printf("probe: fork level 4: child report %s after %lld ms%s%s%s (parent pid %d)",
                   observed ? "OBSERVED" : "NOT SEEN (timeout/EOF)", report_ms,
                   observed ? ": '" : "", observed ? line : "", observed ? "'" : "", (int)getpid());
  }

  // Reap; SIGKILL a child that will not exit, then reap again.
  const long long wait_start = now_ms();
  int killed = 0;
  int wait_errno = 0;
  while (!reaped) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = 1;
      break;
    }
    if (r < 0 && errno != EINTR) {
      wait_errno = errno;
      break;
    }
    const long long waited = now_ms() - wait_start;
    if (!killed && waited >= kExitTimeoutMs) {
      killed = 1;
      crumb("rfork child did not exit, sending SIGKILL");
      if (kill(pid, SIGKILL) != 0) {
        const int err = errno;
        ps5_log_printf("probe: fork level %d: kill(%d, SIGKILL) failed: errno %d %s", level,
                       (int)pid, err, strerror(err));
      }
    }
    if (waited >= kExitTimeoutMs + kKillTimeoutMs) {
      break;
    }
    sleep_ms(20);
  }
  char desc[32] = "";
  if (reaped) {
    describe_status(status, desc, sizeof(desc));
  }
  ps5_log_printf("probe: fork level %d: waitpid(%d) %s%s%s after %lld ms%s", level, (int)pid,
                 reaped ? "-> " : "FAILED: ", reaped ? desc : "",
                 reaped ? "" : (wait_errno ? strerror(wait_errno) : "not reaped (timeout)"),
                 now_ms() - wait_start, killed ? " (KILLED: it hung)" : "");

  if (pair[0] >= 0) {
    close(pair[0]);
  }
  munmap(stack, (size_t)kRforkStackSize);
  return observed;
}

#endif  // PROSPEROWEB_NATIVE_TITLE

static atomic_int g_probe_done;

static void* probe_main(void* arg) {
  const int level = (int)(intptr_t)arg;
  int h_fork = -1, h_rfork_thread = -1, h_vfork = -1, h_pdfork = -1, h_rfork = -1;
  crumb("resolving fork, rfork_thread, vfork, pdfork, rfork in libkernel");
  g_fork = (fork_fn)resolve("fork", &h_fork);
  g_rfork_thread = (rfork_thread_fn)resolve("rfork_thread", &h_rfork_thread);
  g_vfork = (fork_fn)resolve("vfork", &h_vfork);
  g_pdfork = (pdfork_fn)resolve("pdfork", &h_pdfork);
  void* rfork = resolve("rfork", &h_rfork);  // reported only: rfork_thread covers it
  char s[5][64];
  describe_symbol(s[0], sizeof(s[0]), "fork", (void*)g_fork, h_fork);
  describe_symbol(s[1], sizeof(s[1]), "rfork_thread", (void*)g_rfork_thread, h_rfork_thread);
  describe_symbol(s[2], sizeof(s[2]), "vfork", (void*)g_vfork, h_vfork);
  describe_symbol(s[3], sizeof(s[3]), "pdfork", (void*)g_pdfork, h_pdfork);
  describe_symbol(s[4], sizeof(s[4]), "rfork", rfork, h_rfork);
  ps5_log_printf("probe: fork symbols: %s; %s; %s; %s; %s", s[0], s[1], s[2], s[3], s[4]);

  if (level >= 3) {
    // The real NID-linked rfork_thread test (level 3 or 4 exactly, no sweep):
    // each level is run on its own so a panic points at one flag combination.
#if PROSPEROWEB_NATIVE_TITLE
    const int ok = rfork_probe(level);
    ps5_log_printf("probe: fork level %d RESULT: rfork_thread(%s) %s", level,
                   level == 3 ? "RFPROC|RFCFDG|RFMEM" : "RFPROC|RFFDG",
                   ok ? "created a child that reported back" : "did NOT report back (see above)");
#else
    ps5_log_printf("probe: fork level %d: the rfork_thread import path is not built "
                   "(needs the native title); nothing attempted",
                   level);
#endif
  } else {
    int works = -1;
    for (int m = 0; m < kMethods && works < 0; m++) {
      if (!method_available((enum method)m)) {
        ps5_log_printf("probe: fork %s: not exported to the title, skipped",
                       method_name((enum method)m));
        continue;
      }
      if (try_method((enum method)m, 1)) {
        works = m;
      }
    }
    if (works < 0) {
      ps5_log_printf("probe: fork RESULT: no method created a child that reported back");
    } else {
      ps5_log_printf("probe: fork RESULT: %s creates a child that inherits descriptors",
                     method_name((enum method)works));
      if (level >= 2) {
        ps5_log_printf("probe: fork level 2 RESULT: %s",
                       try_method((enum method)works, 2) ? "child ran its IPC step (see above)"
                                                         : "child did NOT report back");
      }
    }
  }
  crumb("probe finished");
  atomic_store(&g_probe_done, 1);
  return NULL;
}

void native_fork_probe(int level) {
  if (level <= 0) {
    return;
  }
  ps5_log_printf("probe: fork probe level %d (config.json \"probe_fork\"): pid %d uid %d, "
                 "before the decoder, the broker and WebKit",
                 level, (int)getpid(), (int)getuid());
  crumb("starting the probe thread");
  pthread_t thread;
  const int rc = pthread_create(&thread, NULL, probe_main, (void*)(intptr_t)level);
  if (rc != 0) {
    ps5_log_printf("probe: fork: cannot start the probe thread: error %d %s", rc, strerror(rc));
    return;
  }
  pthread_detach(thread);
  const long long start = now_ms();
  while (!atomic_load(&g_probe_done) && now_ms() - start < kStartupWaitMs) {
    sleep_ms(50);
  }
  if (!atomic_load(&g_probe_done)) {
    crumb("still running, start-up continues without it");
    ps5_log_printf("probe: fork probe still running after %d ms (a call hung); start-up "
                   "continues without it",
                   kStartupWaitMs);
  }
}
