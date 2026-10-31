// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <doctest/doctest.h>

#include "broker_client.h"
#include "broker_proto.h"
#include "broker_server.h"

using namespace ps5browser::spawn;
using nlohmann::json;

// The broker logs through the executable's ps5_log_*; print only on demand.
extern "C" void ps5_log_line(const char* line) {
  if (std::getenv("PS5BROWSER_TEST_LOG")) {
    std::fprintf(stderr, "[log] %s\n", line);
  }
}

extern "C" void ps5_log_printf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  ps5_log_line(buf);
}

namespace {

struct SocketPair {
  int a = -1, b = -1;
  SocketPair() {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    a = sv[0];
    b = sv[1];
  }
  ~SocketPair() {
    close(a);
    close(b);
  }
};

std::string read_some(int fd) {
  char buf[256];
  const ssize_t n = read(fd, buf, sizeof(buf));
  return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

std::atomic<int> g_last_src_pid{-1};

// The fake spawn backend: fork()s a child that writes "hello <argv[0]>" to
// child fd 3 (fds[0]) and then exits with argv[2] ("exit N") or sleeps
// until killed ("sleep"). Descriptors named over TCP (src_pid) belong to the
// test process itself, which the fork inherits, so they work the same.
BrokerSpawnResult fake_spawn(const BrokerSpawnRequest& req) {
  BrokerSpawnResult res;
  g_last_src_pid.store(req.src_pid);
  if (req.argv.size() < 2) {
    res.stage = "fake argv";
    res.err = EINVAL;
    return res;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    res.stage = "fork";
    res.err = errno;
    return res;
  }
  if (pid == 0) {
    const std::string hello = "hello " + req.argv[0];
    if (!req.fds.empty()) {
      (void)!write(req.fds[0], hello.data(), hello.size());
    }
    if (req.argv[1] == "sleep") {
      for (;;) {
        pause();
      }
    }
    _exit(req.argv.size() > 2 ? std::atoi(req.argv[2].c_str()) : 0);
  }
  res.pid = pid;
  return res;
}

struct TempDir {
  std::string path;
  TempDir() {
    char tmpl[] = "/tmp/ps5browser-broker-XXXXXX";
    REQUIRE(mkdtemp(tmpl) != nullptr);
    path = tmpl;
  }
  ~TempDir() { rmdir(path.c_str()); }
};

// Collects the exited notifications the client reader thread delivers.
struct Exits {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::pair<int, int>> seen;

  void add(int pid, int status) {
    std::lock_guard<std::mutex> lock(mu);
    seen.emplace_back(pid, status);
    cv.notify_all();
  }
  bool wait_for(int pid, int* status, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
      for (const auto& [p, s] : seen) {
        if (p == pid) {
          *status = s;
          return true;
        }
      }
      return false;
    });
  }
};

}  // namespace

