// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_engine_view.h"

#include <WebKit/WKContext.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKContextConfigurationPlayStation.h>
#include <WebKit/WKContextPrivate.h>
#include <WebKit/WKEvent.h>
#include <WebKit/WKString.h>
#include <WebKit/WKViewClient.h>
#include <WebKit/WKWebsiteDataStoreConfigurationRef.h>
#include <WebKit/WKWebsiteDataStoreRef.h>
#include <wpe/wpe.h>  // WPE_KEY_* keysyms

#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

#include "ps5_log.h"
#include "wk_helpers.h"

namespace prosperoweb::frontend {

namespace {

constexpr int kButtonsToLog = 16;

int to_wk_mouse_button(shell::MouseButton b) {
  switch (b) {
    case shell::MouseButton::Left:
      return kWKEventMouseButtonLeftButton;
    case shell::MouseButton::Middle:
      return kWKEventMouseButtonMiddleButton;
    case shell::MouseButton::Right:
      return kWKEventMouseButtonRightButton;
  }
  return kWKEventMouseButtonLeftButton;
}

// WKPageHandleKeyboardEvent feeds virtualKeyCode to WebKit as a WPE keysym
// (libwpe WebEventFactory), not a DOM key code: translate. Returns 0 for keys
// without a mapping.
std::uint32_t wpe_keysym_for_dom(std::uint32_t dom, std::uint32_t modifiers) {
  const bool shift = (modifiers & shell::kModShift) != 0;
  if (dom >= 'A' && dom <= 'Z') {
    return (shift ? WPE_KEY_A : WPE_KEY_a) + (dom - 'A');
  }
  if (dom >= '0' && dom <= '9') {
    return WPE_KEY_0 + (dom - '0');
  }
  if (dom >= 112 && dom <= 123) {
    return WPE_KEY_F1 + (dom - 112);
  }
  switch (dom) {
    case 8: return WPE_KEY_BackSpace;
    case 9: return shift ? WPE_KEY_ISO_Left_Tab : WPE_KEY_Tab;
    case 13: return WPE_KEY_Return;
    case 27: return WPE_KEY_Escape;
    case 32: return WPE_KEY_space;
    case 33: return WPE_KEY_Page_Up;
    case 34: return WPE_KEY_Page_Down;
    case 35: return WPE_KEY_End;
    case 36: return WPE_KEY_Home;
    case 37: return WPE_KEY_Left;
    case 38: return WPE_KEY_Up;
    case 39: return WPE_KEY_Right;
    case 40: return WPE_KEY_Down;
    case 45: return WPE_KEY_Insert;
    case 46: return WPE_KEY_Delete;
    default: return 0;
  }
}

// The text WebKit needs on a control key's event for keypress and its default
// action (Return submits a form); printable text goes through insert_text().
const char* control_key_text(std::uint32_t keysym) {
  switch (keysym) {
    case WPE_KEY_Return: return "\r";
    case WPE_KEY_BackSpace: return "\x08";
    case WPE_KEY_Tab:
    case WPE_KEY_ISO_Left_Tab: return "\t";
    default: return "";
  }
}

// Escapes a UTF-8 string into a JS string literal body (without quotes).
std::string js_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\'': out += "\\'"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

}  // namespace

Ps5EngineView::Ps5EngineView(shell::EngineViewClient& client, WKContextRef context,
                             WKWebsiteDataStoreRef data_store,
                             const std::vector<std::string>& user_scripts,
                             const std::string& user_agent)
    : client_(client) {
  WkWebView::Callbacks cb;
  cb.load_started = [this]() { client_.load_started(); };
  cb.load_finished = [this]() { client_.load_finished(); };
  cb.load_failed = [this](const std::string& url, const std::string& err) {
    client_.load_failed(url, err);
  };
  cb.url_changed = [this](const std::string& url) { client_.url_changed(url); };
  cb.title_changed = [this](const std::string& t) { client_.title_changed(t); };
  cb.nav_state_changed = [this](bool b, bool f) { client_.nav_state_changed(b, f); };
  cb.enter_fullscreen = [this]() { client_.fullscreen_entered(); };
  cb.exit_fullscreen = [this]() { client_.fullscreen_exited(); };
  cb.cursor_changed = [this](std::uint32_t type) {
    const shell::PageCursor cursor = type == kWKCursorTypeNone   ? shell::PageCursor::None
                                     : type == kWKCursorTypeHand ? shell::PageCursor::Hand
                                                                 : shell::PageCursor::Pointer;
    if ((cursor == shell::PageCursor::None) != (cursor_ == shell::PageCursor::None)) {
      log_printf("view: page cursor %s", cursor == shell::PageCursor::None ? "none (pointer hidden)" : "shown");
    }
    cursor_ = cursor;
    client_.cursor_changed(cursor);
  };
  cb.create_new_page = [this](const std::string& url) {
    client_.new_window_requested(url);
  };
  cb.script_message = [this](const std::string& json) {
    // Focus reports from core::kFocusReporterScript drive the OSK auto-open;
    // everything else (EME reports, ...) goes to the shell as is.
    const auto msg = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    const auto type = msg.is_object() ? msg.find("type") : msg.end();
    if (type != msg.end() && *type == "input_trace") {
      log_printf("page: dom %s", json.c_str());
      return;
    }
    if (type != msg.end() && *type == "focus") {
      log_printf("page: focus report %s", json.c_str());
      const auto editable = msg.find("editable");
      client_.text_input_focused(editable != msg.end() && editable->is_boolean() &&
                                 editable->get<bool>());
    } else {
      client_.script_message(json);
    }
  };

  web_ = std::make_unique<WkWebView>(context, data_store, "prosperoweb", user_scripts,
                                     /*fullscreen_enabled=*/true, std::move(cb));
  web_->set_size(1920, 1080);
  if (!user_agent.empty()) {
    web_->set_user_agent(user_agent);
  }
}

void Ps5EngineView::load(const std::string& url) { web_->load_url(url); }
void Ps5EngineView::go_back() { web_->go_back(); }
void Ps5EngineView::go_forward() { web_->go_forward(); }
void Ps5EngineView::reload() { web_->reload(); }
void Ps5EngineView::stop() { web_->stop(); }
void Ps5EngineView::set_size(int w, int h) { web_->set_size(w, h); }
void Ps5EngineView::set_visible(bool v) { web_->set_visible(v); }
void Ps5EngineView::pointer_move(int x, int y) { web_->pointer_move(x, y); }

void Ps5EngineView::pointer_button(int x, int y, shell::MouseButton button, bool pressed) {
  if (buttons_logged_ < kButtonsToLog) {
    ++buttons_logged_;
    log_printf("page: %s at %d,%d", pressed ? "press" : "release", x, y);
  }
  web_->pointer_button(x, y, to_wk_mouse_button(button), pressed);
}

void Ps5EngineView::scroll(int x, int y, int dx, int dy) { web_->scroll(x, y, dx, dy); }

void Ps5EngineView::key(std::uint32_t key_code, std::uint32_t modifiers, bool pressed,
                        const std::string& text) {
  // shell::KeyModifier bits match WKEventModifiers bits (shift/control/alt/meta);
  // WebKit patch 0011 forwards them and the event text.
  const std::uint32_t keysym = wpe_keysym_for_dom(key_code, modifiers);
  if (keysym == 0) {
    return;
  }
  web_->key(keysym, modifiers, pressed, text.empty() ? control_key_text(keysym) : text);
}

void Ps5EngineView::insert_text(const std::string& utf8) {
  // No public PlayStation C API for programmatic text insertion; fall back to
  // execCommand('insertText') in the focused editable element.
  web_->run_javascript("document.execCommand('insertText', false, '" +
                       js_escape(utf8) + "')");
}

void Ps5EngineView::request_exit_fullscreen() { web_->request_exit_fullscreen(); }
void Ps5EngineView::run_javascript(const std::string& js) { web_->run_javascript(js); }
void Ps5EngineView::set_user_agent(const std::string& ua) { web_->set_user_agent(ua); }

// --- factory ----------------------------------------------------------------

Ps5EngineFactory::Ps5EngineFactory(const std::string& base_dir,
                                   std::vector<std::string> user_scripts,
                                   std::string user_agent)
    : user_scripts_(std::move(user_scripts)), user_agent_(std::move(user_agent)) {
  WKContextConfigurationRef ctx_config = WKContextConfigurationCreate();
  // The launcher's argv[0] is the role name; our patched
  // ProcessLauncherPlayStation ignores the stored path, but set the role names
  // so the contract is explicit.
  {
    auto web = make_wk_string("WebProcess");
    WKContextConfigurationSetWebProcessPath(ctx_config, web.get());
    auto net = make_wk_string("NetworkProcess");
    WKContextConfigurationSetNetworkProcessPath(ctx_config, net.get());
  }
  context_ = WKContextCreateWithConfiguration(ctx_config);
  WKRelease(ctx_config);

  // Prewarm a WebProcess so opening the first tab does not pay the full spawn
  // + WebKit startup latency. Gated by PROSPEROWEB_PREWARM (default on;
  // "0"/"off" disables). The prewarmed process's pid shows in the broker's
  // spawn log ("spawn: role=WebProcess ... ok"); the C API reports no pid here.
  if (const char* prewarm = std::getenv("PROSPEROWEB_PREWARM");
      prewarm && (!std::strcmp(prewarm, "0") || !std::strcmp(prewarm, "off"))) {
    log_line("prewarm: disabled (PROSPEROWEB_PREWARM=off)");
  } else {
    WKContextSetPrewarmsProcessesAutomatically(context_, true);
    WKContextWarmInitialProcess(context_);
    log_line("prewarm: WebProcess requested (automatic warming on; pid in the broker spawn log)");
  }

  WKWebsiteDataStoreConfigurationRef ds_config = WKWebsiteDataStoreConfigurationCreate();
  {
    auto cache = make_wk_string(base_dir + "/cache");
    WKWebsiteDataStoreConfigurationSetNetworkCacheDirectory(ds_config, cache.get());
    auto storage = make_wk_string(base_dir + "/storage");
    WKWebsiteDataStoreConfigurationSetGeneralStorageDirectory(ds_config, storage.get());
    auto local = make_wk_string(base_dir + "/localstorage");
    WKWebsiteDataStoreConfigurationSetLocalStorageDirectory(ds_config, local.get());
    auto idb = make_wk_string(base_dir + "/indexeddb");
    WKWebsiteDataStoreConfigurationSetIndexedDBDatabaseDirectory(ds_config, idb.get());
  }
  data_store_ = WKWebsiteDataStoreCreateWithConfiguration(ds_config);
  WKRelease(ds_config);
}

Ps5EngineFactory::~Ps5EngineFactory() {
  if (data_store_) {
    WKRelease(data_store_);
  }
  if (context_) {
    WKRelease(context_);
  }
}

std::unique_ptr<shell::EngineView> Ps5EngineFactory::create(
    shell::EngineViewClient& client) {
  return std::make_unique<Ps5EngineView>(client, context_, data_store_, user_scripts_,
                                         user_agent_);
}

}  // namespace prosperoweb::frontend
