// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaSourcePrivate of the ffmpeg media engine. The generic base class does
// the work (buffered ranges, seek target waiting, re-enqueueing); this class
// creates SourceBufferPrivateFFmpeg objects and forwards duration and
// active-buffer changes to MediaPlayerPrivateFFmpegMSE. Everything runs on
// the main thread (the default SourceBufferPrivate dispatcher).

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)

#include "MediaSourcePrivate.h"
#include <wtf/WeakPtr.h>

namespace WebCore {

class MediaPlayerPrivateFFmpegMSE;

class MediaSourcePrivateFFmpeg final : public MediaSourcePrivate {
public:
    static Ref<MediaSourcePrivateFFmpeg> open(MediaSourcePrivateClient&, MediaPlayerPrivateFFmpegMSE&);
    ~MediaSourcePrivateFFmpeg();

    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::FFmpeg; }

    RefPtr<MediaPlayerPrivateInterface> player() const final;
    void setPlayer(MediaPlayerPrivateInterface*) final;
    RefPtr<MediaPlayerPrivateFFmpegMSE> platformPlayer() const;

    AddStatus addSourceBuffer(const ContentType&, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>&) final;
    void removeSourceBuffer(SourceBufferPrivate&) final;
    void notifyActiveSourceBuffersChanged() final;
    void durationChanged(const MediaTime&) final;
    void markEndOfStream(EndOfStreamStatus) final;
    void unmarkEndOfStream() final;

    unsigned nextSourceBufferIdentifier() { return ++m_sourceBufferCount; }

private:
    MediaSourcePrivateFFmpeg(MediaSourcePrivateClient&, MediaPlayerPrivateFFmpegMSE&);

    WeakPtr<MediaPlayerPrivateFFmpegMSE> m_player;
    unsigned m_sourceBufferCount { 0 };
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::MediaSourcePrivateFFmpeg)
static bool isType(const WebCore::MediaSourcePrivate& mediaSource) { return mediaSource.platformType() == WebCore::MediaPlatformType::FFmpeg; }
SPECIALIZE_TYPE_TRAITS_END()

#endif // ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)
