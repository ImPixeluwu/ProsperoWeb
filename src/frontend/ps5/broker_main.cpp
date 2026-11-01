// SPDX-License-Identifier: GPL-3.0-or-later
//
// prosperoweb-broker.elf: the spawn broker alone, without WebKit (a few
// hundred KB instead of the 176 MB multi-call ELF). The native title starts
// it through elfldr, which reads the whole payload into its own heap first
// (elfldr uri.c path_read), or through websrv. It spawns the WebProcess and
// NetworkProcess from PROSPEROWEB_SELF_PATH (the multi-call prosperoweb.elf)
// exactly as the "spawn-broker" role of that ELF does. --env=NAME=VALUE
// arguments are exported first (elfldr passes no environment).

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "prosperoweb/spawn/spawn.h"
#include "ps5_log.h"
#include "ps5_runtime_env.h"

int main(int argc, char** argv) {
  using namespace prosperoweb::frontend;
  for (int i = 1; i < argc; i++) {
    if (!argv[i] || std::strncmp(argv[i], "--env=", 6) != 0) {
      continue;
    }
    const std::string entry = argv[i] + 6;
    const size_t eq = entry.find('=');
    if (eq != 0 && eq != std::string::npos) {
      setenv(entry.substr(0, eq).c_str(), entry.substr(eq + 1).c_str(), 1);
    }
  }
  umask(0);
  // The loader's stdout/stderr may be a closed pipe.
  std::signal(SIGPIPE, SIG_IGN);
  mkdir(kPs5DataDir, 0755);
  log_set_role("broker");
  log_open(std::string(kPs5DataDir) + "/log.txt");
  const char* self = std::getenv("PROSPEROWEB_SELF_PATH");
  log_printf("main: broker-only ELF pid=%d ppid=%d argv0=%s, children from %s",
             static_cast<int>(getpid()), static_cast<int>(getppid()),
             argc > 0 && argv[0] ? argv[0] : "(none)", self ? self : "(default)");
  const int rc = ps5_spawn_broker_main();
  log_printf("main: broker returned %d", rc);
  return rc;
}
