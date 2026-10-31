// SPDX-License-Identifier: GPL-3.0-or-later
//
// Remote diagnostics for the console: screen dumps and a form-control probe
// of the active page, both written under /data/ps5browser/tmp so the
// operator can fetch them over FTP. Requests come from the operator creating
// a request file there (polled once per second, then deleted) or, for the
// screen dump, from a long press of Share.
#pragma once

#include <cstdint>
#include <string>

namespace ps5browser::frontend {

// Request and output files, relative to the data directory's tmp/.
inline constexpr const char* kScreenshotRequestFile = "screenshot.request";
inline constexpr const char* kProbeRequestFile = "probe.request";
inline constexpr const char* kScreenLatestFile = "screen-latest.png";
inline constexpr const char* kProbeLatestFile = "probe-latest.json";

// Returns true and deletes path if it exists.
bool take_request_file(const std::string& path);

// Writes data to path through path + ".part" and a rename, so a reader never
// sees a partial file. Returns false (and logs) on failure.
bool write_file_atomically(const std::string& path, const std::string& data);

// Encodes a 32-bit surface whose bytes are R, G, B, X in memory (SDL
// ABGR8888 on little endian) as an RGB PNG. Returns an empty string on
// failure.
std::string encode_png_rgbx(const std::uint8_t* pixels, int width, int height,
                            int pitch_bytes);

// Body of an async JS function that returns, as a JSON string, what the
// page computes for its first form controls and the page environment.
const char* form_probe_script();

}  // namespace ps5browser::frontend
