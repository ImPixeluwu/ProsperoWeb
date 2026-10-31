// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc_selftest.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "broker_client.h"
#include "broker_proto.h"
#include "prosperoweb/spawn/spawn.h"

namespace prosperoweb::spawn {

namespace {

using nlohmann::json;

constexpr std::size_t kShmSize = 0x4000;
constexpr int kStepTimeoutMs = 4000;
constexpr const char* kUiPattern = "PRWB-UI-SHM";
constexpr const char* kBrokerPattern = "PRWB-BROKER-SHM";

std::string err(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

void set_timeout(int fd, int ms) {
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

const char* type_name(int type) {
  switch (type) {
    case SOCK_STREAM:
      return "SOCK_STREAM";
    case SOCK_DGRAM:
      return "SOCK_DGRAM";
    case SOCK_SEQPACKET:
      return "SOCK_SEQPACKET";
    default:
      return "other";
  }
}

// An anonymous shared-memory object holding pattern, or -1 (*why).
int make_shm(const char* pattern, std::string* why) {
  const int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
  if (fd < 0) {
    *why = err("shm_open(SHM_ANON)");
    return -1;
  }
  if (ftruncate(fd, kShmSize) != 0) {
    *why = err("ftruncate");
    close(fd);
    return -1;
  }
  void* p = mmap(nullptr, kShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    *why = err("mmap");
    close(fd);
    return -1;
  }
  std::strcpy(static_cast<char*>(p), pattern);
  munmap(p, kShmSize);
  return fd;
}

// Maps fd and compares its start with pattern: "ok" or why not.
std::string check_shm(int fd, const char* pattern) {
  void* p = mmap(nullptr, kShmSize, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    return err("mmap of the received object");
  }
  const bool same = std::strncmp(static_cast<const char*>(p), pattern, std::strlen(pattern)) == 0;
  munmap(p, kShmSize);
  return same ? "ok" : "mapped, but the content differs";
}

bool send_fd(int sock, int fd, std::string* why) {
  char byte = 'F';
  iovec iov{&byte, 1};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  cmsghdr* c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
  if (sendmsg(sock, &msg, 0) != 1) {
    *why = err("sendmsg(SCM_RIGHTS)");
    return false;
  }
  return true;
}

// The descriptor that came with the next message, or -1 (*why).
int recv_fd(int sock, std::string* why) {
  char byte = 0;
  iovec iov{&byte, 1};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  const ssize_t n = recvmsg(sock, &msg, 0);
  if (n < 0) {
    *why = err("recvmsg");
    return -1;
  }
  if (msg.msg_flags & MSG_CTRUNC) {
    *why = "recvmsg: control data truncated (descriptor dropped)";
    return -1;
  }
  for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int fd = -1;
      std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
      return fd;
    }
  }
  *why = "message (" + std::to_string(n) + " bytes) came without a descriptor";
  return -1;
}

std::string describe_fd(int fd) {
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    return err("fstat");
  }
  std::string out = S_ISSOCK(st.st_mode) ? "socket" : "not a socket (mode " +
                                                           std::to_string(st.st_mode) + ")";
  int type = 0;
  socklen_t len = sizeof(type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0) {
    out += std::string(", ") + type_name(type);
  } else {
    out += ", " + err("SO_TYPE");
  }
  sockaddr_storage addr{};
  len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
    out += ", family " + std::to_string(addr.ss_family);
  } else {
    out += ", " + err("getsockname");
  }
  return out;
}

}  // namespace

