// SPDX-License-Identifier: GPL-3.0-or-later
#include "broker_proto.h"

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>

namespace ps5browser::spawn {

namespace {

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

void put_be32(uint32_t v, unsigned char* out) {
  out[0] = static_cast<unsigned char>(v >> 24);
  out[1] = static_cast<unsigned char>(v >> 16);
  out[2] = static_cast<unsigned char>(v >> 8);
  out[3] = static_cast<unsigned char>(v);
}

uint32_t get_be32(const unsigned char* in) {
  return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
         (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

void close_all(std::vector<int>* fds, size_t from) {
  for (size_t i = from; i < fds->size(); i++) {
    close((*fds)[i]);
  }
  fds->resize(from);
}

// Collects the SCM_RIGHTS descriptors of one recvmsg.
void take_fds(msghdr* mh, std::vector<int>* fds) {
  for (cmsghdr* c = CMSG_FIRSTHDR(mh); c; c = CMSG_NXTHDR(mh, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
      continue;
    }
    const size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    const unsigned char* data = CMSG_DATA(c);
    for (size_t i = 0; i < n; i++) {
      int fd;
      std::memcpy(&fd, data + i * sizeof(int), sizeof(int));
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      fds->push_back(fd);
    }
  }
}

// Reads exactly len bytes, collecting any descriptors that come along.
// Returns 1 on success, 0 if the peer closed before the first byte, -1 on
// error or a close mid-way.
int recv_exact(int sock, unsigned char* buf, size_t len, std::vector<int>* fds) {
  size_t done = 0;
  while (done < len) {
    iovec iov{buf + done, len - done};
    alignas(cmsghdr) unsigned char ctl[CMSG_SPACE(sizeof(int) * kBrokerMaxFds)];
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl;
    mh.msg_controllen = sizeof(ctl);
    int flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    const ssize_t n = recvmsg(sock, &mh, flags);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      return -1;
    }
    if (mh.msg_controllen > 0) {
      take_fds(&mh, fds);
    }
    if (n == 0) {
      if (done == 0) {
        return 0;
      }
      errno = ECONNRESET;
      return -1;
    }
    if (mh.msg_flags & MSG_CTRUNC) {
      errno = EMSGSIZE;
      return -1;
    }
    done += static_cast<size_t>(n);
  }
  return 1;
}

}  // namespace

std::string broker_encode_frame(const nlohmann::json& msg) {
  const std::string payload =
      msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  std::string frame(4, '\0');
  put_be32(static_cast<uint32_t>(payload.size()),
           reinterpret_cast<unsigned char*>(frame.data()));
  frame += payload;
  return frame;
}

bool broker_decode_payload(const std::string& payload, nlohmann::json* msg) {
  *msg = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
  return msg->is_object();
}

bool broker_send_frame(int sock, const nlohmann::json& msg, const int* fds, int nfds) {
  if (nfds < 0 || nfds > kBrokerMaxFds || (nfds > 0 && !fds)) {
    errno = EINVAL;
    return false;
  }
  const std::string frame = broker_encode_frame(msg);
  if (frame.size() - 4 > kBrokerMaxFrame) {
    errno = EMSGSIZE;
    return false;
  }
  size_t done = 0;
  bool first = true;
  while (done < frame.size()) {
    iovec iov{const_cast<char*>(frame.data()) + done, frame.size() - done};
    alignas(cmsghdr) unsigned char ctl[CMSG_SPACE(sizeof(int) * kBrokerMaxFds)];
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (first && nfds > 0) {
      std::memset(ctl, 0, sizeof(ctl));
      mh.msg_control = ctl;
      mh.msg_controllen = CMSG_SPACE(sizeof(int) * static_cast<size_t>(nfds));
      cmsghdr* c = CMSG_FIRSTHDR(&mh);
      c->cmsg_level = SOL_SOCKET;
      c->cmsg_type = SCM_RIGHTS;
      c->cmsg_len = CMSG_LEN(sizeof(int) * static_cast<size_t>(nfds));
      std::memcpy(CMSG_DATA(c), fds, sizeof(int) * static_cast<size_t>(nfds));
    }
    const ssize_t n = sendmsg(sock, &mh, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    first = false;  // the descriptors went with the first chunk
    done += static_cast<size_t>(n);
  }
  return true;
}

BrokerRecv broker_recv_frame(int sock, nlohmann::json* msg, std::vector<int>* fds) {
  const size_t base = fds->size();
  unsigned char header[4];
  const int h = recv_exact(sock, header, sizeof(header), fds);
  if (h <= 0) {
    close_all(fds, base);
    return h == 0 ? BrokerRecv::kClosed : BrokerRecv::kError;
  }
  const uint32_t len = get_be32(header);
  if (len > kBrokerMaxFrame) {
    close_all(fds, base);
    errno = EMSGSIZE;
    return BrokerRecv::kError;
  }
  std::string payload(len, '\0');
  if (len > 0 &&
      recv_exact(sock, reinterpret_cast<unsigned char*>(payload.data()), len, fds) != 1) {
    close_all(fds, base);
    if (errno == 0) {
      errno = ECONNRESET;
    }
    return BrokerRecv::kError;
  }
  if (!broker_decode_payload(payload, msg)) {
    close_all(fds, base);
    errno = EBADMSG;
    return BrokerRecv::kError;
  }
  return BrokerRecv::kOk;
}

std::string broker_get_string(const nlohmann::json& msg, const char* key,
                              const std::string& fallback) {
  auto it = msg.find(key);
  return it != msg.end() && it->is_string() ? it->get<std::string>() : fallback;
}

long long broker_get_int(const nlohmann::json& msg, const char* key, long long fallback) {
  auto it = msg.find(key);
  return it != msg.end() && it->is_number_integer() ? it->get<long long>() : fallback;
}

bool broker_get_bool(const nlohmann::json& msg, const char* key, bool fallback) {
  auto it = msg.find(key);
  return it != msg.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

bool broker_get_strings(const nlohmann::json& msg, const char* key,
                        std::vector<std::string>* out) {
  auto it = msg.find(key);
  if (it == msg.end() || !it->is_array()) {
    return false;
  }
  out->clear();
  for (const auto& v : *it) {
    if (!v.is_string()) {
      return false;
    }
    out->push_back(v.get<std::string>());
  }
  return true;
}

}  // namespace ps5browser::spawn
