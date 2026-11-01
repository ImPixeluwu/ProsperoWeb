// SPDX-License-Identifier: GPL-3.0-or-later
//
// Media-channel rendezvous for the PS5 native title. On the console the UI
// title may run under the Media application category (param.json
// applicationCategoryType 65536), whose sandbox refuses listen() on a
// loopback TCP socket. The audio server (FFmpegAudioWire.h) and the video
// overlay server (FFmpegVideoOverlayWire.h) therefore cannot wait for the
// WebProcess to connect() to a fixed port.
//
// Instead the spawn glue (src/ps5/spawn/spawn_glue.cpp) hands the WebProcess
// an inherited AF_UNIX SOCK_SEQPACKET socket per media service (its number in
// PROSPEROWEB_AUDIO_CONTROL_FD / PROSPEROWEB_VIDEO_CONTROL_FD), the other end
// of which lives in the UI and is polled by that service's server. To open a
// stream the WebProcess makes a socketpair of its own, keeps one end and
// passes the other to the UI over the control socket (one SCM_RIGHTS message
// carrying a single descriptor). The kept end then speaks the ordinary wire
// protocol: only the rendezvous changed, not the framing, so a connection is
// still one socket per stream. No listen(), no connect() to a fixed port.
//
// Engine-independent (POSIX sockets and the C++ standard library only), so the
// host tests drive exactly this code.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

namespace WebCore {
namespace FFmpegMedia {
namespace MediaControl {

// One handoff message is this single data byte plus one descriptor in its
// SCM_RIGHTS ancillary data. The byte carries no meaning beyond making the
// message non-empty (ancillary data needs at least one data byte to travel
// reliably); the dedicated audio/video control sockets already say which
// service the descriptor is for.
constexpr std::uint8_t kHandoffByte = 0x01;

// The control descriptor named by `envName` (PROSPEROWEB_AUDIO_CONTROL_FD or
// PROSPEROWEB_VIDEO_CONTROL_FD), or -1 when it is unset or not a descriptor
// number. A set, valid value means the UI wants the socketpair rendezvous;
// -1 means fall back to the TCP path.
inline int controlFdFromEnv(const char* envName)
{
    const char* value = getenv(envName);
    if (!value || !*value)
        return -1;
    char* end = nullptr;
    long fd = strtol(value, &end, 10);
    if (*end || fd < 0 || fd > 0x7fffffff)
        return -1;
    return static_cast<int>(fd);
}

// Sends `fd` to the UI over the control socket `controlFd`. Thread-safe: the
// control socket is shared by every media player and the audio writer, which
// each open their own stream. Returns false (errno set) on failure.
inline bool sendDescriptor(int controlFd, int fd)
{
    static std::mutex sendLock;
    std::lock_guard<std::mutex> lock(sendLock);
    std::uint8_t byte = kHandoffByte;
    iovec iov { &byte, sizeof(byte) };
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))];
    std::memset(control, 0, sizeof(control));
    msghdr message { };
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &fd, sizeof(int));
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    while (true) {
        ssize_t sent = ::sendmsg(controlFd, &message, flags);
        if (sent >= 0)
            return true;
        if (errno == EINTR)
            continue;
        return false;
    }
}

// Opens a stream over the control socket: makes a SOCK_STREAM socketpair,
// passes one end to the UI through `controlFd` and returns the kept end (the
// caller then runs the wire protocol on it), or -1 with `error` set. A byte
// stream, like the TCP socket it replaces: the audio/video wire protocols
// coalesce several messages into one send() and the server reads with a
// fixed-size buffer, which a SOCK_SEQPACKET datagram would truncate.
inline int openControlConnection(int controlFd, std::string& error)
{
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        error = std::string("socketpair: ") + strerror(errno);
        return -1;
    }
    if (!sendDescriptor(controlFd, pair[1])) {
        error = std::string("passing the stream descriptor to the UI: ") + strerror(errno);
        ::close(pair[0]);
        ::close(pair[1]);
        return -1;
    }
    ::close(pair[1]); // the UI holds its own copy now
    return pair[0];
}

// Outcome of one receiveDescriptor() on the UI side.
enum class Receive : std::uint8_t {
    Received,   // *fd holds a new stream descriptor (close-on-exec set)
    WouldBlock, // nothing pending (the socket is non-blocking)
    Closed,     // the WebProcess end is gone: drop this control socket
    Error,      // a malformed message was skipped; keep polling
};

// Receives one stream descriptor the WebProcess passed (openControlConnection
// on the other end). Non-blocking; the caller polls `controlFd` for POLLIN
// first. On Received, *fd is the new descriptor. The UI runs under the title
// sandbox, so this uses recvmsg only (no fcntl on the socket).
inline Receive receiveDescriptor(int controlFd, int& fd)
{
    fd = -1;
    std::uint8_t byte = 0;
    iovec iov { &byte, sizeof(byte) };
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))];
    msghdr message { };
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    int flags = MSG_DONTWAIT;
#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    ssize_t got;
    do {
        got = ::recvmsg(controlFd, &message, flags);
    } while (got < 0 && errno == EINTR);
    if (got < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? Receive::WouldBlock : Receive::Closed;
    if (got == 0)
        return Receive::Closed;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
            continue;
        int received = -1;
        std::memcpy(&received, CMSG_DATA(header), sizeof(int));
        if (fd < 0)
            fd = received;
        else
            ::close(received); // one descriptor per message; drop any extra
    }
    if (message.msg_flags & MSG_CTRUNC) {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
        return Receive::Error;
    }
    return fd >= 0 ? Receive::Received : Receive::Error;
}

} // namespace MediaControl
} // namespace FFmpegMedia
} // namespace WebCore
