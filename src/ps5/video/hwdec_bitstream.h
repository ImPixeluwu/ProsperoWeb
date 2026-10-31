// SPDX-License-Identifier: GPL-3.0-or-later
//
// Elementary-stream helpers of the UI's hardware video decoder
// (src/frontend/ps5/ps5_hwdec.cpp): hardware decoders take H.264/HEVC
// access units in Annex-B form (start codes, parameter sets in band), while
// MP4/MSE players deliver them length-prefixed with the parameter sets in the
// container's avcC/hvcC record. The WebProcess sends packets as demuxed;
// the conversion happens here, next to the decoder that needs it.
//
// Pure (no platform API), unit-tested by tests/ps5/test_hwdec.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace prosperoweb::video {

enum class VideoCodec { H264, Hevc };

// What a stream's codec configuration (avcC, hvcC or Annex-B extradata)
// says. `nal_length_size` 0 means the packets are Annex-B already.
struct StreamConfig {
  VideoCodec codec = VideoCodec::H264;
  int nal_length_size = 0;
  int profile = -1;          // profile_idc (H.264) / general_profile_idc (HEVC)
  int level = -1;            // level_idc / general_level_idc
  int chroma_format = 1;     // 1 = 4:2:0 (the avcC/hvcC default when absent)
  int bit_depth_luma = 8;
  int bit_depth_chroma = 8;
  // Every parameter set (VPS/SPS/PPS) with 4-byte start codes, ready to be
  // prepended to a keyframe.
  std::vector<std::uint8_t> parameter_sets;
};

// Parses the extradata of a stream: an avcC/hvcC record (first byte 1),
// Annex-B parameter sets, or nothing (packets assumed Annex-B with the
// parameter sets in band). nullopt with `error` set when malformed.
std::optional<StreamConfig> parse_stream_config(VideoCodec codec, const std::uint8_t* data,
                                                std::size_t size, std::string& error);

// NAL unit type of the first byte of a NAL unit.
inline int nal_type(VideoCodec codec, std::uint8_t first) {
  return codec == VideoCodec::H264 ? (first & 0x1f) : ((first >> 1) & 0x3f);
}

// Converts one access unit to Annex-B into `out` (replacing its content).
// Length-prefixed input (config.nal_length_size > 0) gets 4-byte start
// codes; Annex-B input is copied. A keyframe without an in-band SPS gets
// the configuration's parameter sets in front (after a leading access unit
// delimiter, which must stay first). False when a length prefix runs past
// the end of the packet (out is then unspecified).
bool to_annexb(const StreamConfig& config, const std::uint8_t* data, std::size_t size,
               bool keyframe, std::vector<std::uint8_t>& out);

// The NAL unit types of an Annex-B buffer, in order (for tests and logs).
std::vector<int> annexb_nal_types(VideoCodec codec, const std::uint8_t* data, std::size_t size);

// Whether the decoder can take this configuration: 8-bit 4:2:0 H.264
// (Baseline, Main, High; not High 10/4:2:2/4:4:4) and, with `allow_hevc`,
// HEVC Main (8-bit). `reason` explains a refusal.
bool hardware_profile_supported(const StreamConfig& config, bool allow_hevc, std::string& reason);

}  // namespace prosperoweb::video
