// SPDX-License-Identifier: GPL-3.0-or-later
//
// The WebKit spawn contract (webkit-port/include/ps5webkit/spawn.h) and the
// spawn broker role. PS5 target only.
//
// The UI runs as the hbldr big app, and rfork+ptrace+execve from a big app
// panics the console kernel. So by default (PS5BROWSER_SPAWN_MODE=broker)
// the UI asks the spawn broker, a plain payload started through websrv, to
// spawn for it; PS5BROWSER_SPAWN_MODE=direct keeps the in-process spawner
// (spawn.c) for plain-payload (elfldr) launches. See README.md.

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "broker_client.h"
#include "broker_proto.h"
#include "broker_server.h"
#include "spawn_backend.h"

extern char** environ;

namespace {

using namespace ps5browser::spawn;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr int kBrokerStartTimeoutMs = 10000;
constexpr int kWebsrvTimeoutMs = 30000;  // websrv reads the ELF before it answers
constexpr int kSpawnTimeoutMs = 60000;
constexpr int kTerminateTimeoutMs = 5000;
constexpr int kPingTimeoutMs = 5000;
constexpr const char* kDefaultWebsrv = "127.0.0.1:8080";

long long ms_since(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

std::string env_or(const char* name, const char* fallback) {
  const char* v = std::getenv(name);
  return v && v[0] ? v : fallback;
}

const char* getenv_const(const char* name) { return std::getenv(name); }

std::string socket_path() { return env_or("PS5BROWSER_SPAWN_SOCK", kBrokerDefaultSocket); }

bool direct_mode() {
  static const bool direct = [] {
    const std::string mode = env_or("PS5BROWSER_SPAWN_MODE", "broker");
    const bool d = mode == "direct";
    if (!d && mode != "broker") {
      ps5_log_printf("spawn: unknown PS5BROWSER_SPAWN_MODE=%s, using broker", mode.c_str());
    }
    ps5_log_printf("spawn: mode %s", d ? "direct (in-process rfork)" : "broker");
    return d;
  }();
  return direct;
}

std::string describe_status(int status) {
  char buf[32];
  if (status < 0) {
    std::snprintf(buf, sizeof(buf), "status unknown");
  } else if (WIFEXITED(status)) {
    std::snprintf(buf, sizeof(buf), "exit %d", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    std::snprintf(buf, sizeof(buf), "signal %d", WTERMSIG(status));
  } else {
    std::snprintf(buf, sizeof(buf), "status 0x%x", status);
  }
  return buf;
}

void on_child_exit(int pid, int status) {
  ps5_log_printf("spawn: child pid=%d exited (%s)", pid, describe_status(status).c_str());
}

std::mutex g_mu;  // serialises spawns and terminates
std::unique_ptr<BrokerClient> g_client;
std::set<int> g_direct_pids;  // children of a PS5BROWSER_SPAWN_FALLBACK=direct spawn

std::string dir_of(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
}

// Asks websrv to start "<self_path>" as a daemon payload in the broker role.
bool start_broker_via_websrv(const std::string& self_path) {
  const std::string websrv = env_or("PS5BROWSER_WEBSRV", kDefaultWebsrv);
  // argv[0] (also the process name) is the role; --role= and
  // PS5BROWSER_ROLE say the same in case a loader rewrites argv[0].
  const std::string args = "spawn-broker --role=spawn-broker";
  const std::string env = "PS5BROWSER_ROLE=spawn-broker PS5BROWSER_SPAWN_SOCK=" +
                          websrv_escape_token(socket_path()) +
                          " PS5BROWSER_SELF_PATH=" + websrv_escape_token(self_path);
  const std::string target =
      websrv_broker_target(self_path, dir_of(self_path), args, env);
  const auto t0 = Clock::now();
  std::string error;
  const int code = http_get(websrv, target, kWebsrvTimeoutMs, &error);
  if (code < 0) {
    ps5_log_printf("websrv: GET http://%s%s -> failed: %s", websrv.c_str(), target.c_str(),
                   error.c_str());
    return false;
  }
  ps5_log_printf("websrv: GET http://%s%s -> HTTP %d in %lldms", websrv.c_str(),
                 target.c_str(), code, ms_since(t0));
  return code == 200;
}

// Caller holds g_mu. Connects to the broker, starting it if needed.
bool ensure_broker(const std::string& self_path, bool may_start) {
  if (g_client && g_client->connected()) {
    return true;
  }
  if (!g_client) {
    g_client = std::make_unique<BrokerClient>(socket_path(), on_child_exit);
  }
  const auto t0 = Clock::now();
  ps5_log_printf("spawn: broker connecting to %s", socket_path().c_str());
  if (!g_client->connect_once()) {
    if (!may_start) {
      ps5_log_printf("spawn: broker not reachable: %s", std::strerror(errno));
      return false;
    }
    ps5_log_printf("spawn: broker not running (%s), asking websrv", std::strerror(errno));
    if (!start_broker_via_websrv(self_path)) {
      return false;
    }
    if (!g_client->connect_wait(kBrokerStartTimeoutMs)) {
      ps5_log_printf("spawn: broker did not come up within %dms (%s)", kBrokerStartTimeoutMs,
                     std::strerror(errno));
      return false;
    }
  }
  json reply;
  if (!g_client->request(json{{"op", "ping"}}, nullptr, 0, &reply, kPingTimeoutMs) ||
      !broker_get_bool(reply, "ok", false)) {
    ps5_log_printf("spawn: broker did not answer ping (%s)", std::strerror(errno));
    return false;
  }
  ps5_log_printf("spawn: broker ready pid=%lld in %lldms", broker_get_int(reply, "pid", -1),
                 ms_since(t0));
  return true;
}

int spawn_via_broker(const char* const* argv, const int* fds, int nfds) {
  const char* role = argv[0];
  char path[1024];
  if (ps5_spawn_resolve_self_path(nullptr, getenv_const, path, sizeof(path)) != 0) {
    ps5_log_printf("spawn: role=%s FAILED: cannot resolve the self ELF path", role);
    return -1;
  }
  if (nfds < 0 || nfds > kBrokerMaxFds) {
    ps5_log_printf("spawn: role=%s FAILED: invalid fd table (nfds=%d)", role, nfds);
    return -1;
  }

  json msg{{"op", "spawn"}, {"elf", path}, {"nfds", nfds}};
  json jargv = json::array();
  for (int i = 0; argv[i]; i++) {
    jargv.push_back(argv[i]);
  }
  json jenv = json::array();
  for (char** e = environ; e && *e; e++) {
    jenv.push_back(*e);
  }
  msg["argv"] = std::move(jargv);
  msg["env"] = std::move(jenv);

  std::lock_guard<std::mutex> lock(g_mu);
  if (ensure_broker(path, /*may_start=*/true)) {
    const auto t0 = Clock::now();
    json reply;
    if (g_client->request(msg, fds, nfds, &reply, kSpawnTimeoutMs)) {
      if (broker_get_bool(reply, "ok", false)) {
        const int pid = static_cast<int>(broker_get_int(reply, "pid", -1));
        ps5_log_printf("spawn: role=%s pid=%d via broker in %lldms", role, pid, ms_since(t0));
        return pid;
      }
      const int err = static_cast<int>(broker_get_int(reply, "errno", 0));
      ps5_log_printf("spawn: role=%s FAILED via broker at %s: errno %d (%s) %s", role,
                     broker_get_string(reply, "stage", "?").c_str(), err,
                     err ? std::strerror(err) : "no errno",
                     broker_get_string(reply, "error").c_str());
      return -1;
    }
    ps5_log_printf("spawn: role=%s: broker connection lost (%s)", role, std::strerror(errno));
  }

  if (env_or("PS5BROWSER_SPAWN_FALLBACK", "") == "direct") {
    ps5_log_printf("spawn: role=%s: no broker, PS5BROWSER_SPAWN_FALLBACK=direct, "
                   "spawning in-process",
                   role);
    const int pid = ps5_spawn_direct(argv, fds, nfds);
    if (pid > 0) {
      g_direct_pids.insert(pid);
    }
    return pid;
  }
  ps5_log_printf("spawn: role=%s FAILED: no spawn broker (in-process spawning from the "
                 "big app panics the console; PS5BROWSER_SPAWN_FALLBACK=direct forces it)",
                 role);
  return -1;
}

int terminate_via_broker(int pid) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_direct_pids.erase(pid)) {
    return ps5_terminate_direct(pid);
  }
  if (!ensure_broker("", /*may_start=*/false)) {
    ps5_log_printf("spawn: terminate pid=%d FAILED: no broker", pid);
    return -1;
  }
  json reply;
  if (!g_client->request(json{{"op", "terminate"}, {"pid", pid}}, nullptr, 0, &reply,
                         kTerminateTimeoutMs)) {
    ps5_log_printf("spawn: terminate pid=%d FAILED: broker connection lost (%s)", pid,
                   std::strerror(errno));
    return -1;
  }
  if (!broker_get_bool(reply, "ok", false)) {
    ps5_log_printf("spawn: terminate pid=%d FAILED: %s", pid,
                   broker_get_string(reply, "error", "?").c_str());
    return -1;
  }
  return 0;
}

// --- Broker role ------------------------------------------------------------

std::atomic<bool> g_stop{false};

void on_sigterm(int) { g_stop.store(true); }

std::vector<char*> c_strings(const std::vector<std::string>& in) {
  std::vector<char*> out;
  out.reserve(in.size() + 1);
  for (const auto& s : in) {
    out.push_back(const_cast<char*>(s.c_str()));
  }
  out.push_back(nullptr);
  return out;
}

BrokerSpawnResult elfldr_hook(const BrokerSpawnRequest& req) {
  std::vector<char*> argv = c_strings(req.argv);
  std::vector<char*> envp = c_strings(req.env);
  ps5_spawn_failure failure{};
  BrokerSpawnResult res;
  res.pid = ps5_spawn_elfldr(req.elf.c_str(), argv.data(), envp.data(), req.fds.data(),
                             static_cast<int>(req.fds.size()), &failure);
  if (res.pid <= 0) {
    res.pid = -1;
    res.stage = failure.stage ? failure.stage : "unknown stage";
    res.err = failure.err;
    res.error = failure.err ? std::strerror(failure.err) : "";
  }
  return res;
}

}  // namespace

