// SPDX-License-Identifier: GPL-3.0-or-later
//
// The WebKit spawn contract (webkit-port/include/ps5webkit/spawn.h) and the
// spawn broker role. PS5 target only.
//
// The UI runs as the hbldr big app, and rfork+ptrace+execve from a big app
// panics the console kernel. So by default (PROSPEROBROWSER_SPAWN_MODE=broker)
// the UI asks the spawn broker, a plain payload started through websrv, to
// spawn for it; PROSPEROBROWSER_SPAWN_MODE=direct keeps the in-process spawner
// (spawn.c) for plain-payload (elfldr) launches. See README.md.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "broker_client.h"
#include "broker_proto.h"
#include "broker_server.h"
#include "cpuset_assign_ps5.h"
#include "spawn_backend.h"

extern char** environ;

namespace {

using namespace prosperobrowser::spawn;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr int kBrokerStartTimeoutMs = 5000;
// After this many consecutive spawns without a broker, wait kBackoffMs before
// the next attempt: WebKit relaunches a failed process right away, forever,
// and every attempt asks websrv to start another broker.
constexpr int kBackoffAfterFailures = 2;
constexpr int kBackoffMs = 30000;
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

std::vector<BrokerEndpoint> endpoints() {
  return broker_endpoints(std::getenv("PROSPEROBROWSER_SPAWN_SOCK"),
                          std::getenv("PROSPEROBROWSER_SPAWN_PORT"));
}

std::string describe(const std::vector<BrokerEndpoint>& eps) {
  std::string out;
  for (const auto& ep : eps) {
    out += (out.empty() ? "" : ", ") + ep.describe();
  }
  return out;
}

bool direct_mode() {
  static const bool direct = [] {
    const std::string mode = env_or("PROSPEROBROWSER_SPAWN_MODE", "broker");
    const bool d = mode == "direct";
    if (!d && mode != "broker") {
      ps5_log_printf("spawn: unknown PROSPEROBROWSER_SPAWN_MODE=%s, using broker", mode.c_str());
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
std::set<int> g_direct_pids;  // children of a PROSPEROBROWSER_SPAWN_FALLBACK=direct spawn
int g_broker_failures = 0;    // consecutive spawns that found no broker
Clock::time_point g_backoff_until{};

std::string dir_of(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
}

// Asks websrv to start "<self_path>" as a daemon payload in the broker role.
bool start_broker_via_websrv(const std::string& self_path) {
  const std::string websrv = env_or("PROSPEROBROWSER_WEBSRV", kDefaultWebsrv);
  // argv[0] (also the process name) is the role; --role= and
  // PROSPEROBROWSER_ROLE say the same in case a loader rewrites argv[0].
  const std::string args = "spawn-broker --role=spawn-broker";
  // The broker resolves the same endpoints as we do: forward the overrides.
  std::string env = "PROSPEROBROWSER_ROLE=spawn-broker";
  const char* sock = std::getenv("PROSPEROBROWSER_SPAWN_SOCK");
  if (sock && sock[0]) {
    env += " PROSPEROBROWSER_SPAWN_SOCK=" + websrv_escape_token(sock);
  }
  env += " PROSPEROBROWSER_SPAWN_PORT=" +
         std::to_string(broker_tcp_port(std::getenv("PROSPEROBROWSER_SPAWN_PORT")));
  env += " PROSPEROBROWSER_SELF_PATH=" + websrv_escape_token(self_path);
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

std::atomic<ps5_spawn_progress_fn> g_progress{nullptr};
std::atomic<void*> g_progress_data{nullptr};

void report_progress(const char* event) {
  if (const ps5_spawn_progress_fn fn = g_progress.load(std::memory_order_acquire)) {
    fn(event, g_progress_data.load(std::memory_order_acquire));
  }
}

// Caller holds g_mu. Connects to the broker, starting it if needed.
bool ensure_broker(const std::string& self_path, bool may_start) {
  if (g_client && g_client->connected()) {
    return true;
  }
  const std::vector<BrokerEndpoint> eps = endpoints();
  if (!g_client) {
    g_client = std::make_unique<BrokerClient>(eps, on_child_exit);
  }
  const auto t0 = Clock::now();
  ps5_log_printf("spawn: broker connecting, trying %s", describe(eps).c_str());
  std::string failures;
  if (!g_client->connect_once(&failures)) {
    if (!may_start) {
      ps5_log_printf("spawn: broker not reachable: %s", failures.c_str());
      return false;
    }
    ps5_log_printf("spawn: broker not running (%s), asking websrv", failures.c_str());
    if (!start_broker_via_websrv(self_path)) {
      return false;
    }
    if (!g_client->connect_wait(kBrokerStartTimeoutMs, 100, &failures)) {
      ps5_log_printf("spawn: broker did not come up within %dms (%s)", kBrokerStartTimeoutMs,
                     failures.c_str());
      return false;
    }
  }
  const BrokerEndpoint ep = g_client->endpoint();
  ps5_log_printf("spawn: broker connected via %s%s", ep.describe().c_str(),
                 ep.tcp ? " (descriptors passed by rdup from this pid)" : "");
  json reply;
  if (!g_client->request(json{{"op", "ping"}}, nullptr, 0, &reply, kPingTimeoutMs) ||
      !broker_get_bool(reply, "ok", false)) {
    ps5_log_printf("spawn: broker did not answer ping (%s)", std::strerror(errno));
    return false;
  }
  ps5_log_printf("spawn: broker ready pid=%lld in %lldms", broker_get_int(reply, "pid", -1),
                 ms_since(t0));
  report_progress("broker");
  return true;
}

// Waits out a back-off set by earlier failures, without holding g_mu (a
// terminate must not wait for it).
void wait_for_backoff(const char* role) {
  long long left;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    left = std::chrono::duration_cast<std::chrono::milliseconds>(g_backoff_until -
                                                                 Clock::now())
               .count();
  }
  if (left > 0) {
    ps5_log_printf("spawn: role=%s waiting %lldms (broker back-off)", role, left);
    std::this_thread::sleep_for(std::chrono::milliseconds(left));
  }
}

// Caller holds g_mu.
void note_broker_result(bool ok) {
  if (ok) {
    if (g_broker_failures >= kBackoffAfterFailures) {
      ps5_log_line("spawn: broker available again, back-off cleared");
    }
    g_broker_failures = 0;
    return;
  }
  g_broker_failures++;
  if (g_broker_failures >= kBackoffAfterFailures) {
    g_backoff_until = Clock::now() + std::chrono::milliseconds(kBackoffMs);
    ps5_log_printf("spawn: no broker %d times in a row, backing off %dms before the next "
                   "attempt",
                   g_broker_failures, kBackoffMs);
  }
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

  json msg{{"op", "spawn"}, {"elf", path}};
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

  // With the direct fallback a missing broker costs no websrv round-trips
  // after the first, so there is nothing to back off from.
  const bool fallback_direct = env_or("PROSPEROBROWSER_SPAWN_FALLBACK", "") == "direct";
  if (!fallback_direct) {
    wait_for_backoff(role);
  }
  std::lock_guard<std::mutex> lock(g_mu);
  const bool have_broker = ensure_broker(path, /*may_start=*/true);
  note_broker_result(have_broker || fallback_direct);
  if (have_broker) {
    // Over AF_UNIX the descriptors travel as SCM_RIGHTS. Over TCP they
    // cannot: the frame names them and the broker rdups them out of this
    // process. Either way the caller keeps its copies open until request()
    // returns (the WebKit launcher closes the client end only after
    // ps5_spawn_self() returns), so they still exist when the broker injects
    // them into the child.
    const bool tcp = g_client->endpoint().tcp;
    int send_n = nfds;
    if (tcp) {
      msg["nfds"] = 0;
      msg["src_pid"] = static_cast<int>(getpid());
      msg["src_fds"] = json(std::vector<int>(fds, fds + nfds));
      send_n = 0;
    } else {
      msg["nfds"] = nfds;
    }
    const auto t0 = Clock::now();
    json reply;
    if (g_client->request(msg, fds, send_n, &reply, kSpawnTimeoutMs)) {
      if (broker_get_bool(reply, "ok", false)) {
        const int pid = static_cast<int>(broker_get_int(reply, "pid", -1));
        ps5_log_printf("spawn: role=%s pid=%d via broker (%s) in %lldms", role, pid,
                       tcp ? "tcp, rdup" : "unix, SCM_RIGHTS", ms_since(t0));
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

  if (fallback_direct) {
    ps5_log_printf("spawn: role=%s: no broker, PROSPEROBROWSER_SPAWN_FALLBACK=direct, "
                   "spawning in-process",
                   role);
    const int pid = ps5_spawn_direct(argv, fds, nfds);
    if (pid > 0) {
      g_direct_pids.insert(pid);
    }
    return pid;
  }
  ps5_log_printf("spawn: role=%s FAILED: no spawn broker (in-process spawning from the "
                 "big app panics the console; PROSPEROBROWSER_SPAWN_FALLBACK=direct forces it)",
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
                             static_cast<int>(req.fds.size()), req.src_pid, &failure);
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
  const int pid =
      direct_mode() ? ps5_spawn_direct(argv, fds, nfds) : spawn_via_broker(argv, fds, nfds);
  if (pid > 0) {
    report_progress(argv[0]);
  }
  return pid;
}

extern "C" void ps5_spawn_set_progress(ps5_spawn_progress_fn fn, void* data) {
  g_progress_data.store(data, std::memory_order_release);
  g_progress.store(fn, std::memory_order_release);
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

  // The broker itself started in the system cpuset; widen it to the root set
  // (no UI process to join yet). Best-effort, non-fatal.
  ps5_cpuset_assign_child(getpid(), 0);

  struct sigaction sa {};
  sa.sa_handler = on_sigterm;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);

  BrokerServerOptions options;
  options.socket_paths = broker_socket_paths(std::getenv("PROSPEROBROWSER_SPAWN_SOCK"));
  for (const auto& path : options.socket_paths) {
    mkdir(dir_of(path).c_str(), 0777);  // best-effort; bind reports the real error
  }
  options.tcp_port = broker_tcp_port(std::getenv("PROSPEROBROWSER_SPAWN_PORT"));
  broker_set_spawn_hook(elfldr_hook);
  return broker_serve(options, &g_stop);
}
