// SPDX-License-Identifier: GPL-3.0-or-later
//
// Multi-call entry point. The browser ships as one static ELF; the role is
// selected by argv[0] (set by ps5_spawn_self in the launcher), then by a
// --role=<name> argument, then by the PROSPEROBROWSER_ROLE environment variable:
//
//   "WebProcess"     -> ps5_webkit_web_process_main    (WebKit static lib)
//   "NetworkProcess" -> ps5_webkit_network_process_main (WebKit static lib)
//   "spawn-broker"   -> ps5_spawn_broker_main          (src/ps5/spawn), the
//                       plain payload websrv starts to spawn the two above
//   anything else    -> the UI process (Ps5App)

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "prosperobrowser/core/legacy_profile.h"
#include "prosperobrowser/spawn/spawn.h"
#include "ps5webkit/entry.h"
#include "ps5_app.h"
#include "ps5_cpu_affinity.h"
#include "ps5_cpu_monitor.h"
#include "ps5_log.h"
#include "ps5_runtime_env.h"

namespace {

// Returns the basename of a path (argv[0] may be absolute).
const char* basename_of(const char* path) {
  if (!path) {
    return "";
  }
  const char* slash = std::strrchr(path, '/');
  return slash ? slash + 1 : path;
}

constexpr const char* kBrokerRole = "spawn-broker";

// Resolves the process role from argv[0], then --role=, then PROSPEROBROWSER_ROLE.
std::string resolve_role(int argc, char** argv) {
  if (argc > 0 && argv[0]) {
    std::string role = basename_of(argv[0]);
    if (role == "WebProcess" || role == "NetworkProcess" || role == kBrokerRole) {
      return role;
    }
  }
  for (int i = 0; i < argc; i++) {
    if (argv[i] && std::strncmp(argv[i], "--role=", 7) == 0 && argv[i][7]) {
      return argv[i] + 7;
    }
  }
  if (const char* env = std::getenv("PROSPEROBROWSER_ROLE")) {
    return env;
  }
  return "UI";
}

// The UI's own ELF is what the spawner re-executes for WebProcess and
// NetworkProcess. When the loader passed an absolute *.elf path as argv[0],
// export it, so the children run exactly the binary the UI was started from
// (not a stale copy at the default install path). PROSPEROBROWSER_SELF_PATH set
// by the user wins; otherwise the spawner uses /data/prosperobrowser/prosperobrowser.elf.
void export_self_path(int argc, char** argv) {
  if (argc < 1 || !argv[0] || argv[0][0] != '/') {
    return;
  }
  const std::string path = argv[0];
  const std::string suffix = ".elf";
  if (path.size() <= suffix.size() ||
      path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0 ||
      access(path.c_str(), R_OK) != 0) {
    return;
  }
  setenv("PROSPEROBROWSER_SELF_PATH", path.c_str(), /*overwrite=*/0);
}

// The project was called ps5browser before: carry its profile over once
// (UI role, before config.json is read). Copies, never moves, so the old
// build keeps working from /data/ps5browser.
void migrate_legacy_profile_dir() {
  using namespace prosperobrowser::frontend;
  const auto result = prosperobrowser::core::migrate_legacy_profile(kLegacyDataDir, kPs5DataDir);
  if (!result.ran) {
    return;
  }
  log_printf("migrate: copied %d items from %s", result.copied, kLegacyDataDir);
  for (const auto& failure : result.failed) {
    log_printf("migrate: failed: %s", failure.c_str());
  }
}

// Logs the PS5BROWSER_* variables adopted at startup, once per launch.
void log_legacy_env(const std::vector<std::string>& adopted) {
  using prosperobrowser::frontend::log_printf;
  if (adopted.empty()) {
    return;
  }
  std::string names;
  for (const auto& name : adopted) {
    names += (names.empty() ? "" : " ") + name;
  }
  log_printf("env: deprecated PS5BROWSER_* variables used (%s); rename them to "
             "PROSPEROBROWSER_*, the old names stop working in the next release",
             names.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  using namespace prosperobrowser::frontend;
  // Before anything reads the environment (the role, then every module).
  // The children inherit the new names, so only the UI or broker logs this.
  const std::vector<std::string> legacy_env = prosperobrowser::core::adopt_legacy_env();
  std::string role = resolve_role(argc, argv);
  const bool child = role == "WebProcess" || role == "NetworkProcess";
  const bool broker = role == kBrokerRole;
  const std::string log_path = std::string(kPs5DataDir) + "/log.txt";

  // websrv starts the broker with stdout/stderr on a pipe it has already
  // closed: logging must not die of SIGPIPE before the broker redirects them.
  if (broker) {
    std::signal(SIGPIPE, SIG_IGN);
  }

  // The UI logs from the first line on (its environment defaults included);
  // the children only open the shared log after their (identical) environment
  // setup, to keep it readable.
  log_set_role(broker ? "broker" : role);
  if (!child) {
    log_open(log_path);
    log_printf("main: pid=%d ppid=%d argv0=%s", static_cast<int>(getpid()),
               static_cast<int>(getppid()), argc > 0 && argv[0] ? argv[0] : "(none)");
    log_legacy_env(legacy_env);
  }
  if (!child && !broker) {
    migrate_legacy_profile_dir();
  }

  // Before any role starts: WebProcess and NetworkProcess read fontconfig,
  // TLS and temp-dir settings from their own environment.
  setup_runtime_env(kPs5DataDir);
  if (!child) {
    export_self_path(argc, argv);
  } else {
    log_open(log_path);
    log_printf("main: pid=%d ppid=%d started", static_cast<int>(getpid()),
               static_cast<int>(getppid()));
  }

  // Every role, before it starts a thread: the threads inherit the mask.
  tune_process_cpus();

  if (broker) {
    const int rc = ps5_spawn_broker_main();
    log_printf("main: broker returned %d", rc);
    return rc;
  }
  if (role == "WebProcess") {
    // The main thread also runs the media presentation tick: above the
    // decoder threads, which inherit the default priority.
    raise_main_thread_priority();
    start_cpu_monitor();
    const int rc = ps5_webkit_web_process_main(argc, argv);
    log_printf("main: returned %d", rc);
    return rc;
  }
  if (role == "NetworkProcess") {
    const int rc = ps5_webkit_network_process_main(argc, argv);
    log_printf("main: returned %d", rc);
    return rc;
  }

  start_cpu_monitor();
  Ps5App app;
  return app.run();
}
