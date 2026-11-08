// SPDX-License-Identifier: GPL-3.0-or-later
#include "host_app.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "gamepad.h"
#include "gtk_engine_view.h"
#include "prosperoweb/core/file_storage.h"
#include "prosperoweb/core/in_memory_storage.h"

namespace prosperoweb::host {

using core::Button;

namespace {

constexpr guint kTickMs = 16;

// Host keyboard -> gamepad buttons (README "Host build").
std::optional<Button> button_for_key(guint keyval) {
  switch (keyval) {
    case GDK_KEY_Up: return Button::Up;
    case GDK_KEY_Down: return Button::Down;
    case GDK_KEY_Left: return Button::Left;
    case GDK_KEY_Right: return Button::Right;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter: return Button::Cross;
    case GDK_KEY_Escape: return Button::Circle;
    case GDK_KEY_F1: return Button::Triangle;
    case GDK_KEY_F2: return Button::Square;
    case GDK_KEY_Page_Up: return Button::L2;
    case GDK_KEY_Page_Down: return Button::R2;
    case GDK_KEY_bracketleft: return Button::L1;
    case GDK_KEY_bracketright: return Button::R1;
    case GDK_KEY_F11: return Button::R3;
    case GDK_KEY_F10: return Button::L3;
    case GDK_KEY_F9: return Button::Options;
    case GDK_KEY_F8: return Button::Share;
    case GDK_KEY_F7: return Button::Touchpad;
    default: return std::nullopt;
  }
}

bool is_function_key(guint keyval) { return keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35; }

bool is_stick_key(guint keyval) {
  switch (keyval) {
    case GDK_KEY_w: case GDK_KEY_a: case GDK_KEY_s: case GDK_KEY_d:
    case GDK_KEY_i: case GDK_KEY_j: case GDK_KEY_k: case GDK_KEY_l: return true;
    default: return false;
  }
}

std::optional<Button> button_by_name(const std::string& name) {
  static const std::map<std::string, Button> names = {
      {"cross", Button::Cross},     {"circle", Button::Circle},   {"square", Button::Square},
      {"triangle", Button::Triangle}, {"l1", Button::L1},         {"r1", Button::R1},
      {"l2", Button::L2},           {"r2", Button::R2},           {"l3", Button::L3},
      {"r3", Button::R3},           {"up", Button::Up},           {"down", Button::Down},
      {"left", Button::Left},       {"right", Button::Right},     {"options", Button::Options},
      {"share", Button::Share},     {"touchpad", Button::Touchpad},
  };
  const auto it = names.find(name);
  return it == names.end() ? std::nullopt : std::optional<Button>(it->second);
}

bool file_exists(const std::string& path) { return g_file_test(path.c_str(), G_FILE_TEST_EXISTS); }

std::string find_chrome_dir(const std::string& explicit_dir) {
  std::vector<std::string> candidates;
  if (!explicit_dir.empty()) candidates.push_back(explicit_dir);
  if (const char* env = std::getenv("PROSPEROWEB_CHROME_DIR")) candidates.emplace_back(env);
  candidates.emplace_back(PROSPEROWEB_CHROME_DIR);
  candidates.emplace_back(PROSPEROWEB_CHROME_SOURCE_DIR);
  for (const auto& dir : candidates) {
    if (file_exists(dir + "/chrome.html")) {
      return dir;
    }
  }
  return {};
}

}  // namespace

HostApp::HostApp(Options options) : options_(std::move(options)) {
  if (options_.profile_dir.empty()) {
    options_.profile_dir = std::string(g_get_user_data_dir()) + "/prosperoweb";
  }
  if (options_.demo.empty()) {
    storage_ = std::make_unique<core::FileStorage>(options_.profile_dir + "/state");
  } else {
    storage_ = std::make_unique<core::InMemoryStorage>();
  }
  const std::string data_dir = options_.profile_dir + "/webkit-data";
  const std::string cache_dir = options_.profile_dir + "/webkit-cache";
  session_ = webkit_network_session_new(data_dir.c_str(), cache_dir.c_str());
  gamepad_ = std::make_unique<Gamepad>();
}

HostApp::~HostApp() {
  shell_.reset();  // destroys the content views
  gamepad_.reset();
  if (session_) g_object_unref(session_);
  if (window_) gtk_window_destroy(window_);
  if (loop_) g_main_loop_unref(loop_);
}

std::unique_ptr<shell::EngineView> HostApp::create(shell::EngineViewClient& client) {
  return std::make_unique<GtkEngineView>(client, session_, stack_);
}

void HostApp::set_user_scripts(std::vector<std::string> sources) {
  GtkEngineView::set_user_scripts(std::move(sources));
}

// ---------------------------------------------------------------------------
// Window

void HostApp::build_window() {
  window_ = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(window_, "ProsperoWeb (host)");
  gtk_window_set_default_size(window_, options_.width, options_.height);
  gtk_window_set_decorated(window_, FALSE);
  gtk_window_set_resizable(window_, FALSE);
  g_signal_connect(window_, "close-request", G_CALLBACK(on_close_request), this);

  overlay_ = GTK_OVERLAY(gtk_overlay_new());
  stack_ = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(stack_, GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_widget_set_hexpand(GTK_WIDGET(stack_), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(stack_), TRUE);
  gtk_overlay_set_child(overlay_, GTK_WIDGET(stack_));
  gtk_window_set_child(window_, GTK_WIDGET(overlay_));

  GtkEventController* keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), this);
  g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), this);
  gtk_widget_add_controller(GTK_WIDGET(window_), keys);

  GtkEventController* motion = gtk_event_controller_motion_new();
  gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion), this);
  gtk_widget_add_controller(GTK_WIDGET(window_), motion);
}

