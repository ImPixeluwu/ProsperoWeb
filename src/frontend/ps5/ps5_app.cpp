// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_app.h"

#include <algorithm>
#include <cstring>
#include <optional>

#include <WebKit/WKRunLoop.h>

#include "ps5browser/core/gamepad.h"
#include "ps5browser/core/page_scripts.h"
#include "ps5_log.h"
#include "ps5_runtime_env.h"

namespace ps5browser::frontend {

namespace {

constexpr const char* kBaseDir = "/data/ps5browser";
constexpr std::uint32_t kFrameMs = 16;  // ~60 Hz

std::optional<core::Button> sdl_button_to_core(SDL_GameControllerButton b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return core::Button::Cross;
    case SDL_CONTROLLER_BUTTON_B: return core::Button::Circle;
    case SDL_CONTROLLER_BUTTON_X: return core::Button::Square;
    case SDL_CONTROLLER_BUTTON_Y: return core::Button::Triangle;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return core::Button::L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return core::Button::R1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return core::Button::L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return core::Button::R3;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return core::Button::Up;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return core::Button::Down;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return core::Button::Left;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return core::Button::Right;
    case SDL_CONTROLLER_BUTTON_START: return core::Button::Options;
    case SDL_CONTROLLER_BUTTON_BACK: return core::Button::Share;
    case SDL_CONTROLLER_BUTTON_GUIDE: return core::Button::PS;
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return core::Button::Touchpad;
    default: return std::nullopt;
  }
}

std::uint32_t dom_key_code(SDL_Keycode k) {
  if (k >= SDLK_a && k <= SDLK_z) {
    return static_cast<std::uint32_t>('A' + (k - SDLK_a));
  }
  if (k >= SDLK_0 && k <= SDLK_9) {
    return static_cast<std::uint32_t>('0' + (k - SDLK_0));
  }
  switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 13;
    case SDLK_BACKSPACE: return 8;
    case SDLK_TAB: return 9;
    case SDLK_ESCAPE: return 27;
    case SDLK_SPACE: return 32;
    case SDLK_LEFT: return 37;
    case SDLK_UP: return 38;
    case SDLK_RIGHT: return 39;
    case SDLK_DOWN: return 40;
    case SDLK_PAGEUP: return 33;
    case SDLK_PAGEDOWN: return 34;
    case SDLK_END: return 35;
    case SDLK_HOME: return 36;
    case SDLK_INSERT: return 45;
    case SDLK_DELETE: return 46;
    default:
      if (k >= SDLK_F1 && k <= SDLK_F12) {
        return static_cast<std::uint32_t>(112 + (k - SDLK_F1));
      }
      return 0;
  }
}

std::uint32_t dom_modifiers(SDL_Keymod m) {
  std::uint32_t out = 0;
  if (m & KMOD_SHIFT) out |= shell::kModShift;
  if (m & KMOD_CTRL) out |= shell::kModControl;
  if (m & KMOD_ALT) out |= shell::kModAlt;
  if (m & KMOD_GUI) out |= shell::kModMeta;
  return out;
}

// ARGB32 (0xAARRGGBB) -> ABGR8888 (0xAABBGGRR): swap R and B.
inline std::uint32_t argb_to_abgr(std::uint32_t p) {
  return (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
}

// Premultiplied source-over: src + dst * (255 - src.alpha) / 255, per channel.
inline std::uint32_t blend_over(std::uint32_t src, std::uint32_t dst) {
  const std::uint32_t a = src >> 24;
  if (a == 0xFF) {
    return src;
  }
  if (a == 0) {
    return dst;
  }
  const std::uint32_t inv = 255 - a;
  auto scale = [inv](std::uint32_t c) {
    const std::uint32_t t = c * inv + 128;
    return (t + (t >> 8)) >> 8;  // c * inv / 255, rounded
  };
  std::uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const std::uint32_t s = (src >> shift) & 0xFF;
    const std::uint32_t d = (dst >> shift) & 0xFF;
    out |= std::min<std::uint32_t>(255, s + scale(d)) << shift;
  }
  return out;
}

}  // namespace

