// SPDX-License-Identifier: GPL-3.0-or-later
//
// Client side of the video overlay protocol (FFmpegVideoOverlayWire.h): one
// per media player. A sender thread connects to the UI process's overlay
// server, negotiates the pixel transport on the first frame (named shm
// object, then a file mapped MAP_SHARED, then pixels inline over TCP; or the
// one PROSPEROWEB_VIDEO_TRANSPORT forces) and then sends the newest geometry
// and frame whenever they change. Only the latest frame matters: a frame
// submitted while the previous one is still being sent replaces it.
//
// Remote decoding (RemoteVideoStream below): instead of frames the player's
// video decoder sends the compressed packets over the same connection, and
// the UI decodes them; the presenter then only names the frame to show.
// Once connected, a reader thread receives the server's answers (shm
// results, stream answers, decoded-frame notices).
//
// Engine-independent (POSIX, the C++ library; no WebCore/WTF) so the host
// tests drive the same code as the engine (FFmpegVideoOverlay.cpp,
// FFmpegVideoDecoder.cpp).
//
// Threading: start(), stop() on the owner's thread; setGeometry(), submit(),
// clear(), openStream(), showRemote(), state queries and takeStats() from
// any thread. RemoteVideoStream: any thread.

#pragma once

#include "FFmpegVideoOverlayWire.h"
#include "FFmpegMediaControl.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <memory>
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

class VideoOverlayClient;

// One stream the UI decodes for a player (FFmpegVideoDecoder's remote mode).
// Created by VideoOverlayClient::openStream(); it stays valid (shared) after
// the client is gone, then everything fails softly.
class RemoteVideoStream {
public:
    enum class State : uint8_t { Opening, Hardware, Rejected, Failed, Closed };

    // Flow control: packets sent but not yet consumed by the UI's decoder.
    static constexpr uint32_t maxPacketsInFlight = 32;
    static constexpr size_t maxBytesInFlight = 8u << 20;

    explicit RemoteVideoStream(uint32_t id)
        : m_id(id)
    {
    }

    uint32_t id() const { return m_id; }

    State state() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_state;
    }

    std::string reason() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_reason;
    }

    static const char* stateName(State state)
    {
        switch (state) {
        case State::Opening:
            return "opening";
        case State::Hardware:
            return "hardware";
        case State::Rejected:
            return "rejected";
        case State::Failed:
            return "failed";
        case State::Closed:
            return "closed";
        }
        return "?";
    }

    // Waits for the UI's answer to the stream-open. A stream that is still
    // opening when `timeoutSeconds` pass is closed (reason "no answer").
    State waitForAnswer(double timeoutSeconds, const std::atomic<bool>& abort)
    {
        double deadline = overlayMonotonicSeconds() + timeoutSeconds;
        {
            std::unique_lock<std::mutex> lock(m_lock);
            while (m_state == State::Opening && !abort.load()) {
                double left = deadline - overlayMonotonicSeconds();
                if (left <= 0)
                    break;
                m_changed.wait_for(lock, std::chrono::milliseconds(std::max(1, std::min(50, static_cast<int>(left * 1000)))));
            }
            if (m_state != State::Opening)
                return m_state;
        }
        close(abort.load() ? "aborted" : "no answer from the UI");
        return state();
    }

    bool usable() const { return state() == State::Hardware; }

    // Room for one more packet of `bytes` (always for the first one).
    bool hasCredit(size_t bytes) const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return creditLocked(bytes);
    }

    // Waits up to `seconds` for a notice, an answer or credit.
    void waitForEvent(double seconds)
    {
        std::unique_lock<std::mutex> lock(m_lock);
        if (!m_notices.empty() || m_state != State::Hardware)
            return;
        uint32_t consumed = m_consumed;
        m_changed.wait_for(lock, std::chrono::microseconds(static_cast<int64_t>(seconds * 1e6)), [&] {
            return !m_notices.empty() || m_state != State::Hardware || m_consumed != consumed;
        });
    }

    // False when the stream is not (or no longer) decoded by the UI.
    bool sendPacket(const PacketHeader& header, const uint8_t* data, size_t bytes);
    void reconfigure(StreamOpen open);
    void flush(uint32_t serial) { control(Type::Flush, { m_id, serial, 0 }); }
    void drain(uint32_t serial) { control(Type::Drain, { m_id, serial, 0 }); }
    void show(uint32_t serial, int64_t pts) { control(Type::Show, { m_id, serial, pts }); }
    void discard(uint32_t serial, int64_t pts) { control(Type::Discard, { m_id, serial, pts }); }
    // Ends the stream on the UI (stream-close); no more messages.
    void close(const std::string& reason);

    // Frames decoded and end-of-stream notices, oldest first.
    std::deque<Decoded> takeNotices()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        std::deque<Decoded> notices;
        notices.swap(m_notices);
        return notices;
    }

    uint32_t packetsInFlight() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return static_cast<uint32_t>(m_inFlight.size());
    }

    uint32_t maxFrames() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_maxFrames;
    }

