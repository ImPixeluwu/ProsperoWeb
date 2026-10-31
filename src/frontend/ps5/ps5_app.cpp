// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_app.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>

#include <unistd.h>

#include <WebKit/WKRunLoop.h>

#include "compose.h"
#include "ps5browser/core/gamepad.h"
#include "ps5browser/core/page_scripts.h"
#include "ps5_diagnostics.h"
#include "ps5_log.h"
#include "ps5_runtime_env.h"

namespace ps5browser::frontend {

namespace {

// Console diagnostics: reports the first DOM pointer events of each document
// (type, target, detail, buttons, isTrusted, defaultPrevented) through the
// "ps5browser" handler as {type: 'input_trace'}; Ps5EngineView logs them.
// Shows whether a press/release pair became a click and what got focus.
constexpr const char* kInputTraceScript = R"JS((function () {
  'use strict';
  if (window !== window.top) return;
  var left = 24;
  function desc(e) {
    if (!e || !e.tagName) return String(e && e.nodeName);
    var d = e.tagName.toLowerCase();
    if (e.id) d += '#' + e.id;
    return d;
  }
  ['pointerdown', 'mousedown', 'mouseup', 'click', 'dblclick', 'focusin'].forEach(function (t) {
    document.addEventListener(t, function (ev) {
      if (left <= 0) return;
      left--;
      var target = ev.target;
      setTimeout(function () {
        try {
          window.webkit.messageHandlers.ps5browser.postMessage({
            type: 'input_trace', ev: t, target: desc(target),
            x: ev.clientX === undefined ? -1 : ev.clientX, y: ev.clientY === undefined ? -1 : ev.clientY,
            detail: ev.detail === undefined ? -1 : ev.detail, buttons: ev.buttons === undefined ? -1 : ev.buttons,
            trusted: ev.isTrusted, prevented: ev.defaultPrevented,
            active: desc(document.activeElement)
          });
        } catch (e) {}
      }, 0);
    }, true);
  });
})();
)JS";

constexpr const char* kBaseDir = kPs5DataDir;

std::string tmp_path(const std::string& name) { return std::string(kBaseDir) + "/tmp/" + name; }
// Milliseconds since `start` (an SDL_GetPerformanceCounter() value).
double ms_since(std::uint64_t start) {
  static const double ms_per_tick = 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
  return static_cast<double>(SDL_GetPerformanceCounter() - start) * ms_per_tick;
}

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

}  // namespace

Ps5App::Ps5App() = default;

Ps5App::~Ps5App() {
  WkWebView::set_damage_hook(nullptr, nullptr);
  presenter_.reset();     // before the window goes
  audio_server_.reset();  // before SDL_Quit
  if (pad_) {
    SDL_GameControllerClose(pad_);
  }
  if (window_) {
    SDL_DestroyWindow(window_);
  }
  SDL_Quit();
}