Ps5App::Ps5App() = default;

Ps5App::~Ps5App() {
  if (pad_) {
    SDL_GameControllerClose(pad_);
  }
  if (window_) {
    SDL_DestroyWindow(window_);
  }
  SDL_Quit();
}

int Ps5App::run() {
  log_open(std::string(kBaseDir) + "/log.txt");
  log_line("ps5browser UI starting");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
    log_printf("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }

  storage_ = std::make_unique<core::FileStorage>(kBaseDir);
  config_ = Ps5Config::load(*storage_);
  width_ = config_.width;
  height_ = config_.height;

  window_ = SDL_CreateWindow("ps5browser", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, width_, height_, 0);
  if (!window_) {
    log_printf("SDL_CreateWindow failed: %s", SDL_GetError());
    return 1;
  }
  // Printable keyboard input arrives as SDL_TEXTINPUT (see pump_sdl_events).
  SDL_StartTextInput();

  page_buffer_.assign(static_cast<size_t>(width_) * height_, 0xFF000000u);
  chrome_buffer_.assign(static_cast<size_t>(width_) * height_, 0u);

  // fontconfig, TLS roots and language for WebKit; must precede WebKit
  // initialisation and the first auxiliary process spawn.
  setup_runtime_env(kBaseDir, config_.language);

  // WebKit main run loop (also initializes WebKit2).
  WKRunLoopInitializeMain();

  std::vector<std::string> user_scripts = {core::kEmeDetectorScript,
                                           core::kGamepadKeyBridgeScript};
  factory_ = std::make_unique<Ps5EngineFactory>(kBaseDir, user_scripts,
                                                config_.user_agent);

  std::string chrome_url = "file://" + config_.chrome_dir + "/chrome.html";
  chrome_ = std::make_unique<Ps5ChromeUi>(
      factory_->context(), factory_->data_store(), chrome_url, width_, height_,
      [this](const nlohmann::json& e) {
        if (controller_) {
          controller_->on_chrome_event(e);
        }
      });

  shell::ShellController::Config cfg;
  cfg.width = width_;
  cfg.height = height_;
  cfg.home_url = config_.home_url;
  cfg.user_agent = config_.user_agent;
  cfg.language = config_.language;
  controller_ = std::make_unique<shell::ShellController>(*factory_, *chrome_, *storage_,
                                                         cfg);
  controller_->start();

  running_ = true;
  last_frame_ticks_ = SDL_GetTicks64();
  WKRunLoopCallOnMainThread(&Ps5App::frame_trampoline, this);
  WKRunLoopRunMain();

  log_line("ps5browser UI stopped");
  log_close();
  return 0;
}

void Ps5App::frame_trampoline(void* self) {
  static_cast<Ps5App*>(self)->frame();
}

void Ps5App::frame() {
  std::uint64_t now = SDL_GetTicks64();
  float dt = static_cast<float>(now - last_frame_ticks_) / 1000.0f;
  last_frame_ticks_ = now;

  pump_sdl_events();
  handle_axis_motion();
  controller_->tick(dt);
  composite_and_present();

  if (controller_->quit_requested()) {
    controller_->shutdown();
    running_ = false;
    WKRunLoopStopMain();
    return;
  }

  std::uint64_t spent = SDL_GetTicks64() - now;
  if (spent < kFrameMs) {
    SDL_Delay(static_cast<std::uint32_t>(kFrameMs - spent));
  }
  WKRunLoopCallOnMainThread(&Ps5App::frame_trampoline, this);
}

void Ps5App::pump_sdl_events() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        if (controller_) {
          controller_->on_button(core::Button::PS, true);
        }
        break;
      case SDL_CONTROLLERDEVICEADDED:
        if (!pad_) {
          pad_ = SDL_GameControllerOpen(e.cdevice.which);
        }
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        if (pad_) {
          SDL_GameControllerClose(pad_);
          pad_ = nullptr;
        }
        break;
      case SDL_CONTROLLERBUTTONDOWN:
        handle_controller_button(e.cbutton, true);
        break;
      case SDL_CONTROLLERBUTTONUP:
        handle_controller_button(e.cbutton, false);
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        // Key events carry no text: printable characters arrive separately
        // as SDL_TEXTINPUT and are inserted below, so they are not typed twice.
        std::uint32_t code = dom_key_code(e.key.keysym.sym);
        auto* view = controller_->active_view();
        if (view && code != 0) {
          view->key(code, dom_modifiers(SDL_GetModState()), e.type == SDL_KEYDOWN,
                    std::string());
        }
        break;
      }
      case SDL_TEXTINPUT: {
        if (auto* view = controller_->active_view()) {
          view->insert_text(e.text.text);
        }
        break;
      }
      default:
        break;
    }
  }
}

