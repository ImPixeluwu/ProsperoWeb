# ffmpeg media engine

`MediaPlayerPrivateFFmpeg` is a WebCore media engine (`USE(FFMPEG)`) for the PS5 homebrew build, where upstream's PlayStation port has no media backend at all. It plays `<video>`/`<audio>` from progressive files and HLS with software decoding on top of the payload SDK's static ffmpeg 7.0.1.

`ffmpeg/` is the source of truth. It reaches WebKit as `../patches/0900-ffmpeg-media-player.patch`, a regular patch that `scripts/ps5/fetch-webkit.sh` applies after 0001-0010: it adds the files as `Source/WebCore/platform/graphics/ffmpeg/` and wires them up. The `USE_FFMPEG` option itself is part of patch 0001, which owns `OptionsPlayStation.cmake`.

## Files

| File | Role |
|---|---|
| `MediaPlayerPrivateFFmpeg.{h,cpp}` | `MediaPlayerPrivateInterface` implementation, factory and `supportsType`, presentation tick, HTML state machine, painting |
| `FFmpegDemuxer.{h,cpp}` | demuxer thread: `avformat_open_input`/`av_read_frame`, stream selection, per-stream packet queues, seeks, buffered ranges |
| `FFmpegResourceIO.{h,cpp}` | `AVIOContext` over WebCore's `PlatformMediaResourceLoader` (also installed as `AVFormatContext::io_open` for HLS playlists, keys and segments) |
| `FFmpegVideoDecoder.{h,cpp}` | video decoder thread: libavcodec, libswscale to BGRA, frame pool, output queue |
| `FFmpegAudioDecoder.{h,cpp}` | audio decoder thread: libavcodec, libswresample to float32 stereo 48 kHz, ring buffer, audio clock |
| `AudioSink.{h,cpp}` | output: `SDL_OpenAudioDevice` (SDL's PS5 driver is sceAudioOut) or `NullAudioSink` |
| `FFmpegUtilities.h` | ffmpeg RAII types, `FFmpegStreamInfo`, `findSoftwareDecoder()` |
| `FFmpegSupportTable.h`, `FFmpegTimeRanges.h`, `FFmpegAVClock.h`, `FFmpegRingBuffer.h`, `FFmpegPlaybackPolicy.h` | engine-independent logic (no WebCore/WTF/ffmpeg), unit-tested on the host by `tests/media/` |
| `../FFmpeg.cmake` | becomes `Source/WebCore/platform/FFmpeg.cmake`: sources, include dirs, libraries |

## Design

### Threads

| Thread | Work |
|---|---|
| main | `MediaPlayerPrivateFFmpeg`. A `RunLoop::Timer` tick (8 ms while video plays and the page is visible, 50 ms for audio, 100 ms when idle) reads the clock, presents video frames, finishes seeks, detects the end, and updates `buffered`/`readyState`/`networkState`. It also runs the `PlatformMediaResourceLoader` requests and their client callbacks. |
| `FFmpegDemuxer` | opens the input, then loops `av_read_frame` into the queues; handles seek requests; waits when the queues are full |
| `FFmpegVideoDecoder` | packets → frames → BGRA |
| `FFmpegAudioDecoder` | packets → float samples → ring buffer |
| audio sink | SDL's audio callback thread, or the null sink's 10 ms timer thread |

The main thread never blocks on a worker, except to join threads on teardown, and every worker wait polls an abort flag (at most 50 ms). Teardown order: sink, then demuxer (which aborts the packet queues), then decoders.

### Data flow and serials

The demuxer keeps one `FFmpegPacketQueue` each for the selected video and audio stream. A seek increments the player's serial. The decoders learn the seek target for that serial, the demuxer seeks (`avformat_seek_file` to the keyframe at or before the target) and flushes both queues to the new serial, and every packet and frame carries its serial. Each decoder flushes its codec when it sees a new serial. Anything older is dropped: queued packets, decoded frames, ring-buffer audio. Decoders discard output before the target, so seeks are accurate. The seek promise resolves once a video frame of the new serial is shown (or video ended) and audio of the new serial is buffered (or audio ended). The player then calls `timeChanged()`.

Demuxer read-ahead stops at 48 MiB in total, or when every enabled stream has at least 15 s and 25 packets queued (`FFmpegPlaybackPolicy.h`). Separately, each network stream suspends its request above 16 MiB of unread data and resumes it with a `Range` request below 4 MiB, also when the size is unknown. A server without `Range` support is suspended above 64 MiB and resumed by restarting the download and skipping to the position (`networkShouldSuspend` in `FFmpegPlaybackPolicy.h`).

### Clock

`FFmpegAVClock.h`. Audio is the master when there is an audio stream, the sink is running and the rate is 1. The clock is the base pts of the first sample written since the last flush, plus the frames the sink consumed, divided by 48000, minus the sink latency (two SDL buffers). The audio decoder keeps that timeline linear: it fills timestamp gaps with silence and trims to the seek target. In every other case the clock extrapolates monotonic time times the rate from the last anchor: no audio, audio ended before video, playback rate other than 1, or the silent sink after a failure. The extrapolated clock stalls while less than 0.1 s of data is demuxed ahead (buffering). The clock never goes backwards except across a seek.

Each video frame waits until its pts is reached and is dropped if the next frame is also due. While paused or seeking, the first frame at the new position is shown at once. The decoder drops frames that are already 80 ms behind the clock before running swscale, still showing one in eight. After four consecutive late frames it turns on `skip_loop_filter` (no deblocking) until it catches up.

At rates other than 1 there is no time-stretching. The audio decoder discards samples, paced against the clock, and the sink plays silence. When the rate returns to 1, the ring buffer resynchronises at the next decoded frame.

### HTML states

`FFmpegPlaybackPolicy.h` mirrors what the other engines do:

- **`readyState`**: `HaveNothing` until the streams are known. `HaveMetadata` once the duration and natural size are known, and again while seeking or until data exists for the current position. That data is a decoded video frame, or audio for an audio-only resource or a hidden page. Then `HaveCurrentData`, `HaveFutureData` from 0.3 s buffered ahead, and `HaveEnoughData` from 3 s ahead or at the end of the stream.
- **`networkState`**: `Loading` while reading. `Idle` while the demuxer waits on full queues, or before loading when `preload="none"` defers it until `prepareToPlay()`/`play()`. `Loaded` at end of stream. `FormatError` when nothing playable could be opened, which covers a failed fetch before metadata. `NetworkError` after metadata. `DecodeError` when every decoder failed.

`didLoadingProgress()` reports whether the byte count received through the loader changed since the last call. `buffered()` holds the time ranges the demuxer has actually demuxed for the primary stream (video, otherwise audio), merged across seeks.

### Page visibility

`setPageIsVisible(false)` stops video decoding and conversion; audio keeps playing. The video decoder drops packets, and after the page becomes visible again it resumes at the next keyframe.

### Video output

Frames are opaque BGRA from swscale. The colour matrix comes from the stream: BT.601, BT.709 or BT.2020, with full or limited range. Output is capped at 1920x1080, and the sample aspect ratio is applied. `paint()` and `nativeImageForCurrentTime()` wrap the current frame without copying, through `SkImages::RasterFromPixmap`. The `SkImage` holds a reference to the frame, and the decoder only reuses pool frames nobody else references. Rendering is software only: no `platformLayer()`, no accelerated compositing.

## Support

| | |
|---|---|
| Containers / MIME | MP4 (`video/mp4`, `audio/mp4`, `video/x-m4v`, `audio/x-m4a`, `video/quicktime`), WebM/Matroska, HLS (`application/vnd.apple.mpegurl`, `application/x-mpegURL`, `audio/mpegurl`, `audio/x-mpegurl`), `audio/mpeg`, Ogg, FLAC, WAV, ADTS AAC |
| Video | H.264 (`avc1`/`avc3`), HEVC (`hvc1`/`hev1`), VP8, VP9 (`vp8`, `vp9`, `vp09.*`) |
| Audio | AAC (`mp4a.40.*`), MP3, Opus, Vorbis, FLAC, AC-3, E-AC-3 |
| AV1 | **not supported**: the SDK ffmpeg has no libdav1d/libaom, and `ff_av1_decoder` only drives hardware accelerators. `av01` answers "" unless a software AV1 decoder is linked; `findSoftwareDecoder()` checks at run time. |
| `canPlayType` | "" for unknown containers, codecs this build cannot decode, or codecs that are impossible in the container; "maybe" with no `codecs` parameter or with an unrecognised codec string; "probably" when every codec is known and decodable |
| HLS | through libavformat's `hls` demuxer: master playlists (a fixed variant: the highest bit rate at or below 1080p, **no adaptive switching**), VOD with seeking, live (duration +inf, not seekable), AES-128 segments (see the risks) |

Not supported: MSE (`supportsType` rejects `MediaSource`; the decoders, queues, clock and policy only depend on packet queues, so a `SourceBufferPrivate` that demuxes appended fMP4/WebM into the same queues can reuse them), EME and DRM, WebAudio (`audioSourceProvider`; `ENABLE_WEB_AUDIO` is off on PlayStation), hardware decoding (the SDK has no video decoder stubs), subtitle and text tracks, `AudioTrack`/`VideoTrack` lists, `preservesPitch`, picture-in-picture, and multichannel output (everything is downmixed to stereo).

## Risks

- **Audio from the WebProcess.** WebKit decodes in the WebProcess, a separate payload process spawned by the shell. It is unverified on hardware whether sceAudioOut, behind SDL's PS5 driver, can be opened from that process, and whether it can share the audio ports with the UI process. `AudioSink::create()` falls back to `NullAudioSink` when SDL fails, and so does a watchdog: an output that stops pulling samples for 2 s while audio is buffered. `PS5BROWSER_MEDIA_AUDIO=null` forces the null sink. In every case video still plays in sync, without sound. If the WebProcess cannot output audio at all, the fix is to forward PCM to the UI process over IPC.
- **HTTP path.** Media goes through WebCore's loader, which handles cookies, the referrer, CORS (`didPassCORSAccessCheck` for canvas tainting), CSP, `blob:` and the shared NetworkProcess. Byte ranges map to `Range` requests. Servers that ignore `Range` still work but restart the download for every seek and for every resume after the 64 MiB read-ahead cap, which re-downloads everything before the position. Exceptions:
  - HLS AES-128 segments are opened as `crypto+https://…`. They go through ffmpeg's own http/tls (OpenSSL from the SDK), which sends the engine's user agent and **no cookies**. The key URI itself goes through WebCore.
  - Relative URLs in a playlist resolve against the playlist's original URL, not its redirected URL.
  - libavformat's HTTP keep-alive is disabled for HLS (`http_persistent=0`).
  - Any other scheme ffmpeg understands is opened by ffmpeg directly.
- **CPU.** This is software decoding on the PS5's Zen 2: 8 cores at 3.5 GHz, shared with the UI process, JavaScript and Skia painting.
  - 1080p30 H.264 is realistic with four decoder threads.
  - 1080p60 H.264 at high bit rates is borderline, and HEVC/VP9 1080p60 likely is not. Expect frame dropping and the `skip_loop_filter` fallback.
  - On top of decoding, per frame: swscale to BGRA, the copy into the page's backing store, and the shared-memory frame transfer to the UI process.
  - Anything above 1080p is downscaled after decoding, so it pays the full decode cost. HLS variant selection avoids it.
- **Memory.** Per player: about 48 MiB of queued packets, 16 MiB of network read-ahead per open stream (64 MiB for a server without `Range` support), up to 6 + 3 BGRA frames (8 MiB each at 1080p) and a 1 s audio ring.
- **Timestamps.** A live HLS discontinuity with a large forward timestamp jump inserts at most 2 s of silence. The audio and video timelines can then disagree until the next seek.
- **Licensing.** The SDK's ffmpeg is linked with x264, so the combination is GPL. That matches ps5-browser's GPL-3.0-or-later, but the binary must be distributed under GPL terms.

## Integration

The engine is built by default (`USE_FFMPEG=ON` in `scripts/ps5/build-webkit.sh`; `USE_FFMPEG=OFF scripts/ps5/build-webkit.sh configure` turns it off). After editing `ffmpeg/` or `FFmpeg.cmake`:

```sh
webkit-port/media/sync-to-tree.sh            # copy ffmpeg/ + FFmpeg.cmake into third_party/webkit,
                                             # then scripts/ps5/export-webkit-patches.sh
scripts/ps5/build-webkit.sh webcore          # then: webkit, package
```

`export-webkit-patches.sh` owns patch 0900 like every other patch (topic `0900-ffmpeg-media-player`; its file list follows `ffmpeg/`), rewrites it from the tree and updates the patch-set stamp, so `fetch-webkit.sh` stays consistent. `sync-to-tree.sh --patch` / `--apply` only bootstrap the patch from a tree that does not contain the engine yet (for example after moving to a new WebKit commit); `compile-check.sh` belongs to that pre-integration workflow and refuses to run on an integrated tree.

What the patches change:

- `OptionsPlayStation.cmake` (patch 0001): `WEBKIT_OPTION_DEFINE(USE_FFMPEG … PRIVATE ${PS5_HOMEBREW})`, which depends on `ENABLE_VIDEO`. `USE_FFMPEG` reaches `cmakeconfig.h`, so `USE(FFMPEG)` works without an extra `-D`.
- `PlatformPlayStation.cmake`: `include(platform/FFmpeg.cmake)`. That file adds the sources and the include directory, finds the ffmpeg archives with `find_library` (`pkg_check_modules` is a no-op on PlayStation), and defines `FFMPEG_MEDIA_SDL_AUDIO=1` when `SDL2/SDL.h` and `libSDL2.a` exist.
- `MediaPlayer.cpp`: `MediaPlayerPrivateFFmpeg::registerMediaEngine` under `USE(FFMPEG)`.
- New enum value `FFmpeg` in `MediaPlayerType` (`MediaPlayer.h`), `MediaPlayerMediaEngineIdentifier` (`MediaPlayerEnums.h`) and its IPC serialization (`WebCoreArgumentCoders.serialization.in`), plus the exhaustive switch in `MediaPlayerPrivateRemote.cpp`.

### Link line

`libWebKitPS5.a` is static, so the final `ps5browser.elf` link needs ffmpeg's own static dependencies after the WebKit archives. `scripts/ps5/build-webkit.sh` adds them to the link contract (`link_libs`, exported as `build-wk-ps5/ps5webkit-link.cmake` and consumed by `src/frontend/ps5/CMakeLists.txt`) whenever `cmakeconfig.h` has `USE_FFMPEG`. They come from `scripts/ps5/pkg-config --static --libs libavformat libavcodec libswscale libswresample libavutil`:

```
-lavformat -lbz2 -lavcodec -liconv -llzma -lx264 -lpthread -lz -lswscale -lswresample -lavutil -lm -lssl -lcrypto -pthread
```

That is libavformat, libavcodec, libswscale, libswresample and libavutil, plus bzip2, iconv, liblzma, x264 (GPL), zlib and OpenSSL. libass is not linked. SDL2 and its dependencies (`-lSDL2 -lSceSystemService -lSceUserService -lScePad -lSceAudioOut -lsamplerate -lSceVideoOut -lSceKeyboard -lSceImeDialog`) are part of the same group, for the audio sink. OpenSSL, zlib and iconv are the same SDK archives curl and libxml2 use; the link has no duplicate definitions.

## Tests

`tests/media/` (doctest, host build) covers the engine-independent headers: the MIME/codec table, time-range merging, clock maths and frame decisions, the ring buffer, and the read-ahead/`readyState`/`networkState` policies.

```sh
cmake -S . -B build-host && cmake --build build-host -j2 --target ps5browser_media_tests
ctest --test-dir build-host -R media
```
