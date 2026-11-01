// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_hwdec.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <sys/cpuset.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "ps5_cpu_affinity.h"
#include "hwdec_probe.h"
#include "ps5_log.h"

// libkernel (memory). Declared here: the payload SDK has no headers for them.
extern "C" {
int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end,
                                  std::size_t len, std::size_t alignment, int memory_type,
                                  std::int64_t* start);
int sceKernelMapDirectMemory(void** addr, std::size_t len, int prot, int flags,
                             std::int64_t start, std::size_t alignment);
int sceKernelReleaseDirectMemory(std::int64_t start, std::size_t len);
std::int64_t sceKernelGetDirectMemorySize(void);
int sceKernelMapNamedFlexibleMemory(void** addr, std::size_t len, int prot, int flags,
                                    const char* name);
int sceKernelMunmap(void* addr, std::size_t len);
}

#if PROSPEROWEB_NATIVE_TITLE
// The native title imports the decoder by NID like the references do (a
// title has no runtime loader for dlsym by name): the link stub is
// src/frontend/ps5/native/videodec2_stub.c, the module is loaded with
// sceSysmoduleLoadModule before the first call.
extern "C" {
int sceSysmoduleLoadModule(std::uint16_t id);
int sceVideodec2QueryComputeMemoryInfo(void* info);
int sceVideodec2AllocateComputeQueue(const void* config, const void* memory, void** queue);
int sceVideodec2ReleaseComputeQueue(void* queue);
int sceVideodec2QueryDecoderMemoryInfo(const void* config, void* memory);
int sceVideodec2CreateDecoder(const void* config, const void* memory, void** decoder);
int sceVideodec2DeleteDecoder(void* decoder);
int sceVideodec2Decode(void* decoder, const void* input, void* frame, void* output);
int sceVideodec2Flush(void* decoder, void* frame, void* output);
int sceVideodec2Reset(void* decoder);
}
#endif

namespace prosperoweb::frontend {

namespace {

// --- libSceVideodec2 ---------------------------------------------------------
//
// Structure layouts as the projects above declare them; they agree on every
// field used here. The DecoderConfig tail differs between them (three u32
// vs. two bools and a pointer, a PS4 layout); it is all zero either way.

struct ComputeMemoryInfo {
  std::uint64_t this_size;
  std::uint64_t cpu_gpu_size;
  void* cpu_gpu;
};
static_assert(sizeof(ComputeMemoryInfo) == 0x18);

struct ComputeConfigInfo {
  std::uint64_t this_size;
  std::uint16_t pipe_id;
  std::uint16_t queue_id;
  std::uint8_t check_memory_type;
  std::uint8_t reserved0;
  std::uint16_t reserved1;
};
static_assert(sizeof(ComputeConfigInfo) == 0x10);

struct DecoderConfigInfo {
  std::uint64_t this_size;
  std::uint32_t resource_type;  // 1: decoding on a compute queue
  std::uint32_t codec_type;
  std::uint32_t profile;
  std::uint32_t max_level;
  std::int32_t max_width;
  std::int32_t max_height;
  std::int32_t max_dpb_frames;  // -1: the decoder sizes it from the stream
  std::uint32_t pipeline_depth;
  void* compute_queue;
  std::uint64_t cpu_affinity;
  std::int32_t cpu_priority;
  std::uint32_t optimize_progressive;
  std::uint32_t check_memory_type;
  std::uint32_t reserved;
};
static_assert(sizeof(DecoderConfigInfo) == 0x48);
static_assert(offsetof(DecoderConfigInfo, compute_queue) == 0x28);

struct DecoderMemoryInfo {
  std::uint64_t this_size;
  std::uint64_t cpu_size;
  void* cpu;
  std::uint64_t gpu_size;
  void* gpu;
  std::uint64_t cpu_gpu_size;
  void* cpu_gpu;
  std::uint64_t max_frame_size;
  std::uint32_t frame_alignment;
  std::uint32_t reserved;
};
static_assert(sizeof(DecoderMemoryInfo) == 0x48);

struct InputData {
  std::uint64_t this_size;
  void* au;
  std::uint64_t au_size;
  std::uint64_t pts;
  std::uint64_t dts;
  std::uint64_t attached;
};
static_assert(sizeof(InputData) == 0x30);

struct FrameBuffer {
  std::uint64_t this_size;
  void* buffer;
  std::uint64_t buffer_size;
  std::uint8_t accepted;  // the decoder keeps the buffer until it outputs it
  std::uint8_t reserved[7];
};
static_assert(sizeof(FrameBuffer) == 0x20 && offsetof(FrameBuffer, accepted) == 24);

struct OutputInfo {
  std::uint64_t this_size;
  std::uint8_t valid;
  std::uint8_t error;
  std::uint8_t picture_count;
  std::uint8_t padding;
  std::uint32_t codec;
  std::uint32_t width;
  std::uint32_t pitch;  // samples
  std::uint32_t height;
  std::uint32_t reserved;
  void* buffer;
  std::uint64_t buffer_size;
  std::uint32_t frame_format;
  std::uint32_t pitch_bytes;
};
static_assert(sizeof(OutputInfo) == 0x38 && offsetof(OutputInfo, buffer) == 32);

constexpr std::uint32_t kCodecAvc = 1;
constexpr std::uint32_t kCodecHevc = 0xEE049;
constexpr std::uint16_t kSysmoduleVideodec2 = 207;
constexpr int kProtCpuRw = 0x03;
constexpr int kProtGpuRw = 0x30;
constexpr int kDirectMemoryType = 12;  // what every known user allocates
constexpr std::size_t kPage = 0x4000;
// Picture slots: the decoder's picture store plus those waiting to be shown.
constexpr int kSlotsUpTo1080p = 16;
constexpr int kSlots4K = 10;
// Access units: the decoder may still read one after Decode returns, so a
// ring of buffers (VLC saw 3 break on large 4K keyframes).
constexpr int kAuSlots = 8;
constexpr std::size_t kAuSlotBytes = 4u << 20;

struct Api {
  int (*query_compute_memory)(ComputeMemoryInfo*) = nullptr;
  int (*allocate_compute_queue)(const ComputeConfigInfo*, const ComputeMemoryInfo*,
                                void**) = nullptr;
  int (*release_compute_queue)(void*) = nullptr;
  int (*query_decoder_memory)(const DecoderConfigInfo*, DecoderMemoryInfo*) = nullptr;
  int (*create_decoder)(const DecoderConfigInfo*, const DecoderMemoryInfo*, void**) = nullptr;
  int (*delete_decoder)(void*) = nullptr;
  int (*decode)(void*, const InputData*, FrameBuffer*, OutputInfo*) = nullptr;
  int (*flush)(void*, FrameBuffer*, OutputInfo*) = nullptr;
  int (*reset)(void*) = nullptr;
};

std::size_t round_page(std::size_t n) { return (n + kPage - 1) & ~(kPage - 1); }

struct Mem {
  void* addr = nullptr;
  std::size_t len = 0;
  std::int64_t start = -1;  // direct memory; -1 flexible; -2 the shared CPU block
};

bool alloc_direct(Mem& m, std::size_t len, std::size_t align) {
  m = Mem{};
  m.len = round_page(len);
  align = std::max(align, kPage);
  std::int64_t start = 0;
  if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), m.len, align,
                                    kDirectMemoryType, &start) != 0) {
    return false;
  }
  void* addr = nullptr;
  if (sceKernelMapDirectMemory(&addr, m.len, kProtCpuRw | kProtGpuRw, 0, start, align) != 0) {
    sceKernelReleaseDirectMemory(start, m.len);
    return false;
  }
  m.addr = addr;
  m.start = start;
  return true;
}