void ipc_selftest_ui(BrokerClient& client) {
  ps5_log_line("ipc selftest: start");
  std::string why;
  const int own = make_shm(kUiPattern, &why);
  ps5_log_printf("ipc selftest: shm_open(SHM_ANON)+mmap in this process: %s",
                 own >= 0 ? "ok" : why.c_str());
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
    ps5_log_printf("ipc selftest: %s; stop", err("socketpair(SOCK_SEQPACKET)").c_str());
    if (own >= 0) {
      close(own);
    }
    return;
  }
  set_timeout(sv[0], kStepTimeoutMs + 1000);
  std::thread ui([&] {
    char buf[16] = {};
    const ssize_t n = recv(sv[0], buf, sizeof(buf) - 1, 0);
    ps5_log_printf("ipc selftest: UI received from the broker over its rdup'ed end: %s",
                   n > 0 ? buf : err("recv").c_str());
    if (n <= 0) {
      return;
    }
    std::string w;
    ps5_log_printf("ipc selftest: UI -> broker SCM_RIGHTS (shm): %s",
                   own >= 0 && send_fd(sv[0], own, &w) ? "sent" : (own < 0 ? "no shm" : w.c_str()));
    const int got = recv_fd(sv[0], &w);
    if (got < 0) {
      ps5_log_printf("ipc selftest: broker -> UI SCM_RIGHTS: %s", w.c_str());
      return;
    }
    ps5_log_printf("ipc selftest: broker -> UI SCM_RIGHTS: received fd %d (%s), map: %s", got,
                   describe_fd(got).c_str(), check_shm(got, kBrokerPattern).c_str());
    close(got);
  });
  json reply;
  const bool answered =
      client.request(json{{"op", "ipc-selftest"}, {"src_pid", static_cast<int>(getpid())},
                          {"src_fd", sv[1]}},
                     nullptr, 0, &reply, 3 * kStepTimeoutMs);
  ui.join();
  ps5_log_printf("ipc selftest: broker report: %s",
                 answered ? reply.dump().c_str() : "no answer");
  close(sv[0]);
  close(sv[1]);
  if (own >= 0) {
    close(own);
  }
  ps5_log_line("ipc selftest: done");
}

json ipc_selftest_broker(const json& request) {
  json report{{"ok", true}};
  const int src_pid = static_cast<int>(broker_get_int(request, "src_pid", -1));
  const int src_fd = static_cast<int>(broker_get_int(request, "src_fd", -1));
  // rdup (syscall 0x25b) copies a descriptor of another process into this
  // one, as the spawner does into a child (pt.c pt_rdup).
  const int fd = static_cast<int>(syscall(0x25b, src_pid, src_fd));
  if (fd < 0) {
    report["rdup"] = err("rdup");
    ps5_log_printf("ipc selftest (broker): rdup(%d, %d): %s", src_pid, src_fd,
                   report["rdup"].get<std::string>().c_str());
    return report;
  }
  report["rdup"] = "ok, fd " + std::to_string(fd) + " (" + describe_fd(fd) + ")";
  set_timeout(fd, kStepTimeoutMs);
  const char ping[] = "ping";
  report["send"] = send(fd, ping, sizeof(ping), 0) == static_cast<ssize_t>(sizeof(ping))
                       ? "ok"
                       : err("send");
  std::string why;
  const int got = recv_fd(fd, &why);
  if (got >= 0) {
    report["ui_to_broker_scm_rights"] =
        "received (" + describe_fd(got) + "), map: " + check_shm(got, kUiPattern);
    close(got);
  } else {
    report["ui_to_broker_scm_rights"] = why;
  }
  const int own = make_shm(kBrokerPattern, &why);
  if (own >= 0) {
    report["broker_to_ui_scm_rights"] = send_fd(fd, own, &why) ? "sent" : why;
    close(own);
  } else {
    report["broker_to_ui_scm_rights"] = "no shm: " + why;
  }
  close(fd);
  ps5_log_printf("ipc selftest (broker): %s", report.dump().c_str());
  return report;
}

void ipc_describe_inherited_fd(int fd) {
  ps5_log_printf("ipc: fd %d: %s", fd, describe_fd(fd).c_str());
  std::thread([fd] {
    int slept = 0;
    for (int seconds : {2, 10, 30}) {
      std::this_thread::sleep_for(std::chrono::seconds(seconds - slept));
      slept = seconds;
      pollfd p{fd, POLLIN, 0};
      const int n = poll(&p, 1, 0);
      ps5_log_printf("ipc: fd %d after %ds: %s", fd, seconds,
                     n < 0 ? err("poll").c_str()
                           : (p.revents & POLLNVAL ? "closed (taken over or gone)"
                              : p.revents & POLLHUP ? "peer closed"
                              : p.revents & POLLIN  ? "data waiting, not read yet"
                                                    : "nothing waiting"));
    }
  }).detach();
}

}  // namespace prosperoweb::spawn
