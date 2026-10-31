# ffmpeg media engine

`MediaPlayerPrivateFFmpeg` is a WebCore media engine (`USE(FFMPEG)`) for the PS5 homebrew build, where upstream's PlayStation port has no media backend at all. It plays `<video>`/`<audio>` from progressive files and HLS with software decoding on top of the payload SDK's static ffmpeg 7.0.1. `MediaPlayerPrivateFFmpegMSE` (`ffmpeg/mse/`, `ENABLE(MEDIA_SOURCE)`) plays Media Source Extensions content (YouTube, Twitch, VK and other DASH/HLS.js players) on the same decoders.

`ffmpeg/` is the source of truth. It reaches WebKit as `../patches/0900-ffmpeg-media-player.patch`, a regular patch that `scripts/ps5/fetch-webkit.sh` applies after 0001-0010: it adds the files as `Source/WebCore/platform/graphics/ffmpeg/` and wires them up. The `USE_FFMPEG` option itself is part of patch 0001, which owns `OptionsPlayStation.cmake`.

## Files

| File | Role |
|---|---|
| `MediaPlayerPrivateFFmpeg.{h,cpp}` | `MediaPlayerPrivateInterface` implementation, factory and `supportsType`, state tick, HTML state machine, painting |
| `FFmpegVideoPresenter.{h,cpp}` | presenter thread of both players (frames to the overlay or a repaint) and `FFmpegPlaybackClock`, the A/V clock behind a lock |
| `FFmpegDemuxer.{h,cpp}` | demuxer thread: `avformat_open_input`/`av_read_frame`, stream selection, per-stream packet queues, seeks, buffered ranges |
| `FFmpegResourceIO.{h,cpp}` | `AVIOContext` over WebCore's `PlatformMediaResourceLoader` (also installed as `AVFormatContext::io_open` for HLS playlists, keys and segments) |
| `FFmpegVideoDecoder.{h,cpp}` | video decoder thread: libavcodec, libswscale to BGRA, frame pool, output queue |
| `FFmpegAudioDecoder.{h,cpp}` | audio decoder thread: libavcodec, libswresample to float32 stereo 48 kHz, ring buffer, audio clock |
| `AudioSink.{h,cpp}` | output: `RemoteAudioSink` (forwards to the UI process), `SDL_OpenAudioDevice` (SDL's PS5 driver is sceAudioOut) or `NullAudioSink` |
| `FFmpegAudioWire.h`, `FFmpegRemoteAudioClient.h` | audio forwarding protocol, client clock and socket client (no WebCore/WTF; also used by the UI's server in `src/ps5/audio` and by `tests/ps5`) |
| `FFmpegVideoOverlay.{h,cpp}` | video overlay of both players: the element's video box in view coordinates, the hole-punch decision, frames to the client |
| `FFmpegVideoOverlayWire.h`, `FFmpegVideoOverlayClient.h` | video overlay protocol, shared-memory layout and seqlock, sender thread with transport negotiation (no WebCore/WTF; also used by the UI's server in `src/ps5/video` and by `tests/ps5`) |
| `FFmpegUtilities.h` | ffmpeg RAII types, `FFmpegStreamInfo`, `findSoftwareDecoder()` |
| `FFmpegSupportTable.h`, `FFmpegTimeRanges.h`, `FFmpegAVClock.h`, `FFmpegRingBuffer.h`, `FFmpegPlaybackPolicy.h`, `FFmpegPresenterPolicy.h` | engine-independent logic (no WebCore/WTF/ffmpeg), unit-tested on the host by `tests/media/` |
| `mse/MediaPlayerPrivateFFmpegMSE.{h,cpp}` | MSE engine (`MediaEngineIdentifier::FFmpegMSE`): decoder pipelines per selected track, seeks, readyState gating, painting |
| `mse/MediaSourcePrivateFFmpeg.{h,cpp}`, `mse/SourceBufferPrivateFFmpeg.{h,cpp}` | `MediaSourcePrivate`/`SourceBufferPrivate` subclasses on top of the generic WebCore sample maps |
| `mse/FFmpegAppendParser.{h,cpp}` | appended bytes to segments to libavformat (`mov`/`matroska`, from memory) to packets; ffmpeg and the C++ library only |
| `mse/MediaSampleFFmpeg.h` | `MediaSample` around a shared `AVPacket`, track descriptions, codec parameters per initialization segment |
| `mse/FFmpegMSEParsing.h`, `mse/FFmpegMSEPolicy.h` | engine-independent: byte queue, MP4/WebM segment scanner, timestamp helpers, queue feeding, readyState and stall rules (host-tested) |
| `../FFmpeg.cmake` | becomes `Source/WebCore/platform/FFmpeg.cmake`: sources, include dirs, libraries |

## Design

### Threads

| Thread | Work |
|---|---|
| main | `MediaPlayerPrivateFFmpeg`. A `RunLoop::Timer` state tick (250 ms while playing, 50 ms while seeking, 100 ms when idle, and at once when the presenter reports the first frame of a position or the end of the video) finishes seeks, detects the end, and updates `buffered`/`readyState`/`networkState`. A second timer refreshes the video overlay's geometry every 33 ms. The main thread also runs the `PlatformMediaResourceLoader` requests and their client callbacks. |
| `FFmpegPresenter` | one per video pipeline (`FFmpegVideoPresenter.h`): reads the clock, takes the due frame from the decoder and hands it to the video overlay, then sleeps until the next frame is due (1-10 ms). The main thread only gets coalesced `callOnMainThread` events: a repaint when the page paints the video, the first frame of a position. |
| `FFmpegDemuxer` | opens the input, then loops `av_read_frame` into the queues; handles seek requests; waits when the queues are full |
| `FFmpegVideoDecoder` | packets → frames → BGRA |
| `FFmpegAudioDecoder` | packets → float samples → ring buffer |
| audio sink | the remote sink's writer thread (5 ms pump), SDL's audio callback thread, or the null sink's 10 ms timer thread |

The main thread never blocks on a worker, except to join threads on teardown, and every worker wait polls an abort flag (at most 50 ms). Teardown order: sink, then demuxer (which aborts the packet queues), then decoders.

### Data flow and serials

The demuxer keeps one `FFmpegPacketQueue` each for the selected video and audio stream. A seek increments the player's serial. The decoders learn the seek target for that serial, the demuxer seeks (`avformat_seek_file` to the keyframe at or before the target) and flushes both queues to the new serial, and every packet and frame carries its serial. Each decoder flushes its codec when it sees a new serial. Anything older is dropped: queued packets, decoded frames, ring-buffer audio. Decoders discard output before the target, so seeks are accurate. The seek promise resolves once a video frame of the new serial is shown (or video ended) and audio of the new serial is buffered (or audio ended). The player then calls `timeChanged()`.

Demuxer read-ahead stops at 24 MiB in total, or when every enabled stream has at least 10 s (30 s for HLS, whose segment servers stall for many seconds) and 25 packets queued (`FFmpegPlaybackPolicy.h`). Separately, each network stream suspends its request above 8 MiB of unread data and resumes it with a `Range` request below 2 MiB, also when the size is unknown. A server without `Range` support is suspended above 32 MiB and resumed by restarting the download and skipping to the position (`networkShouldSuspend` in `FFmpegPlaybackPolicy.h`).

A request that delivers no bytes for 8 s while the demuxer waits for them, fails with a network error, or answers 5xx, 408 or 429, is cancelled and issued again from the current offset (a `Range` request from the end of the received data, the segment's byte range for `EXT-X-BYTERANGE`), after 0.5, 1 and 2 s; after the third retry the stream fails as before (`NetworkRetry` in `FFmpegPlaybackPolicy.h`). Arriving bytes reset the count. This covers HLS playlists, segments and keys as well, since each is one of these streams. The log shows each decision: `net: stall 8s on <host> at byte N, retry 1/3 in 0.5 s`, `net: HTTP 503 ... retry 2/3`, `net: retry 1/3: requesting <host> from byte N`, `net: retry 1/3: HTTP 206 from <host>`, `net: <host> recovered at byte N after 1 retries`, or `..., giving up after 3 retries`.

### Audio output

Only the foreground application is heard on the PS5, and the WebProcess is a plain payload: SDL opens sceAudioOut there and drains it, silently. `AudioSink::create()` therefore tries, in order:

1. **`remote`**: `RemoteAudioSink` connects to the UI process's audio server on `127.0.0.1:$PROSPEROWEB_AUDIO_PORT` (17772; the UI exports the variable, and it is also the default on PlayStation) within 300 ms. Its writer thread pulls 10 ms chunks from the decoder's ring whenever less than 100 ms (`PROSPEROWEB_AUDIO_AHEAD_MS`) is queued on the server, and sends them with pause/resume, flush (on seek) and close messages. The server mixes every stream into one SDL device and reports, every 20 ms, how many frames of the stream it retired (played, or dropped by a flush or an overflow) and how many are queued. Between reports the position extrapolates in real time for at most 100 ms, so a stalled server stalls the clock and the watchdog below switches to the null sink. If the connection drops, the sink keeps pulling in real time without sound.
2. **`sdl`**: `SDLAudioSink`, an output in the WebProcess itself (audible on desktop builds, not on the console).
3. **`null`**: `NullAudioSink`.

`PROSPEROWEB_MEDIA_AUDIO=remote|sdl|null` picks the first one tried; failures fall through in the same order. The protocol is in `FFmpegAudioWire.h`; the server is `src/ps5/audio` plus `src/frontend/ps5/ps5_audio_server.cpp`; see `docs/ps5-runtime.md` ("Audio path").

### Clock

`FFmpegAVClock.h`. Audio is the master when there is an audio stream, the sink is running and the rate is 1. The clock is the base pts of the first sample written since the last flush, plus the frames the sink consumed, divided by 48000, minus the sink latency: two SDL buffers for the local sink; for the remote sink, the frames sent but not yet retired by the server plus the server's two SDL buffers. The audio decoder keeps that timeline linear: it fills timestamp gaps with silence and trims to the seek target. In every other case the clock extrapolates monotonic time times the rate from the last anchor: no audio, audio ended before video, playback rate other than 1, or the silent sink after a failure. The extrapolated clock stalls while less than 0.1 s of data is demuxed ahead (buffering). The clock never goes backwards except across a seek.

Video frames are presented by the presenter thread, not the main thread: on the console, YouTube's page repaints starved an 8 ms main-thread tick to about 10 frames per second while 26 were decoded. Each frame waits until its pts is reached (5 ms early tolerance). A due frame is shown even when late, unless it is more than 1.5 frame intervals late and a newer frame is already decoded; then it is dropped (`FFmpegPresenterPolicy.h`). While paused or seeking, the first frame at the new position is shown at once. The presenter logs `presenter: due N shown N late-dropped N max lateness X ms` every 5 s while frames are due (` [mse]` for the MSE player). The clock (`FFmpegPlaybackClock`) is the `AVClock` behind a lock with the audio decoder and sink attached, so the presenter and `currentTime()` read the same position. The decoder drops frames that are already 80 ms behind the clock before running swscale, still showing one in eight. After four consecutive late frames it turns on `skip_loop_filter` (no deblocking) until it catches up.

At rates other than 1 there is no time-stretching. The audio decoder discards samples, paced against the clock, and the sink plays silence. When the rate returns to 1, the ring buffer resynchronises at the next decoded frame.

### HTML states

`FFmpegPlaybackPolicy.h` mirrors what the other engines do:

- **`readyState`**: `HaveNothing` until the streams are known. `HaveMetadata` once the duration and natural size are known, and again while seeking or until data exists for the current position. That data is a decoded video frame, or audio for an audio-only resource or a hidden page. Then `HaveCurrentData`, `HaveFutureData` from 0.3 s buffered ahead, and `HaveEnoughData` from 3 s ahead or at the end of the stream.
- **`networkState`**: `Loading` while reading. `Idle` while the demuxer waits on full queues, or before loading when `preload="none"` defers it until `prepareToPlay()`/`play()`. `Loaded` at end of stream. `FormatError` when nothing playable could be opened, which covers a failed fetch before metadata. `NetworkError` after metadata. `DecodeError` when every decoder failed.

`didLoadingProgress()` reports whether the byte count received through the loader changed since the last call. `buffered()` holds the time ranges the demuxer has actually demuxed for the primary stream (video, otherwise audio), merged across seeks.

### Page visibility

`setPageIsVisible(false)` stops video decoding and conversion; audio keeps playing. The video decoder drops packets, and after the page becomes visible again it resumes at the next keyframe.

### Video output

Frames are opaque BGRA from swscale, in `av_malloc` blocks whose rows are padded to 64 bytes plus a 256-byte tail (`videoFrameStride`/`videoFrameBytes` in `FFmpegPlaybackPolicy.h`): libswscale's x86 output loops store 8 pixels per iteration, and a tightly packed buffer of a width that is not a multiple of 8 was overrun after its last row, into the allocator's metadata. The colour matrix comes from the stream: BT.601, BT.709 or BT.2020, with full or limited range. Output is capped at 1920x1080, and the sample aspect ratio is applied. `paint()` and `nativeImageForCurrentTime()` wrap the current frame without copying, through `SkImages::RasterFromPixmap`. The `SkImage` holds a reference to the frame, and the decoder only reuses pool frames nobody else references. Rendering is software only: no `platformLayer()`, no accelerated compositing.

On PlayStation the frames normally bypass page painting (the video overlay, `FFmpegVideoOverlay.h`): `paint()` clears the video box to transparent while the UI process shows the frames under the page, and each presented frame is handed by the presenter thread to a per-player sender thread, which copies it into a shared-memory slot (or sends it inline), instead of repainting the element. With the overlay off the presenter asks the main thread for one repaint per shown frame. `paintCurrentFrameInContext()` (canvas, snapshots) always draws the frame. `PROSPEROWEB_VIDEO_OVERLAY=0` restores per-frame page painting; see `docs/ps5-runtime.md` ("Video overlay").

## Support

| | |
|---|---|
| Containers / MIME | MP4 (`video/mp4`, `audio/mp4`, `video/x-m4v`, `audio/x-m4a`, `video/quicktime`), WebM/Matroska, HLS (`application/vnd.apple.mpegurl`, `application/x-mpegURL`, `audio/mpegurl`, `audio/x-mpegurl`), `audio/mpeg`, Ogg, FLAC, WAV, ADTS AAC |
| Video | H.264 (`avc1`/`avc3`), HEVC (`hvc1`/`hev1`), VP8, VP9 (`vp8`, `vp9`, `vp09.*`) |
| Audio | AAC (`mp4a.40.*`), MP3, Opus, Vorbis, FLAC, AC-3, E-AC-3 |
| AV1 | **not supported**: the SDK ffmpeg has no libdav1d/libaom, and `ff_av1_decoder` only drives hardware accelerators. `av01` answers "" unless a software AV1 decoder is linked; `findSoftwareDecoder()` checks at run time. |
| `canPlayType` | "" for unknown containers, codecs this build cannot decode, or codecs that are impossible in the container; "maybe" with no `codecs` parameter or with an unrecognised codec string; "probably" when every codec is known and decodable |
| HLS | through libavformat's `hls` demuxer: master playlists (a fixed variant: the highest bit rate at or below 1080p, **no adaptive switching**), VOD with seeking, live (duration +inf, not seekable), AES-128 segments (see the risks), `EXT-X-BYTERANGE` segments (the byte range becomes the request's `Range`). FairPlay (`SAMPLE-AES` with an `skd://` key, which is what Vimeo serves a Safari user agent) is refused with `MEDIA_ERR_SRC_NOT_SUPPORTED` instead of decoding garbage. |

Not supported: EME and DRM (`cryptoblockformat`, encrypted content), WebAudio (`audioSourceProvider`; `ENABLE_WEB_AUDIO` is off on PlayStation), hardware decoding (the SDK has no video decoder stubs), subtitle and text tracks, `AudioTrack`/`VideoTrack` lists, `preservesPitch`, picture-in-picture, and multichannel output (everything is downmixed to stereo).

## Media Source Extensions

`ENABLE_MEDIA_SOURCE` is on for `PS5_HOMEBREW` (patch 0001, `scripts/ps5/build-webkit.sh`). `MediaPlayer` picks `MediaPlayerPrivateFFmpegMSE` for every `MediaSource` load (its `supportsType` answers "maybe" to the empty type of a `blob:` MediaSource URL); the progressive engine refuses MediaSource loads.

### Appends

`SourceBufferPrivateFFmpeg::appendInternal` runs synchronously on the main thread (the default `SourceBufferPrivate` dispatcher):

1. `SegmentScanner` (`FFmpegMSEParsing.h`) cuts the bytes at top-level boundaries and keeps partial data for the next append. MP4: `ftyp`+`moov` is an initialization segment, each `moof`+`mdat` pair is media, and `styp`, `sidx`, `emsg`, `prft`, `free` are dropped. WebM: EBML header + `Segment` header + `Info` + `Tracks` is the initialization segment (`SeekHead`, `Void`, `Cues`, `Tags` are dropped, so no stale offsets reach the demuxer), and `Cluster`s of known or unknown size are media. A media segment before any initialization segment, or a malformed box, rejects the append (`ParsingError`, the DOM's append error).
2. `FFmpegAppendParser` opens each initialization segment with libavformat (`mov`, or `matroska` with an empty `Cluster` appended so its header parser stops) to describe the tracks. It demuxes the media segments of an append in one go from memory, together with the current initialization segment, so libavformat always sees a complete file and never blocks waiting for bytes. Missing packet durations (Matroska blocks without `BlockDuration`) are filled from the next packet's decode time.
3. Tracks become `InitializationSegment` entries, the codec as a four-character code (`avc1`, `hvc1`, `vp09`, `mp4a`, `opus`, ...). Profiles are left out, so an H.264 Main/High switch is not a codec change. Packets become `MediaSampleFFmpeg`s (shared `AVPacket`, exact `MediaTime`s in the stream timebase, sync flag) passed to `didReceiveSample`.

Everything after that is the generic `SourceBufferPrivate`/`TrackBuffer` code: sample maps, `timestampOffset`, append windows, sequence mode, `remove()`, coded frame eviction and `QuotaExceededError`. The quota (`platformMaximumBufferSize`) is 64 MiB for a buffer with video and 8 MiB for audio only (`FFmpegMSEPolicy.h`). `abort()` drops partial data and keeps the last initialization segment.

### Playback

The first sample the generic code enqueues for a video (audio) track binds that SourceBuffer track to the player's video (audio) pipeline: an `FFmpegPacketQueue` in a 1/1000000 timebase and the same `FFmpegVideoDecoder`/`FFmpegAudioDecoder` as progressive playback, plus the audio sink. Samples of other tracks of the same type are dropped. Each pipeline has its own serial; the presenter, clock (`FFmpegAVClock.h`), audio sink, watchdog and painting (`MediaPlayerPrivateFFmpeg::createNativeImage`) work as for progressive media.

- **Feeding**: a track is "ready for more samples" until its queue holds 1.5 s or 240 packets, and is asked again (`provideMediaData`) below 0.75 s.
- **Configuration changes**: a sample from a new initialization segment with different extradata (YouTube quality switches bring a new `avcC`/`esds`) carries `AV_PKT_DATA_NEW_EXTRADATA`, and the h264 and aac decoders reconfigure themselves. The decoder's per-frame scaler setup handles the new size. A different codec (`changeType` between codecs) is logged and not supported.
- **Seeks**: `seekToTarget` flushes both pipelines at once, waits for `MediaSourcePrivate::waitForTarget` (data at the target), flushes again at the resolved time and calls `reenqueueMediaForTime`, which re-enqueues from the preceding sync sample. Decoders trim output before the target. The seek completes when the first frame of the new serial is shown and audio of the new serial is decoded, or after 3 s (logged).
- **readyState**: the MediaSource computes it from buffered ranges (`monitorSourceBuffers`). The player reports it, but holds `HAVE_METADATA` while seeking and until the frame at the position is on screen (or decoded audio exists, for audio-only media and hidden pages), like the GStreamer MSE player waiting for preroll. When the buffer runs out, the MediaSource lowers readyState and `HTMLMediaElement` pauses the engine (`waiting`) until appends bring it back. The extrapolated clock also holds when less than 0.1 s is buffered ahead.
- **End**: after `endOfStream()` the generic code signals `allSamplesInTrackEnqueued`, the pipeline queues an end of stream, and playback ends when the decoders drained (or the position passed the duration).
- `buffered()` is empty (the element asks the MediaSource), `didLoadingProgress()` follows appended bytes, and `getVideoPlaybackQuality()` reports decoded and dropped frames (YouTube lowers the quality on drops).

### Supported types

`MediaSource.isTypeSupported()` and `addSourceBuffer()` use `FFmpegMedia::evaluateMediaSource`:

| | |
|---|---|
| Byte streams | `video/mp4`, `audio/mp4` (fragmented ISO BMFF), `video/webm`, `audio/webm` |
| Codecs | H.264, HEVC, AAC (LC, HE, HE v2), Opus, Vorbis, FLAC, AC-3, E-AC-3, as far as the linked libavcodec has software decoders; VP8/VP9 only with `PROSPEROWEB_MEDIA_VP9=1` (also for `canPlayType`: software VP9 480p could not keep up on the console, so players are steered to H.264); AV1 only with libdav1d/libaom (not in the SDK); video codecs in `audio/*` are refused |
| Parameters | "" for sizes above 1080p (`width`/`height` above 1920, or both above 1088 on the shorter side; `PROSPEROWEB_MEDIA_MAX_VIDEO=3840x2160` raises it), `framerate` above 120, `channels` above 8, non-numeric values, `eotf` other than `bt709` (no HDR, so `smpte2084` and `arib-std-b67` are refused), `decode-to-texture` other than `true`/`false`, `experimental` other than `allowed`/`true`/`false`, `tunnelmode=true`, and any `cryptoblockformat` (no EME) |

The parameter rules apply to `canPlayType()` as well. YouTube probes them (`width=99999`, `framerate=9999`, `eotf=catavision`, ...) to decide whether the browser's answers can be trusted, then picks H.264 (MP4) with AAC or Opus (VP9 when enabled).

### MSE risks

- **Software decoding**: the console log of the first YouTube run showed VP9 854x480 dropping a third of its frames, hence H.264 and 1080p by default.
- **Memory**: per SourceBuffer up to 64 MiB (video) or 8 MiB (audio) of samples, plus about 1.5 s in the decoder queues and the frame pool. Each append copies its media segments once more while demuxing.
- **Untested on hardware** at the time of writing; see the `mse:` log lines.

## Risks

- **Audio from the WebProcess.** Verified on hardware: sceAudioOut opens in the WebProcess and is drained in real time, but nothing is heard, because only the foreground application (the UI) is mixed to the output. Audio is therefore forwarded to the UI (see "Audio output"). Costs and limits: about 100 ms of queued audio plus 43 ms of SDL buffering between decoding and hearing (the clock compensates, so A/V sync holds, but a seek or play takes that long to become audible); each media element is one connection and one mixer stream, so several playing elements are mixed, not ducked; and anything that stalls the UI's server thread for more than the queued 100 ms causes an underrun. A watchdog still switches to `NullAudioSink` when an output stops advancing for 2 s while audio is buffered. In every case video still plays in sync.
- **HTTP path.** Media goes through WebCore's loader, which handles cookies, the referrer, CORS (`didPassCORSAccessCheck` for canvas tainting), CSP, `blob:` and the shared NetworkProcess. Byte ranges map to `Range` requests. Servers that ignore `Range` still work but restart the download for every seek and for every resume after the 32 MiB read-ahead cap, which re-downloads everything before the position. Exceptions:
  - HLS AES-128 segments are opened as `crypto+https://…`. They go through ffmpeg's own http/tls (OpenSSL from the SDK), which sends the engine's user agent and **no cookies**. The key URI itself goes through WebCore.
  - Relative URLs in a playlist resolve against the playlist's original URL, not its redirected URL.
  - libavformat's HTTP keep-alive is disabled for HLS (`http_persistent=0`).
  - Any other scheme ffmpeg understands is opened by ffmpeg directly.
- **CPU.** This is software decoding on the PS5's Zen 2: 8 cores at 3.5 GHz, shared with the UI process, JavaScript and Skia painting.
  - 1080p30 H.264 is realistic with four decoder threads.
  - 1080p60 H.264 at high bit rates is borderline, and HEVC/VP9 1080p60 likely is not. Expect frame dropping and the `skip_loop_filter` fallback.
  - On top of decoding, per frame: swscale to BGRA and, with the video overlay, one copy into shared memory (the UI process scales it); without the overlay, Skia's scaled draw into the page's backing store and the WebKit IPC transfer to the UI process.
  - Anything above 1080p is downscaled after decoding, so it pays the full decode cost. HLS variant selection avoids it.
- **Memory.** Per player: at most 24 MiB of queued packets, 8 MiB of network read-ahead per open stream (32 MiB for a server without `Range` support), a pool of up to 5 BGRA frames (8 MiB each at 1080p; the queue holds 3 of them) and a 1 s audio ring. Video decoding uses slice threads only (`PROSPEROWEB_MEDIA_FRAME_THREADS=1` turns frame threading back on, which keeps one extra picture per thread).
- **Timestamps.** A live HLS discontinuity with a large forward timestamp jump inserts at most 2 s of silence. The audio and video timelines can then disagree until the next seek.
- **Licensing.** The SDK's ffmpeg is linked with x264, so the combination is GPL. That matches ProsperoWeb's GPL-3.0-or-later, but the binary must be distributed under GPL terms.

## Integration

The engine is built by default (`USE_FFMPEG=ON` in `scripts/ps5/build-webkit.sh`; `USE_FFMPEG=OFF scripts/ps5/build-webkit.sh configure` turns it off). After editing `ffmpeg/` or `FFmpeg.cmake`:

```sh
webkit-port/media/sync-to-tree.sh            # copy ffmpeg/, ffmpeg/mse/ + FFmpeg.cmake into third_party/webkit,
                                             # wire the engines, then scripts/ps5/export-webkit-patches.sh
scripts/ps5/build-webkit.sh webcore          # then: webkit, package
```

`export-webkit-patches.sh` owns patch 0900 like every other patch (topic `0900-ffmpeg-media-player`; its file list follows `ffmpeg/` and `ffmpeg/mse/`), rewrites it from the tree and updates the patch-set stamp, so `fetch-webkit.sh` stays consistent. `sync-to-tree.sh --patch` / `--apply` only bootstrap the patch from a tree that does not contain the engine yet (for example after moving to a new WebKit commit). The wiring edits are idempotent, so `sync-to-tree.sh` also upgrades a tree that has an older wiring. `compile-check.sh` works in both situations (see Tests).

What the patches change:

- `OptionsPlayStation.cmake` (patch 0001): `WEBKIT_OPTION_DEFINE(USE_FFMPEG … PRIVATE ${PS5_HOMEBREW})`, which depends on `ENABLE_VIDEO`. `USE_FFMPEG` reaches `cmakeconfig.h`, so `USE(FFMPEG)` works without an extra `-D`. `ENABLE_MEDIA_SOURCE` defaults on for `PS5_HOMEBREW` (and `build-webkit.sh` passes `ENABLE_MEDIA_SOURCE=ON`; `ENABLE_MEDIA_SOURCE=OFF scripts/ps5/build-webkit.sh configure` turns it off).
- `MediaSourcePrivate.h` (patch 0017): drops a `WEBCORE_EXPORT` on a member of the exported class, an error where the macro is `dllexport`.
- `PlatformPlayStation.cmake`: `include(platform/FFmpeg.cmake)`. That file adds the sources and the include directory, finds the ffmpeg archives with `find_library` (`pkg_check_modules` is a no-op on PlayStation), and defines `FFMPEG_MEDIA_SDL_AUDIO=1` when `SDL2/SDL.h` and `libSDL2.a` exist.
- `MediaPlayer.cpp`: `MediaPlayerPrivateFFmpeg::registerMediaEngine` under `USE(FFMPEG)`, and `MediaPlayerPrivateFFmpegMSE::registerMediaEngine` with `ENABLE(MEDIA_SOURCE)`.
- New enum values `FFmpeg` and `FFmpegMSE` in `MediaPlayerType` (`MediaPlayer.h`), `MediaPlayerMediaEngineIdentifier` (`MediaPlayerEnums.h`) and its IPC serialization (`WebCoreArgumentCoders.serialization.in`), plus the exhaustive switch in `MediaPlayerPrivateRemote.cpp`; `FFmpeg` in `MediaPlatformType` (for the `MediaSourcePrivate`/`SourceBufferPrivate` type casts).

### Link line

`libWebKitPS5.a` is static, so the final `prosperoweb.elf` link needs ffmpeg's own static dependencies after the WebKit archives. `scripts/ps5/build-webkit.sh` adds them to the link contract (`link_libs`, exported as `build-wk-ps5/ps5webkit-link.cmake` and consumed by `src/frontend/ps5/CMakeLists.txt`) whenever `cmakeconfig.h` has `USE_FFMPEG`. They come from `scripts/ps5/pkg-config --static --libs libavformat libavcodec libswscale libswresample libavutil`:

```
-lavformat -lbz2 -lavcodec -liconv -llzma -lx264 -lpthread -lz -lswscale -lswresample -lavutil -lm -lssl -lcrypto -pthread
```

That is libavformat, libavcodec, libswscale, libswresample and libavutil, plus bzip2, iconv, liblzma, x264 (GPL), zlib and OpenSSL. libass is not linked. SDL2 and its dependencies (`-lSDL2 -lSceSystemService -lSceUserService -lScePad -lSceAudioOut -lsamplerate -lSceVideoOut -lSceKeyboard -lSceImeDialog`) are part of the same group, for the audio sink. OpenSSL, zlib and iconv are the same SDK archives curl and libxml2 use; the link has no duplicate definitions.

## Console log

On the PS5 the engine writes `media: ...` lines to `/data/prosperoweb/log.txt` through the shell's `ps5_log_line()` (declared weak in `FFmpegLog.h`, so other builds link without it): `supportsType` answers (each distinct query once), `load` with scheme and host, the demuxer's format, duration and every stream (the selected ones marked `[VIDEO]`/`[AUDIO]`), decoder and audio sink results (the remote connection and its stream id, SDL driver, requested and obtained spec, or the failure and the fallback), the first video frame, `readyState`/`networkState` transitions, seeks (start, demuxer seek and flush, first frame after the seek, finish), `Range` requests and HTTP errors, `cancelLoad`, and a `stats:` line every 5 s while playing or seeking (decoded/dropped frames, queue depths, audio clock against the video pts).

The MSE engine logs `mse:` lines: `addSourceBuffer <type> -> #n`; `#n init segment #g (<mime>): video 1 h264 1280x720` or `audio 1 aac 48000 Hz 2ch`; `#n append N bytes -> M samples (a-b), buffer X KiB of Q KiB` (the first eight appends of a buffer, then every 20th); `#n evicted: buffer X KiB -> Y KiB`; `video|audio pipeline: SourceBuffer #n track t <codec>`; `configuration change`; `seek to`, `seek data at ... available after`, `seek finished`; `stalled at`/`resumed at`; `readyState`; `endOfStream`; `ended`; failed appends with the parser error; and `mse stats:` every 5 s while playing.

## Tests

`tests/media/` (doctest, host build) covers the engine-independent headers: the MIME/codec table, time-range merging, clock maths and frame decisions, the ring buffer, the read-ahead/`readyState`/`networkState` policies, and for MSE the `isTypeSupported` answers to YouTube-style probes, the byte queue's read/seek semantics, MP4/WebM segment splitting across arbitrary append boundaries, timestamp conversion, duration filling, queue feeding and the readyState/stall rules. `compile-check.sh` checks the sources against the real WebCore flags before a full build; on an integrated tree it compiles `ffmpeg/` and `ffmpeg/mse/` directly with `ENABLE_MEDIA_SOURCE` forced on. The "Load MSE (fMP4)" button of `src/chrome-ui/video.html` plays DASH test content through two SourceBuffers and logs each step on the page. `tests/ps5/test_audio_server.cpp` covers the audio forwarding: wire framing, the client clock, the UI mixer, and a loopback run of `RemoteAudioClient` against the server (`ctest --test-dir build-host -R audio`). `tests/ps5/test_video_overlay.cpp` covers the video overlay: wire framing, the slot seqlock, the UI's scaler and composition, and loopback runs of `VideoOverlayClient` against the server over each transport (`ctest --test-dir build-host -R video`).

```sh
cmake -S . -B build-host && cmake --build build-host -j2 --target prosperoweb_media_tests
ctest --test-dir build-host -R media
```
