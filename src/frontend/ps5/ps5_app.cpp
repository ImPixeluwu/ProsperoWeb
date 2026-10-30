// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_app.h"

#include <algorithm>
#include <cstring>
#include <optional>

#include <WebKit/WKRunLoop.h>

#include "ps5browser/core/gamepad.h"
#include "ps5browser/core/page_scripts.h"
#include "ps5_log.h"

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
    case SDLK_DELETE: return 46;
    default: return 0;
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

  backbuffer_.assign(static_cast<size_t>(width_) * height_, 0xFF000000u);
  chrome_buffer_.assign(static_cast<size_t>(width_) * height_, 0u);

  // WebKit main run loop (also initializes WebKit2).
  WKRunLoopInitializeMain();

  std::vector<std::string> user_scripts = {core::kEmeDetectorScript,
                                           core::kGamepadKeyBridgeScript};
  factory_ = std::make_unique<Ps5EngineFactory>(kBaseDir, user_scripts,
                                                config_.user_agent);

  std::string chrome_url = "file://" + config_.chrome_dir + "/chrome.html";
  chrome_ = std::make_unique<Ps5ChromeUi>(
      factory_->context(), factory_->data_store(), chrome_url,
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
        if (auto* view = controller_->active_view()) {
          std::uint32_t code = dom_key_code(e.key.keysym.sym);
          view->key(code, dom_modifiers(SDL_GetModState()), e.type == SDL_KEYDOWN,
                    std::string());
        }
        break;
      }
      case SDL_TEXTINPUT: {
        if (auto* view = controller_->active_view()) {
          view->key(0, 0, true, e.text.text);
          view->key(0, 0, false, e.text.text);
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

  // L2/R2 analog triggers → L2/R2 buttons past half-press.
  bool l2 = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
  bool r2 = SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
  controller_->on_button(core::Button::L2, l2);
  controller_->on_button(core::Button::R2, r2);
}

std::vector<SDL_Rect> Ps5App::derive_chrome_rects() const {
  nlohmann::json state = controller_->chrome_state();
  std::vector<SDL_Rect> rects;

  // Explicit rects win if the state provides them.
  if (state.contains("chrome_rects") && state["chrome_rects"].is_array()) {
    for (const auto& r : state["chrome_rects"]) {
      rects.push_back(SDL_Rect{r.value("x", 0), r.value("y", 0), r.value("width", 0),
                               r.value("height", 0)});
    }
    return rects;
  }

  // Fallback compromise: top band, OSK band, and a full overlay for modals.
  bool chrome_visible = state.value("chrome_visible", true);
  if (chrome_visible) {
    rects.push_back(SDL_Rect{0, 0, width_, shell::ShellController::kChromeBandHeight});
  }
  if (state.contains("osk") && state["osk"].value("open", false)) {
    rects.push_back(SDL_Rect{0, height_ * 2 / 3, width_, height_ / 3});
  }
  bool menu_open = state.contains("menu") && state["menu"].value("open", false);
  bool notice = state.contains("notice") && state["notice"].is_object() &&
                !state["notice"].value("kind", nlohmann::json()).is_null();
  if (menu_open || notice) {
    rects.push_back(SDL_Rect{0, 0, width_, height_});  // modal: composite all
  }
  return rects;
}

void Ps5App::composite_and_present() {
  SDL_Surface* surface = SDL_GetWindowSurface(window_);
  if (!surface) {
    return;
  }

  // 1. Paint the active content view into the backbuffer.
  if (auto* view = controller_->active_view()) {
    auto* ev = static_cast<Ps5EngineView*>(view);
    DirtyRect dirty = ev->web().take_dirty();
    ev->web().paint(reinterpret_cast<unsigned char*>(backbuffer_.data()), width_,
                    height_, dirty.valid ? dirty : DirtyRect{true, 0, 0, width_, height_});
  }

  // 2. Paint the chrome view into its own buffer and overlay declared rects.
  std::vector<SDL_Rect> chrome_rects = derive_chrome_rects();
  if (!chrome_rects.empty()) {
    chrome_->web().paint(reinterpret_cast<unsigned char*>(chrome_buffer_.data()), width_,
                         height_, DirtyRect{true, 0, 0, width_, height_});
    for (const auto& r : chrome_rects) {
      int x0 = std::max(0, r.x);
      int y0 = std::max(0, r.y);
      int x1 = std::min(width_, r.x + r.w);
      int y1 = std::min(height_, r.y + r.h);
      for (int y = y0; y < y1; y++) {
        std::uint32_t* src = &chrome_buffer_[static_cast<size_t>(y) * width_];
        std::uint32_t* dst = &backbuffer_[static_cast<size_t>(y) * width_];
        for (int x = x0; x < x1; x++) {
          std::uint32_t px = src[x];
          if ((px >> 24) != 0) {  // only over non-transparent chrome pixels
            dst[x] = px;
          }
        }
      }
    }
  }

  // 3. Convert the ARGB32 backbuffer into the ABGR8888 window surface.
  if (SDL_MUSTLOCK(surface)) {
    SDL_LockSurface(surface);
  }
  auto* out = static_cast<std::uint32_t*>(surface->pixels);
  int stride = surface->pitch / 4;
  for (int y = 0; y < height_; y++) {
    const std::uint32_t* row = &backbuffer_[static_cast<size_t>(y) * width_];
    std::uint32_t* orow = &out[static_cast<size_t>(y) * stride];
    for (int x = 0; x < width_; x++) {
      orow[x] = argb_to_abgr(row[x]);
    }
  }
  if (SDL_MUSTLOCK(surface)) {
    SDL_UnlockSurface(surface);
  }
  SDL_UpdateWindowSurface(window_);
}

}  // namespace ps5browser::frontend
