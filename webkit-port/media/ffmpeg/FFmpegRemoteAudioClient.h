// SPDX-License-Identifier: GPL-3.0-or-later
//
// Client side of the audio forwarding protocol (FFmpegAudioWire.h): connects
// to the UI process's audio server over loopback TCP and streams PCM pulled
// from a render callback, keeping about `targetAhead` seconds queued on the
// server. Engine-independent (POSIX sockets, no WebCore/WTF) so the host
// tests drive the same code as RemoteAudioSink.
//
// Threading: connect() runs once on the creating thread. pump() runs on one
// writer thread and does all socket I/O. setPlaying(), requestFlush(),
// setVolume() and latencySeconds() may be called from any thread.

#pragma once

#include "FFmpegAudioWire.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace WebCore {
namespace FFmpegMedia {
namespace AudioWire {

inline double monotonicSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

class RemoteAudioClient {
public:
    static constexpr double defaultTargetAhead = 0.1;
    static constexpr size_t chunkFrames = 480; // 10 ms

    RemoteAudioClient() = default;
    RemoteAudioClient(const RemoteAudioClient&) = delete;
    RemoteAudioClient& operator=(const RemoteAudioClient&) = delete;

    ~RemoteAudioClient()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }

    void setTargetAhead(double seconds) { m_targetAhead = std::clamp(seconds, 0.02, 0.9); }
    double targetAhead() const { return m_targetAhead; }

    // Connects to 127.0.0.1:port, sends the hello and waits for "accepted",
    // all within `timeoutMs`. On failure `error` says why.
    bool connect(int port, int timeoutMs, std::string& error)
    {
        double deadline = monotonicSeconds() + timeoutMs / 1000.0;
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0)
            return fail(error, "socket");
        int flags = fcntl(m_fd, F_GETFL, 0);
        fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
        int one = 1;
        setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(m_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        sockaddr_in address { };
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
            if (errno != EINPROGRESS)
                return fail(error, "connect");
            if (!waitFor(POLLOUT, deadline))
                return fail(error, "connect timed out", false);
            int soError = 0;
            socklen_t length = sizeof(soError);
            getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &soError, &length);
            if (soError) {
                errno = soError;
                return fail(error, "connect");
            }
        }
        uint8_t hello[helloSize];
        encodeHello(Hello { }, hello);
        if (!sendAll(hello, sizeof(hello), deadline))
            return fail(error, "sending the hello");
        while (true) {
            auto result = m_parser.next();
            if (result == FrameParser::Result::Message) {
                auto& message = m_parser.message();
                if (message.type == Type::Accepted) {
                    auto accepted = decodeAccepted(message.payload, message.bytes);
                    if (!accepted)
                        return fail(error, "malformed accepted message", false);
                    m_accepted = *accepted;
                    std::lock_guard<std::mutex> lock(m_lock);
                    m_clock.setOutputLatencyFrames(accepted->outputLatencyFrames);
                    return true;
                }
                if (message.type == Type::Rejected)
                    return fail(error, "the server rejected the stream format", false);
                continue;
            }
            if (result == FrameParser::Result::Error)
                return fail(error, "protocol error", false);
            if (!waitFor(POLLIN, deadline))
                return fail(error, "no answer from the server", false);
            if (!receive())
                return fail(error, "connection closed during the handshake", false);
        }
    }

    const Accepted& accepted() const { return m_accepted; }
    bool failed() const { return m_failed; }
    const std::string& failure() const { return m_failure; }

    void setPlaying(bool playing) { m_wantPlaying = playing; }
    void requestFlush()
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_clock.retireAllSent();
        }
        m_flushRequested = true;
    }
    void setVolume(float gain)
    {
        m_volume = gain;
        m_volumeChanged = true;
    }
    void requestClose() { m_closeRequested = true; }

    double latencySeconds()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_clock.latencySeconds(monotonicSeconds());
    }

    uint64_t totalSent()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_clock.totalSent();
    }

    uint64_t estimatedRetired()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_clock.estimatedRetired(monotonicSeconds());
    }

    // One step of the writer thread: reads played reports, forwards control
    // requests, then tops the server up to targetAhead() with frames from
    // `pull(float* interleaved, size_t frames)`. After a connection failure
    // it keeps pulling in real time and discards the samples, so the clock
    // still advances (like the null sink). Returns false once failed.
    template<typename Pull>
    bool pump(Pull&& pull)
    {
        double now = monotonicSeconds();
        if (m_failed) {
            paceLocally(now, pull);
            return false;
        }
        if (!receive() || !handleMessages(now)) {
            setFailed("connection lost");
            return false;
        }
        m_out.clear();
        if (m_flushRequested.exchange(false))
            appendMessage(m_out, Type::Flush);
        if (m_volumeChanged.exchange(false))
            appendVolume(m_out, m_volume);
        bool playing = m_wantPlaying;
        if (playing != m_sentPlaying) {
            appendPause(m_out, !playing);
            m_sentPlaying = playing;
            std::lock_guard<std::mutex> lock(m_lock);
            m_clock.setPlaying(playing, now);
        }
        bool closing = m_closeRequested;
        uint64_t ahead;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            ahead = m_clock.aheadFrames(now);
        }
        uint64_t target = static_cast<uint64_t>(m_targetAhead * sampleRate);
        uint64_t pulled = 0;
        while (playing && !closing && ahead + pulled < target) {
            size_t frames = static_cast<size_t>(std::min<uint64_t>(chunkFrames, target - ahead - pulled));
            m_samples.resize(frames * channels);
            pull(m_samples.data(), frames);
            appendMessage(m_out, Type::Pcm, m_samples.data(), m_samples.size() * sizeof(float));
            pulled += frames;
            // Counted as soon as the render callback consumed them: the
            // engine's clock already includes these frames.
            std::lock_guard<std::mutex> lock(m_lock);
            m_clock.sent(frames);
        }
        if (closing)
            appendMessage(m_out, Type::Close);
        if (!m_out.empty() && !sendAll(m_out.data(), m_out.size(), now + 1)) {
            setFailed("send failed");
            return false;
        }
        if (closing) {
            ::close(m_fd);
            m_fd = -1;
            m_failed = true;
            m_failure = "closed";
        }
        return true;
    }

