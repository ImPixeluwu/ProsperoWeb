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
// ("Hardware decoding") for the structures and what is uncertain. The
// library is loaded with dlopen, so the build needs no stub for it.
//
// Nothing of the decoder runs on the startup path: the first console run
// stopped for good inside the decoder's first calls, before the splash. The
// library, the compute queue and the memory answers are obtained by a
// background probe (src/ps5/video/hwdec_probe.h) on the first video stream
// (or right after startup with PROSPEROWEB_HWDEC_PROBE=startup), each call
// logged before and after and watched: one that has not returned in 3 s
// turns hardware decoding off for the session and streams fall back to the
// WebProcess's software decoding.
#pragma once

#include "hwdec_stream.h"

namespace prosperoweb::frontend {

// Called once the UI is up (splash, WebKit, shell): logs the mode and, with
// PROSPEROWEB_HWDEC_PROBE=startup, starts the probe. Never blocks.
void hwdec_startup_done();

// The overlay server's decoder factory; empty when hardware decoding is
// switched off (PROSPEROWEB_VIDEO_DECODE=webprocess or PROSPEROWEB_HWDEC=0).
// The first stream starts the probe and waits for it a few seconds at most;
// streams are refused while it runs too long or after it failed.
video::DecoderFactory hwdec_factory();

}  // namespace prosperoweb::frontend
