// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "FFmpegVideoOverlay.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "CachedResourceLoader.h"
#include "Document.h"
#include "DocumentView.h"
#include "FFmpegLog.h"
#include "FFmpegVideoDecoder.h"
#include "FFmpegVideoOverlayClient.h"
#include "FloatQuad.h"
#include "HTMLVideoElement.h"
#include "LocalFrameView.h"
#include "MediaPlayer.h"
#include "Page.h"
#include "RenderElementInlines.h"
#include "RenderObjectInlines.h"
#include "RenderVideo.h"
#include "RenderVideoInlines.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FFmpegVideoOverlay);

namespace {

namespace Wire = FFmpegMedia::VideoWire;

bool overlaysEnabled()
{
    const char* value = getenv("PROSPEROBROWSER_VIDEO_OVERLAY");
    if (value && *value)
        return strcmp(value, "0") && strcmp(value, "off");
#if PLATFORM(PLAYSTATION)
    return true;
#else
    return false;
#endif
}

// -1 = disabled.
int overlayPort()
{
    const char* value = getenv("PROSPEROBROWSER_VIDEO_PORT");
    if (!value || !*value)
        return Wire::defaultPort;
    if (!strcmp(value, "off"))
        return -1;
    char* end = nullptr;
    long port = strtol(value, &end, 10);
    if (*end || port <= 0 || port > 65535)
        return -1;
    return static_cast<int>(port);
}

std::string fileDirectory()
{
    if (const char* value = getenv("PROSPEROBROWSER_VIDEO_SHM_DIR"); value && *value)
        return value;
#if PLATFORM(PLAYSTATION)
    return "/data/prosperobrowser/tmp";
#else
    return "/tmp";
#endif
}

struct ElementGeometry {
    Wire::Geometry geometry;
    bool rectilinear { true };
};

// The video box of the element playing `player`, in view coordinates. The
// engine only knows its MediaPlayer; the element is found through the
// document the player loads for (platform code reaching into the DOM, kept
// to this one function).
ElementGeometry elementGeometry(MediaPlayer& player)
{
    ElementGeometry result;
    auto* loader = player.cachedResourceLoader();
    RefPtr document = loader ? loader->document() : nullptr;
    if (!document)
        return result;
    RefPtr<HTMLVideoElement> video;
    document->forEachMediaElement([&](HTMLMediaElement& element) {
        if (!video && element.player() == &player)
            video = dynamicDowncast<HTMLVideoElement>(element);
    });
    if (!video)
        return result;
    if (RefPtr page = document->page()) {
        if (auto identifier = page->identifier())
            result.geometry.pageId = identifier->toUInt64();
    }
    result.geometry.fullscreen = video->isFullscreen();
    CheckedPtr renderer = video->renderer();
    RefPtr view = document->view();
    if (!renderer || !view || renderer->usedStyle().visibility() != UsedVisibility::Visible)
        return result;
    FloatQuad quad = renderer->localToAbsoluteQuad(FloatQuad(FloatRect(renderer->videoBox())));
    quad = view->contentsToMainFrameView(quad);
    result.rectilinear = quad.isRectilinear();
    IntRect rect = enclosingIntRect(quad.boundingBox());
    result.geometry.x = rect.x();
    result.geometry.y = rect.y();
    result.geometry.width = rect.width();
    result.geometry.height = rect.height();
    result.geometry.visible = !rect.isEmpty();
    return result;
}

std::atomic<unsigned> nextPlayer { 1 };

} // namespace

std::unique_ptr<FFmpegVideoOverlay> FFmpegVideoOverlay::createIfEnabled()
{
    static bool loggedDisabled;
    if (!overlaysEnabled()) {
        if (!std::exchange(loggedDisabled, true))
            ffmpegMediaLog("video overlay: off (PROSPEROBROWSER_VIDEO_OVERLAY), video is painted into the page");
        return nullptr;
    }
    int port = overlayPort();
    if (port < 0) {
        if (!std::exchange(loggedDisabled, true))
            ffmpegMediaLog("video overlay: off (PROSPEROBROWSER_VIDEO_PORT=%s)", getenv("PROSPEROBROWSER_VIDEO_PORT"));
        return nullptr;
    }
    return std::unique_ptr<FFmpegVideoOverlay>(new FFmpegVideoOverlay(nextPlayer++, port));
}

