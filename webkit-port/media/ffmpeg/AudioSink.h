// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio output of the ffmpeg media engine. The sink owns an output thread
// (SDL's audio callback, or a timer thread for the null sink) that asks the
// render callback for interleaved float32 stereo at 48 kHz.
//
// Risk: the WebProcess is a separate payload process. Whether sceAudioOut
// (SDL's PS5 driver) can be opened from it, and whether two processes may
// hold audio ports at once, is unverified on hardware. create() therefore
// falls back to NullAudioSink, which consumes samples in real time so the
// audio clock and A/V sync keep working without sound.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include <memory>
#include <span>
#include <wtf/Function.h>

namespace WebCore {

class AudioSink {
public:
    static constexpr int sampleRate = 48000;
    static constexpr int channels = 2;

    // Called on the sink thread; must fill the whole span (silence if no
    // data is available).
    using RenderCallback = Function<void(std::span<float> interleaved)>;

    // SDL output when available and not disabled with
    // PS5BROWSER_MEDIA_AUDIO=null, otherwise a NullAudioSink.
    static std::unique_ptr<AudioSink> create(RenderCallback&&);
    static std::unique_ptr<AudioSink> createNull(RenderCallback&&);

    virtual ~AudioSink() = default;

    virtual void play() = 0;
    virtual void pause() = 0;
    // Seconds between rendering a sample and hearing it.
    virtual double latency() const = 0;
    virtual bool isNull() const = 0;
    virtual const char* name() const = 0;
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