TEST_SUITE("broker_proto") {
  TEST_CASE("frame encoding is a big-endian length and the JSON payload") {
    const json msg{{"op", "ping"}};
    const std::string frame = broker_encode_frame(msg);
    REQUIRE(frame.size() > 4);
    const std::string payload = frame.substr(4);
    const uint32_t len = (static_cast<uint32_t>(static_cast<unsigned char>(frame[0])) << 24) |
                         (static_cast<uint32_t>(static_cast<unsigned char>(frame[1])) << 16) |
                         (static_cast<uint32_t>(static_cast<unsigned char>(frame[2])) << 8) |
                         static_cast<uint32_t>(static_cast<unsigned char>(frame[3]));
    CHECK(len == payload.size());
    json back;
    REQUIRE(broker_decode_payload(payload, &back));
    CHECK(back == msg);
    CHECK_FALSE(broker_decode_payload("[1,2]", &back));
    CHECK_FALSE(broker_decode_payload("{not json", &back));
  }

  TEST_CASE("frames round-trip over a stream socket") {
    SocketPair sp;
    const json spawn{{"op", "spawn"},
                     {"elf", "/data/ps5browser/ps5browser.elf"},
                     {"argv", {"WebProcess", "7", "3"}},
                     {"env", {"HOME=/data/ps5browser", "A=b c"}},
                     {"nfds", 0}};
    REQUIRE(broker_send_frame(sp.a, spawn));
    REQUIRE(broker_send_frame(sp.a, json{{"ev", "exited"}, {"pid", 12}, {"status", 256}}));
    json got;
    std::vector<int> fds;
    REQUIRE(broker_recv_frame(sp.b, &got, &fds) == BrokerRecv::kOk);
    CHECK(got == spawn);
    CHECK(fds.empty());
    std::vector<std::string> argv;
    CHECK(broker_get_strings(got, "argv", &argv));
    CHECK(argv == std::vector<std::string>{"WebProcess", "7", "3"});
    CHECK(broker_get_string(got, "elf") == "/data/ps5browser/ps5browser.elf");
    CHECK(broker_get_int(got, "nfds", -1) == 0);
    CHECK(broker_get_int(got, "elf", -1) == -1);  // wrong type -> fallback
    CHECK(broker_get_bool(got, "ok", true));       // missing -> fallback
    REQUIRE(broker_recv_frame(sp.b, &got, &fds) == BrokerRecv::kOk);
    CHECK(broker_get_string(got, "ev") == "exited");
    CHECK(broker_get_int(got, "status", 0) == 256);
    close(sp.a);
    sp.a = -1;
    CHECK(broker_recv_frame(sp.b, &got, &fds) == BrokerRecv::kClosed);
  }

  TEST_CASE("invalid UTF-8 in an environment string still encodes") {
    SocketPair sp;
    REQUIRE(broker_send_frame(sp.a, json{{"env", {std::string("X=\xff\xfe")}}}));
    json got;
    std::vector<int> fds;
    CHECK(broker_recv_frame(sp.b, &got, &fds) == BrokerRecv::kOk);
  }

  TEST_CASE("an oversized or truncated frame is an error") {
    SocketPair sp;
    const unsigned char huge[4] = {0x7f, 0xff, 0xff, 0xff};
    REQUIRE(write(sp.a, huge, sizeof(huge)) == 4);
    json got;
    std::vector<int> fds;
    CHECK(broker_recv_frame(sp.b, &got, &fds) == BrokerRecv::kError);

    SocketPair sp2;
    const unsigned char partial[6] = {0, 0, 0, 10, '{', '}'};
    REQUIRE(write(sp2.a, partial, sizeof(partial)) == 6);
    close(sp2.a);
    sp2.a = -1;
    CHECK(broker_recv_frame(sp2.b, &got, &fds) == BrokerRecv::kError);
  }
}