private:
    bool fail(std::string& error, const char* what, bool withErrno = true)
    {
        error = what;
        if (withErrno) {
            error += ": ";
            error += strerror(errno);
        }
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = -1;
        return false;
    }

    void setFailed(const char* why)
    {
        m_failed = true;
        m_failure = why;
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = -1;
        std::lock_guard<std::mutex> lock(m_lock);
        m_clock.setOutputLatencyFrames(0);
        m_clock.retireAllSent();
        m_paceTime = monotonicSeconds();
        m_paceFrames = 0;
    }

    template<typename Pull>
    void paceLocally(double now, Pull& pull)
    {
        if (!m_wantPlaying) {
            m_paceTime = now;
            return;
        }
        m_paceFrames += std::min(now - m_paceTime, 0.1) * sampleRate;
        m_paceTime = now;
        size_t frames = static_cast<size_t>(m_paceFrames);
        m_paceFrames -= frames;
        if (!frames)
            return;
        m_samples.resize(frames * channels);
        pull(m_samples.data(), frames);
        std::lock_guard<std::mutex> lock(m_lock);
        m_clock.sent(frames);
        m_clock.retireAllSent();
    }

    bool waitFor(short events, double deadline)
    {
        while (true) {
            int timeout = static_cast<int>((deadline - monotonicSeconds()) * 1000);
            if (timeout < 0)
                return false;
            pollfd entry { m_fd, events, 0 };
            int result = ::poll(&entry, 1, timeout);
            if (result > 0)
                return true;
            if (!result || errno != EINTR)
                return false;
        }
    }

    bool sendAll(const uint8_t* data, size_t bytes, double deadline)
    {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
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
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waitFor(POLLOUT, deadline))
                continue;
            return false;
        }
        return true;
    }

    // Non-blocking read of whatever arrived. False when the peer closed.
    bool receive()
    {
        uint8_t buffer[4096];
        while (true) {
            ssize_t got = ::recv(m_fd, buffer, sizeof(buffer), 0);
            if (got > 0) {
                m_parser.append(buffer, static_cast<size_t>(got));
                continue;
            }
            if (!got)
                return false;
            if (errno == EINTR)
                continue;
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
    }

    bool handleMessages(double now)
    {
        while (true) {
            auto result = m_parser.next();
            if (result == FrameParser::Result::NeedMore)
                return true;
            if (result != FrameParser::Result::Message)
                return false;
            auto& message = m_parser.message();
            if (message.type == Type::Played) {
                if (auto played = decodePlayed(message.payload, message.bytes)) {
                    std::lock_guard<std::mutex> lock(m_lock);
                    m_clock.report(*played, now);
                }
            }
        }
    }

    int m_fd { -1 };
    FrameParser m_parser;
    Accepted m_accepted;
    std::mutex m_lock;
    RemoteAudioClock m_clock; // guarded by m_lock
    double m_targetAhead { defaultTargetAhead };
    std::vector<uint8_t> m_out;
    std::vector<float> m_samples;
    std::atomic<bool> m_wantPlaying { false };
    std::atomic<bool> m_flushRequested { false };
    std::atomic<bool> m_volumeChanged { false };
    std::atomic<float> m_volume { 1 };
    std::atomic<bool> m_closeRequested { false };
    bool m_sentPlaying { true }; // the server starts a stream unpaused
    bool m_failed { false };
    std::string m_failure;
    double m_paceTime { 0 };
    double m_paceFrames { 0 };
};

} // namespace AudioWire
} // namespace FFmpegMedia
} // namespace WebCore