void HostApp::build_chrome_view(const std::string& chrome_dir) {
  WebKitUserContentManager* ucm = webkit_user_content_manager_new();
  webkit_user_content_manager_register_script_message_handler(ucm, "chrome", nullptr);
  g_signal_connect(ucm, "script-message-received::chrome", G_CALLBACK(on_chrome_message), this);

  WebKitSettings* settings = webkit_settings_new();
  webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
  webkit_settings_set_enable_developer_extras(settings, TRUE);
  // Chrome errors and console.log() end up on stdout of the host.
  webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);

  WebKitNetworkSession* chrome_session = webkit_network_session_new_ephemeral();
  chrome_view_ = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session",
                                              chrome_session, "user-content-manager", ucm,
                                              "settings", settings, nullptr));
  g_object_unref(chrome_session);
  g_object_unref(settings);
  g_object_unref(ucm);

  const GdkRGBA transparent = {0.0f, 0.0f, 0.0f, 0.0f};
  webkit_web_view_set_background_color(chrome_view_, &transparent);
  gtk_widget_set_hexpand(GTK_WIDGET(chrome_view_), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(chrome_view_), TRUE);
  g_signal_connect(chrome_view_, "load-changed", G_CALLBACK(on_chrome_load_changed), this);
  gtk_overlay_add_overlay(overlay_, GTK_WIDGET(chrome_view_));

  gchar* uri = g_filename_to_uri((chrome_dir + "/chrome.html").c_str(), nullptr, nullptr);
  std::string full = uri ? uri : "";
  g_free(uri);
  if (!options_.demo.empty()) {
    full += "?demo=" + options_.demo;
  }
  webkit_web_view_load_uri(chrome_view_, full.c_str());
}

int HostApp::run() {
  const std::string chrome_dir = find_chrome_dir(options_.chrome_dir);
  if (chrome_dir.empty()) {
    std::fprintf(stderr, "prosperoweb-host: chrome.html not found (use --chrome-dir)\n");
    return 1;
  }
  loop_ = g_main_loop_new(nullptr, FALSE);
  build_window();

  shell::ShellController::Config config;
  config.width = options_.width;
  config.height = options_.height;
  config.home_url = options_.demo.empty() ? "https://duckduckgo.com/" : "about:blank";
  config.user_agent = options_.user_agent;
  config.dpad_links = options_.dpad_links;
  config.user_scripts_backend = this;
  config.language = options_.language;
  if (file_exists(chrome_dir + "/diag.html")) {
    gchar* diag = g_filename_to_uri((chrome_dir + "/diag.html").c_str(), nullptr, nullptr);
    if (diag) {
      config.diagnostics_url = diag;
      g_free(diag);
    }
  }
  remote::RemoteService::Options remote_options;
  remote_options.port = options_.remote_port;
  remote_ = std::make_unique<remote::RemoteService>(remote_options);
  config.remote_control = remote_.get();
  shell_ = std::make_unique<shell::ShellController>(*this, *this, *storage_, config);
  shell_->start();
  if (options_.remote) {
    std::string error;
    if (const auto info = remote_->start(error)) {
      std::printf("remote: %s PIN %s\n", info->url.c_str(), info->pin.c_str());
    } else {
      std::fprintf(stderr, "remote: %s\n", error.c_str());
    }
    std::fflush(stdout);
  }
  build_chrome_view(chrome_dir);
  if (!options_.url.empty()) {
    shell_->navigate(options_.url);
  }

  gtk_window_present(window_);
  if (auto* view = dynamic_cast<GtkEngineView*>(shell_->active_view())) {
    view->grab_focus();
  }

  last_tick_us_ = g_get_monotonic_time();
  tick_source_ = g_timeout_add(kTickMs, on_tick, this);
  if (!options_.input.empty()) {
    g_timeout_add(static_cast<guint>(options_.input_delay_ms), on_input_timer, this);
  }
  if (!options_.screenshot.empty()) {
    g_timeout_add(static_cast<guint>(options_.after_ms), on_screenshot_timer, this);
  }

  g_main_loop_run(loop_);
  return exit_code_;
}

