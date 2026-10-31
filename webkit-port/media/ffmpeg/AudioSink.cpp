// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "AudioSink.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegLog.h"
#include "FFmpegRemoteAudioClient.h"
#include "Logging.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

#if defined(FFMPEG_MEDIA_SDL_AUDIO) && FFMPEG_MEDIA_SDL_AUDIO
#include <SDL2/SDL.h>
#endif

namespace WebCore {

namespace {

// Consumes samples at the real-time rate and discards them.
class NullAudioSink final : public AudioSink {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NullAudioSink);
public:
    explicit NullAudioSink(RenderCallback&& render)
        : m_render(WTF::move(render))
        , m_scratch(static_cast<size_t>(sampleRate / 10) * channels)
    {
        m_thread = Thread::create("FFmpegNullAudio"_s, [this] {
            run();
        });
    }

    ~NullAudioSink() final
    {
        {
            Locker locker { m_lock };
            m_quit = true;
            m_condition.notifyAll();
        }
        m_thread->waitForCompletion();
    }

    void play() final
    {
        Locker locker { m_lock };
        if (!m_playing)
            m_lastTick = MonotonicTime::now();
        m_playing = true;
        m_condition.notifyAll();
    }

    void pause() final
    {
        Locker locker { m_lock };
        m_playing = false;
    }

    double latency() const final { return 0; }
    bool isNull() const final { return true; }
    const char* name() const final { return "null"; }

private:
    void run()
    {
        double pendingFrames = 0;
        while (true) {
            size_t frames;
            {
                Locker locker { m_lock };
                while (!m_quit && !m_playing)
                    m_condition.wait(m_lock);
                if (m_quit)
                    return;
                m_condition.waitFor(m_lock, 10_ms);
                if (m_quit)
                    return;
                if (!m_playing)
                    continue;
                auto now = MonotonicTime::now();
                pendingFrames += (now - m_lastTick).seconds() * sampleRate;
                m_lastTick = now;
                frames = std::min(static_cast<size_t>(pendingFrames), m_scratch.size() / channels);
                pendingFrames -= frames;
            }
            if (frames)
                m_render(m_scratch.mutableSpan().first(frames * channels));
        }
    }

    RenderCallback m_render;
    Vector<float> m_scratch;
    RefPtr<Thread> m_thread;
    Lock m_lock;
    Condition m_condition;
    bool m_playing WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_quit WTF_GUARDED_BY_LOCK(m_lock) { false };
    MonotonicTime m_lastTick WTF_GUARDED_BY_LOCK(m_lock);
};

// Forwards samples to the UI process's audio server (FFmpegAudioWire.h).
// A writer thread pulls from the render callback whenever less than the
// target (100 ms, PROSPEROWEB_AUDIO_AHEAD_MS) is queued on the server, so the
// main thread never touches the socket. latency() follows the server's
// played reports, which makes the engine's audio clock track what is heard.
class RemoteAudioSink final : public AudioSink {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(RemoteAudioSink);
public:
    static std::unique_ptr<AudioSink> create(RenderCallback& render, int port)
    {
        auto sink = std::unique_ptr<RemoteAudioSink>(new RemoteAudioSink(WTF::move(render)));
        if (const char* ahead = getenv("PROSPEROWEB_AUDIO_AHEAD_MS"); ahead && atoi(ahead) > 0)
            sink->m_client.setTargetAhead(atoi(ahead) / 1000.0);
        std::string error;
        if (!sink->m_client.connect(port, 300, error)) {
            ffmpegMediaLog("audio sink: remote 127.0.0.1:%d failed: %s", port, error.c_str());
            render = WTF::move(sink->m_render);
            return nullptr;
        }
        const auto& accepted = sink->m_client.accepted();
        ffmpegMediaLog("audio sink: remote 127.0.0.1:%d connected (stream %u, output latency %u frames, %.0f ms ahead)",
            port, accepted.streamId, accepted.outputLatencyFrames, sink->m_client.targetAhead() * 1000);
        sink->m_port = port;
        sink->m_thread = Thread::create("FFmpegRemoteAudio"_s, [self = sink.get()] {
            self->run();
        });
        return sink;
    }

    ~RemoteAudioSink() final
    {
        {
            Locker locker { m_lock };
            m_quit = true;
            m_condition.notifyAll();
        }
        m_thread->waitForCompletion();
    }

    void play() final
    {
        m_client.setPlaying(true);
        wake();
    }

    void pause() final
    {
        m_client.setPlaying(false);
        wake();
    }

    void flush() final
    {
        m_client.requestFlush();
        wake();
    }

    double latency() const final { return m_client.latencySeconds(); }
    bool isNull() const final { return false; }
    const char* name() const final { return "remote"; }

private:
    explicit RemoteAudioSink(RenderCallback&& render)
        : m_render(WTF::move(render))
    {
    }

    void wake()
    {
        Locker locker { m_lock };
        m_condition.notifyAll();
    }

    void run()
    {
        auto pull = [this](float* interleaved, size_t frames) {
            m_render(std::span<float>(interleaved, frames * channels));
        };
        bool loggedFailure = false;
        while (true) {
            {
                Locker locker { m_lock };
                if (m_quit)
                    break;
            }
            if (!m_client.pump(pull) && !loggedFailure) {
                loggedFailure = true;
                ffmpegMediaLog("audio sink: remote 127.0.0.1:%d %s, continuing without sound",
                    m_port, m_client.failure().c_str());
            }
            Locker locker { m_lock };
            if (!m_quit)
                m_condition.waitFor(m_lock, 5_ms);
        }
        // Sends the close message.
        m_client.requestClose();
        if (!m_client.failed())
            m_client.pump(pull);
    }