struct State {
  std::mutex lock;
  Api api;
  void* queue = nullptr;
  Mem queue_mem;
  // The decoders' CPU memory: one flexible block reused by one decoder after
  // another. VLC found flexible memory given back never came back (75
  // decoders later the app had none left).
  void* cpu_block = nullptr;
  std::size_t cpu_block_len = 0;
  bool cpu_block_busy = false;
  // Memory answers obtained at startup (queries may be refused later).
  struct Cached {
    DecoderConfigInfo config;
    DecoderMemoryInfo memory;
  };
  std::vector<Cached> cached;
  std::uint64_t affinity = 0x3f;
};

State& state() {
  static State s;
  return s;
}

void release(Mem& m) {
  if (!m.addr) {
    return;
  }
  if (m.start == -2) {
    std::lock_guard<std::mutex> lock(state().lock);
    state().cpu_block_busy = false;
  } else {
    sceKernelMunmap(m.addr, m.len);
    if (m.start >= 0) {
      sceKernelReleaseDirectMemory(m.start, m.len);
    }
  }
  m = Mem{};
}

bool alloc_cpu(Mem& m, std::size_t len) {
  m = Mem{};
  len = round_page(len);
  State& s = state();
  {
    std::lock_guard<std::mutex> lock(s.lock);
    if (!s.cpu_block_busy) {
      if (len > s.cpu_block_len) {
        // Sized with room (4K HEVC wants about 76 MiB) so it rarely grows.
        const std::size_t want = std::max(len, std::size_t{96} << 20);
        void* addr = nullptr;
        if (sceKernelMapNamedFlexibleMemory(&addr, want, kProtCpuRw, 0, "ProsperoWebVdec") == 0) {
          if (s.cpu_block) {
            log_printf("hwdec: CPU block grown %zu -> %zu KiB", s.cpu_block_len >> 10,
                       want >> 10);
          }
          s.cpu_block = addr;
          s.cpu_block_len = want;
        }
      }
      if (s.cpu_block && len <= s.cpu_block_len) {
        s.cpu_block_busy = true;
        m.addr = s.cpu_block;
        m.len = len;
        m.start = -2;
        return true;
      }
    }
  }
  // A second decoder at the same time: its own block.
  void* addr = nullptr;
  if (sceKernelMapNamedFlexibleMemory(&addr, len, kProtCpuRw, 0, "ProsperoWebVdec2") != 0) {
    return false;
  }
  m.addr = addr;
  m.len = len;
  return true;
}

const char* codec_name(std::uint32_t codec) { return codec == kCodecAvc ? "h264" : "hevc"; }

DecoderConfigInfo make_config(std::uint32_t codec, std::uint32_t profile, std::uint32_t level,
                              int width, int height, std::uint32_t depth) {
  DecoderConfigInfo c{};
  c.this_size = sizeof(c);
  c.resource_type = 1;
  c.codec_type = codec;
  c.profile = profile;
  c.max_level = level;
  c.max_width = width;
  c.max_height = height;
  c.max_dpb_frames = -1;
  c.pipeline_depth = depth;
  c.compute_queue = state().queue;
  c.cpu_affinity = state().affinity;
  c.cpu_priority = 700;
  c.optimize_progressive = 1;
  return c;
}