void HostApp::quit() {
  if (quitting_) {
    return;
  }
  quitting_ = true;
  if (tick_source_) {
    g_source_remove(tick_source_);
    tick_source_ = 0;
  }
  if (shell_ && options_.demo.empty()) {
    shell_->shutdown();
  }
  g_main_loop_quit(loop_);
}

// ---------------------------------------------------------------------------
// Chrome

void HostApp::render(const nlohmann::json& state) {
  const std::string focus = state.value("focus", std::string("page"));
  const bool focus_changed = focus != focus_;
  focus_ = focus;
  band_height_ = state.contains("content") ? state["content"].value("y", 0) : 0;
  gtk_widget_set_margin_top(GTK_WIDGET(stack_), band_height_);
  update_chrome_targeting(last_pointer_y_);

  if (focus_changed && focus_ == "page") {
    if (auto* view = dynamic_cast<GtkEngineView*>(shell_->active_view())) {
      view->grab_focus();
    }
  }
  if (!options_.demo.empty()) {
    return;  // the chrome shows its static demo state
  }
  pending_state_ = "window.ps5chrome && window.ps5chrome.render(" +
                   state.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ");";
  flush_chrome();
}

void HostApp::flush_chrome() {
  if (!chrome_loaded_ || pending_state_.empty() || !chrome_view_) {
    return;
  }
  webkit_web_view_evaluate_javascript(chrome_view_, pending_state_.c_str(),
                                      static_cast<gssize>(pending_state_.size()), nullptr,
                                      nullptr, nullptr, nullptr, nullptr);
  pending_state_.clear();
}

void HostApp::pointer(int x, int y, bool visible) {
  // Before the chrome loaded the next render() places the ring.
  if (!chrome_loaded_ || !chrome_view_ || !options_.demo.empty()) {
    return;
  }
  const std::string js = "window.ps5chrome && window.ps5chrome.pointer(" + std::to_string(x) +
                         "," + std::to_string(y) + "," + (visible ? "true" : "false") + ");";
  webkit_web_view_evaluate_javascript(chrome_view_, js.c_str(), static_cast<gssize>(js.size()),
                                      nullptr, nullptr, nullptr, nullptr, nullptr);
}

void HostApp::click(int x, int y) {
  if (!chrome_view_) {
    return;
  }
  const std::string js = "window.ps5chrome && window.ps5chrome.click(" + std::to_string(x) + "," +
                         std::to_string(y) + ");";
  webkit_web_view_evaluate_javascript(chrome_view_, js.c_str(), static_cast<gssize>(js.size()),
                                      nullptr, nullptr, nullptr, nullptr, nullptr);
}

// The chrome view covers the whole window but must not steal the mouse from
// the page outside its visible parts. GTK4 has no per-pixel input shape for
// a child widget, so the compromise is: the chrome is targetable while it
// has focus (OSK, menu, notice, address bar typing) or while the mouse is
// over the top band; otherwise mouse events go straight to the page.
void HostApp::update_chrome_targeting(double pointer_y) {
  if (!chrome_view_) {
    return;
  }
  const bool target = focus_ != "page" || (band_height_ > 0 && pointer_y < band_height_);
  gtk_widget_set_can_target(GTK_WIDGET(chrome_view_), target);
}