int Ps5App::run() {
  log_open(std::string(kBaseDir) + "/log.txt");  // no-op: main() opened it
  log_line("ps5browser UI starting");

  log_line("SDL_Init: calling");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
    log_printf("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  log_printf("SDL_Init: ok, video driver %s",
             SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");

  // Before the WebProcess is spawned, so its media can connect at once.
  audio_server_ = Ps5AudioServer::start();
  if (const char* test = std::getenv("PS5BROWSER_AUDIO_TEST");
      audio_server_ && test && !std::strcmp(test, "1")) {
    audio_server_->play_test_tone();
  }

  storage_ = std::make_unique<core::FileStorage>(kBaseDir);
  config_ = Ps5Config::load(*storage_);
  width_ = config_.width;
  height_ = config_.height;

  log_printf("SDL_CreateWindow: calling %dx%d", width_, height_);
  window_ = SDL_CreateWindow("ps5browser", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, width_, height_, 0);
  if (!window_) {
    log_printf("SDL_CreateWindow failed: %s", SDL_GetError());
    return 1;
  }
  log_line("SDL_CreateWindow: ok");
  // Before any WebKit work: proves the display path independently of
  // WebKit painting (a WKPagePaint without a WebProcess leaves zeros).
  draw_splash();
  // Printable keyboard input arrives as SDL_TEXTINPUT (see pump_sdl_events).
  // Without the hint SDL_StartTextInput() would also open the system
  // keyboard dialog (SDL's PS5 driver implements the screen keyboard with
  // sceImeDialog); Ps5ImeTextInput opens it on demand instead.
  SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
  SDL_StartTextInput();

  page_buffer_.assign(static_cast<size_t>(width_) * height_, 0xFF000000u);
  chrome_buffer_.assign(static_cast<size_t>(width_) * height_, 0u);

  // main() set the runtime defaults for every role; the language comes from
  // config.json and must precede WebKit initialisation and the first
  // auxiliary process spawn (which copies the environment).
  apply_language_env(config_.language);

  // WebKit main run loop (also initializes WebKit2).
  log_line("WKRunLoopInitializeMain: calling");
  WKRunLoopInitializeMain();
  log_line("WKRunLoopInitializeMain: ok");

  std::vector<std::string> user_scripts = {core::kEmeDetectorScript,
                                           core::kFocusReporterScript,
                                           core::kGamepadKeyBridgeScript,
                                           kInputTraceScript};
  factory_ = std::make_unique<Ps5EngineFactory>(kBaseDir, user_scripts,
                                                config_.user_agent);
  log_line("WebKit: context and data store created");

  const std::string chrome_path = config_.chrome_dir + "/chrome.html";
  std::string chrome_url = "file://" + chrome_path;
  if (access(chrome_path.c_str(), R_OK) != 0) {
    log_printf("chrome: %s is missing (%s); reinstall the chrome/ directory",
               chrome_path.c_str(), std::strerror(errno));
    chrome_url.clear();  // Ps5ChromeUi shows its built-in fallback
  }
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
  cfg.diagnostics_url = "file://" + config_.chrome_dir + "/diag.html";
  if (use_system_ime(config_.osk)) {
    ime_input_ = std::make_unique<Ps5ImeTextInput>();
    cfg.text_input = ime_input_.get();
    log_line("text input: system keyboard dialog (built-in OSK as fallback)");
  } else {
    log_line("text input: built-in OSK (PS5BROWSER_OSK / config osk = builtin)");
  }
  controller_ = std::make_unique<shell::ShellController>(*factory_, *chrome_, *storage_,
                                                         cfg);
  log_line("shell: starting");
  controller_->start();
  log_line("shell: started");

  running_ = true;
  last_frame_ticks_ = SDL_GetTicks64();
  {
    const char* env = std::getenv("PS5BROWSER_PRESENT_THREAD");
    const bool threaded = !(env && !std::strcmp(env, "0"));
    presenter_ = std::make_unique<Ps5Presenter>(window_, threaded, [this] { request_frame(); });
    log_printf("present: %s, frames paced by flips and a %u ms tick",
               threaded ? "presenter thread" : "main thread (PS5BROWSER_PRESENT_THREAD=0)",
               Ps5Presenter::kTickMs);
  }
  WkWebView::set_damage_hook(
      [](void* self) { static_cast<Ps5App*>(self)->request_frame(); }, this);
  request_frame();
  log_line("WKRunLoopRunMain: entering");
  WKRunLoopRunMain();

  log_line("ps5browser UI stopped");
  log_close();
  return 0;
}

void Ps5App::draw_splash() {
  const std::uint64_t t0 = SDL_GetTicks64();
  SDL_Surface* surface = SDL_GetWindowSurface(window_);
  if (!surface) {
    log_printf("splash: SDL_GetWindowSurface failed: %s", SDL_GetError());
    return;
  }
  // Opaque dark blue, an orange top band and a white centre square: three
  // colours that cannot be mistaken for an empty (black) buffer.
  const SDL_Rect band{0, 0, surface->w, std::min(96, surface->h)};
  const int side = std::min({200, surface->w, surface->h});
  const SDL_Rect square{(surface->w - side) / 2, (surface->h - side) / 2, side, side};
  SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, 0x1A, 0x2B, 0x4C, 0xFF));
  SDL_FillRect(surface, &band, SDL_MapRGBA(surface->format, 0xFF, 0x8C, 0x00, 0xFF));
  SDL_FillRect(surface, &square, SDL_MapRGBA(surface->format, 0xFF, 0xFF, 0xFF, 0xFF));
  if (SDL_UpdateWindowSurface(window_) != 0) {
    log_printf("splash: SDL_UpdateWindowSurface failed: %s", SDL_GetError());
    return;
  }
  log_printf("splash: filled and flipped (%llu ms)",
             static_cast<unsigned long long>(SDL_GetTicks64() - t0));

  const char* hold = std::getenv("PS5BROWSER_SPLASH_HOLD_MS");
  const long hold_ms = hold ? std::strtol(hold, nullptr, 10) : 0;
  if (hold_ms > 0) {
    log_printf("splash: holding %ld ms (PS5BROWSER_SPLASH_HOLD_MS)", hold_ms);
    SDL_Delay(static_cast<std::uint32_t>(hold_ms));
    log_line("splash: hold done");
  }
}