private:
    friend class VideoOverlayClient;

    bool creditLocked(size_t bytes) const
    {
        return m_inFlight.empty() || (m_inFlight.size() < maxPacketsInFlight && m_inFlightBytes + bytes <= maxBytesInFlight);
    }

    void control(Type type, const StreamControl& control);

    // Reader thread of the client.
    void received(Type type, const uint8_t* payload, size_t bytes)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        switch (type) {
        case Type::StreamOk:
            if (auto ok = decodeStreamOk(payload, bytes); ok && ok->stream == m_id && m_state == State::Opening) {
                m_state = State::Hardware;
                m_maxFrames = ok->maxFrames;
            }
            break;
        case Type::StreamReject:
        case Type::StreamError:
            if (auto failure = decodeStreamFailure(payload, bytes); failure && failure->stream == m_id && (m_state == State::Opening || m_state == State::Hardware)) {
                m_state = type == Type::StreamReject ? State::Rejected : State::Failed;
                m_reason = failure->reason;
                m_client = nullptr;
            }
            break;
        case Type::Decoded:
            if (auto decoded = decodeDecoded(payload, bytes); decoded && decoded->stream == m_id) {
                // Counts may arrive out of order (two UI threads send them).
                while (static_cast<int32_t>(decoded->consumed - m_consumed) > 0 && !m_inFlight.empty()) {
                    m_inFlightBytes -= m_inFlight.front();
                    m_inFlight.pop_front();
                    ++m_consumed;
                }
                if (static_cast<int32_t>(decoded->consumed - m_consumed) > 0)
                    m_consumed = decoded->consumed;
                if (decoded->hasFrame() || decoded->drained())
                    m_notices.push_back(*decoded);
            }
            break;
        default:
            return;
        }
        m_changed.notify_all();
    }

    // The client is going away (or lost its connection).
    void detach(const char* reason)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_client = nullptr;
        if (m_state == State::Opening || m_state == State::Hardware) {
            m_state = State::Failed;
            m_reason = reason;
        }
        m_changed.notify_all();
    }

    const uint32_t m_id;
    mutable std::mutex m_lock;
    std::condition_variable m_changed;
    VideoOverlayClient* m_client { nullptr };
    State m_state { State::Opening };
    std::string m_reason;
    uint32_t m_maxFrames { 0 };
    uint32_t m_consumed { 0 };
    std::deque<size_t> m_inFlight;
    size_t m_inFlightBytes { 0 };
    std::deque<Decoded> m_notices;
};

class VideoOverlayClient {
public:
    static constexpr uint32_t slotCount = 3;

    struct Config {
        int port { defaultPort };
        // >= 0: open the connection over this inherited control socket
        // (FFmpegMediaControl.h) instead of connecting to `port`. Set under
        // the Media application-category sandbox, where the UI cannot listen().
        int controlFd { -1 };
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
        detachStream("the overlay closed");
    }

