// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hardware video decoding in the UI process with the console's
// libSceVideodec2: the DecoderBackend of the overlay server's remote
// streams (src/ps5/video/hwdec_stream.h). The WebProcess is a plain payload
// and the decoder wants the foreground (big) application, which the UI is,
// so WebProcess players send their compressed video here
// (PROSPEROWEB_VIDEO_DECODE=ui, the default) and fall back to their own
// software decoding whenever this refuses or fails.
//
// The API is not documented. Its use follows open-source PS5 homebrew that
// drives it (all GPL): VLC for PS5 (ps5/modules/ps5vdec.c), the unofficial
// Stremio port (src/hwdec_ps5.cpp), OpenNOW-PS5
// (src/stream/native/hardware_decoder.cpp), xbox-cloud-gaming-ps5
// (src/media/hw_decoder_ps5.cpp); see webkit-port/media/README.md
// ("Hardware decoding") for the structures and what is uncertain.
//
// Two builds (docs/native-title.md):
// - The native title (PRWB00001/eboot.bin, PROSPEROWEB_NATIVE_TITLE)
//   imports the decoder by NID and creates the compute queue at the very
//   start of main() (hwdec_native_startup), like VLC for PS5. On by
//   default; PROSPEROWEB_HWDEC=0 turns it off.
// - The payload (websrv tile) loads the library with dlopen. Off by default
//   there (PROSPEROWEB_HWDEC=1 opts in): the first console run stopped for
//   good inside the decoder's first calls in the hbldr payload context.
//   When on, nothing of the decoder runs on the startup path: a background
//   probe on the first video stream (or right after startup with
//   PROSPEROWEB_HWDEC_PROBE=startup).
//
// Either way every decoder call of the probe goes through
// src/ps5/video/hwdec_probe.h, logged before and after and watched: one
// that has not returned in 3 s turns hardware decoding off for the session
// and streams fall back to the WebProcess's software decoding.
#pragma once

#include "hwdec_stream.h"

namespace prosperoweb::frontend {

// Native title only (PROSPEROWEB_NATIVE_TITLE), the first thing main()
// does: loads the decoder module and creates the shared compute queue
// before any other thread or privileged helper exists (VLC for PS5's
// order), waiting a few seconds at most. A no-op in the payload build.
void hwdec_native_startup();

// Called once the UI is up (splash, WebKit, shell): logs the mode and, with
// PROSPEROWEB_HWDEC_PROBE=startup, starts the probe. Never blocks.
void hwdec_startup_done();

// The overlay server's decoder factory; empty when hardware decoding is
// switched off (PROSPEROWEB_VIDEO_DECODE=webprocess, PROSPEROWEB_HWDEC=0, or
// the payload build without PROSPEROWEB_HWDEC=1).
// The first stream starts the probe and waits for it a few seconds at most;
// streams are refused while it runs too long or after it failed.
video::DecoderFactory hwdec_factory();

}  // namespace prosperoweb::frontend
