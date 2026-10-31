// SPDX-License-Identifier: GPL-3.0-or-later
//
// Client side of the video overlay protocol (FFmpegVideoOverlayWire.h): one
// per media player. A sender thread connects to the UI process's overlay
// server, negotiates the pixel transport on the first frame (named shm
// object, then a file mapped MAP_SHARED, then pixels inline over TCP; or the
// one PS5BROWSER_VIDEO_TRANSPORT forces) and then sends the newest geometry
// and frame whenever they change. Only the latest frame matters: a frame
// submitted while the previous one is still being sent replaces it.
//
// Engine-independent (POSIX, the C++ library; no WebCore/WTF) so the host
// tests drive the same code as the engine (FFmpegVideoOverlay.cpp).
//
// Threading: start(), stop() on the owner's thread; setGeometry(), submit(),
// clear(), state queries and takeStats() from any thread.

#pragma once

#include "FFmpegVideoOverlayWire.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace WebCore {
namespace FFmpegMedia {
namespace VideoWire {

enum class Transport : uint8_t { None, Shm, File, Tcp };

inline const char* transportName(Transport transport)
{
    switch (transport) {
    case Transport::Shm:
        return "shm";
    case Transport::File:
        return "file";
    case Transport::Tcp:
        return "tcp";
    case Transport::None:
        break;
    }
    return "none";
}

// "shm", "file", "tcp"; anything else (or nothing) = automatic.
inline Transport parseTransport(const char* value)
{
    if (!value)
        return Transport::None;
    if (!strcmp(value, "shm"))
        return Transport::Shm;
    if (!strcmp(value, "file"))
        return Transport::File;
    if (!strcmp(value, "tcp"))
        return Transport::Tcp;
    return Transport::None;
}

inline double overlayMonotonicSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

class VideoOverlayClient {
public:
    static constexpr uint32_t slotCount = 3;

    struct Config {
        int port { defaultPort };
        uint32_t pid { 0 };
        uint32_t player { 0 };
        Transport force { Transport::None };
        // Directory of the File transport's backing files.
        std::string fileDirectory { "/tmp" };
        int connectTimeoutMs { 500 };
        // Diagnostics (thread-safe); may be empty.
        std::function<void(const std::string&)> log;
    };

    // A frame to send. `pixels` stays valid until `release` runs (on the
    // sender thread, once the pixels were copied or the frame was replaced).
    struct Frame {
        const uint8_t* pixels { nullptr };
        uint32_t width { 0 };
        uint32_t height { 0 };
        size_t stride { 0 };
        double pts { 0 };
        std::function<void()> release;
    };

    enum class State : uint8_t { Connecting, Connected, Failed, Stopped };

    struct Stats {
        unsigned sent { 0 };
        unsigned replaced { 0 }; // submitted but superseded before sending
        double copyMilliseconds { 0 }; // average per sent frame
    };

    VideoOverlayClient() = default;
    VideoOverlayClient(const VideoOverlayClient&) = delete;
    VideoOverlayClient& operator=(const VideoOverlayClient&) = delete;
    ~VideoOverlayClient() { stop(); }

    void start(Config config)
    {
        m_config = std::move(config);
        m_thread = std::thread([this] { run(); });
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_stopping = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        releasePending();
    }

    State state() const { return m_state.load(); }
    // Connected, and frames can be sent (the transport is chosen on the
    // first frame, so this does not wait for it).
    bool usable() const { return m_state.load() == State::Connected; }
    Transport transport() const { return m_transport.load(); }

    void setGeometry(const Geometry& geometry)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_haveGeometry && m_geometry == geometry)
                return;
            m_geometry = geometry;
            m_haveGeometry = true;
            m_geometryDirty = true;
        }
        m_wake.notify_all();
    }

    void submit(Frame&& frame)
    {
        Frame replaced;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_havePending) {
                replaced = std::move(m_pending);
                ++m_replaced;
            }
            m_pending = std::move(frame);
            m_havePending = true;
            m_clearRequested = false;
        }
        m_wake.notify_all();
        if (replaced.release)
            replaced.release();
    }

    void clear()
    {
        Frame dropped;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_havePending) {
                dropped = std::move(m_pending);
                m_havePending = false;
            }
            m_clearRequested = true;
        }
        m_wake.notify_all();
        if (dropped.release)
            dropped.release();
    }

    Stats takeStats()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        Stats stats;
        stats.sent = m_sent;
        stats.replaced = m_replaced;
        stats.copyMilliseconds = m_sent ? m_copySeconds * 1000 / m_sent : 0;
        m_sent = m_replaced = 0;
        m_copySeconds = 0;
        return stats;
    }

    uint32_t key() const { return m_key.load(); }