void Ps5App::handle_controller_button(const SDL_ControllerButtonEvent& e, bool pressed) {
  if (auto b = sdl_button_to_core(static_cast<SDL_GameControllerButton>(e.button))) {
    controller_->on_button(*b, pressed);
  }
}

void Ps5App::handle_axis_motion() {
  if (!pad_) {
    controller_->on_sticks(0, 0, 0, 0);
    trigger_edge(core::Button::L2, false, l2_down_);
    trigger_edge(core::Button::R2, false, r2_down_);
    return;
  }
  auto norm = [this](SDL_GameControllerAxis axis) {
    float v = SDL_GameControllerGetAxis(pad_, axis) / 32767.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (v > -0.15f && v < 0.15f) v = 0.0f;  // deadzone
    return v;
  };
  controller_->on_sticks(norm(SDL_CONTROLLER_AXIS_LEFTX), norm(SDL_CONTROLLER_AXIS_LEFTY),
                         norm(SDL_CONTROLLER_AXIS_RIGHTX),
                         norm(SDL_CONTROLLER_AXIS_RIGHTY));

  // L2/R2 analog triggers act as buttons past half-press. Report only the
  // press/release edges: the shell runs an action on every press.
  trigger_edge(core::Button::L2,
               SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000,
               l2_down_);
  trigger_edge(core::Button::R2,
               SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000,
               r2_down_);
}

void Ps5App::trigger_edge(core::Button button, bool down, bool& was_down) {
  if (down != was_down) {
    was_down = down;
    controller_->on_button(button, down);
  }
}

std::vector<SDL_Rect> Ps5App::derive_chrome_rects() {
  // The chrome renders asynchronously (WebProcess round trip), so its pixels
  // lag the shell state by a frame or two: keep composing the areas of the
  // last few frames, or a moving pointer or a closing panel would be cut off.
  std::vector<SDL_Rect> current;
  for (const auto& r : controller_->chrome_rects()) {
    current.push_back(SDL_Rect{r.x, r.y, r.width, r.height});
  }
  std::vector<SDL_Rect> rects = current;
  for (const auto& old : recent_chrome_rects_) {
    rects.insert(rects.end(), old.begin(), old.end());
  }
  recent_chrome_rects_.push_front(std::move(current));
  if (recent_chrome_rects_.size() > kChromeRectHistory) {
    recent_chrome_rects_.pop_back();
  }
  return rects;
}

void Ps5App::paint_chrome() {
  DirtyRect dirty = chrome_->web().take_dirty();
  if (!chrome_painted_) {
    dirty = DirtyRect{true, 0, 0, width_, height_};
    chrome_painted_ = true;
  }
  if (!dirty.valid) {
    return;
  }
  // WebKit blends its backing store over the target (source-over), so the
  // area must start out transparent or old chrome pixels would show through.
  const int x0 = std::clamp(dirty.x, 0, width_);
  const int x1 = std::clamp(dirty.x + dirty.width, 0, width_);
  const int y0 = std::clamp(dirty.y, 0, height_);
  const int y1 = std::clamp(dirty.y + dirty.height, 0, height_);
  for (int y = y0; y < y1; y++) {
    std::uint32_t* row = &chrome_buffer_[static_cast<size_t>(y) * width_];
    std::fill(row + x0, row + x1, 0u);
  }
  chrome_->web().paint(reinterpret_cast<unsigned char*>(chrome_buffer_.data()), width_,
                       height_, dirty);
}

