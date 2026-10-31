// SPDX-License-Identifier: GPL-3.0-or-later
//
// Definition of the WebKit spawn contract (webkit-port/include/ps5webkit/
// spawn.h) on top of the elfldr-derived loader. PS5 target only.

#include "ps5browser/spawn/spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "elfldr.h"

// Our own ELF image, mapped (or, if mmap fails, read) once and kept for the
// lifetime of the process. WebKit spawns auxiliary processes repeatedly, so
// re-reading the file each time would be wasteful; a read-only private
// mapping keeps the large image out of the UI heap.
static pthread_mutex_t g_image_lock = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t* g_image_bytes = NULL;
static size_t g_image_size = 0;

// Adapter: this libc declares getenv() as char*(const char*); the resolver
// takes a const char*(const char*) lookup.
static const char* getenv_const(const char* name) { return getenv(name); }

static uint8_t* read_whole_file(int fd, size_t size) {
  uint8_t* bytes = malloc(size);
  if (!bytes) {
    return NULL;
  }
  size_t done = 0;
  while (done < size) {
    ssize_t n = read(fd, bytes + done, size - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      free(bytes);
      return NULL;
    }
    done += (size_t)n;
  }
  return bytes;
}

static int load_self_image(const char* argv0) {
  char path[1024];
  if (ps5_spawn_resolve_self_path(argv0, getenv_const, path, sizeof(path)) != 0) {
    fprintf(stderr, "ps5_spawn: cannot resolve self ELF path\n");
    return -1;
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "ps5_spawn: cannot open %s: %s\n", path, strerror(errno));
    return -1;
  }

  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    fprintf(stderr, "ps5_spawn: cannot stat %s\n", path);
    close(fd);
    return -1;
  }

  size_t size = (size_t)st.st_size;
  const uint8_t* bytes = NULL;
  int mapped = 0;
  void* map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map != MAP_FAILED) {
    bytes = map;
    mapped = 1;
  } else {
    fprintf(stderr, "ps5_spawn: mmap %s failed (%s), reading it\n", path, strerror(errno));
    bytes = read_whole_file(fd, size);
  }
  close(fd);
  if (!bytes) {
    fprintf(stderr, "ps5_spawn: cannot read %s\n", path);
    return -1;
  }

  if (elfldr_sanity_check(bytes, size) != 0) {
    fprintf(stderr, "ps5_spawn: %s is not a valid ELF\n", path);
    if (mapped) {
      munmap((void*)bytes, size);
    } else {
      free((void*)bytes);
    }
    return -1;
  }

  fprintf(stderr, "ps5_spawn: self ELF %s (%zu bytes, %s)\n", path, size,
          mapped ? "mapped" : "read");
  g_image_bytes = bytes;
  g_image_size = size;
  return 0;
}

// Returns the cached image, loading it on first use. argv0 is only used as a
// last-resort path hint (see ps5_spawn_resolve_self_path).
static const uint8_t* self_image(const char* argv0, size_t* out_size) {
  const uint8_t* result = NULL;
  pthread_mutex_lock(&g_image_lock);
  if (!g_image_bytes) {
    load_self_image(argv0);
  }
  if (g_image_bytes) {
    result = g_image_bytes;
    *out_size = g_image_size;
  }
  pthread_mutex_unlock(&g_image_lock);
  return result;
}

// --- Child bookkeeping -------------------------------------------------------
//
// Every child is tracked until it is reaped, either by ps5_terminate_process()
// or, for children that exit on their own (a WebProcess crash, a
// NetworkProcess exit), by a reaper thread that polls them. Only tracked pids
// are waited for: a waitpid(-1) would also swallow the ptrace stops the
// loader waits for while it sets up a new child.

#define PS5_SPAWN_MAX_CHILDREN 64
#define PS5_SPAWN_REAP_INTERVAL_MS 1000
#define PS5_SPAWN_KILL_TIMEOUT_MS 2000

static pthread_mutex_t g_children_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_children[PS5_SPAWN_MAX_CHILDREN];
static int g_nchildren = 0;
static int g_reaper_started = 0;

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

// 1 if pid is gone (reaped now, or not our child any more), 0 if still alive.
static int try_reap(pid_t pid) {
  for (;;) {
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      return 1;
    }
    if (r == 0) {
      return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    return 1;  // ECHILD: already reaped
  }
}