FFmpegVideoOverlay::FFmpegVideoOverlay(unsigned player, int port)
    : m_client(new Wire::VideoOverlayClient())
    , m_player(player)
{
    Wire::VideoOverlayClient::Config config;
    config.port = port;
    config.pid = static_cast<uint32_t>(getpid());
    config.player = player;
    const char* forced = getenv("PROSPEROBROWSER_VIDEO_TRANSPORT");
    config.force = Wire::parseTransport(forced);
    config.fileDirectory = fileDirectory();
    config.log = [](const std::string& line) {
        ffmpegMediaLog("%s", line.c_str());
    };
    ffmpegMediaLog("video overlay: player %u: on (pid %d, port %d, transport %s)", player, static_cast<int>(getpid()), port,
        config.force == Wire::Transport::None ? "auto: shm, file, tcp" : Wire::transportName(config.force));
    m_client->start(WTF::move(config));
}

FFmpegVideoOverlay::~FFmpegVideoOverlay()
{
    m_client->stop();
}

bool FFmpegVideoOverlay::update(MediaPlayer& player, bool pageVisible, bool hasFrame)
{
    auto element = elementGeometry(player);
    Wire::Geometry geometry = element.geometry;
    geometry.visible = geometry.visible && pageVisible && element.rectilinear;
    if (!element.rectilinear && !std::exchange(m_loggedSkew, true))
        ffmpegMediaLog("video overlay: player %u: the video box is rotated or skewed, painting it into the page", m_player);
    if (m_client->usable() && (!m_geometry || *m_geometry != geometry)) {
        if (!m_geometry || m_geometry->visible != geometry.visible || m_geometry->width != geometry.width || m_geometry->height != geometry.height || m_geometry->fullscreen != geometry.fullscreen) {
            ffmpegMediaLog("video overlay: player %u: rect %d,%d %dx%d page %llu%s%s", m_player, geometry.x, geometry.y, geometry.width, geometry.height,
                static_cast<unsigned long long>(geometry.pageId), geometry.visible ? "" : " hidden", geometry.fullscreen ? " fullscreen" : "");
        }
        m_geometry = geometry;
        m_client->setGeometry(geometry);
    }
    if (m_client->state() == Wire::VideoOverlayClient::State::Failed && !std::exchange(m_loggedFailure, true))
        ffmpegMediaLog("video overlay: player %u: no overlay server, painting video into the page", m_player);

    bool punch = m_client->usable() && geometry.visible && hasFrame;
    if (punch == m_punching)
        return false;
    m_punching = punch;
    ffmpegMediaLog("video overlay: player %u: hole punch %s", m_player, punch ? "on" : "off");
    return true;
}

void FFmpegVideoOverlay::submit(FFmpegVideoFrame& frame)
{
    if (!m_client->usable() || !m_geometry || !m_geometry->visible)
        return;
    Ref protectedFrame = frame;
    FFmpegVideoFrame* held = &protectedFrame.leakRef();
    Wire::VideoOverlayClient::Frame wireFrame;
    wireFrame.pixels = held->data();
    wireFrame.width = static_cast<uint32_t>(held->width());
    wireFrame.height = static_cast<uint32_t>(held->height());
    wireFrame.stride = held->stride();
    wireFrame.pts = held->pts;
    wireFrame.release = [held] {
        held->deref();
    };
    m_client->submit(WTF::move(wireFrame));
}

void FFmpegVideoOverlay::clear()
{
    m_client->clear();
}

void FFmpegVideoOverlay::logStats(double seconds)
{
    auto stats = m_client->takeStats();
    const char* state = "connecting";
    switch (m_client->state()) {
    case Wire::VideoOverlayClient::State::Connected:
        state = Wire::transportName(m_client->transport());
        break;
    case Wire::VideoOverlayClient::State::Failed:
        state = "failed";
        break;
    case Wire::VideoOverlayClient::State::Stopped:
        state = "stopped";
        break;
    case Wire::VideoOverlayClient::State::Connecting:
        break;
    }
    Wire::Geometry geometry = m_geometry.value_or(Wire::Geometry { });
    ffmpegMediaLog("video overlay: player %u: %s sent %.1f/s replaced %u copy %.2f ms | hole %s rect %d,%d %dx%d%s", m_player, state,
        seconds > 0 ? stats.sent / seconds : 0.0, stats.replaced, stats.copyMilliseconds, m_punching ? "on" : "off",
        geometry.x, geometry.y, geometry.width, geometry.height, geometry.visible ? "" : " hidden");
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