// The memory a config needs: asked now, else a startup answer for the same
// codec, profile, level and depth at least as large (the config then takes
// its size).
int query_memory(DecoderConfigInfo& config, DecoderMemoryInfo& memory) {
  memory = DecoderMemoryInfo{};
  memory.this_size = sizeof(memory);
  const int rc = state().api.query_decoder_memory(&config, &memory);
  if (rc == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(state().lock);
  for (const auto& cached : state().cached) {
    const DecoderConfigInfo& c = cached.config;
    if (c.codec_type == config.codec_type && c.profile == config.profile &&
        c.max_level == config.max_level && c.pipeline_depth == config.pipeline_depth &&
        c.max_width >= config.max_width && c.max_height >= config.max_height) {
      log_printf("hwdec: memory query %#x: using the startup answer for %dx%d", rc, c.max_width,
                 c.max_height);
      config.max_width = c.max_width;
      config.max_height = c.max_height;
      memory = cached.memory;
      return 0;
    }
  }
  return rc;
}

#if !PROSPEROWEB_NATIVE_TITLE
template <typename T>
bool resolve(void* lib, const char* name, T& out) {
  out = reinterpret_cast<T>(dlsym(lib, name));
  if (!out) {
    log_printf("hwdec: %s not found in libSceVideodec2", name);
  }
  return out != nullptr;
}
#endif

std::uint64_t decoder_affinity() {
  cpu::CpuMask mask;
  if (get_cpu_mask(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, mask) != 0) {
    return 0x3f;
  }
  std::uint64_t bits = 0;
  for (int cpu = 0; cpu < 16; ++cpu) {
    if (mask.test(cpu)) {
      bits |= std::uint64_t{1} << cpu;
    }
  }
  // Six logical CPUs as VLC and the Stremio port use, within the process's.
  const std::uint64_t preferred = bits & 0x3f;
  return preferred ? preferred : (bits ? bits : 0x3f);
}

bool env_off(const char* name, const char* off_value) {
  const char* value = std::getenv(name);
  return value && !std::strcmp(value, off_value);
}

// --- the backend --------------------------------------------------------------

class Videodec2Backend final : public video::DecoderBackend {
 public:
  ~Videodec2Backend() override {
    if (decoder_) {
      state().api.delete_decoder(decoder_);
    }
    for (Mem& m : au_) {
      release(m);
    }
    release(frames_);
    release(cpu_gpu_);
    release(gpu_);
    release(cpu_);
  }

  // nullptr with `reason` set when the decoder cannot be made.
  static std::unique_ptr<Videodec2Backend> create(const video::DecoderRequest& request,
                                                  std::string& reason) {
    std::unique_ptr<Videodec2Backend> self(new Videodec2Backend());
    if (!self->open(request, reason)) {
      return nullptr;
    }
    return self;
  }

  int slot_count() const override { return slots_; }
  int max_width() const override { return config_.max_width; }
  int max_height() const override { return config_.max_height; }

  Result decode(const std::uint8_t* au, std::size_t size, std::int64_t pts, int target) override {
    Result result;
    Mem& slot = au_[static_cast<std::size_t>(au_next_)];
    au_next_ = (au_next_ + 1) % kAuSlots;
    if (size > slot.len) {
      release(slot);
      if (!alloc_direct(slot, size, kPage)) {
        result.status = Result::Status::Fatal;
        log_printf("hwdec: no memory for a %zu-byte access unit", size);
        return result;
      }
    }
    std::memcpy(slot.addr, au, size);
    InputData in{};
    in.this_size = sizeof(in);
    in.au = slot.addr;
    in.au_size = size;
    in.pts = static_cast<std::uint64_t>(pts);
    in.dts = UINT64_MAX;
    FrameBuffer fb = frame_buffer(target);
    OutputInfo out{};
    out.this_size = sizeof(out);
    const int rc = state().api.decode(decoder_, &in, &fb, &out);
    result.target_taken = fb.accepted != 0;
    return finish(rc, out, result);
  }

  Result flush(int target) override {
    Result result;
    FrameBuffer fb = frame_buffer(target);
    OutputInfo out{};
    out.this_size = sizeof(out);
    const int rc = state().api.flush(decoder_, &fb, &out);
    result.target_taken = fb.accepted != 0;
    if (rc == 0 && !out.valid) {
      return result;  // nothing left
    }
    return finish(rc, out, result);
  }

  bool reset() override {
    const int rc = state().api.reset(decoder_);
    if (rc != 0) {
      log_printf("hwdec: reset %#x", rc);
    }
    return rc == 0;
  }

 private:
  Videodec2Backend() = default;

  bool open(const video::DecoderRequest& request, std::string& reason) {
    const bool avc = request.config.codec == video::VideoCodec::H264;
    width_ = request.width;
    height_ = request.height;
    // Room for the video's size (rounded to 16), at least 1080p (smaller
    // sizes were refused, 0x811D0200), at most 4K.
    const int w = std::clamp((std::max(width_, 1) + 15) & ~15, 1920, 3840);
    const int h = std::clamp((std::max(height_, 1) + 15) & ~15, 1088, 2176);
    if (width_ > 3840 || height_ > 2176) {
      reason = "larger than 4K";
      return false;
    }
    slots_ = (w <= 1920 && h <= 1088) ? kSlotsUpTo1080p : kSlots4K;
    const std::uint32_t codec = avc ? kCodecAvc : kCodecHevc;
    struct Try {
      std::uint32_t profile, level, depth;
    };
    // Pipeline depth 4 keeps the hardware busy (VLC: depth 1 managed 20 of
    // 24 fps at 4K); depth 1 needs less memory. Levels from the most
    // capable down.
    const std::vector<Try> tries =
        avc ? std::vector<Try>{{100, 52, 4}, {100, 51, 4}, {100, 52, 1}, {100, 51, 1}}
            : std::vector<Try>{{1, 153, 4}, {1, 156, 4}, {2, 153, 4}, {1, 153, 1}, {2, 153, 1}};
    int rc = -1;
    for (const Try& t : tries) {
      config_ = make_config(codec, t.profile, t.level, w, h, t.depth);
      rc = bring_up();
      if (rc == 0) {
        break;
      }
      log_printf("hwdec: %s %dx%d profile %u level %u pipeline %u: %#x", codec_name(codec), w, h,
                 t.profile, t.level, t.depth, rc);
      // 0x811D01xx: the hardware itself (resources, state); other configs
      // will not do better.
      if ((static_cast<std::uint32_t>(rc) & 0xFFFFFF00u) == 0x811D0100u) {
        break;
      }
    }
    if (rc != 0) {
      char text[64];
      std::snprintf(text, sizeof(text), "create failed %#x", static_cast<unsigned>(rc));
      reason = text;
      return false;
    }
    const std::size_t total = memory_.cpu_size + memory_.gpu_size + memory_.cpu_gpu_size +
                              frames_.len + kAuSlots * kAuSlotBytes;
    log_printf(
        "hwdec: open %s %dx%d -> hw ok (decoder mem %zu MiB: cpu %" PRIu64 " / gpu %" PRIu64
        " / cpu+gpu %" PRIu64 " KiB, %d frames x %zu KiB; profile %u level %u, pipeline %u, "
        "room %dx%d, affinity %#" PRIx64 ")",
        codec_name(codec), width_, height_, total >> 20, memory_.cpu_size >> 10,
        memory_.gpu_size >> 10, memory_.cpu_gpu_size >> 10, slots_, frame_size_ >> 10,
        config_.profile, config_.max_level, config_.pipeline_depth, config_.max_width,
        config_.max_height, config_.cpu_affinity);
    return true;
  }

  // Memory and the decoder for config_; on failure everything is given back.
  int bring_up() {
    int rc = query_memory(config_, memory_);
    if (rc != 0) {
      return rc;
    }
    const std::size_t align = std::max<std::size_t>(memory_.frame_alignment, kPage);
    frame_size_ = (memory_.max_frame_size + align - 1) & ~(align - 1);
    bool ok = (!memory_.cpu_size || alloc_cpu(cpu_, memory_.cpu_size)) &&
              (!memory_.gpu_size || alloc_direct(gpu_, memory_.gpu_size, kPage)) &&
              (!memory_.cpu_gpu_size || alloc_direct(cpu_gpu_, memory_.cpu_gpu_size, kPage)) &&
              frame_size_ > 0 &&
              alloc_direct(frames_, frame_size_ * static_cast<std::size_t>(slots_), align);
    for (int i = 0; ok && i < kAuSlots; ++i) {
      ok = alloc_direct(au_[static_cast<std::size_t>(i)], kAuSlotBytes, kPage);
    }
    if (ok) {
      // Memory comes back as the previous decoder left it; a picture the
      // hardware did not finish showed the video before (VLC). Clear it,
      // pictures mid-grey.
      if (cpu_.addr) {
        std::memset(cpu_.addr, 0, cpu_.len);
      }
      if (gpu_.addr) {
        std::memset(gpu_.addr, 0, gpu_.len);
      }
      if (cpu_gpu_.addr) {
        std::memset(cpu_gpu_.addr, 0, cpu_gpu_.len);
      }
      std::memset(frames_.addr, 0x80, frames_.len);
      memory_.cpu = cpu_.addr;
      memory_.gpu = gpu_.addr;
      memory_.cpu_gpu = cpu_gpu_.addr;
      rc = state().api.create_decoder(&config_, &memory_, &decoder_);
      if (rc == 0) {
        rc = state().api.reset(decoder_);
        if (rc != 0) {
          log_printf("hwdec: reset after create %#x (ignored)", rc);
        }
        return 0;
      }
      decoder_ = nullptr;
    } else {
      log_printf("hwdec: no memory (cpu %" PRIu64 ", gpu %" PRIu64 ", cpu+gpu %" PRIu64
                 " KiB, frames %d x %zu KiB)",
                 memory_.cpu_size >> 10, memory_.gpu_size >> 10, memory_.cpu_gpu_size >> 10,
                 slots_, frame_size_ >> 10);
      rc = -1;
    }
    for (Mem& m : au_) {
      release(m);
    }
    release(frames_);
    release(cpu_gpu_);
    release(gpu_);
    release(cpu_);
    return rc;
  }

  FrameBuffer frame_buffer(int target) const {
    FrameBuffer fb{};
    fb.this_size = sizeof(fb);
    fb.buffer = static_cast<std::uint8_t*>(frames_.addr) +
                static_cast<std::size_t>(target) * frame_size_;
    fb.buffer_size = frame_size_;
    return fb;
  }

  Result finish(int rc, const OutputInfo& out, Result& result) {
    if (rc != 0) {
      result.code = static_cast<std::uint32_t>(rc);
      result.status = (result.code & 0xFFFFFF00u) == 0x811D0100u ? Result::Status::Fatal
                                                                  : Result::Status::StreamError;
      return result;
    }
    if (!out.valid || !out.buffer) {
      return result;
    }
    const auto* base = static_cast<const std::uint8_t*>(frames_.addr);
    const auto* buffer = static_cast<const std::uint8_t*>(out.buffer);
    const std::ptrdiff_t offset = buffer - base;
    if (offset < 0 || frame_size_ == 0 || static_cast<std::size_t>(offset) % frame_size_ ||
        static_cast<std::size_t>(offset) / frame_size_ >= static_cast<std::size_t>(slots_)) {
      log_printf("hwdec: picture in an unknown buffer %p (frames at %p)", out.buffer,
                 frames_.addr);
      result.status = Result::Status::Fatal;
      return result;
    }
    result.frame_slot = static_cast<int>(static_cast<std::size_t>(offset) / frame_size_);
    if (out.pitch_bytes >= 2 * out.pitch) {
      // 16-bit samples (10-bit video): not converted here.
      log_printf("hwdec: 16-bit picture (pitch %u samples, %u bytes)", out.pitch, out.pitch_bytes);
      result.status = Result::Status::Fatal;
      result.code = 0;
      return result;
    }
    result.corrupt = out.error != 0;
    // The chroma plane follows the decoder's luma rows (1088 for a padded
    // 1080; 2160 for 4K: VLC found that reading it 2176 rows down was wrong).
    video::Nv12View& v = result.picture;
    v.y = buffer;
    v.uv = buffer + static_cast<std::size_t>(out.pitch_bytes) * out.height;
    v.y_pitch = static_cast<int>(out.pitch_bytes);
    v.uv_pitch = static_cast<int>(out.pitch_bytes);
    // The visible size: the decoder's picture (it may be the coded size,
    // 1088 rows for 1080p). The container's size crops only that padding:
    // after a quality switch the pictures outgrow it (a 1280x720 stream
    // switching to 1080p) and cropping to it showed the top-left part.
    // HwStream crops to the visible size of the current parameter sets.
    v.width = static_cast<int>(out.width);
    v.height = static_cast<int>(out.height);
    if (width_ > 0 && height_ > 0 && width_ <= v.width && height_ <= v.height &&
        v.width - width_ < 32 && v.height - height_ < 32) {
      v.width = width_;
      v.height = height_;
    }
    v.width &= ~1;
    v.height &= ~1;
    if (out.width != logged_width_ || out.height != logged_height_ ||
        out.pitch_bytes != logged_pitch_) {
      logged_width_ = out.width;
      logged_height_ = out.height;
      logged_pitch_ = out.pitch_bytes;
      log_printf("hwdec: pictures %ux%u (shown %dx%d), pitch %u bytes, buffer %" PRIu64
                 " bytes, format %u",
                 out.width, out.height, v.width, v.height, out.pitch_bytes, out.buffer_size,
                 out.frame_format);
    }
    return result;
  }

  DecoderConfigInfo config_{};
  DecoderMemoryInfo memory_{};
  void* decoder_ = nullptr;
  Mem cpu_, gpu_, cpu_gpu_, frames_;
  Mem au_[kAuSlots];
  int au_next_ = 0;
  std::size_t frame_size_ = 0;
  int slots_ = kSlotsUpTo1080p;
  int width_ = 0;
  int height_ = 0;
  std::uint32_t logged_width_ = 0, logged_height_ = 0, logged_pitch_ = 0;
};

// --- the probe ----------------------------------------------------------------
//
// Off the startup path, every call through the probe (logged before and
// after, watched: hwdec_probe.h).

std::string hex(std::uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "%#" PRIx64, value);
  return text;
}

