// SPDX-License-Identifier: GPL-3.0-or-later
//
// Video overlay of the ffmpeg media engine ("hole punch"). Accelerated
// compositing is off in the PS5 port, so painting a video frame meant Skia
// scaling it into the page's backing store, WebKit IPC to the UI process and
// two more copies there, for every frame. With the overlay, the element
// paints a transparent rect instead (once, not per frame), and each frame
// goes to the UI process's overlay server (FFmpegVideoOverlayClient.h), which
// composes it under the page: whatever the page draws over the video
// (controls, captions) stays on top.
//
// A player owns one FFmpegVideoOverlay when overlays are enabled (default
// on PlayStation; PROSPEROWEB_VIDEO_OVERLAY=0 turns them off, =1 on
// elsewhere). The player's tick calls update(): it reads the element's video
// box in view coordinates (layout, scrolling, fullscreen) and decides whether
// the hole is punched. It is not when the overlay cannot show the video as
// the page would: not connected, a rotated/skewed box, a hidden page or
// element, or no frame yet. In that case paint() draws the frame as before.
//
// Main thread only, except submit() and punchesHole(), which the player's
// presenter thread (FFmpegVideoPresenter.h) calls; frames are copied and
// sent on the client's sender thread.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegVideoOverlayWire.h"
#include <atomic>
#include <memory>
#include <string>
#include <optional>
#include <wtf/TZoneMalloc.h>

namespace WebCore {

class FFmpegVideoFrame;
class MediaPlayer;

namespace FFmpegMedia {
namespace VideoWire {
class RemoteVideoStream;
class VideoOverlayClient;
}
}

class FFmpegVideoOverlay {
    WTF_MAKE_TZONE_ALLOCATED(FFmpegVideoOverlay);
public:
    // nullptr when overlays are disabled.
    static std::unique_ptr<FFmpegVideoOverlay> createIfEnabled();
    ~FFmpegVideoOverlay();

    // Refreshes the geometry (sent to the UI when it changed) and the
    // hole-punch decision. Returns true when the decision changed: the
    // caller repaints the element so the hole appears or goes away.
    bool update(MediaPlayer&, bool pageVisible, bool hasFrame);
    // paint() clears the video box instead of drawing the frame.
    bool punchesHole() const { return m_punching; }
    // The frame to show now (the player's A/V clock decided). Any thread.
    // A remotely decoded frame is named to the UI by its pts.
    void submit(FFmpegVideoFrame&);
    // Asks the UI to decode a stream (FFmpegVideoDecoder's remote mode).
    // Any thread.
    std::shared_ptr<FFmpegMedia::VideoWire::RemoteVideoStream> openRemoteStream(const FFmpegMedia::VideoWire::StreamOpen&);
    // The picture is gone (new load, decoder reset).
    void clear();
    // The page was hidden or shown: the next update() reports the video box
    // again even when it did not move (the UI resumes presenting on it).
    void invalidateGeometry() { m_geometry = std::nullopt; }
    // One "video overlay: player N: <transport> sent N/s ..." line covering
    // the last `seconds` (the player's stats interval).
    void logStats(double seconds);

private:
    FFmpegVideoOverlay(unsigned player, int port, int controlFd);

    std::unique_ptr<FFmpegMedia::VideoWire::VideoOverlayClient> m_client;
    unsigned m_player;
    std::optional<FFmpegMedia::VideoWire::Geometry> m_geometry;
    bool m_rectilinear { true };
    std::atomic<bool> m_punching { false };
    // The geometry is visible: submitted frames are sent.
    std::atomic<bool> m_acceptsFrames { false };
    bool m_loggedFailure { false };
    bool m_loggedSkew { false };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
