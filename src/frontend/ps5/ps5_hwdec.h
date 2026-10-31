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
// hwdec_prepare() runs early in the UI's main(): VLC found the library and
// the compute queue refused once the process had been given full access
// (0x80020063, 0x811D0111) and decoder memory queries refused after it
// (0x811D0200), so the library, the shared compute queue and the memory
// answers for the usual sizes are obtained as early as possible.
#pragma once

#include "hwdec_stream.h"

namespace prosperoweb::frontend {

// Loads the library and makes the compute queue (logged). Idempotent.
void hwdec_prepare();

// The overlay server's decoder factory; empty when hardware decoding is
// unavailable (the library did not load, PROSPEROWEB_VIDEO_DECODE=webprocess
// or PROSPEROWEB_HWDEC=0).
video::DecoderFactory hwdec_factory();

}  // namespace prosperoweb::frontend