std::string ptr(const void* p) {
  char text[24];
  std::snprintf(text, sizeof(text), "%p", p);
  return text;
}

#if !PROSPEROWEB_NATIVE_TITLE
// dlopen as a watched call.
void* load_library(video::HwdecProbe& probe, const char* name) {
  void* handle = nullptr;
  probe.call("dlopen", name, [&](std::string& detail) {
    handle = dlopen(name, RTLD_LAZY);
    if (!handle) {
      const char* error = dlerror();
      detail = error ? error : "failed";
      return -1;
    }
    detail = "handle " + ptr(handle);
    return 0;
  });
  return handle;
}
#endif

// Direct memory for the compute queue, each kernel call watched.
bool queue_memory(video::HwdecProbe& probe, Mem& m, std::size_t len) {
  m = Mem{};
  m.len = round_page(len);
  std::int64_t size = 0;
  probe.call("sceKernelGetDirectMemorySize", "", [&](std::string& detail) {
    size = sceKernelGetDirectMemorySize();
    detail = "size " + hex(static_cast<std::uint64_t>(size));
    return 0;
  });
  std::int64_t start = 0;
  int rc = probe.call(
      "sceKernelAllocateDirectMemory",
      "0, " + hex(static_cast<std::uint64_t>(size)) + ", len " + hex(m.len) + ", align " +
          hex(kPage) + ", type " + std::to_string(kDirectMemoryType),
      [&](std::string& detail) {
        const int r = sceKernelAllocateDirectMemory(0, size, m.len, kPage, kDirectMemoryType,
                                                    &start);
        detail = "start " + hex(static_cast<std::uint64_t>(start));
        return r;
      });
  if (rc != 0) {
    return false;
  }
  void* addr = nullptr;
  rc = probe.call("sceKernelMapDirectMemory",
                  "len " + hex(m.len) + ", prot " + hex(kProtCpuRw | kProtGpuRw) + ", start " +
                      hex(static_cast<std::uint64_t>(start)) + ", align " + hex(kPage),
                  [&](std::string& detail) {
                    const int r = sceKernelMapDirectMemory(&addr, m.len, kProtCpuRw | kProtGpuRw,
                                                           0, start, kPage);
                    detail = "addr " + ptr(addr);
                    return r;
                  });
  if (rc != 0) {
    if (rc != video::HwdecProbe::kAbandoned) {
      sceKernelReleaseDirectMemory(start, m.len);
    }
    return false;
  }
  m.addr = addr;
  m.start = start;
  return true;
}