    RenderCallback m_render;
    mutable FFmpegMedia::AudioWire::RemoteAudioClient m_client;
    RefPtr<Thread> m_thread;
    int m_port { 0 };
    Lock m_lock;
    Condition m_condition;
    bool m_quit WTF_GUARDED_BY_LOCK(m_lock) { false };
};

#if defined(FFMPEG_MEDIA_SDL_AUDIO) && FFMPEG_MEDIA_SDL_AUDIO
class SDLAudioSink final : public AudioSink {
public:
    static std::unique_ptr<AudioSink> create(RenderCallback& render)
    {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            ffmpegMediaLog("audio sink: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
            return nullptr;
        }
        const char* driver = SDL_GetCurrentAudioDriver();
        int devices = SDL_GetNumAudioDevices(0);
        const char* deviceName = devices > 0 ? SDL_GetAudioDeviceName(0, 0) : nullptr;
        ffmpegMediaLog("audio sink: SDL driver %s, %d output devices, opening the default (%s)",
            driver ? driver : "(none)", devices, deviceName ? deviceName : "unnamed");
        auto sink = std::unique_ptr<SDLAudioSink>(new SDLAudioSink(WTF::move(render)));
        SDL_AudioSpec desired { };
        desired.freq = sampleRate;
        desired.format = AUDIO_F32SYS;
        desired.channels = channels;
        desired.samples = 1024;
        desired.callback = callback;
        desired.userdata = sink.get();
        SDL_AudioSpec obtained { };
        // No allowed changes: SDL converts to the device format itself.
        sink->m_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (!sink->m_device) {
            ffmpegMediaLog("audio sink: SDL_OpenAudioDevice(%d Hz, f32, %d ch, %d samples) failed: %s",
                desired.freq, desired.channels, desired.samples, SDL_GetError());
            // The sink's destructor balances SDL_InitSubSystem.
            render = WTF::move(sink->m_render);
            return nullptr;
        }
        ffmpegMediaLog("audio sink: SDL device %u open: requested %d Hz f32 %d ch %d samples, obtained %d Hz format 0x%x %d ch %d samples",
            static_cast<unsigned>(sink->m_device), desired.freq, desired.channels, desired.samples,
            obtained.freq, static_cast<unsigned>(obtained.format), obtained.channels, obtained.samples);
        // The callback fills one buffer while the device plays the previous.
        sink->m_latency = 2.0 * obtained.samples / sampleRate;
        return sink;
    }

    ~SDLAudioSink() final
    {
        if (m_device) {
            // Returns once no callback is running, so `this` is safe to free.
            SDL_CloseAudioDevice(m_device);
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    void play() final { SDL_PauseAudioDevice(m_device, 0); }
    void pause() final { SDL_PauseAudioDevice(m_device, 1); }
    double latency() const final { return m_latency; }
    bool isNull() const final { return false; }
    const char* name() const final { return "sdl"; }

private:
    explicit SDLAudioSink(RenderCallback&& render)
        : m_render(WTF::move(render))
    {
    }

    static void callback(void* userData, Uint8* stream, int length)
    {
        auto* sink = static_cast<SDLAudioSink*>(userData);
        size_t count = static_cast<size_t>(length) / sizeof(float);
        sink->m_render(std::span<float>(reinterpret_cast<float*>(stream), count));
    }

    RenderCallback m_render;
    SDL_AudioDeviceID m_device { 0 };
    double m_latency { 0 };
};

#endif

// The audio server's port: PROSPEROWEB_AUDIO_PORT (the UI exports it), else
// the default on PlayStation. 0 when forwarding is off.
int remotePort()
{
    const char* value = getenv("PROSPEROWEB_AUDIO_PORT");
    if (!value || !*value) {
#if PLATFORM(PLAYSTATION)
        return FFmpegMedia::AudioWire::defaultPort;
#else
        return 0;
#endif
    }
    char* end = nullptr;
    long port = strtol(value, &end, 10);
    return (!*end && port > 0 && port <= 65535) ? static_cast<int>(port) : 0;
}

} // namespace

std::unique_ptr<AudioSink> AudioSink::createNull(RenderCallback&& render)
{
    return makeUnique<NullAudioSink>(WTF::move(render));
}

std::unique_ptr<AudioSink> AudioSink::create(RenderCallback&& render)
{
    const char* requested = getenv("PROSPEROWEB_MEDIA_AUDIO");
    std::string mode = requested ? requested : "";
    if (mode == "null") {
        ffmpegMediaLog("audio sink: PROSPEROWEB_MEDIA_AUDIO=null, using the null sink");
        return createNull(WTF::move(render));
    }
    if (!mode.empty() && mode != "remote" && mode != "sdl")
        ffmpegMediaLog("audio sink: unknown PROSPEROWEB_MEDIA_AUDIO=%s, using the default order", mode.c_str());

    int port = remotePort();
    if (mode == "remote" && !port)
        port = FFmpegMedia::AudioWire::defaultPort;
    if (mode != "sdl" && port) {
        if (auto sink = RemoteAudioSink::create(render, port))
            return sink;
        ffmpegMediaLog("audio sink: remote unavailable, trying the local SDL output (inaudible from the WebProcess on the PS5)");
    }

#if defined(FFMPEG_MEDIA_SDL_AUDIO) && FFMPEG_MEDIA_SDL_AUDIO
    if (auto sink = SDLAudioSink::create(render))
        return sink;
    ffmpegMediaLog("audio sink: falling back to the null sink (no sound, the clock keeps running)");
#else
    ffmpegMediaLog("audio sink: built without SDL audio, using the null sink");
#endif
    return createNull(WTF::move(render));
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