void HostApp::on_chrome_message(WebKitUserContentManager* /*manager*/, JSCValue* value,
                                gpointer self) {
  auto* me = static_cast<HostApp*>(self);
  char* text = jsc_value_is_string(value) ? jsc_value_to_string(value) : jsc_value_to_json(value, 0);
  if (!text) {
    return;
  }
  const auto event = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  g_free(text);
  if (me->shell_ && !event.is_discarded()) {
    me->shell_->on_chrome_event(event);
  }
}

void HostApp::on_chrome_load_changed(WebKitWebView* /*view*/, WebKitLoadEvent event,
                                     gpointer self) {
  auto* me = static_cast<HostApp*>(self);
  if (event == WEBKIT_LOAD_FINISHED) {
    me->chrome_loaded_ = true;
    if (me->shell_) {
      // As if the chrome had just said "ready": renders buffered before the
      // load finished keep only the latest state, which may have lost the UI
      // strings the shell sends once after the chrome's own "ready".
      me->shell_->on_chrome_event({{"type", "ready"}});
      me->shell_->render_now();
    }
  }
}

// ---------------------------------------------------------------------------
// Input

bool HostApp::on_key(guint keyval, bool pressed) {
  keyval = gdk_keyval_to_lower(keyval);
  const auto button = button_for_key(keyval);
  const bool stick = is_stick_key(keyval);
  if (!button && !stick) {
    return false;
  }
  // Let typing reach the HTML address field or a focused page text field;
  // function keys stay mapped so the browser remains controllable.
  const bool typing = focus_ == "chrome" || (focus_ == "page" && shell_->text_input_focused());
  if (typing && !is_function_key(keyval)) {
    return false;
  }
  if (pressed) {
    if (!held_keys_.insert(keyval).second) {
      return true;  // keyboard auto-repeat; the shell has its own
    }
  } else if (held_keys_.erase(keyval) == 0) {
    return false;
  }
  if (button) {
    shell_->on_button(*button, pressed);
  }
  return true;
}

void HostApp::update_key_sticks() {
  auto held = [this](guint k) { return held_keys_.count(k) ? 1.0f : 0.0f; };
  float lx = held(GDK_KEY_d) - held(GDK_KEY_a);
  float ly = held(GDK_KEY_s) - held(GDK_KEY_w);
  float rx = held(GDK_KEY_l) - held(GDK_KEY_j);
  float ry = held(GDK_KEY_k) - held(GDK_KEY_i);
  const float* pad = gamepad_->sticks();
  auto merge = [](float a, float b) { return std::clamp(a + b, -1.0f, 1.0f); };
  shell_->on_sticks(merge(lx, pad[0]), merge(ly, pad[1]), merge(rx, pad[2]), merge(ry, pad[3]));
}

gboolean HostApp::on_key_pressed(GtkEventControllerKey* /*controller*/, guint keyval,
                                 guint /*keycode*/, GdkModifierType /*state*/, gpointer self) {
  return static_cast<HostApp*>(self)->on_key(keyval, true) ? TRUE : FALSE;
}

void HostApp::on_key_released(GtkEventControllerKey* /*controller*/, guint keyval,
                              guint /*keycode*/, GdkModifierType /*state*/, gpointer self) {
  static_cast<HostApp*>(self)->on_key(keyval, false);
}

void HostApp::on_motion(GtkEventControllerMotion* /*controller*/, double x, double y,
                        gpointer self) {
  auto* me = static_cast<HostApp*>(self);
  me->last_pointer_y_ = y;
  me->update_chrome_targeting(y);
  if (me->shell_) {
    me->shell_->on_mouse_move(static_cast<float>(x), static_cast<float>(y));
  }
}

gboolean HostApp::on_close_request(GtkWindow* /*window*/, gpointer self) {
  static_cast<HostApp*>(self)->quit();
  return TRUE;
}