TEST_SUITE("broker_endpoints") {
  TEST_CASE("AF_UNIX paths are tried env first, then /tmp, then /data") {
    CHECK(broker_socket_paths(nullptr) ==
          std::vector<std::string>{"/tmp/ps5browser-spawn.sock",
                                   "/data/ps5browser/tmp/spawn.sock"});
    CHECK(broker_socket_paths("") == broker_socket_paths(nullptr));
    CHECK(broker_socket_paths("/x/s.sock") ==
          std::vector<std::string>{"/x/s.sock", "/tmp/ps5browser-spawn.sock",
                                   "/data/ps5browser/tmp/spawn.sock"});
    CHECK(broker_socket_paths("/tmp/ps5browser-spawn.sock").size() == 2);  // no duplicate
  }

  TEST_CASE("the TCP port defaults to 17771 and ignores junk") {
    CHECK(broker_tcp_port(nullptr) == 17771);
    CHECK(broker_tcp_port("") == 17771);
    CHECK(broker_tcp_port("18000") == 18000);
    CHECK(broker_tcp_port("0") == 17771);
    CHECK(broker_tcp_port("70000") == 17771);
    CHECK(broker_tcp_port("12ab") == 17771);
  }

  TEST_CASE("the client tries every AF_UNIX path, then TCP") {
    const auto eps = broker_endpoints("/x/s.sock", "18000");
    REQUIRE(eps.size() == 4);
    CHECK(eps[0].describe() == "unix /x/s.sock");
    CHECK(eps[2].describe() == "unix /data/ps5browser/tmp/spawn.sock");
    CHECK(eps[3].tcp);
    CHECK(eps[3].describe() == "tcp 127.0.0.1:18000");
  }

  TEST_CASE("src_fds are parsed strictly") {
    std::vector<int> fds;
    std::string why;
    CHECK(broker_get_src_fds(json{{"op", "spawn"}}, &fds, &why) == BrokerSrcFds::kAbsent);
    CHECK(broker_get_src_fds(json{{"src_fds", json::array()}}, &fds, &why) ==
          BrokerSrcFds::kAbsent);
    REQUIRE(broker_get_src_fds(json{{"src_pid", 93}, {"src_fds", {13, 14}}}, &fds, &why) ==
            BrokerSrcFds::kPresent);
    CHECK(fds == std::vector<int>{13, 14});
    CHECK(broker_get_src_fds(json{{"src_fds", {-1}}}, &fds, &why) == BrokerSrcFds::kInvalid);
    CHECK(broker_get_src_fds(json{{"src_fds", {"13"}}}, &fds, &why) == BrokerSrcFds::kInvalid);
    CHECK(broker_get_src_fds(json{{"src_fds", 13}}, &fds, &why) == BrokerSrcFds::kInvalid);
    CHECK(broker_get_src_fds(json{{"src_fds", std::vector<int>(kBrokerMaxFds + 1, 3)}}, &fds,
                             &why) == BrokerSrcFds::kInvalid);
    CHECK(fds.empty());
  }
}