// Memory for the usual configs, asked while the probe runs (VLC found them
// refused later in a process; a stream then uses these answers).
bool cache_memory_answers(video::HwdecProbe& probe) {
  State& s = state();
  struct Size {
    std::uint32_t codec, profile, level;
    int w, h;
  };
  const Size sizes[] = {{kCodecAvc, 100, 52, 1920, 1088}, {kCodecAvc, 100, 52, 3840, 2176},
                        {kCodecHevc, 1, 153, 1920, 1088}, {kCodecHevc, 1, 153, 3840, 2176}};
  for (const Size& size : sizes) {
    for (std::uint32_t depth : {4u, 1u}) {
      DecoderConfigInfo config = make_config(size.codec, size.profile, size.level, size.w, size.h,
                                             depth);
      DecoderMemoryInfo memory{};
      memory.this_size = sizeof(memory);
      char args[128];
      std::snprintf(args, sizeof(args), "%s profile %u level %u %dx%d pipeline %u, queue %p",
                    codec_name(size.codec), size.profile, size.level, size.w, size.h, depth,
                    config.compute_queue);
      const int rc = probe.call(
          "sceVideodec2QueryDecoderMemoryInfo", args, [&](std::string& detail) {
            const int r = s.api.query_decoder_memory(&config, &memory);
            char text[160];
            std::snprintf(text, sizeof(text),
                          "cpu %" PRIu64 " / gpu %" PRIu64 " / cpu+gpu %" PRIu64
                          " KiB, frame %" PRIu64 " KiB, align %u",
                          memory.cpu_size >> 10, memory.gpu_size >> 10,
                          memory.cpu_gpu_size >> 10, memory.max_frame_size >> 10,
                          memory.frame_alignment);
            detail = text;
            return r;
          });
      if (rc == video::HwdecProbe::kAbandoned) {
        return false;
      }
      if (rc == 0) {
        std::lock_guard<std::mutex> lock(s.lock);
        s.cached.push_back({config, memory});
      }
    }
  }
  return true;
}

