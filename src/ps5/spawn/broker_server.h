// SPDX-License-Identifier: GPL-3.0-or-later
//
// The spawn broker server: a plain-payload process that performs the
// rfork/ptrace/execve spawns on behalf of the UI (a big app, where doing
// them in-process panics the kernel). See README.md, "Spawn broker".
//
// Platform-independent: the actual spawn is a hook (elfldr on the PS5, a
// fork() fake in tests/ps5).
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace ps5browser::spawn {

struct BrokerSpawnRequest {
  std::string elf;                // ELF to run (mapped once per path)
  std::vector<std::string> argv;  // argv[0] is the role name
  std::vector<std::string> env;   // the child's whole environment
  std::vector<int> fds;           // broker-side descriptors -> child fd 3+i
};

struct BrokerSpawnResult {
  int pid = -1;       // > 0 on success
  std::string stage;  // failed stage, empty on success
  int err = 0;        // errno of the failed stage, 0 if none applies
  std::string error;  // human-readable detail
};

using BrokerSpawnHook = BrokerSpawnResult (*)(const BrokerSpawnRequest& request);

// Sets the function that performs a spawn. Must be called before
// broker_serve(); without a hook every spawn fails.
void broker_set_spawn_hook(BrokerSpawnHook hook);

struct BrokerServerOptions {
  std::string socket_path;
  // Exit once no client is connected and no child is alive for this long.
  int idle_ms = 10000;
  // With no client for this long, kill the remaining children and exit.
  int orphan_ms = 60000;
  // How often the reaper polls the children.
  int reap_interval_ms = 500;
};

// Runs the broker until it is idle, *stop becomes true (may be null), or
// a fatal socket error. Returns a process exit code (0 on a clean exit).
int broker_serve(const BrokerServerOptions& options, const std::atomic<bool>* stop);

}  // namespace ps5browser::spawn
