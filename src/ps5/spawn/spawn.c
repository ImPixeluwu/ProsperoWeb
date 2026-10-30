// SPDX-License-Identifier: GPL-3.0-or-later
//
// Definition of the WebKit spawn contract (webkit-port/include/ps5webkit/
// spawn.h) on top of the elfldr-derived loader. PS5 target only.

#include "ps5browser/spawn/spawn.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/wait.h>

#include "elfldr.h"

// Our own ELF image, read once and cached for the lifetime of the process.
// WebKit spawns auxiliary processes repeatedly, so re-reading the file each
// time would be wasteful.
static pthread_mutex_t g_image_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t* g_image_bytes = NULL;
static size_t g_image_size = 0;

// Adapter: this libc declares getenv() as char*(const char*); the resolver
// takes a const char*(const char*) lookup.
static const char* getenv_const(const char* name) { return getenv(name); }

static int load_self_image(const char* argv0) {
  char path[1024];
  if (ps5_spawn_resolve_self_path(argv0, getenv_const, path, sizeof(path)) != 0) {
    fprintf(stderr, "ps5_spawn: cannot resolve self ELF path\n");
    return -1;
  }

  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "ps5_spawn: cannot open %s\n", path);
    return -1;
  }

  struct stat st;
  if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) {
    fprintf(stderr, "ps5_spawn: cannot stat %s\n", path);
    fclose(f);
    return -1;
  }

  size_t size = (size_t)st.st_size;
  uint8_t* bytes = malloc(size);
  if (!bytes) {
    fclose(f);
    return -1;
  }

  if (fread(bytes, 1, size, f) != size) {
    fprintf(stderr, "ps5_spawn: short read of %s\n", path);
    free(bytes);
    fclose(f);
    return -1;
  }
  fclose(f);

  if (elfldr_sanity_check(bytes, size) != 0) {
    fprintf(stderr, "ps5_spawn: %s is not a valid ELF\n", path);
    free(bytes);
    return -1;
  }

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

int ps5_spawn_self(const char* const* argv, const int* fds, int nfds) {
  if (!argv || !argv[0]) {
    return -1;
  }

  struct ps5_spawn_fd_entry table[PS5_SPAWN_MAX_FDS];
  int ntable = ps5_spawn_build_fd_table(fds, nfds, table, PS5_SPAWN_MAX_FDS);
  if (ntable < 0) {
    return -1;
  }

  size_t image_size = 0;
  const uint8_t* image = self_image(argv[0], &image_size);
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
  // SIGKILL is best-effort: the process may already be gone.
  kill(pid, SIGKILL);

  // Reap without blocking; ESRCH/ECHILD simply mean it was already reaped.
  for (;;) {
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == 0 || r == pid || r < 0) {
      break;
    }
  }
  return 0;
}