// The process's credentials, logged before the first Sony call. The
// references run the decoder in a sandboxed title; VLC-PS5 found the
// library (0x80020063) and the compute queue (0x811D0111) refused once its
// title had been given full access (src/platform_ps5.c). hbldr gives this
// process uid 0, every capability and no jail before main() runs.
void log_process_identity() {
#if PROSPEROWEB_NATIVE_TITLE
  // A title has no kernel R/W (the payload crt's kernel_* helpers are not
  // linked); it runs with the application credentials the system gave it.
  log_printf("hwdec: process uid %u euid %u (native title, application credentials)",
             static_cast<unsigned>(getuid()), static_cast<unsigned>(geteuid()));
#else
  const pid_t self = getpid();
  std::uint8_t caps[16] = {};
  std::string caps_hex = "?";
  if (kernel_get_ucred_caps(self, caps) == 0) {
    caps_hex.clear();
    for (std::uint8_t byte : caps) {
      char two[3];
      std::snprintf(two, sizeof(two), "%02x", byte);
      caps_hex += two;
    }
  }
  const intptr_t jail = kernel_get_proc_jaildir(self);
  const intptr_t root = kernel_get_proc_rootdir(self);
  log_printf("hwdec: process uid %u (kernel %u) euid %u authid %#" PRIx64
             " caps %s jaildir %s rootdir %s fw %#x",
             static_cast<unsigned>(getuid()), static_cast<unsigned>(kernel_get_ucred_uid(self)),
             static_cast<unsigned>(geteuid()),
             static_cast<std::uint64_t>(kernel_get_ucred_authid(self)), caps_hex.c_str(),
             jail ? "set" : "none", root == kernel_get_root_vnode() ? "/" : "other",
             static_cast<unsigned>(kernel_get_fw_version()));
#endif
}