void Ps5App::frame_trampoline(void* self) {
  static_cast<Ps5App*>(self)->frame();
}

void Ps5App::request_frame() {
  // At most one frame queued on the run loop: a slow frame does not pile up
  // ticks behind it.
  if (!frame_queued_.exchange(true, std::memory_order_acq_rel)) {
    WKRunLoopCallOnMainThread(&Ps5App::frame_trampoline, this);
  }
}

void Ps5App::frame() {
  frame_queued_.store(false, std::memory_order_release);
  if (!running_) {
    return;
  }
  std::uint64_t now = SDL_GetTicks64();
  if (frames_ == 0) {
    log_line("frame: first frame");
  }
  frames_++;
  stats_.frames++;
  float dt = static_cast<float>(now - last_frame_ticks_) / 1000.0f;
  last_frame_ticks_ = now;

  // Before SDL_PollEvent(): SDL's PS5 driver watches the same dialog there
  // and would otherwise consume its result first (see Ps5SystemIme).
  if (ime_input_) {
    ime_input_->poll();
  }
  pump_sdl_events();
  handle_axis_motion();
  if (share_down_ && !share_long_fired_ && now - share_down_ticks_ >= kShareLongPressMs) {
    share_long_fired_ = true;
    screenshot_pending_ = true;
    log_line("screenshot: requested by a Share long press");
  }
  poll_diagnostic_requests(now);
  // Page and chrome script messages (see WkWebView::poll_all()).
  WkWebView::poll_all();
  controller_->tick(dt);
  composite_and_present();
  collect_and_log_stats(now);

  if (controller_->quit_requested()) {
    controller_->shutdown();
    running_ = false;
    presenter_->stop();
    WKRunLoopStopMain();
  }
  // The next frame comes from the presenter (after a flip, or its tick):
  // the run loop stays free for WebKit's IPC in between.
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
        const bool is_return = e.key.keysym.sym == SDLK_RETURN;
        if (ime_input_ && ime_input_->ime().is_open()) {
          if (is_return && e.type == SDL_KEYDOWN) {
            ime_input_->ime().note_sdl_return();
            swallow_return_up_ = true;
          }
          break;  // the dialog owns text entry
        }
        if (is_return && e.type == SDL_KEYUP && swallow_return_up_) {
          swallow_return_up_ = false;
          break;
        }
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
        if (ime_input_ && ime_input_->ime().is_open()) {
          break;
        }
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
  auto b = sdl_button_to_core(static_cast<SDL_GameControllerButton>(e.button));
  if (!b) {
    return;
  }
  if (*b == core::Button::Share) {
    // Decided on release: a long press (see frame()) is a screen dump and
    // never reaches the shell; a short one is delivered as a whole.
    if (pressed) {
      share_down_ = true;
      share_long_fired_ = false;
      share_down_ticks_ = SDL_GetTicks64();
    } else if (share_down_) {
      share_down_ = false;
      if (!share_long_fired_) {
        controller_->on_button(*b, true);
        controller_->on_button(*b, false);
      }
    }
    return;
  }
  controller_->on_button(*b, pressed);
}

