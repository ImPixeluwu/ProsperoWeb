// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "MediaSourcePrivateFFmpeg.h"

#if ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)

#include "ContentType.h"
#include "FFmpegLog.h"
#include "MediaPlayerPrivateFFmpegMSE.h"
#include "MediaSourcePrivateClient.h"
#include "SourceBufferPrivateFFmpeg.h"

namespace WebCore {

Ref<MediaSourcePrivateFFmpeg> MediaSourcePrivateFFmpeg::open(MediaSourcePrivateClient& client, MediaPlayerPrivateFFmpegMSE& player)
{
    Ref source = adoptRef(*new MediaSourcePrivateFFmpeg(client, player));
    client.setPrivateAndOpen(source.copyRef());
    return source;
}

MediaSourcePrivateFFmpeg::MediaSourcePrivateFFmpeg(MediaSourcePrivateClient& client, MediaPlayerPrivateFFmpegMSE& player)
    : MediaSourcePrivate(client)
    , m_player(player)
{
#if !RELEASE_LOG_DISABLED
    client.setLogIdentifier(player.mediaPlayerLogIdentifier());
#endif
}

MediaSourcePrivateFFmpeg::~MediaSourcePrivateFFmpeg() = default;

RefPtr<MediaPlayerPrivateInterface> MediaSourcePrivateFFmpeg::player() const
{
    return m_player.get();
}

RefPtr<MediaPlayerPrivateFFmpegMSE> MediaSourcePrivateFFmpeg::platformPlayer() const
{
    return m_player.get();
}

void MediaSourcePrivateFFmpeg::setPlayer(MediaPlayerPrivateInterface* player)
{
    if (player && player->mediaPlayerType() == MediaPlayerType::FFmpegMSE)
        m_player = static_cast<MediaPlayerPrivateFFmpegMSE*>(player);
    else
        m_player = nullptr;
}

MediaSourcePrivate::AddStatus MediaSourcePrivateFFmpeg::addSourceBuffer(const ContentType& contentType, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>& sourceBufferPrivate)
{
    MediaEngineSupportParameters parameters {
        .platformType = PlatformMediaDecodingType::MediaSource,
        .type = contentType
    };
    auto raw = contentType.raw().utf8();
    if (contentType.containerType().isEmpty() || MediaPlayerPrivateFFmpegMSE::supportsType(parameters) == MediaPlayer::SupportsType::IsNotSupported) {
        ffmpegMediaLog("mse: addSourceBuffer %s -> NotSupported", raw.legacyCStringPointer());
        return AddStatus::NotSupported;
    }

    auto sourceBuffer = SourceBufferPrivateFFmpeg::create(*this, contentType, nextSourceBufferIdentifier());
    {
        Locker locker { m_lock };
        m_sourceBuffers.append(sourceBuffer.copyRef());
    }
    sourceBuffer->setMediaSourceDuration(duration());
    sourceBufferPrivate = WTF::move(sourceBuffer);
    ffmpegMediaLog("mse: addSourceBuffer %s -> #%u", raw.legacyCStringPointer(), m_sourceBufferCount);
    return AddStatus::Ok;
}

void MediaSourcePrivateFFmpeg::removeSourceBuffer(SourceBufferPrivate& sourceBuffer)
{
    if (RefPtr player = platformPlayer(); player && sourceBuffer.platformType() == MediaPlatformType::FFmpeg)
        player->sourceBufferRemoved(static_cast<SourceBufferPrivateFFmpeg&>(sourceBuffer));
    MediaSourcePrivate::removeSourceBuffer(sourceBuffer);
}

void MediaSourcePrivateFFmpeg::notifyActiveSourceBuffersChanged()
{
    if (RefPtr player = platformPlayer())
        player->activeSourceBuffersChanged();
}

void MediaSourcePrivateFFmpeg::durationChanged(const MediaTime& duration)
{
    MediaSourcePrivate::durationChanged(duration);
    ffmpegMediaLog("mse: duration %.3f", duration.isValid() ? duration.toDouble() : -1.0);
    if (RefPtr player = platformPlayer())
        player->durationChangedFromMediaSource();
}

void MediaSourcePrivateFFmpeg::markEndOfStream(EndOfStreamStatus status)
{
    ffmpegMediaLog("mse: endOfStream (%s)", status == EndOfStreamStatus::NoError ? "no error" : status == EndOfStreamStatus::NetworkError ? "network error" : "decode error");
    MediaSourcePrivate::markEndOfStream(status);
}

void MediaSourcePrivateFFmpeg::unmarkEndOfStream()
{
    ffmpegMediaLog("mse: endOfStream cleared");
    MediaSourcePrivate::unmarkEndOfStream();
    if (RefPtr player = platformPlayer())
        player->endOfStreamCleared();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)