// The probe body: libraries, functions, the shared compute queue, memory
// answers. The order follows the references (ps5_hwdec.h): the GPU
// libraries in the process before the decoder's (all four have libSceAgc
// and libSceAgcDriver among the title's own imports), the decoder's system
// module, then the queue in 16 KiB-aligned CPU+GPU direct memory of type 12.
#if PROSPEROWEB_NATIVE_TITLE
// The native title: sysmodule 207, then the imported functions (VLC for
// PS5's order: the library and its compute queue before anything else in
// the process, ZiZc3/VLC src/platform_ps5.c).
bool load_api(video::HwdecProbe& probe, Api& api) {
  const int rc = probe.call("sceSysmoduleLoadModule", std::to_string(kSysmoduleVideodec2),
                            [&](std::string&) { return sceSysmoduleLoadModule(kSysmoduleVideodec2); });
  if (rc != 0) {
    probe.fail("sysmodule " + std::to_string(kSysmoduleVideodec2) + " " +
               hex(static_cast<std::uint32_t>(rc)));
    return false;
  }
  api.query_compute_memory = reinterpret_cast<decltype(api.query_compute_memory)>(
      &sceVideodec2QueryComputeMemoryInfo);
  api.allocate_compute_queue = reinterpret_cast<decltype(api.allocate_compute_queue)>(
      &sceVideodec2AllocateComputeQueue);
  api.release_compute_queue = reinterpret_cast<decltype(api.release_compute_queue)>(
      &sceVideodec2ReleaseComputeQueue);
  api.query_decoder_memory = reinterpret_cast<decltype(api.query_decoder_memory)>(
      &sceVideodec2QueryDecoderMemoryInfo);
  api.create_decoder =
      reinterpret_cast<decltype(api.create_decoder)>(&sceVideodec2CreateDecoder);
  api.delete_decoder =
      reinterpret_cast<decltype(api.delete_decoder)>(&sceVideodec2DeleteDecoder);
  api.decode = reinterpret_cast<decltype(api.decode)>(&sceVideodec2Decode);
  api.flush = reinterpret_cast<decltype(api.flush)>(&sceVideodec2Flush);
  api.reset = reinterpret_cast<decltype(api.reset)>(&sceVideodec2Reset);
  log_line("hwdec: libSceVideodec2 functions imported (native title)");
  return true;
}
#else
// The payload: the libraries are loaded with dlopen and the functions
// resolved by name through the payload SDK's runtime loader.
bool load_api(video::HwdecProbe& probe, Api& api) {
  if (!env_off("PROSPEROWEB_HWDEC_AGC", "0")) {
    for (const char* name : {"libSceAgcDriver.sprx", "libSceAgc.sprx"}) {
      load_library(probe, name);  // a failure is logged; the decoder may still work
    }
  } else {
    log_line("hwdec: GPU libraries not preloaded (PROSPEROWEB_HWDEC_AGC=0)");
  }
  void* sysmodule = load_library(probe, "libSceSysmodule.sprx");
  auto load_module = sysmodule ? reinterpret_cast<int (*)(std::uint16_t)>(
                                     dlsym(sysmodule, "sceSysmoduleLoadModule"))
                               : nullptr;
  if (load_module) {
    const int rc = probe.call("sceSysmoduleLoadModule", std::to_string(kSysmoduleVideodec2),
                              [&](std::string&) { return load_module(kSysmoduleVideodec2); });
    if (rc == video::HwdecProbe::kAbandoned) {
      return false;
    }
  } else {
    log_line("hwdec: sceSysmoduleLoadModule not found; loading the library directly");
  }
  void* lib = load_library(probe, "libSceVideodec2.sprx");
  if (!lib) {
    probe.fail("libSceVideodec2.sprx did not load");
    return false;
  }
  const bool ok =
      resolve(lib, "sceVideodec2QueryComputeMemoryInfo", api.query_compute_memory) &&
      resolve(lib, "sceVideodec2AllocateComputeQueue", api.allocate_compute_queue) &&
      resolve(lib, "sceVideodec2ReleaseComputeQueue", api.release_compute_queue) &&
      resolve(lib, "sceVideodec2QueryDecoderMemoryInfo", api.query_decoder_memory) &&
      resolve(lib, "sceVideodec2CreateDecoder", api.create_decoder) &&
      resolve(lib, "sceVideodec2DeleteDecoder", api.delete_decoder) &&
      resolve(lib, "sceVideodec2Decode", api.decode) &&
      resolve(lib, "sceVideodec2Flush", api.flush) && resolve(lib, "sceVideodec2Reset", api.reset);
  log_printf("hwdec: libSceVideodec2 functions %s", ok ? "resolved" : "MISSING");
  if (!ok) {
    probe.fail("libSceVideodec2 functions missing");
    return false;
  }
  return true;
}
#endif