// Caller holds g_children_lock. Returns 1 if pid was tracked.
static int untrack_locked(pid_t pid) {
  for (int i = 0; i < g_nchildren; i++) {
    if (g_children[i] == pid) {
      g_children[i] = g_children[--g_nchildren];
      return 1;
    }
  }
  return 0;
}

static void* reaper_main(void* arg) {
  (void)arg;
  for (;;) {
    sleep_ms(PS5_SPAWN_REAP_INTERVAL_MS);
    pthread_mutex_lock(&g_children_lock);
    for (int i = 0; i < g_nchildren;) {
      if (try_reap(g_children[i])) {
        fprintf(stderr, "ps5_spawn: child %d exited\n", (int)g_children[i]);
        g_children[i] = g_children[--g_nchildren];
      } else {
        i++;
      }
    }
    pthread_mutex_unlock(&g_children_lock);
  }
  return NULL;
}

static void track_child(pid_t pid) {
  pthread_mutex_lock(&g_children_lock);
  if (g_nchildren < PS5_SPAWN_MAX_CHILDREN) {
    g_children[g_nchildren++] = pid;
  } else {
    fprintf(stderr, "ps5_spawn: too many children, %d is not reaped\n", (int)pid);
  }
  if (!g_reaper_started) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, reaper_main, NULL) == 0) {
      pthread_detach(thread);
      g_reaper_started = 1;
    } else {
      fprintf(stderr, "ps5_spawn: cannot start the reaper thread\n");
    }
  }
  pthread_mutex_unlock(&g_children_lock);
}

int ps5_spawn_self(const char* const* argv, const int* fds, int nfds) {
  if (!argv || !argv[0]) {
    return -1;
  }

  struct ps5_spawn_fd_entry table[PS5_SPAWN_MAX_FDS];
  int ntable = ps5_spawn_build_fd_table(fds, nfds, table, PS5_SPAWN_MAX_FDS);
  if (ntable < 0) {
    return -1;
  }

  // argv[0] is the child's role name, never a path: resolve the image from
  // PS5BROWSER_SELF_PATH (exported by main() from the UI's own argv[0]) or
  // the default install path.
  size_t image_size = 0;
  const uint8_t* image = self_image(NULL, &image_size);
  if (!image) {
    return -1;
  }

  // rfork_thread/execve want a mutable, NUL-terminated argv; copy the caller's
  // (which is only valid for the duration of this call).
  int argc = 0;
  while (argv[argc]) {
    argc++;
  }
  char** child_argv = calloc((size_t)argc + 1, sizeof(char*));
  if (!child_argv) {
    return -1;
  }
  int ok = 1;
  for (int i = 0; i < argc && ok; i++) {
    child_argv[i] = strdup(argv[i]);
    ok = child_argv[i] != NULL;
  }

  int pid = -1;
  if (ok) {
    pid = elfldr_spawn_fds(child_argv, table, ntable, image, image_size);
  }
  if (pid > 0) {
    track_child(pid);
  }

  for (int i = 0; i < argc; i++) {
    free(child_argv[i]);
  }
  free(child_argv);

  return pid;
}

int ps5_terminate_process(int pid) {
  if (pid <= 0) {
    return -1;
  }
  // An untracked pid was already reaped (by the reaper thread); it may have
  // been recycled since, so it must not be signalled.
  pthread_mutex_lock(&g_children_lock);
  int tracked = untrack_locked(pid);
  pthread_mutex_unlock(&g_children_lock);
  if (!tracked) {
    return 0;
  }

  kill(pid, SIGKILL);  // best-effort: it may already be a zombie

  // SIGKILL is asynchronous: wait (bounded) for the child to become
  // reapable instead of a single WNOHANG that usually comes too early.
  for (int waited = 0; waited < PS5_SPAWN_KILL_TIMEOUT_MS; waited += 10) {
    if (try_reap(pid)) {
      return 0;
    }
    sleep_ms(10);
  }
  fprintf(stderr, "ps5_spawn: child %d did not exit after SIGKILL\n", pid);
  track_child(pid);  // let the reaper collect it later
  return -1;
}