gboolean HostApp::on_tick(gpointer self) {
  auto* me = static_cast<HostApp*>(self);
  const gint64 now = g_get_monotonic_time();
  const float dt = static_cast<float>(now - me->last_tick_us_) / 1e6f;
  me->last_tick_us_ = now;

  me->gamepad_->poll([me](Button b, bool pressed) { me->shell_->on_button(b, pressed); });
  // A DualSense touchpad drives the pointer as on the console. There is no
  // host mouse emulation of it: the real mouse already goes to the page.
  me->shell_->on_touchpad(me->gamepad_->touch(), static_cast<std::uint64_t>(now / 1000));
  if (me->input_release_) {
    me->shell_->on_button(*me->input_release_, false);
    me->input_release_.reset();
  }
  me->update_key_sticks();
  me->remote_->pump(*me->shell_);
  me->shell_->tick(dt);
  if (me->shell_->quit_requested()) {
    me->tick_source_ = 0;
    me->quit();
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void HostApp::run_next_input() {
  if (next_input_ >= options_.input.size()) {
    return;
  }
  const std::string name = options_.input[next_input_++];
  if (const auto b = button_by_name(name)) {
    std::fprintf(stderr, "prosperoweb-host: input %s\n", name.c_str());
    shell_->on_button(*b, true);
    input_release_ = *b;
  } else if (int x = 0, y = 0; std::sscanf(name.c_str(), "pointer:%dx%d", &x, &y) == 2) {
    // Places the pointer (window coordinates) for a following "cross", e.g.
    // to press a chrome button; the shell treats it like a mouse move.
    std::fprintf(stderr, "prosperoweb-host: input %s\n", name.c_str());
    shell_->on_mouse_move(static_cast<float>(x), static_cast<float>(y));
  } else if (name != "wait") {
    std::fprintf(stderr, "prosperoweb-host: unknown input '%s'\n", name.c_str());
  }
}

gboolean HostApp::on_input_timer(gpointer self) {
  auto* me = static_cast<HostApp*>(self);
  me->run_next_input();
  if (me->next_input_ < me->options_.input.size()) {
    g_timeout_add(static_cast<guint>(me->options_.input_step_ms), on_input_timer, me);
  }
  return G_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// Screenshot

bool HostApp::save_window_png(const std::string& path) {
  GtkWidget* widget = GTK_WIDGET(overlay_);
  const int w = gtk_widget_get_width(widget);
  const int h = gtk_widget_get_height(widget);
  if (w <= 0 || h <= 0) {
    return false;
  }
  GdkPaintable* paintable = gtk_widget_paintable_new(widget);
  GtkSnapshot* snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), w, h);
  GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
  g_object_unref(paintable);
  if (!node) {
    return false;
  }
  GskRenderer* renderer = gtk_native_get_renderer(GTK_NATIVE(window_));
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0.0f, 0.0f, static_cast<float>(w),
                                              static_cast<float>(h));
  GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &bounds);
  gsk_render_node_unref(node);
  if (!texture) {
    return false;
  }
  const bool ok = gdk_texture_save_to_png(texture, path.c_str());
  g_object_unref(texture);
  return ok;
}

// Two capture paths:
//  - "gtk": renders the window's widget tree to a texture. Exact, but with
//    WebKit's default DMA-BUF renderer the web views come out blank (their
//    buffers are not readable through the snapshot); it works with
//    WEBKIT_DISABLE_DMABUF_RENDERER=1.
//  - "x11": ImageMagick `import -window root`; captures what is on screen,
//    which under Xvfb is exactly the undecorated window at the origin.
// Default: x11 when running on X11 with `import` available, else gtk.
// PROSPEROWEB_SCREENSHOT_METHOD=gtk|x11 forces one.
bool HostApp::save_x11_png(const std::string& path) {
  gchar* argv[] = {const_cast<gchar*>("import"), const_cast<gchar*>("-window"),
                   const_cast<gchar*>("root"), const_cast<gchar*>(path.c_str()), nullptr};
  gint status = 0;
  return g_spawn_sync(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                      nullptr, &status, nullptr) &&
         status == 0;
}

void HostApp::take_screenshot() {
  std::string method;
  if (const char* env = std::getenv("PROSPEROWEB_SCREENSHOT_METHOD")) {
    method = env;
  } else {
    gchar* import = g_find_program_in_path("import");
    const bool x11 = std::string(G_OBJECT_TYPE_NAME(gdk_display_get_default())) == "GdkX11Display";
    method = (import && x11) ? "x11" : "gtk";
    g_free(import);
  }
  const bool ok = method == "x11" ? save_x11_png(options_.screenshot)
                                  : save_window_png(options_.screenshot);
  std::fprintf(stderr, "prosperoweb-host: screenshot (%s) %s: %s\n", method.c_str(),
               ok ? "saved" : "FAILED", options_.screenshot.c_str());
  exit_code_ = ok ? 0 : 2;
}

gboolean HostApp::on_screenshot_timer(gpointer self) {
  auto* me = static_cast<HostApp*>(self);
  me->take_screenshot();
  me->quit();
  return G_SOURCE_REMOVE;
}

}  // namespace prosperoweb::host