bool probe_body(video::HwdecProbe& probe) {
  State& s = state();
  log_process_identity();
  Api& api = s.api;
  if (!load_api(probe, api)) {
    return false;
  }
  s.affinity = decoder_affinity();

  // One compute queue for the process: every decoder shares it.
  ComputeMemoryInfo info{};
  info.this_size = sizeof(info);
  int rc = probe.call("sceVideodec2QueryComputeMemoryInfo", "this_size " + hex(info.this_size),
                      [&](std::string& detail) {
                        const int r = api.query_compute_memory(&info);
                        detail = "cpu_gpu_size " + hex(info.cpu_gpu_size);
                        return r;
                      });
  if (rc != 0) {
    probe.fail("compute memory query " + hex(static_cast<std::uint32_t>(rc)));
    return false;
  }
  if (!queue_memory(probe, s.queue_mem, info.cpu_gpu_size)) {
    probe.fail("no direct memory for the compute queue");
    return false;
  }
  info.cpu_gpu = s.queue_mem.addr;
  info.cpu_gpu_size = s.queue_mem.len;
  ComputeConfigInfo config{};
  config.this_size = sizeof(config);
  void* queue = nullptr;
  rc = probe.call("sceVideodec2AllocateComputeQueue",
                  "config this_size " + hex(config.this_size) + " pipe 0 queue 0, memory " +
                      ptr(info.cpu_gpu) + " size " + hex(info.cpu_gpu_size),
                  [&](std::string& detail) {
                    const int r = api.allocate_compute_queue(&config, &info, &queue);
                    detail = "queue " + ptr(queue);
                    return r;
                  });
  if (rc != 0 || !queue) {
    if (rc != video::HwdecProbe::kAbandoned) {
      release(s.queue_mem);
    }
    probe.fail("compute queue " + hex(static_cast<std::uint32_t>(rc)));
    return false;
  }
  s.queue = queue;
  log_printf("hwdec: compute queue ready (%" PRIu64 " KiB)",
             static_cast<std::uint64_t>(info.cpu_gpu_size >> 10));
  return cache_memory_answers(probe);
}

// How long the decoder factory waits for a probe still running before it
// refuses the stream (the WebProcess decodes that one in software).
constexpr std::chrono::milliseconds kOpenWait{4000};
// How long the native title's main() waits for the startup probe.
[[maybe_unused]] constexpr std::chrono::milliseconds kNativeStartupWait{8000};

std::shared_ptr<video::HwdecProbe>& probe_slot() {
  static auto* probe = new std::shared_ptr<video::HwdecProbe>();  // never destroyed
  return *probe;
}

// The native title decodes in hardware unless PROSPEROWEB_HWDEC=0. The
// payload (websrv) build does not by default: in the hbldr payload context
// the decoder's first calls stopped the process for good on a console
// (docs/ps5-runtime.md, "Hardware decoding"); PROSPEROWEB_HWDEC=1 opts in.
bool hwdec_disabled() {
  if (env_off("PROSPEROWEB_VIDEO_DECODE", "webprocess")) {
    return true;
  }
  const char* value = std::getenv("PROSPEROWEB_HWDEC");
#if PROSPEROWEB_NATIVE_TITLE
  return value && !std::strcmp(value, "0");
#else
  return !value || std::strcmp(value, "1") != 0;
#endif
}

constexpr const char* kHwdecOffReason =
#if PROSPEROWEB_NATIVE_TITLE
    "PROSPEROWEB_VIDEO_DECODE=webprocess or PROSPEROWEB_HWDEC=0";
#else
    "payload build: off unless PROSPEROWEB_HWDEC=1; or PROSPEROWEB_VIDEO_DECODE=webprocess";
#endif

std::shared_ptr<video::HwdecProbe> ensure_probe(const char* why) {
  static std::once_flag once;
  std::call_once(once, [why] {
    auto probe = video::HwdecProbe::create(video::kHwdecCallLimit,
                                           [](const std::string& line) { log_line(line); });
    log_printf("hwdec: probing the hardware decoder in the background (%s; %s per call)", why,
               probe->limit_text().c_str());
    probe->start(probe_body);
    probe_slot() = probe;
  });
  return probe_slot();
}

}  // namespace

void hwdec_native_startup() {
#if PROSPEROWEB_NATIVE_TITLE
  if (hwdec_disabled()) {
    log_printf("hwdec: off (%s)", kHwdecOffReason);
    return;
  }
  // Before WebKit, SDL and every other thread: the decoder library and its
  // compute queue first, as VLC for PS5 does. Watched like any probe, so a
  // hang turns hardware decoding off instead of holding the splash.
  auto probe = ensure_probe("native title startup");
  const auto result = probe->wait(kNativeStartupWait);
  log_printf("hwdec: native startup probe %s%s%s", video::to_string(result),
             probe->reason().empty() ? "" : ": ", probe->reason().c_str());
#endif
}

void hwdec_startup_done() {
  if (hwdec_disabled()) {
    log_printf("hwdec: off (%s)", kHwdecOffReason);
    return;
  }
#if PROSPEROWEB_NATIVE_TITLE
  log_line("hwdec: compute queue created at startup (native title)");
  return;
#endif
  const char* when = std::getenv("PROSPEROWEB_HWDEC_PROBE");
  if (when && !std::strcmp(when, "startup")) {
    ensure_probe("after startup, PROSPEROWEB_HWDEC_PROBE=startup");
  } else {
    log_line("hwdec: the hardware decoder is probed on the first video stream");
  }
}

video::DecoderFactory hwdec_factory() {
  if (hwdec_disabled()) {
    return {};
  }
  const bool allow_hevc = !env_off("PROSPEROWEB_HWDEC_HEVC", "0");
  return [allow_hevc](const video::DecoderRequest& request,
                      std::string& reason) -> std::unique_ptr<video::DecoderBackend> {
    if (!video::hardware_profile_supported(request.config, allow_hevc, reason)) {
      return nullptr;
    }
    auto probe = ensure_probe("first video stream");
    const auto state = probe->wait(kOpenWait);
    if (state != video::HwdecProbe::State::Ready) {
      reason = state == video::HwdecProbe::State::Probing
                   ? "hardware decoder still starting"
                   : "hardware decoder unavailable: " + probe->reason();
      return nullptr;
    }
    return Videodec2Backend::create(request, reason);
  };
}

}  // namespace prosperoweb::frontend