void Ps5App::poll_diagnostic_requests(std::uint64_t now) {
  if (now - last_request_poll_ticks_ < kRequestPollMs) {
    return;
  }
  last_request_poll_ticks_ = now;
  if (take_request_file(tmp_path(kScreenshotRequestFile))) {
    log_printf("screenshot: requested by %s", kScreenshotRequestFile);
    screenshot_pending_ = true;
  }
  if (take_request_file(tmp_path(kProbeRequestFile))) {
    log_printf("probe: requested by %s", kProbeRequestFile);
    run_form_probe();
  }
}

void Ps5App::dump_screen(SDL_Surface* surface) {
  screenshot_pending_ = false;
  const std::uint64_t t0 = SDL_GetTicks64();
  // The window surface is ABGR8888 (R, G, B, A bytes); convert anything else.
  SDL_Surface* converted = nullptr;
  SDL_Surface* src = surface;
  if (surface->format->format != SDL_PIXELFORMAT_ABGR8888) {
    converted = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
    if (!converted) {
      log_printf("screenshot: converting the surface failed: %s", SDL_GetError());
      return;
    }
    src = converted;
  }
  const std::string png = encode_png_rgbx(static_cast<const std::uint8_t*>(src->pixels),
                                          src->w, src->h, src->pitch);
  const int w = src->w;
  const int h = src->h;
  if (converted) {
    SDL_FreeSurface(converted);
  }
  if (png.empty()) {
    log_line("screenshot: PNG encoding failed");
    return;
  }
  const std::string path = tmp_path("screen-" + std::to_string(t0) + ".png");
  const bool ok = write_file_atomically(path, png);
  const bool latest_ok = write_file_atomically(tmp_path(kScreenLatestFile), png);
  if (ok) {
    log_printf("screenshot: written %s (%llu ms, %dx%d, %zu bytes%s)", path.c_str(),
               static_cast<unsigned long long>(SDL_GetTicks64() - t0), w, h, png.size(),
               latest_ok ? ", also screen-latest.png" : "");
  }
}

