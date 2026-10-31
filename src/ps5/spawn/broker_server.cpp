// SPDX-License-Identifier: GPL-3.0-or-later
#include "broker_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "broker_proto.h"
#include "ps5browser/spawn/spawn.h"

namespace ps5browser::spawn {

namespace {

using Clock = std::chrono::steady_clock;
using nlohmann::json;

constexpr int kKillTimeoutMs = 2000;
constexpr int kPollMs = 200;

std::atomic<BrokerSpawnHook> g_hook{nullptr};

long long ms_since(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

// 1 if pid is gone (reaped now, or not our child any more), 0 if alive.
// *status is the wait status when reaped now, -1 otherwise. Only tracked
// pids are waited for: waitpid(-1) would also take the ptrace stops the
// loader waits for while it sets up a new child.
int try_reap(int pid, int* status) {
  *status = -1;
  for (;;) {
    int st = 0;
    const pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) {
      *status = st;
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

std::string describe_status(int status) {
  char buf[32];
  if (status == -1) {
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

json error_reply(const std::string& stage, int err, const std::string& error) {
  return json{{"ok", false}, {"stage", stage}, {"errno", err}, {"error", error}};
}

class Server {
 public:
  explicit Server(const BrokerServerOptions& options) : opt_(options) {}

  int run(const std::atomic<bool>* stop);

 private:
  bool listen_socket();
  void reaper_main();
  void send_to_client(const json& msg);
  void drop_client(const char* why);
  void handle(const json& msg, std::vector<int>* fds);
  json do_spawn(const json& msg, std::vector<int>* fds);
  json do_terminate(const json& msg);
  size_t child_count();
  void kill_all_children();

  BrokerServerOptions opt_;
  int listen_fd_ = -1;

  std::mutex send_mu_;  // guards client_fd_ for writers (the reaper)
  int client_fd_ = -1;  // only the main thread changes it

  std::mutex children_mu_;
  std::vector<int> children_;

  std::mutex reaper_mu_;
  std::condition_variable reaper_cv_;
  bool reaper_stop_ = false;
};

bool Server::listen_socket() {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (opt_.socket_path.empty() || opt_.socket_path.size() >= sizeof(addr.sun_path)) {
    ps5_log_printf("broker: invalid socket path '%s'", opt_.socket_path.c_str());
    return false;
  }
  std::memcpy(addr.sun_path, opt_.socket_path.c_str(), opt_.socket_path.size() + 1);

  // A stale socket from a broker that died; a live one would have been
  // found by the client before it asked for a new broker.
  if (unlink(opt_.socket_path.c_str()) == 0) {
    ps5_log_printf("broker: removed stale %s", opt_.socket_path.c_str());
  }
  listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    ps5_log_printf("broker: socket: %s", std::strerror(errno));
    return false;
  }
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ps5_log_printf("broker: bind %s: %s", opt_.socket_path.c_str(), std::strerror(errno));
    return false;
  }
  // The UI runs as a big app with another identity; let it connect.
  if (chmod(opt_.socket_path.c_str(), 0666) != 0) {
    ps5_log_printf("broker: chmod 0666 %s: %s", opt_.socket_path.c_str(),
                   std::strerror(errno));
  }
  if (listen(listen_fd_, 4) != 0) {
    ps5_log_printf("broker: listen: %s", std::strerror(errno));
    return false;
  }
  return true;
}

size_t Server::child_count() {
  std::lock_guard<std::mutex> lock(children_mu_);
  return children_.size();
}

void Server::send_to_client(const json& msg) {
  std::lock_guard<std::mutex> lock(send_mu_);
  if (client_fd_ >= 0 && !broker_send_frame(client_fd_, msg)) {
    ps5_log_printf("broker: send to client failed: %s", std::strerror(errno));
  }
}

void Server::drop_client(const char* why) {
  std::lock_guard<std::mutex> lock(send_mu_);
  if (client_fd_ >= 0) {
    ps5_log_printf("broker: client disconnected (%s), %zu children alive", why,
                   [this] {
                     std::lock_guard<std::mutex> l(children_mu_);
                     return children_.size();
                   }());
    close(client_fd_);
    client_fd_ = -1;
  }
}

void Server::reaper_main() {
  std::unique_lock<std::mutex> lock(reaper_mu_);
  while (!reaper_stop_) {
    reaper_cv_.wait_for(lock, std::chrono::milliseconds(opt_.reap_interval_ms));
    if (reaper_stop_) {
      break;
    }
    std::vector<std::pair<int, int>> exited;
    {
      std::lock_guard<std::mutex> l(children_mu_);
      for (size_t i = 0; i < children_.size();) {
        int status;
        if (try_reap(children_[i], &status)) {
          exited.emplace_back(children_[i], status);
          children_[i] = children_.back();
          children_.pop_back();
        } else {
          i++;
        }
      }
    }
    for (const auto& [pid, status] : exited) {
      ps5_log_printf("broker: child pid=%d exited (%s)", pid, describe_status(status).c_str());
      send_to_client(json{{"ev", "exited"}, {"pid", pid}, {"status", status}});
    }
  }
}

json Server::do_spawn(const json& msg, std::vector<int>* fds) {
  BrokerSpawnRequest req;
  req.elf = broker_get_string(msg, "elf");
  const long long nfds = broker_get_int(msg, "nfds", 0);
  if (req.elf.empty() || !broker_get_strings(msg, "argv", &req.argv) || req.argv.empty() ||
      req.argv[0].empty() || !broker_get_strings(msg, "env", &req.env)) {
    return error_reply("request", EINVAL, "spawn needs elf, argv[0] and env");
  }
  if (nfds != static_cast<long long>(fds->size())) {
    return error_reply("request", EINVAL,
                       "nfds=" + std::to_string(nfds) + " but " +
                           std::to_string(fds->size()) + " descriptors arrived");
  }
  req.fds = *fds;

  const BrokerSpawnHook hook = g_hook.load();
  if (!hook) {
    return error_reply("hook", ENOSYS, "no spawn backend");
  }
  const auto t0 = Clock::now();
  const BrokerSpawnResult res = hook(req);
  if (res.pid <= 0) {
    ps5_log_printf("broker: role=%s FAILED at %s: errno %d (%s)", req.argv[0].c_str(),
                   res.stage.empty() ? "unknown stage" : res.stage.c_str(), res.err,
                   res.error.c_str());
    return error_reply(res.stage.empty() ? "unknown stage" : res.stage, res.err, res.error);
  }
  {
    std::lock_guard<std::mutex> lock(children_mu_);
    children_.push_back(res.pid);
  }
  ps5_log_printf("broker: role=%s pid=%d spawned in %lldms", req.argv[0].c_str(), res.pid,
                 ms_since(t0));
  return json{{"ok", true}, {"pid", res.pid}};
}

json Server::do_terminate(const json& msg) {
  const long long pid = broker_get_int(msg, "pid", -1);
  if (pid <= 0) {
    return error_reply("request", EINVAL, "terminate needs a pid");
  }
  // An untracked pid was already reaped (and may have been recycled since),
  // so it must not be signalled.
  {
    std::lock_guard<std::mutex> lock(children_mu_);
    auto it = std::find(children_.begin(), children_.end(), static_cast<int>(pid));
    if (it == children_.end()) {
      return json{{"ok", true}};
    }
    children_.erase(it);
  }
  kill(static_cast<pid_t>(pid), SIGKILL);  // best-effort: it may be a zombie already
  for (int waited = 0; waited < kKillTimeoutMs; waited += 10) {
    int status;
    if (try_reap(static_cast<int>(pid), &status)) {
      ps5_log_printf("broker: child pid=%lld terminated (%s)", pid,
                     describe_status(status).c_str());
      return json{{"ok", true}};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ps5_log_printf("broker: child pid=%lld did not exit after SIGKILL", pid);
  {
    std::lock_guard<std::mutex> lock(children_mu_);
    children_.push_back(static_cast<int>(pid));  // the reaper collects it later
  }
  return error_reply("terminate", ETIMEDOUT, "child did not exit after SIGKILL");
}

void Server::handle(const json& msg, std::vector<int>* fds) {
  const std::string op = broker_get_string(msg, "op");
  json reply;
  if (op == "spawn") {
    reply = do_spawn(msg, fds);
  } else if (op == "terminate") {
    reply = do_terminate(msg);
  } else if (op == "ping") {
    reply = json{{"ok", true}, {"pid", static_cast<int>(getpid())}};
  } else {
    reply = error_reply("request", EINVAL, "unknown op '" + op + "'");
  }
  // The child holds its own duplicates by now.
  for (int fd : *fds) {
    close(fd);
  }
  fds->clear();
  send_to_client(reply);
}

void Server::kill_all_children() {
  std::vector<int> pids;
  {
    std::lock_guard<std::mutex> lock(children_mu_);
    pids.swap(children_);
  }
  for (int pid : pids) {
    kill(pid, SIGKILL);
  }
  for (int pid : pids) {
    int status;
    for (int waited = 0; waited < kKillTimeoutMs && !try_reap(pid, &status); waited += 10) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ps5_log_printf("broker: killed child pid=%d", pid);
  }
}

int Server::run(const std::atomic<bool>* stop) {
  if (!listen_socket()) {
    if (listen_fd_ >= 0) {
      close(listen_fd_);
    }
    return 1;
  }
  ps5_log_printf("broker: listening on %s, pid %d", opt_.socket_path.c_str(),
                 static_cast<int>(getpid()));

  std::thread reaper([this] { reaper_main(); });
  auto alone_since = Clock::now();  // no client since then
  int rc = 0;

  for (;;) {
    if (stop && stop->load()) {
      ps5_log_line("broker: stop requested");
      break;
    }
    if (client_fd_ < 0) {
      const size_t alive = child_count();
      const long long alone = ms_since(alone_since);
      if (alive == 0 && alone >= opt_.idle_ms) {
        ps5_log_printf("broker: no client and no children for %lldms, exiting", alone);
        break;
      }
      if (alive > 0 && alone >= opt_.orphan_ms) {
        ps5_log_printf("broker: no client for %lldms, killing %zu orphaned children", alone,
                       alive);
        kill_all_children();
        break;
      }
    }

    pollfd pfd[2] = {{listen_fd_, POLLIN, 0}, {client_fd_, POLLIN, 0}};
    const int n = poll(pfd, client_fd_ >= 0 ? 2 : 1, kPollMs);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      ps5_log_printf("broker: poll: %s", std::strerror(errno));
      rc = 1;
      break;
    }

    if (pfd[0].revents & POLLIN) {
      const int fd = accept(listen_fd_, nullptr, nullptr);
      if (fd >= 0) {
        if (client_fd_ >= 0) {
          drop_client("replaced by a new connection");
        }
        std::lock_guard<std::mutex> lock(send_mu_);
        client_fd_ = fd;
        ps5_log_printf("broker: client connected, %zu children alive", child_count());
        continue;
      }
      if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED) {
        ps5_log_printf("broker: accept: %s", std::strerror(errno));
      }
    }

    if (client_fd_ >= 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      json msg;
      std::vector<int> fds;
      const BrokerRecv r = broker_recv_frame(client_fd_, &msg, &fds);
      if (r == BrokerRecv::kOk) {
        handle(msg, &fds);
      } else {
        drop_client(r == BrokerRecv::kClosed ? "closed" : std::strerror(errno));
        alone_since = Clock::now();
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(reaper_mu_);
    reaper_stop_ = true;
  }
  reaper_cv_.notify_all();
  reaper.join();
  kill_all_children();
  drop_client("broker exiting");
  close(listen_fd_);
  unlink(opt_.socket_path.c_str());
  ps5_log_printf("broker: exiting (%d)", rc);
  return rc;
}

}  // namespace

void broker_set_spawn_hook(BrokerSpawnHook hook) { g_hook.store(hook); }

int broker_serve(const BrokerServerOptions& options, const std::atomic<bool>* stop) {
  Server server(options);
  return server.run(stop);
}

}  // namespace ps5browser::spawn