extern "C" int ps5_spawn_self(const char* const* argv, const int* fds, int nfds) {
  if (!argv || !argv[0]) {
    ps5_log_line("spawn: called without a role");
    return -1;
  }
  return direct_mode() ? ps5_spawn_direct(argv, fds, nfds) : spawn_via_broker(argv, fds, nfds);
}

extern "C" int ps5_terminate_process(int pid) {
  if (pid <= 0) {
    return -1;
  }
  return direct_mode() ? ps5_terminate_direct(pid) : terminate_via_broker(pid);
}

extern "C" int ps5_spawn_broker_main(void) {
  // websrv hands the daemon the write end of a pipe as stdout/stderr and
  // closes the read end when the request does not ask for pipe=1: point
  // stdio at /dev/null (log.txt and klog carry the log), then restore
  // SIGPIPE (main() ignored it) so the children start as before.
  const int null_fd = open("/dev/null", O_RDWR);
  if (null_fd >= 0) {
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO) {
      close(null_fd);
    }
  }
  std::signal(SIGPIPE, SIG_DFL);

  struct sigaction sa {};
  sa.sa_handler = on_sigterm;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);

  BrokerServerOptions options;
  options.socket_path = socket_path();
  mkdir(dir_of(options.socket_path).c_str(), 0777);
  broker_set_spawn_hook(elfldr_hook);
  return broker_serve(options, &g_stop);
}
