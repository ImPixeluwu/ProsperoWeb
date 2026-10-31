// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "AudioSink.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegLog.h"
#include "Logging.h"
#include <cstdlib>
#include <cstring>
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

bool nullSinkRequested()
{
    const char* value = getenv("PS5BROWSER_MEDIA_AUDIO");
    return value && !strcmp(value, "null");
}
#endif

} // namespace

std::unique_ptr<AudioSink> AudioSink::createNull(RenderCallback&& render)
{
    return makeUnique<NullAudioSink>(WTF::move(render));
}

std::unique_ptr<AudioSink> AudioSink::create(RenderCallback&& render)
{
#if defined(FFMPEG_MEDIA_SDL_AUDIO) && FFMPEG_MEDIA_SDL_AUDIO
    if (!nullSinkRequested()) {
        if (auto sink = SDLAudioSink::create(render))
            return sink;
        ffmpegMediaLog("audio sink: falling back to the null sink (no sound, the clock keeps running)");
    } else
        ffmpegMediaLog("audio sink: PS5BROWSER_MEDIA_AUDIO=null, using the null sink");
#else
    ffmpegMediaLog("audio sink: built without SDL audio, using the null sink");
#endif
    return createNull(WTF::move(render));
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