void Ps5App::run_form_probe() {
  auto* view = static_cast<Ps5EngineView*>(controller_->active_view());
  if (!view) {
    log_line("probe: no active page");
    return;
  }
  const std::uint64_t t0 = SDL_GetTicks64();
  view->web().evaluate(form_probe_script(), [t0](std::optional<std::string> result) {
    if (!result) {
      log_line("probe: the page script returned no result (failed, or no page process)");
      return;
    }
    const auto doc = nlohmann::json::parse(*result, nullptr, /*allow_exceptions=*/false);
    std::string text = *result;
    if (!doc.is_discarded()) {
      text = doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    text += '\n';
    const std::string path = tmp_path("probe-" + std::to_string(t0) + ".json");
    const bool ok = write_file_atomically(path, text);
    write_file_atomically(tmp_path(kProbeLatestFile), text);
    if (ok) {
      const auto controls = doc.is_object() ? doc.find("controlCount") : doc.end();
      log_printf("probe: written %s (%llu ms, %s controls, %s)", path.c_str(),
                 static_cast<unsigned long long>(SDL_GetTicks64() - t0),
                 controls != doc.end() ? controls->dump().c_str() : "?",
                 doc.is_object() ? doc.value("url", std::string("?")).c_str() : "?");
    }
  });
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

std::vector<display::Rect> Ps5App::derive_chrome_rects() {
  // The chrome renders asynchronously (WebProcess round trip), so its pixels
  // lag the shell state by a frame or two: keep composing the areas of the
  // last few frames, or a moving pointer or a closing panel would be cut off.
  std::vector<display::Rect> current;
  for (const auto& r : controller_->chrome_rects()) {
    current.push_back(display::Rect{r.x, r.y, r.width, r.height});
  }
  std::vector<display::Rect> rects = current;
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
  display::Region dirty = chrome_->web().take_dirty();
  webkit_painted_ = webkit_painted_ || !dirty.empty();
  if (!chrome_painted_) {
    dirty.clear();
    dirty.add(display::Rect{0, 0, width_, height_});
    chrome_painted_ = true;
  }
  dirty.clip(display::Rect{0, 0, width_, height_});
  for (const display::Rect& r : dirty.rects()) {
    // WebKit blends its backing store over the target (source-over), so the
    // area must start out transparent or old chrome pixels would show through.
    for (int y = r.y; y < r.bottom(); y++) {
      std::uint32_t* row = &chrome_buffer_[static_cast<size_t>(y) * width_];
      std::fill(row + r.x, row + r.right(), 0u);
    }
    const std::uint64_t t0 = SDL_GetPerformanceCounter();
    chrome_->web().paint(reinterpret_cast<unsigned char*>(chrome_buffer_.data()), width_,
                         height_, r);
    stats_.chrome_paint.add(ms_since(t0), r.area());
  }
  // Visible only inside the chrome rects; compose_damage() clips it.
  pending_chrome_damage_.add(dirty);
}

void Ps5App::paint_content() {
  auto* view = static_cast<Ps5EngineView*>(controller_->active_view());
  const std::uint64_t tab = controller_->tabs().active_id().value_or(0);
  const int top = std::clamp(controller_->content_top(), 0, height_);
  const int content_height = height_ - top;
  display::Region dirty = view ? view->web().take_dirty() : display::Region{};
  webkit_painted_ = webkit_painted_ || !dirty.empty();

  // A different view or geometry invalidates the whole cache (and screen);
  // otherwise only what WebKit reported changed is repainted (nothing on an
  // idle page).
  const bool full = tab != painted_tab_ || top != painted_top_;
  if (full) {
    std::fill(page_buffer_.begin(), page_buffer_.end(), 0xFF000000u);
    painted_tab_ = tab;
    painted_top_ = top;
    dirty.clear();
    dirty.add(display::Rect{0, 0, width_, content_height});
    pending_damage_.add(display::Rect{0, 0, width_, height_});
  }
  if (!view || dirty.empty() || content_height <= 0) {
    return;
  }
  dirty.clip(display::Rect{0, 0, width_, content_height});
  // The view is width_ x content_height and sits at window y = top, the same
  // offset ShellController::to_view() removes from pointer coordinates.
  auto* origin = reinterpret_cast<unsigned char*>(page_buffer_.data() +
                                                  static_cast<size_t>(top) * width_);
  for (const display::Rect& r : dirty.rects()) {
    const std::uint64_t t0 = SDL_GetPerformanceCounter();
    view->web().paint(origin, width_, content_height, r);
    stats_.page_paint.add(ms_since(t0), r.area());
  }
  dirty.translate(0, top);
  pending_damage_.add(dirty);
}

void Ps5App::collect_and_log_stats(std::uint64_t now) {
  if (auto* view = static_cast<Ps5EngineView*>(controller_->active_view())) {
    stats_.page_needs_display.merge(view->web().take_needs_display_stats());
  }
  stats_.chrome_needs_display.merge(chrome_->web().take_needs_display_stats());
  if (stats_start_ticks_ == 0) {
    stats_start_ticks_ = now;
    return;
  }
  const std::uint64_t elapsed = now - stats_start_ticks_;
  if (elapsed < kStatsWindowMs) {
    return;
  }
  if (presenter_) {
    stats_.present.merge(presenter_->take_stats());
  }
  if (stats_.active()) {
    log_line(stats_.format(static_cast<double>(elapsed) / 1000.0));
  }
  stats_ = display::FrameStats{};
  stats_start_ticks_ = now;
}

display::Region Ps5App::take_frame_damage(const std::vector<display::Rect>& chrome_rects) {
  display::Region damage = pending_damage_;
  // A chrome rect that appeared, moved or went away changes what is blended
  // where: recompose the old and the new areas.
  if (chrome_rects != composed_chrome_rects_) {
    for (const display::Rect& r : composed_chrome_rects_) {
      damage.add(r);
    }
    for (const display::Rect& r : chrome_rects) {
      damage.add(r);
    }
  }
  // Chrome repaints show only inside the chrome rects.
  for (const display::Rect& d : pending_chrome_damage_.rects()) {
    for (const display::Rect& c : chrome_rects) {
      damage.add(display::intersect(d, c));
    }
  }
  damage.clip(display::Rect{0, 0, width_, height_});
  pending_damage_.clear();
  pending_chrome_damage_.clear();
  composed_chrome_rects_ = chrome_rects;
  return damage;
}

void Ps5App::composite_and_present() {
  SDL_Surface* surface = SDL_GetWindowSurface(window_);
  if (!surface_logged_) {
    surface_logged_ = true;
    if (surface) {
      log_printf("SDL_GetWindowSurface: %dx%d %s pitch %d", surface->w, surface->h,
                 SDL_GetPixelFormatName(surface->format->format), surface->pitch);
    } else {
      log_printf("SDL_GetWindowSurface failed: %s", SDL_GetError());
    }
  }
  if (!surface) {
    return;
  }

  // 1. Bring the content and chrome caches up to date (dirty areas only);
  //    their damage accumulates until it is composed.
  paint_content();
  paint_chrome();
  const std::vector<display::Rect> chrome_rects = derive_chrome_rects();

  // Keep the splash until WebKit has painted something real; composing the
  // still empty caches would replace it with black.
  if (!splash_replaced_) {
    if (!webkit_painted_) {
      return;
    }
    splash_replaced_ = true;
    pending_damage_.add(display::Rect{0, 0, width_, height_});
    log_printf("splash: replaced by the first WebKit frame (frame %llu)",
               static_cast<unsigned long long>(frames_));
  }

  // 2. The surface belongs to the presenter while a flip is in flight; the
  //    damage waits for the next frame (which the flip completion requests).
  if (presenter_->busy()) {
    if (!pending_damage_.empty() || !pending_chrome_damage_.empty()) {
      stats_.present_waits++;
    }
    return;
  }
  const display::Region damage = take_frame_damage(chrome_rects);
  if (damage.empty()) {
    if (screenshot_pending_) {
      dump_screen(surface);  // the surface holds the last composed frame
    }
    return;
  }

  // 3. Compose the damaged rects into the ABGR8888 window surface: the page,
  //    with the premultiplied chrome blended over it inside the chrome rects.
  //    Neither cache is written here, so a closed overlay leaves no trace.
  const std::uint64_t compose_t0 = SDL_GetPerformanceCounter();
  if (SDL_MUSTLOCK(surface)) {
    SDL_LockSurface(surface);
  }
  const display::Layers layers{page_buffer_.data(), chrome_buffer_.data(), width_, height_};
  const int out_w = std::min(width_, surface->w);
  const int out_h = std::min(height_, surface->h);
  std::vector<SDL_Rect> present_rects;
  for (const display::Rect& r : damage.rects()) {
    const display::Rect clipped = display::intersect(r, display::Rect{0, 0, out_w, out_h});
    if (clipped.empty()) {
      continue;
    }
    display::compose_rect(layers, chrome_rects, clipped, static_cast<std::uint32_t*>(surface->pixels),
                          surface->pitch / 4);
    present_rects.push_back(SDL_Rect{clipped.x, clipped.y, clipped.w, clipped.h});
  }
  stats_.compose.add(ms_since(compose_t0), damage.area());
  // The composed frame, before the flip hands the buffer to the display.
  if (screenshot_pending_) {
    dump_screen(surface);
  }
  if (SDL_MUSTLOCK(surface)) {
    SDL_UnlockSurface(surface);
  }
  if (present_rects.empty()) {
    return;
  }
  // Under the SDL PS5 driver this de-tiles the rects' bounding box and
  // submits the flip (sceVideoOutSubmitFlip), see ps5_presenter.h.
  stats_.damage_rects += static_cast<std::uint32_t>(present_rects.size());
  presenter_->submit(std::move(present_rects));
}

}  // namespace ps5browser::frontend
