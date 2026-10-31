// SPDX-License-Identifier: GPL-3.0-or-later
#include "hwdec_bitstream.h"

#include <cstdio>

namespace prosperoweb::video {

namespace {

constexpr std::uint8_t kStartCode[4] = {0, 0, 0, 1};

int sps_type(VideoCodec codec) { return codec == VideoCodec::H264 ? 7 : 33; }
int aud_type(VideoCodec codec) { return codec == VideoCodec::H264 ? 9 : 35; }

void append_nal(std::vector<std::uint8_t>& out, const std::uint8_t* nal, std::size_t size) {
  out.insert(out.end(), kStartCode, kStartCode + 4);
  out.insert(out.end(), nal, nal + size);
}

std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

bool looks_annexb(const std::uint8_t* data, std::size_t size) {
  return (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
         (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
}

// Calls f(nal, size) for every NAL unit of an Annex-B buffer.
template <typename F>
void for_each_annexb_nal(const std::uint8_t* data, std::size_t size, F&& f) {
  std::size_t i = 0;
  // Find the first start code.
  auto find_start = [&](std::size_t from) -> std::size_t {
    for (std::size_t j = from; j + 2 < size; ++j) {
      if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) {
        return j;
      }
    }
    return size;
  };
  i = find_start(0);
  while (i < size) {
    const std::size_t begin = i + 3;
    std::size_t next = find_start(begin);
    std::size_t end = next;
    // A 4-byte start code's leading zero (and trailing_zero_8bits) belong
    // to the separator, not the NAL.
    while (end > begin && data[end - 1] == 0) {
      --end;
    }
    if (end > begin) {
      f(data + begin, end - begin);
    }
    i = next;
  }
}

bool parse_avcc(const std::uint8_t* d, std::size_t n, StreamConfig& c, std::string& error) {
  if (n < 7) {
    error = "avcC too short";
    return false;
  }
  c.profile = d[1];
  c.level = d[3];
  c.nal_length_size = (d[4] & 3) + 1;
  if (c.nal_length_size == 3) {
    error = "avcC length size 3";
    return false;
  }
  std::size_t p = 5;
  for (int list = 0; list < 2; ++list) {
    if (p >= n) {
      error = "avcC truncated";
      return false;
    }
    const int count = list == 0 ? (d[p] & 0x1f) : d[p];
    ++p;
    for (int i = 0; i < count; ++i) {
      if (p + 2 > n || p + 2 + be16(d + p) > n) {
        error = "avcC parameter set truncated";
        return false;
      }
      const std::size_t len = be16(d + p);
      if (len) {
        append_nal(c.parameter_sets, d + p + 2, len);
      }
      p += 2 + len;
    }
  }
  // High profiles may carry chroma format and bit depths (ISO 14496-15
  // 5.3.3.1.2); old muxers leave them out.
  const bool high = c.profile == 100 || c.profile == 110 || c.profile == 122 || c.profile == 144;
  if (high && p + 3 <= n) {
    c.chroma_format = d[p] & 3;
    c.bit_depth_luma = (d[p + 1] & 7) + 8;
    c.bit_depth_chroma = (d[p + 2] & 7) + 8;
  } else if (c.profile == 110) {
    c.bit_depth_luma = c.bit_depth_chroma = 10;
  } else if (c.profile == 122 || c.profile == 244) {
    c.chroma_format = c.profile == 122 ? 2 : 3;
  }
  return true;
}

bool parse_hvcc(const std::uint8_t* d, std::size_t n, StreamConfig& c, std::string& error) {
  if (n < 23) {
    error = "hvcC too short";
    return false;
  }
  c.profile = d[1] & 0x1f;
  c.level = d[12];
  c.chroma_format = d[16] & 3;
  c.bit_depth_luma = (d[17] & 7) + 8;
  c.bit_depth_chroma = (d[18] & 7) + 8;
  c.nal_length_size = (d[21] & 3) + 1;
  if (c.nal_length_size == 3) {
    error = "hvcC length size 3";
    return false;
  }
  const int arrays = d[22];
  std::size_t p = 23;
  for (int a = 0; a < arrays; ++a) {
    if (p + 3 > n) {
      error = "hvcC array truncated";
      return false;
    }
    const int count = be16(d + p + 1);
    p += 3;
    for (int i = 0; i < count; ++i) {
      if (p + 2 > n || p + 2 + be16(d + p) > n) {
        error = "hvcC NAL unit truncated";
        return false;
      }
      const std::size_t len = be16(d + p);
      if (len) {
        append_nal(c.parameter_sets, d + p + 2, len);
      }
      p += 2 + len;
    }
  }
  return true;
}

}  // namespace

std::optional<StreamConfig> parse_stream_config(VideoCodec codec, const std::uint8_t* data,
                                                std::size_t size, std::string& error) {
  StreamConfig config;
  config.codec = codec;
  if (!data || !size) {
    return config;  // Annex-B with in-band parameter sets
  }
  if (looks_annexb(data, size)) {
    for_each_annexb_nal(data, size, [&](const std::uint8_t* nal, std::size_t len) {
      append_nal(config.parameter_sets, nal, len);
    });
    // profile/level from the SPS header bytes when they are plain.
    for_each_annexb_nal(data, size, [&](const std::uint8_t* nal, std::size_t len) {
      if (nal_type(codec, nal[0]) != sps_type(codec) || config.profile >= 0) {
        return;
      }
      if (codec == VideoCodec::H264 && len >= 4) {
        config.profile = nal[1];
        config.level = nal[3];
      } else if (codec == VideoCodec::Hevc && len >= 15) {
        // 2-byte NAL header, then sps_video_parameter_set_id(4)
        // max_sub_layers(3) temporal_id_nesting(1), then the profile_tier_level.
        config.profile = nal[3] & 0x1f;
        config.level = nal[14];
      }
    });
    return config;
  }
  if (data[0] != 1) {
    error = "unknown extradata format";
    return std::nullopt;
  }
  const bool ok = codec == VideoCodec::H264 ? parse_avcc(data, size, config, error)
                                            : parse_hvcc(data, size, config, error);
  if (!ok) {
    return std::nullopt;
  }
  return config;
}

bool to_annexb(const StreamConfig& config, const std::uint8_t* data, std::size_t size,
               bool keyframe, std::vector<std::uint8_t>& out) {
  out.clear();
  out.reserve(size + config.parameter_sets.size() + 64);
  // Collect the units first so the parameter sets can go in the right place.
  struct Unit {
    const std::uint8_t* p;
    std::size_t n;
  };
  std::vector<Unit> units;
  if (config.nal_length_size > 0) {
    const std::size_t ls = static_cast<std::size_t>(config.nal_length_size);
    std::size_t p = 0;
    while (p < size) {
      if (p + ls > size) {
        return false;
      }
      std::size_t len = 0;
      for (std::size_t i = 0; i < ls; ++i) {
        len = (len << 8) | data[p + i];
      }
      p += ls;
      if (len > size - p) {
        return false;
      }
      if (len) {
        units.push_back({data + p, len});
      }
      p += len;
    }
  } else {
    for_each_annexb_nal(data, size, [&](const std::uint8_t* nal, std::size_t len) {
      units.push_back({nal, len});
    });
  }
  bool has_sps = false;
  for (const Unit& u : units) {
    if (nal_type(config.codec, u.p[0]) == sps_type(config.codec)) {
      has_sps = true;
    }
  }
  const bool insert = keyframe && !has_sps && !config.parameter_sets.empty();
  std::size_t i = 0;
  if (insert && !units.empty() && nal_type(config.codec, units[0].p[0]) == aud_type(config.codec)) {
    append_nal(out, units[0].p, units[0].n);
    i = 1;
  }
  if (insert) {
    out.insert(out.end(), config.parameter_sets.begin(), config.parameter_sets.end());
  }
  for (; i < units.size(); ++i) {
    append_nal(out, units[i].p, units[i].n);
  }
  return true;
}

std::vector<int> annexb_nal_types(VideoCodec codec, const std::uint8_t* data, std::size_t size) {
  std::vector<int> types;
  for_each_annexb_nal(data, size, [&](const std::uint8_t* nal, std::size_t) {
    types.push_back(nal_type(codec, nal[0]));
  });
  return types;
}

bool hardware_profile_supported(const StreamConfig& config, bool allow_hevc,
                                std::string& reason) {
  char text[96];
  if (config.chroma_format != 1 || config.bit_depth_luma != 8 || config.bit_depth_chroma != 8) {
    std::snprintf(text, sizeof(text), "chroma format %d, %d-bit (only 8-bit 4:2:0)",
                  config.chroma_format, config.bit_depth_luma);
    reason = text;
    return false;
  }
  if (config.codec == VideoCodec::H264) {
    switch (config.profile) {
      case -1:   // unknown (Annex-B without SPS yet): let the decoder try
      case 66:   // Baseline (constrained)
      case 77:   // Main
      case 88:   // Extended
      case 100:  // High
        return true;
      default:
        std::snprintf(text, sizeof(text), "h264 profile %d", config.profile);
        reason = text;
        return false;
    }
  }
  if (!allow_hevc) {
    reason = "hevc disabled";
    return false;
  }
  if (config.profile == -1 || config.profile == 1) {
    return true;  // Main
  }
  std::snprintf(text, sizeof(text), "hevc profile %d (only Main)", config.profile);
  reason = text;
  return false;
}

}  // namespace prosperoweb::video
