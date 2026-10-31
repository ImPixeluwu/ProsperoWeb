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
  std::vector<int> fds;           // descriptors of src_pid -> child fd 3+i
  // Process that owns fds: 0 for the broker itself (they arrived as
  // SCM_RIGHTS over AF_UNIX), the client's pid when the client named them
  // over TCP ("src_pid"/"src_fds"; the backend copies them with rdup).
  int src_pid = 0;
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
  // AF_UNIX paths tried in order; the first that binds is used.
  std::vector<std::string> socket_paths;
  // Loopback TCP port, listened on in addition to the AF_UNIX socket (and
  // alone when no path binds). 0 picks an ephemeral port, -1 disables TCP.
  int tcp_port = -1;
  // If set, receives the bound TCP port (or -1) before clients are served.
  std::atomic<int>* bound_tcp_port = nullptr;
  // Exit if no client connects within this long after start. Once a client
  // has connected, the broker exits as soon as that connection closes,
  // killing the children.
  int idle_ms = 10000;
  // How often the reaper polls the children.
  int reap_interval_ms = 500;
};

// Runs the broker until it is idle, *stop becomes true (may be null), or
// a fatal socket error (including: nothing could be listened on). Returns a process exit code (0 on a clean exit).
int broker_serve(const BrokerServerOptions& options, const std::atomic<bool>* stop);

}  // namespace ps5browser::spawn