private:
    void log(const std::string& line)
    {
        if (m_config.log)
            m_config.log(line);
    }

    [[gnu::format(printf, 2, 3)]] void logf(const char* format, ...)
    {
        char line[400];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(line, sizeof(line), format, arguments);
        va_end(arguments);
        log(line);
    }

    void releasePending()
    {
        Frame dropped;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (!m_havePending)
                return;
            dropped = std::move(m_pending);
            m_havePending = false;
        }
        if (dropped.release)
            dropped.release();
    }

    void run()
    {
        if (!connect()) {
            m_state = State::Failed;
            closeSocket();
            releasePending();
            return;
        }
        m_state = State::Connected;
        while (true) {
            Frame frame;
            bool haveFrame = false;
            bool sendGeometry = false;
            bool sendClear = false;
            Geometry geometry;
            {
                std::unique_lock<std::mutex> lock(m_lock);
                m_wake.wait_for(lock, std::chrono::milliseconds(200), [this] {
                    return m_stopping || m_geometryDirty || m_havePending || m_clearRequested;
                });
                if (m_stopping)
                    break;
                if (m_geometryDirty) {
                    geometry = m_geometry;
                    sendGeometry = true;
                    m_geometryDirty = false;
                }
                if (m_clearRequested) {
                    sendClear = true;
                    m_clearRequested = false;
                }
                if (m_havePending) {
                    frame = std::move(m_pending);
                    m_havePending = false;
                    haveFrame = true;
                }
            }
            bool ok = true;
            m_out.clear();
            if (sendGeometry)
                appendGeometry(m_out, geometry);
            if (sendClear)
                appendMessage(m_out, Type::Clear);
            if (!m_out.empty())
                ok = sendAll(m_out.data(), m_out.size(), overlayMonotonicSeconds() + 2);
            if (ok && haveFrame)
                ok = sendFrame(frame);
            if (frame.release)
                frame.release();
            if (ok && !haveFrame && !sendGeometry && !sendClear)
                ok = checkAlive();
            if (!ok) {
                logf("video overlay: player %u: connection lost (%s)", m_config.player, strerror(errno));
                m_state = State::Failed;
                break;
            }
        }
        if (m_state.load() != State::Failed) {
            m_out.clear();
            appendMessage(m_out, Type::Close);
            sendAll(m_out.data(), m_out.size(), overlayMonotonicSeconds() + 0.2);
            m_state = State::Stopped;
        }
        unmapSegment();
        closeSocket();
        releasePending();
    }

    bool connect()
    {
        double deadline = overlayMonotonicSeconds() + m_config.connectTimeoutMs / 1000.0;
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0)
            return failed("socket");
        int flags = fcntl(m_fd, F_GETFL, 0);
        fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
        int one = 1;
        setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(m_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        int sendBuffer = 4 << 20;
        setsockopt(m_fd, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
        sockaddr_in address { };
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(m_config.port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
            if (errno != EINPROGRESS)
                return failed("connect");
            if (!waitFor(POLLOUT, deadline)) {
                errno = ETIMEDOUT;
                return failed("connect");
            }
            int soError = 0;
            socklen_t length = sizeof(soError);
            getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &soError, &length);
            if (soError) {
                errno = soError;
                return failed("connect");
            }
        }
        uint8_t hello[helloSize];
        encodeHello(Hello { version, m_config.pid, m_config.player }, hello);
        if (!sendAll(hello, sizeof(hello), deadline))
            return failed("sending the hello");
        auto message = receiveMessage(deadline);
        if (!message || message->type != Type::Accepted)
            return failed("no accepted message");
        auto key = decodeAccepted(message->payload, message->bytes);
        if (!key)
            return failed("malformed accepted message");
        m_key = *key;
        logf("video overlay: player %u: connected to 127.0.0.1:%d (server key %u)", m_config.player, m_config.port, *key);
        return true;
    }

    bool failed(const char* what)
    {
        logf("video overlay: player %u: %s failed: %s (127.0.0.1:%d)", m_config.player, what, strerror(errno), m_config.port);
        return false;
    }

    void closeSocket()
    {
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = -1;
    }

    bool waitFor(short events, double deadline)
    {
        while (true) {
            double left = deadline - overlayMonotonicSeconds();
            if (left <= 0)
                return false;
            pollfd descriptor { m_fd, events, 0 };
            int ready = ::poll(&descriptor, 1, std::max(1, static_cast<int>(left * 1000)));
            if (ready > 0)
                return !(descriptor.revents & (POLLERR | POLLNVAL)) || (descriptor.revents & events);
            if (ready < 0 && errno != EINTR)
                return false;
        }
    }

    bool sendAll(const uint8_t* data, size_t bytes, double deadline)
    {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        while (bytes) {
            ssize_t sent = ::send(m_fd, data, bytes, flags);
            if (sent > 0) {
                data += sent;
                bytes -= static_cast<size_t>(sent);
                continue;
            }
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!waitFor(POLLOUT, deadline)) {
                    errno = ETIMEDOUT;
                    return false;
                }
                continue;
            }
            return false;
        }
        return true;
    }

    // Reads until one message is complete or the deadline passes.
    std::optional<Parser::Message> receiveMessage(double deadline)
    {
        while (true) {
            auto result = m_parser.next();
            if (result == Parser::Result::Message)
                return m_parser.message();
            if (result == Parser::Result::Error)
                return std::nullopt;
            if (!waitFor(POLLIN, deadline))
                return std::nullopt;
            uint8_t* buffer = m_parser.writable(4096);
            ssize_t got = ::recv(m_fd, buffer, 4096, 0);
            if (got > 0)
                m_parser.commit(static_cast<size_t>(got));
            else if (!got || (errno != EAGAIN && errno != EINTR))
                return std::nullopt;
        }
    }

    // Idle: notices a server that went away (EOF), drains anything it sent.
    bool checkAlive()
    {
        uint8_t buffer[256];
        while (true) {
            ssize_t got = ::recv(m_fd, buffer, sizeof(buffer), 0);
            if (got > 0)
                continue;
            if (!got) {
                errno = ECONNRESET;
                return false;
            }
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }
    }

    bool sendFrame(const Frame& frame)
    {
        if (!frame.pixels || !frame.width || !frame.height || frame.width > maxFrameWidth || frame.height > maxFrameHeight)
            return true;
        size_t rowBytes = static_cast<size_t>(frame.width) * 4;
        if (m_transport.load() == Transport::None || (m_transport.load() != Transport::Tcp && ShmLayout::slotBytesFor(frame.width, frame.height) > m_layout.slotBytes)) {
            if (!negotiate(frame.width, frame.height))
                return false;
        }
        double start = overlayMonotonicSeconds();
        FrameInfo info { frame.width, frame.height, static_cast<uint32_t>(rowBytes), frame.pts };
        m_out.clear();
        if (m_transport.load() == Transport::Tcp) {
            size_t payload = frameHeaderBytes + info.bytes();
            m_out.resize(headerSize + payload);
            encodeHeader(Type::Frame, static_cast<uint32_t>(payload), m_out.data());
            encodeFrameHeader(info, m_out.data() + headerSize);
            copyRows(frame, m_out.data() + headerSize + frameHeaderBytes, rowBytes);
        } else {
            uint32_t slot = m_nextSlot;
            m_nextSlot = (m_nextSlot + 1) % m_layout.slots;
            SlotWriter writer(m_segment, slot);
            copyRows(frame, m_segment + m_layout.slotOffset(slot), rowBytes);
            uint32_t seq = writer.finish();
            appendPresent(m_out, Present { slot, seq, info });
        }
        double copied = overlayMonotonicSeconds();
        if (!sendAll(m_out.data(), m_out.size(), copied + 2))
            return false;
        std::lock_guard<std::mutex> lock(m_lock);
        ++m_sent;
        m_copySeconds += copied - start;
        return true;
    }

    static void copyRows(const Frame& frame, uint8_t* out, size_t rowBytes)
    {
        if (frame.stride == rowBytes) {
            memcpy(out, frame.pixels, rowBytes * frame.height);
            return;
        }
        for (uint32_t y = 0; y < frame.height; ++y)
            memcpy(out + y * rowBytes, frame.pixels + y * frame.stride, rowBytes);
    }

    // Chooses the pixel transport for frames up to width x height: the forced
    // one, else the first of shm, file, tcp the server accepts.
    bool negotiate(uint32_t width, uint32_t height)
    {
        unmapSegment();
        Transport previous = m_transport.load();
        m_transport = Transport::None;
        uint32_t slotBytes = ShmLayout::slotBytesFor(width, height);
        for (Transport candidate : { Transport::Shm, Transport::File, Transport::Tcp }) {
            if (m_config.force != Transport::None && candidate != m_config.force)
                continue;
            if (candidate == Transport::Tcp) {
                m_transport = Transport::Tcp;
                if (previous != Transport::Tcp)
                    logf("video overlay: player %u: transport tcp (pixels inline, %ux%u)", m_config.player, width, height);
                return true;
            }
            bool connectionOk = true;
            if (offerSegment(candidate, slotBytes, connectionOk)) {
                m_transport = candidate;
                return true;
            }
            if (!connectionOk)
                return false;
        }
        logf("video overlay: player %u: no transport (forced %s failed)", m_config.player, transportName(m_config.force));
        errno = ENOTSUP;
        return false;
    }

    bool offerSegment(Transport kind, uint32_t slotBytes, bool& connectionOk)
    {
        ShmLayout layout { slotCount, slotBytes };
        char name[256];
        ++m_generation;
        int fd;
        if (kind == Transport::Shm) {
            snprintf(name, sizeof(name), "/ps5browser-video-%u-%u-%u", m_config.pid, m_config.player, m_generation);
            fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
        } else {
            snprintf(name, sizeof(name), "%s/video-%u-%u-%u.shm", m_config.fileDirectory.c_str(), m_config.pid, m_config.player, m_generation);
            fd = ::open(name, O_CREAT | O_TRUNC | O_RDWR, 0666);
        }
        auto unlinkName = [&] {
            if (kind == Transport::Shm)
                shm_unlink(name);
            else
                ::unlink(name);
        };
        if (fd < 0) {
            logf("video overlay: player %u: %s %s: open failed: %s", m_config.player, transportName(kind), name, strerror(errno));
            return false;
        }
        // The creator's umask must not keep the UI process out.
        fchmod(fd, 0666);
        if (ftruncate(fd, static_cast<off_t>(layout.totalBytes()))) {
            logf("video overlay: player %u: %s %s: ftruncate(%zu) failed: %s", m_config.player, transportName(kind), name, layout.totalBytes(), strerror(errno));
            ::close(fd);
            unlinkName();
            return false;
        }
        int mapFlags = MAP_SHARED;
#ifdef MAP_NOSYNC
        // A file-backed segment must not be written back to disk.
        if (kind == Transport::File)
            mapFlags |= MAP_NOSYNC;
#endif
        void* mapped = mmap(nullptr, layout.totalBytes(), PROT_READ | PROT_WRITE, mapFlags, fd, 0);
        ::close(fd);
        if (mapped == MAP_FAILED) {
            logf("video overlay: player %u: %s %s: mmap failed: %s", m_config.player, transportName(kind), name, strerror(errno));
            unlinkName();
            return false;
        }
        auto* base = static_cast<uint8_t*>(mapped);
        layout.writeHeader(base);

        m_out.clear();
        appendShmOffer(m_out, ShmOffer { kind == Transport::Shm ? ShmKind::Named : ShmKind::File, layout.slots, layout.slotBytes, name });
        double deadline = overlayMonotonicSeconds() + 2;
        std::optional<ShmResult> result;
        if (sendAll(m_out.data(), m_out.size(), deadline)) {
            while (auto message = receiveMessage(deadline)) {
                if (message->type == Type::ShmResult) {
                    result = decodeShmResult(message->payload, message->bytes);
                    break;
                }
            }
        }
        // Mapped on both sides (or refused): the name is no longer needed,
        // so nothing is left behind even if a process dies.
        unlinkName();
        if (!result) {
            connectionOk = false;
            munmap(mapped, layout.totalBytes());
            logf("video overlay: player %u: no answer to the %s offer", m_config.player, transportName(kind));
            return false;
        }
        if (!result->ok) {
            munmap(mapped, layout.totalBytes());
            logf("video overlay: player %u: the UI could not map %s %s: %s", m_config.player, transportName(kind), name, strerror(static_cast<int>(result->error)));
            return false;
        }
        m_segment = base;
        m_layout = layout;
        m_nextSlot = 0;
        logf("video overlay: player %u: transport %s (%s, %u x %u bytes)", m_config.player, transportName(kind), name, layout.slots, layout.slotBytes);
        return true;
    }

    void unmapSegment()
    {
        if (m_segment)
            munmap(m_segment, m_layout.totalBytes());
        m_segment = nullptr;
        m_layout = { };
    }

    Config m_config;
    std::thread m_thread;
    std::atomic<State> m_state { State::Connecting };
    std::atomic<Transport> m_transport { Transport::None };
    std::atomic<uint32_t> m_key { 0 };

    std::mutex m_lock;
    std::condition_variable m_wake;
    bool m_stopping { false };
    Geometry m_geometry;
    bool m_haveGeometry { false };
    bool m_geometryDirty { false };
    Frame m_pending;
    bool m_havePending { false };
    bool m_clearRequested { false };
    unsigned m_sent { 0 };
    unsigned m_replaced { 0 };
    double m_copySeconds { 0 };

    // Sender thread only.
    int m_fd { -1 };
    Parser m_parser;
    std::vector<uint8_t> m_out;
    uint8_t* m_segment { nullptr };
    ShmLayout m_layout;
    uint32_t m_nextSlot { 0 };
    uint32_t m_generation { 0 };
};

} // namespace VideoWire
} // namespace FFmpegMedia
} // namespace WebCore