TEST_SUITE("broker_fd_passing") {
  TEST_CASE("descriptors arrive with their frame and stay usable") {
    SocketPair sp;
    int p1[2], p2[2];
    REQUIRE(pipe(p1) == 0);
    REQUIRE(pipe(p2) == 0);
    const int send_fds[] = {p1[1], p2[1]};
    REQUIRE(broker_send_frame(sp.a, json{{"op", "spawn"}, {"nfds", 2}}, send_fds, 2));
    // The sender may close its copies right away, as the WebKit launcher does.
    close(p1[1]);
    close(p2[1]);

    json got;
    std::vector<int> fds;
    REQUIRE(broker_recv_frame(sp.b, &got, &fds) == BrokerRecv::kOk);
    REQUIRE(fds.size() == 2);
    CHECK(broker_get_int(got, "nfds", 0) == 2);
    CHECK((fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0);
    REQUIRE(write(fds[0], "one", 3) == 3);
    REQUIRE(write(fds[1], "two", 3) == 3);
    CHECK(read_some(p1[0]) == "one");
    CHECK(read_some(p2[0]) == "two");
    for (int fd : fds) {
      close(fd);
    }
    close(p1[0]);
    close(p2[0]);
  }

  TEST_CASE("a socketpair end passed over the channel carries traffic") {
    SocketPair channel;
    SocketPair ipc;  // stands in for WebKit's SOCK_SEQPACKET pair
    REQUIRE(broker_send_frame(channel.a, json{{"op", "spawn"}, {"nfds", 1}}, &ipc.b, 1));
    json got;
    std::vector<int> fds;
    REQUIRE(broker_recv_frame(channel.b, &got, &fds) == BrokerRecv::kOk);
    REQUIRE(fds.size() == 1);
    REQUIRE(write(ipc.a, "ping", 4) == 4);
    CHECK(read_some(fds[0]) == "ping");
    REQUIRE(write(fds[0], "pong", 4) == 4);
    CHECK(read_some(ipc.a) == "pong");
    close(fds[0]);
  }

  TEST_CASE("too many descriptors are refused by the sender") {
    SocketPair sp;
    int many[kBrokerMaxFds + 1];
    for (int& fd : many) {
      fd = sp.a;
    }
    CHECK_FALSE(broker_send_frame(sp.a, json{{"op", "spawn"}}, many, kBrokerMaxFds + 1));
  }
}

TEST_SUITE("broker_e2e") {
  TEST_CASE("spawn, exit notification, terminate, stop") {
    TempDir dir;
    BrokerServerOptions opt;
    const std::string sock = dir.path + "/spawn.sock";
    opt.socket_paths = {sock};
    opt.idle_ms = 60000;
    opt.reap_interval_ms = 20;
    broker_set_spawn_hook(fake_spawn);

    std::atomic<bool> stop{false};
    int server_rc = -1;
    std::thread server([&] { server_rc = broker_serve(opt, &stop); });

    Exits exits;
    BrokerClient client(sock, [&](int pid, int status) { exits.add(pid, status); });
    REQUIRE(client.connect_wait(5000, 10));
    struct stat st {};
    REQUIRE(stat(sock.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0666);

    json reply;
    REQUIRE(client.request(json{{"op", "ping"}}, nullptr, 0, &reply, 5000));
    CHECK(broker_get_bool(reply, "ok", false));
    CHECK(broker_get_int(reply, "pid", -1) == getpid());

    // spawn -> pid, the child talks through the passed fd, then exits 7.
    int out[2];
    REQUIRE(pipe(out) == 0);
    const json spawn{{"op", "spawn"},
                     {"elf", "/fake.elf"},
                     {"argv", {"WebProcess", "exit", "7"}},
                     {"env", {"A=1"}},
                     {"nfds", 1}};
    REQUIRE(client.request(spawn, &out[1], 1, &reply, 5000));
    close(out[1]);
    REQUIRE(broker_get_bool(reply, "ok", false));
    const int pid = static_cast<int>(broker_get_int(reply, "pid", -1));
    REQUIRE(pid > 0);
    CHECK(read_some(out[0]) == "hello WebProcess");
    close(out[0]);
    int status = -1;
    REQUIRE(exits.wait_for(pid, &status, 5000));
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 7);

    // A long-running child is terminated (and reaped) on request.
    REQUIRE(pipe(out) == 0);
    json sleeper = spawn;
    sleeper["argv"] = {"NetworkProcess", "sleep"};
    REQUIRE(client.request(sleeper, &out[1], 1, &reply, 5000));
    close(out[1]);
    const int pid2 = static_cast<int>(broker_get_int(reply, "pid", -1));
    REQUIRE(pid2 > 0);
    CHECK(read_some(out[0]) == "hello NetworkProcess");
    close(out[0]);
    REQUIRE(client.request(json{{"op", "terminate"}, {"pid", pid2}}, nullptr, 0, &reply, 5000));
    CHECK(broker_get_bool(reply, "ok", false));
    CHECK(kill(pid2, 0) != 0);  // reaped: no such process any more
    // Terminating it again (or any unknown pid) is a no-op success.
    REQUIRE(client.request(json{{"op", "terminate"}, {"pid", pid2}}, nullptr, 0, &reply, 5000));
    CHECK(broker_get_bool(reply, "ok", false));

    // Bad requests get an error reply with a stage, not a dropped connection.
    REQUIRE(client.request(json{{"op", "spawn"}, {"elf", "/fake.elf"}, {"argv", {"X", "exit"}},
                                {"env", json::array()}, {"nfds", 1}},
                           nullptr, 0, &reply, 5000));
    CHECK_FALSE(broker_get_bool(reply, "ok", true));
    CHECK(broker_get_string(reply, "stage") == "request");
    CHECK(broker_get_int(reply, "errno", 0) == EINVAL);
    REQUIRE(client.request(json{{"op", "bogus"}}, nullptr, 0, &reply, 5000));
    CHECK_FALSE(broker_get_bool(reply, "ok", true));

    stop.store(true);
    server.join();
    CHECK(server_rc == 0);
    CHECK(stat(sock.c_str(), &st) != 0);  // removed on exit
    CHECK_FALSE(client.request(json{{"op", "ping"}}, nullptr, 0, &reply, 2000));
    broker_set_spawn_hook(nullptr);
  }

  TEST_CASE("the broker exits once idle with no client and no children") {
    TempDir dir;
    BrokerServerOptions opt;
    const std::string sock = dir.path + "/spawn.sock";
    opt.socket_paths = {sock};
    opt.idle_ms = 300;
    broker_set_spawn_hook(fake_spawn);

    std::thread server([&] { CHECK(broker_serve(opt, nullptr) == 0); });
    {
      BrokerClient client(sock, nullptr);
      REQUIRE(client.connect_wait(5000, 10));
      // Connected: the idle clock does not run.
      std::this_thread::sleep_for(std::chrono::milliseconds(600));
      json reply;
      REQUIRE(client.request(json{{"op", "ping"}}, nullptr, 0, &reply, 5000));
    }  // disconnect
    const auto t0 = std::chrono::steady_clock::now();
    server.join();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    broker_set_spawn_hook(nullptr);
  }

  TEST_CASE("the broker exits with its client and kills the children") {
    TempDir dir;
    BrokerServerOptions opt;
    const std::string sock = dir.path + "/spawn.sock";
    opt.socket_paths = {sock};
    opt.idle_ms = 60000;  // only the disconnect may end it
    opt.reap_interval_ms = 20;
    broker_set_spawn_hook(fake_spawn);

    int server_rc = -1;
    std::thread server([&] { server_rc = broker_serve(opt, nullptr); });
    int pid = -1;
    {
      BrokerClient client(sock, nullptr);
      REQUIRE(client.connect_wait(5000, 10));
      int out[2];
      REQUIRE(pipe(out) == 0);
      const json sleeper{{"op", "spawn"},
                         {"elf", "/fake.elf"},
                         {"argv", {"WebProcess", "sleep"}},
                         {"env", {"A=1"}},
                         {"nfds", 1}};
      json reply;
      REQUIRE(client.request(sleeper, &out[1], 1, &reply, 5000));
      close(out[1]);
      pid = static_cast<int>(broker_get_int(reply, "pid", -1));
      REQUIRE(pid > 0);
      CHECK(read_some(out[0]) == "hello WebProcess");
      close(out[0]);
    }  // the UI goes away
    const auto t0 = std::chrono::steady_clock::now();
    server.join();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    CHECK(server_rc == 0);
    CHECK(kill(pid, 0) != 0);  // killed and reaped
    struct stat st {};
    CHECK(stat(sock.c_str(), &st) != 0);
    broker_set_spawn_hook(nullptr);
  }

  TEST_CASE("a stale socket file is replaced") {
    TempDir dir;
    BrokerServerOptions opt;
    const std::string sock = dir.path + "/spawn.sock";
    opt.socket_paths = {sock};
    opt.idle_ms = 100;
    const int fd = open(sock.c_str(), O_CREAT | O_WRONLY, 0600);
    REQUIRE(fd >= 0);
    close(fd);
    CHECK(broker_serve(opt, nullptr) == 0);
  }
}

TEST_SUITE("broker_tcp") {
  // A broker whose only AF_UNIX path cannot be bound (as /data on the
  // console: EOPNOTSUPP there, ENOENT here) listens on TCP alone.
  struct TcpBroker {
    BrokerServerOptions opt;
    std::atomic<bool> stop{false};
    std::atomic<int> port{0};
    int rc = -1;
    std::thread thread;

    TcpBroker() {
      opt.socket_paths = {"/nonexistent-ps5browser-dir/spawn.sock"};
      opt.tcp_port = 0;  // ephemeral
      opt.bound_tcp_port = &port;
      opt.idle_ms = 60000;
      opt.reap_interval_ms = 20;
      broker_set_spawn_hook(fake_spawn);
      thread = std::thread([this] { rc = broker_serve(opt, &stop); });
      for (int i = 0; i < 500 && port.load() == 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    ~TcpBroker() {
      stop.store(true);
      thread.join();
      broker_set_spawn_hook(nullptr);
    }
    std::vector<BrokerEndpoint> endpoints() const {
      BrokerEndpoint unix_ep;
      unix_ep.path = opt.socket_paths[0];
      BrokerEndpoint tcp_ep;
      tcp_ep.tcp = true;
      tcp_ep.port = port.load();
      return {unix_ep, tcp_ep};
    }
  };

  TEST_CASE("frames round-trip over loopback TCP") {
    TcpBroker broker;
    REQUIRE(broker.port.load() > 0);
    BrokerClient client(broker.endpoints(), nullptr);
    std::string failures;
    REQUIRE(client.connect_wait(5000, 10, &failures));
    CHECK(client.endpoint().tcp);
    CHECK(client.endpoint().port == broker.port.load());

    json reply;
    REQUIRE(client.request(json{{"op", "ping"}}, nullptr, 0, &reply, 5000));
    CHECK(broker_get_bool(reply, "ok", false));
    CHECK(broker_get_int(reply, "pid", -1) == getpid());
    // A large frame (the UI's whole environment) arrives intact in pieces.
    std::vector<std::string> env(2000, std::string(100, 'x'));
    REQUIRE(client.request(json{{"op", "spawn"}, {"elf", "/fake.elf"}, {"argv", {"X"}},
                                {"env", env}, {"nfds", 0}},
                           nullptr, 0, &reply, 5000));
    CHECK_FALSE(broker_get_bool(reply, "ok", true));  // the fake wants argv[1]
    CHECK(broker_get_string(reply, "stage") == "fake argv");
  }

  TEST_CASE("descriptors named by src_pid/src_fds reach the child") {
    TcpBroker broker;
    REQUIRE(broker.port.load() > 0);
    Exits exits;
    BrokerClient client(broker.endpoints(), [&](int pid, int status) { exits.add(pid, status); });
    REQUIRE(client.connect_wait(5000, 10));
    REQUIRE(client.endpoint().tcp);

    int out[2];
    REQUIRE(pipe(out) == 0);
    json reply;
    // The sender keeps out[1] open until the reply: the broker copies it
    // out of this process while it sets the child up.
    const json spawn{{"op", "spawn"},
                     {"elf", "/fake.elf"},
                     {"argv", {"WebProcess", "exit", "3"}},
                     {"env", {"A=1"}},
                     {"nfds", 0},
                     {"src_pid", static_cast<int>(getpid())},
                     {"src_fds", {out[1]}}};
    g_last_src_pid.store(-1);
    REQUIRE(client.request(spawn, nullptr, 0, &reply, 5000));
    close(out[1]);
    REQUIRE(broker_get_bool(reply, "ok", false));
    CHECK(g_last_src_pid.load() == getpid());
    const int pid = static_cast<int>(broker_get_int(reply, "pid", -1));
    REQUIRE(pid > 0);
    CHECK(read_some(out[0]) == "hello WebProcess");
    close(out[0]);
    int status = -1;
    REQUIRE(exits.wait_for(pid, &status, 5000));
    CHECK(WEXITSTATUS(status) == 3);

    // Malformed variants are refused with a request error.
    json bad = spawn;
    bad.erase("src_pid");
    REQUIRE(client.request(bad, nullptr, 0, &reply, 5000));
    CHECK(broker_get_string(reply, "stage") == "request");
    bad = spawn;
    bad["src_fds"] = {-4};
    REQUIRE(client.request(bad, nullptr, 0, &reply, 5000));
    CHECK(broker_get_string(reply, "stage") == "request");
    CHECK(broker_get_int(reply, "errno", 0) == EINVAL);
  }

  TEST_CASE("SCM_RIGHTS descriptors arrive with src_pid 0") {
    TempDir dir;
    BrokerServerOptions opt;
    const std::string sock = dir.path + "/spawn.sock";
    opt.socket_paths = {sock};
    opt.tcp_port = 0;
    std::atomic<int> port{0};
    opt.bound_tcp_port = &port;
    opt.idle_ms = 60000;
    broker_set_spawn_hook(fake_spawn);
    std::atomic<bool> stop{false};
    std::thread server([&] { broker_serve(opt, &stop); });

    // AF_UNIX binds, so it is tried (and used) first; TCP listens as well.
    BrokerClient client(sock, nullptr);
    REQUIRE(client.connect_wait(5000, 10));
    CHECK_FALSE(client.endpoint().tcp);
    int out[2];
    REQUIRE(pipe(out) == 0);
    json reply;
    g_last_src_pid.store(-1);
    REQUIRE(client.request(json{{"op", "spawn"}, {"elf", "/fake.elf"},
                                {"argv", {"WebProcess", "exit", "0"}}, {"env", json::array()},
                                {"nfds", 1}},
                           &out[1], 1, &reply, 5000));
    close(out[1]);
    CHECK(broker_get_bool(reply, "ok", false));
    CHECK(g_last_src_pid.load() == 0);
    CHECK(read_some(out[0]) == "hello WebProcess");
    close(out[0]);
    CHECK(port.load() > 0);
    stop.store(true);
    server.join();
    broker_set_spawn_hook(nullptr);
  }

  TEST_CASE("nothing to listen on is a startup failure") {
    BrokerServerOptions opt;
    opt.socket_paths = {"/nonexistent-ps5browser-dir/spawn.sock"};
    opt.tcp_port = -1;
    CHECK(broker_serve(opt, nullptr) == 1);
  }

  TEST_CASE("the client reports every endpoint that failed") {
    BrokerEndpoint a;
    a.path = "/nonexistent-ps5browser-dir/spawn.sock";
    BrokerEndpoint b;
    b.tcp = true;
    b.port = 1;  // nothing listens on tcp/1
    BrokerClient client({a, b}, nullptr);
    std::string failures;
    CHECK_FALSE(client.connect_once(&failures));
    CHECK(failures.find("unix /nonexistent-ps5browser-dir/spawn.sock: ") == 0);
    CHECK(failures.find("; tcp 127.0.0.1:1: ") != std::string::npos);
  }
}

TEST_SUITE("broker_websrv") {
  TEST_CASE("the websrv request matches sys_launch_daemon's parsing") {
    CHECK(url_encode("/data/ps5browser/ps5browser.elf") == "/data/ps5browser/ps5browser.elf");
    CHECK(url_encode("a b=c&d") == "a%20b%3Dc%26d");
    CHECK(websrv_escape_token("/a b\\c") == "/a\\ b\\\\c");
    CHECK(websrv_broker_target("/data/ps5browser/ps5browser.elf", "/data/ps5browser",
                               "spawn-broker --role=spawn-broker",
                               "PS5BROWSER_ROLE=spawn-broker") ==
          "/hbldr?path=/data/ps5browser/ps5browser.elf&cwd=/data/ps5browser&daemon=1"
          "&args=spawn-broker%20--role%3Dspawn-broker&env=PS5BROWSER_ROLE%3Dspawn-broker");
  }

  TEST_CASE("http_get sends an HTTP/1.0 GET and returns the status") {
    const int lfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(lfd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(lfd, 1) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const std::string host_port = "127.0.0.1:" + std::to_string(ntohs(addr.sin_port));

    std::string request;
    std::thread fake([&] {
      const int c = accept(lfd, nullptr, nullptr);
      char buf[2048];
      while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = read(c, buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
        request.append(buf, static_cast<size_t>(n));
      }
      const char resp[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
      (void)!write(c, resp, sizeof(resp) - 1);
      close(c);
    });
    std::string error;
    const int code = http_get(host_port, "/hbldr?path=/x&daemon=1", 5000, &error);
    fake.join();
    close(lfd);
    CHECK(code == 503);
    CHECK(request.rfind("GET /hbldr?path=/x&daemon=1 HTTP/1.0\r\n", 0) == 0);
    CHECK(request.find("Host: " + host_port + "\r\n") != std::string::npos);

    CHECK(http_get("localhost:8080", "/", 1000, &error) == -1);  // IPv4 literals only
  }
}