void Ps5App::paint_content() {
  auto* view = static_cast<Ps5EngineView*>(controller_->active_view());
  const std::uint64_t tab = controller_->tabs().active_id().value_or(0);
  const int top = std::clamp(controller_->content_top(), 0, height_);
  const int content_height = height_ - top;
  DirtyRect dirty = view ? view->web().take_dirty() : DirtyRect{};

  // A different view or geometry invalidates the whole cache; otherwise only
  // what WebKit reported changed is repainted (nothing on an idle page).
  const bool full = tab != painted_tab_ || top != painted_top_;
  if (full) {
    std::fill(page_buffer_.begin(), page_buffer_.end(), 0xFF000000u);
    painted_tab_ = tab;
    painted_top_ = top;
    dirty = DirtyRect{true, 0, 0, width_, content_height};
  }
  if (!view || !dirty.valid || content_height <= 0) {
    return;
  }
  // The view is width_ x content_height and sits at window y = top, the same
  // offset ShellController::to_view() removes from pointer coordinates.
  auto* origin = reinterpret_cast<unsigned char*>(page_buffer_.data() +
                                                  static_cast<size_t>(top) * width_);
  view->web().paint(origin, width_, content_height, dirty);
}

void Ps5App::composite_and_present() {
  SDL_Surface* surface = SDL_GetWindowSurface(window_);
  if (!surface) {
    return;
  }

  // 1. Bring the content and chrome caches up to date (dirty areas only).
  paint_content();
  paint_chrome();
  const std::vector<SDL_Rect> chrome_rects = derive_chrome_rects();

  // 2. Compose into the ABGR8888 window surface: the page, with the
  //    premultiplied chrome blended over it inside the chrome rects. Neither
  //    cache is written here, so a closed overlay leaves no trace.
  if (SDL_MUSTLOCK(surface)) {
    SDL_LockSurface(surface);
  }
  auto* out = static_cast<std::uint32_t*>(surface->pixels);
  const int stride = surface->pitch / 4;
  std::vector<std::pair<int, int>> spans;
  for (int y = 0; y < height_; y++) {
    const std::uint32_t* page = &page_buffer_[static_cast<size_t>(y) * width_];
    const std::uint32_t* chrome = &chrome_buffer_[static_cast<size_t>(y) * width_];
    std::uint32_t* orow = &out[static_cast<size_t>(y) * stride];

    // Merge the chrome rects crossing this row into disjoint spans, so no
    // pixel is blended twice where rects overlap.
    spans.clear();
    for (const auto& r : chrome_rects) {
      if (y >= r.y && y < r.y + r.h) {
        const int x0 = std::max(0, r.x);
        const int x1 = std::min(width_, r.x + r.w);
        if (x0 < x1) {
          spans.emplace_back(x0, x1);
        }
      }
    }
    std::sort(spans.begin(), spans.end());
    int x = 0;
    for (std::size_t i = 0; i < spans.size();) {
      int s0 = spans[i].first;
      int s1 = spans[i].second;
      for (++i; i < spans.size() && spans[i].first <= s1; ++i) {
        s1 = std::max(s1, spans[i].second);
      }
      for (; x < s0; x++) {
        orow[x] = argb_to_abgr(page[x]);
      }
      for (; x < s1; x++) {
        orow[x] = argb_to_abgr(blend_over(chrome[x], page[x]));
      }
    }
    for (; x < width_; x++) {
      orow[x] = argb_to_abgr(page[x]);
    }
  }
  if (SDL_MUSTLOCK(surface)) {
    SDL_UnlockSurface(surface);
  }
  SDL_UpdateWindowSurface(window_);
}

}  // namespace ps5browser::frontend