    // Asks the UI to decode a stream (the answer arrives asynchronously:
    // RemoteVideoStream::waitForAnswer). Replaces the previous stream.
    std::shared_ptr<RemoteVideoStream> openStream(StreamOpen open)
    {
        auto stream = std::make_shared<RemoteVideoStream>(m_nextStream.fetch_add(1));
        open.stream = stream->id();
        detachStream("replaced by a new stream");
        if (m_state.load() == State::Failed || m_state.load() == State::Stopped) {
            stream->detach("no overlay server");
            return stream;
        }
        {
            std::lock_guard<std::mutex> streamLock(stream->m_lock);
            stream->m_client = this;
        }
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_stream = stream;
            appendStreamOpen(m_streamOut, open);
        }
        m_wake.notify_all();
        return stream;
    }

    // The presenter shows a remotely decoded frame.
    void showRemote(uint32_t stream, uint32_t serial, int64_t pts)
    {
        std::shared_ptr<RemoteVideoStream> current;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            current = m_stream;
        }
        if (current && current->id() == stream)
            current->show(serial, pts);
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
    friend class RemoteVideoStream;

    // From a RemoteVideoStream, holding its lock (lock order: stream, then
    // client).
    void enqueueStream(const std::vector<uint8_t>& bytes, const uint8_t* data = nullptr, size_t dataBytes = 0)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_streamOut.insert(m_streamOut.end(), bytes.begin(), bytes.end());
            if (dataBytes)
                m_streamOut.insert(m_streamOut.end(), data, data + dataBytes);
        }
        m_wake.notify_all();
    }

    void detachStream(const char* reason)
    {
        std::shared_ptr<RemoteVideoStream> stream;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            stream = std::move(m_stream);
            m_stream = nullptr;
        }
        if (stream)
            stream->detach(reason);
    }

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
            detachStream("no overlay server");
            return;
        }
        m_state = State::Connected;
        m_readerStop = false;
        m_reader = std::thread([this] { readLoop(); });
        std::vector<uint8_t> streamBytes;
        while (true) {
            Frame frame;
            bool haveFrame = false;
            bool sendGeometry = false;
            bool sendClear = false;
            Geometry geometry;
            {
                std::unique_lock<std::mutex> lock(m_lock);
                m_wake.wait_for(lock, std::chrono::milliseconds(200), [this] {
                    return m_stopping || m_geometryDirty || m_havePending || m_clearRequested || !m_streamOut.empty() || m_peerGone;
                });
                if (m_stopping)
                    break;
                streamBytes.clear();
                streamBytes.swap(m_streamOut);
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
            bool ok = !m_peerGone.load();
            if (!ok)
                errno = ECONNRESET;
            m_out.clear();
            if (sendGeometry)
                appendGeometry(m_out, geometry);
            if (sendClear)
                appendMessage(m_out, Type::Clear);
            if (ok && !m_out.empty())
                ok = sendAll(m_out.data(), m_out.size(), overlayMonotonicSeconds() + 2);
            if (ok && !streamBytes.empty())
                ok = sendAll(streamBytes.data(), streamBytes.size(), overlayMonotonicSeconds() + 2);
            if (ok && haveFrame)
                ok = sendFrame(frame);
            if (frame.release)
                frame.release();
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
        m_readerStop = true;
        if (m_reader.joinable())
            m_reader.join();
        detachStream(m_state.load() == State::Failed ? "connection lost" : "the overlay closed");
        unmapSegment();
        closeSocket();
        releasePending();
    }

    // Reader thread: everything the server sends after the hello.
    void readLoop()
    {
        Parser parser;
        while (!m_readerStop.load()) {
            pollfd descriptor { m_fd, POLLIN, 0 };
            int ready = ::poll(&descriptor, 1, 100);
            if (ready < 0 && errno != EINTR)
                break;
            if (ready <= 0)
                continue;
            bool gone = false;
            while (true) {
                uint8_t* buffer = parser.writable(16384);
                ssize_t got = ::recv(m_fd, buffer, 16384, 0);
                if (got > 0) {
                    parser.commit(static_cast<size_t>(got));
                    continue;
                }
                if (!got || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    gone = true;
                break;
            }
            while (true) {
                auto result = parser.next();
                if (result == Parser::Result::Error) {
                    gone = true;
                    break;
                }
                if (result != Parser::Result::Message)
                    break;
                dispatch(parser.message());
            }
            if (gone) {
                m_peerGone = true;
                m_wake.notify_all();
                detachStream("connection lost");
                return;
            }
        }
    }

    void dispatch(const Parser::Message& message)
    {
        if (message.type == Type::ShmResult) {
            {
                std::lock_guard<std::mutex> lock(m_lock);
                m_shmResult = decodeShmResult(message.payload, message.bytes);
                m_shmAnswered = true;
            }
            m_wake.notify_all();
            return;
        }
        std::shared_ptr<RemoteVideoStream> stream;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            stream = m_stream;
        }
        if (stream)
            stream->received(message.type, message.payload, message.bytes);
    }

    bool connect()
    {
        double deadline = overlayMonotonicSeconds() + m_config.connectTimeoutMs / 1000.0;
        const bool useControl = m_config.controlFd >= 0;
        if (useControl) {
            std::string error;
            m_fd = FFmpegMedia::MediaControl::openControlConnection(m_config.controlFd, error);
            if (m_fd < 0) {
                logf("video overlay: player %u: control fd %d failed: %s", m_config.player, m_config.controlFd, error.c_str());
                return false;
            }
            int flags = fcntl(m_fd, F_GETFL, 0);
            fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
            int one = 1;
            (void)one;
#ifdef SO_NOSIGPIPE
            setsockopt(m_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            int sendBuffer = 4 << 20;
            setsockopt(m_fd, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
        } else {
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
        if (useControl)
            logf("video overlay: player %u: connected over control fd %d (server key %u)", m_config.player, m_config.controlFd, *key);
        else
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
            snprintf(name, sizeof(name), "/prosperoweb-video-%u-%u-%u", m_config.pid, m_config.player, m_generation);
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
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_shmAnswered = false;
            m_shmResult.reset();
        }
        if (sendAll(m_out.data(), m_out.size(), deadline)) {
            // The reader thread receives the answer.
            std::unique_lock<std::mutex> lock(m_lock);
            while (!m_shmAnswered && !m_peerGone.load() && !m_stopping) {
                double left = deadline - overlayMonotonicSeconds();
                if (left <= 0)
                    break;
                m_wake.wait_for(lock, std::chrono::milliseconds(std::max(1, static_cast<int>(left * 1000))));
            }
            if (m_shmAnswered)
                result = m_shmResult;
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

    std::vector<uint8_t> m_streamOut;
    std::shared_ptr<RemoteVideoStream> m_stream;
    std::atomic<uint32_t> m_nextStream { 1 };
    bool m_shmAnswered { false };
    std::optional<ShmResult> m_shmResult;
    std::thread m_reader;
    std::atomic<bool> m_readerStop { false };
    std::atomic<bool> m_peerGone { false };

    // Sender thread only.
    int m_fd { -1 };
    Parser m_parser;
    std::vector<uint8_t> m_out;
    uint8_t* m_segment { nullptr };
    ShmLayout m_layout;
    uint32_t m_nextSlot { 0 };
    uint32_t m_generation { 0 };
};

inline bool RemoteVideoStream::sendPacket(const PacketHeader& header, const uint8_t* data, size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_state != State::Hardware || !m_client)
        return false;
    std::vector<uint8_t> message;
    PacketHeader packet = header;
    packet.stream = m_id;
    // Header only; the data is appended straight into the client's outbox.
    appendPacket(message, packet, nullptr, 0);
    putU32(message.data() + 4, static_cast<uint32_t>(packetHeaderBytes + bytes));
    m_client->enqueueStream(message, data, bytes);
    m_inFlight.push_back(bytes);
    m_inFlightBytes += bytes;
    return true;
}

inline void RemoteVideoStream::reconfigure(StreamOpen open)
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_state != State::Hardware || !m_client)
        return;
    open.stream = m_id;
    std::vector<uint8_t> message;
    appendStreamOpen(message, open);
    m_client->enqueueStream(message);
}

inline void RemoteVideoStream::control(Type type, const StreamControl& control)
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_state != State::Hardware || !m_client)
        return;
    std::vector<uint8_t> message;
    appendStreamControl(message, type, control);
    m_client->enqueueStream(message);
}

inline void RemoteVideoStream::close(const std::string& reason)
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_client) {
        std::vector<uint8_t> message;
        appendStreamControl(message, Type::StreamClose, { m_id, 0, 0 });
        m_client->enqueueStream(message);
        m_client = nullptr;
    }
    if (m_state == State::Opening || m_state == State::Hardware) {
        m_state = State::Closed;
        m_reason = reason;
    }
    m_changed.notify_all();
}

} // namespace VideoWire
} // namespace FFmpegMedia
} // namespace WebCore
