// SPDX-License-Identifier: GPL-3.0-or-later
//
// Multi-call entry point. The browser ships as one static ELF; the role is
// selected by argv[0] (set by ps5_spawn_self in the launcher) and, as a
// fallback, by the PS5BROWSER_ROLE environment variable:
//
//   "WebProcess"     -> ps5_webkit_web_process_main    (WebKit static lib)
//   "NetworkProcess" -> ps5_webkit_network_process_main (WebKit static lib)
//   anything else    -> the UI process (Ps5App)

#include <cstdlib>
#include <cstring>
#include <string>

#include "ps5webkit/entry.h"
#include "ps5_app.h"
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

// Resolves the process role from argv[0], then PS5BROWSER_ROLE.
std::string resolve_role(int argc, char** argv) {
  if (argc > 0 && argv[0]) {
    std::string role = basename_of(argv[0]);
    if (role == "WebProcess" || role == "NetworkProcess") {
      return role;
    }
  }
  if (const char* env = std::getenv("PS5BROWSER_ROLE")) {
    return env;
  }
  return "UI";
}

}  // namespace

int main(int argc, char** argv) {
  std::string role = resolve_role(argc, argv);

  // Before any role starts: WebProcess and NetworkProcess read fontconfig,
  // TLS and temp-dir settings from their own environment.
  ps5browser::frontend::setup_runtime_env(ps5browser::frontend::kPs5DataDir);

  if (role == "WebProcess") {
    return ps5_webkit_web_process_main(argc, argv);
  }
  if (role == "NetworkProcess") {
    return ps5_webkit_network_process_main(argc, argv);
  }

  ps5browser::frontend::Ps5App app;
  return app.run();
}
