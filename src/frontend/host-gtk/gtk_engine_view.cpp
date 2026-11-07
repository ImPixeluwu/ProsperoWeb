// SPDX-License-Identifier: GPL-3.0-or-later
#include "gtk_engine_view.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/page_scripts.h"

namespace prosperoweb::host {

namespace {

std::string js_string(const std::string& s) {
  return nlohmann::json(s).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Every live view, for set_user_scripts().
std::vector<GtkEngineView*>& all_views() {
  static std::vector<GtkEngineView*> views;
  return views;
}

std::vector<std::string>& user_scripts() {
  static std::vector<std::string> sources;
  return sources;
}

void add_user_script(WebKitUserContentManager* manager, const char* source) {
  WebKitUserScript* script =
      webkit_user_script_new(source, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                             WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  webkit_user_content_manager_add_script(manager, script);
  webkit_user_script_unref(script);
}

}  // namespace

GtkEngineView::GtkEngineView(shell::EngineViewClient& client, WebKitNetworkSession* session,
                             GtkStack* stack)
    : client_(client), stack_(stack) {
  content_manager_ = webkit_user_content_manager_new();
  install_scripts();
  all_views().push_back(this);
  webkit_user_content_manager_register_script_message_handler(content_manager_, "prosperoweb",
                                                              nullptr);
  g_signal_connect(content_manager_, "script-message-received::prosperoweb",
                   G_CALLBACK(on_script_message), this);

  WebKitSettings* settings = webkit_settings_new();
  webkit_settings_set_enable_fullscreen(settings, TRUE);
  webkit_settings_set_enable_media(settings, TRUE);
  webkit_settings_set_enable_mediasource(settings, TRUE);
  webkit_settings_set_enable_encrypted_media(settings, TRUE);
  webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
  webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);

  view_ = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session,
                                       "user-content-manager", content_manager_, "settings",
                                       settings, nullptr));
  g_object_unref(settings);
  g_object_ref_sink(view_);

  g_signal_connect(view_, "load-changed", G_CALLBACK(on_load_changed), this);
  g_signal_connect(view_, "load-failed", G_CALLBACK(on_load_failed), this);
  g_signal_connect(view_, "notify::uri", G_CALLBACK(on_notify_uri), this);
  g_signal_connect(view_, "notify::title", G_CALLBACK(on_notify_title), this);
  g_signal_connect(view_, "enter-fullscreen", G_CALLBACK(on_enter_fullscreen), this);
  g_signal_connect(view_, "leave-fullscreen", G_CALLBACK(on_leave_fullscreen), this);
  g_signal_connect(view_, "create", G_CALLBACK(on_create), this);
  g_signal_connect(webkit_web_view_get_back_forward_list(view_), "changed",
                   G_CALLBACK(on_back_forward_changed), this);

  gtk_widget_set_hexpand(GTK_WIDGET(view_), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(view_), TRUE);
  gtk_stack_add_child(stack_, GTK_WIDGET(view_));
}

void GtkEngineView::install_scripts() {
  webkit_user_content_manager_remove_all_scripts(content_manager_);
  add_user_script(content_manager_, core::kEmeDetectorScript);
  add_user_script(content_manager_, core::kFocusReporterScript);
  for (const auto& source : user_scripts()) {
    add_user_script(content_manager_, source.c_str());
  }
}

void GtkEngineView::set_user_scripts(std::vector<std::string> sources) {
  user_scripts() = std::move(sources);
  for (auto* view : all_views()) {
    view->install_scripts();
  }
}

GtkEngineView::~GtkEngineView() {
  auto& views = all_views();
  views.erase(std::remove(views.begin(), views.end(), this), views.end());
  g_signal_handlers_disconnect_by_data(webkit_web_view_get_back_forward_list(view_), this);
  g_signal_handlers_disconnect_by_data(view_, this);
  g_signal_handlers_disconnect_by_data(content_manager_, this);
  gtk_stack_remove(stack_, GTK_WIDGET(view_));
  webkit_web_view_try_close(view_);
  g_object_unref(view_);
  g_object_unref(content_manager_);
}

// ---------------------------------------------------------------------------
// Navigation

void GtkEngineView::load(const std::string& url) { webkit_web_view_load_uri(view_, url.c_str()); }
void GtkEngineView::go_back() { webkit_web_view_go_back(view_); }
void GtkEngineView::go_forward() { webkit_web_view_go_forward(view_); }
void GtkEngineView::reload() { webkit_web_view_reload(view_); }
void GtkEngineView::stop() { webkit_web_view_stop_loading(view_); }

// GTK lays the view out to fill the stack; the stack's top margin follows
// the chrome band (see HostApp::render), so there is nothing to do here.
void GtkEngineView::set_size(int /*width*/, int /*height*/) {}

void GtkEngineView::set_visible(bool visible) {
  if (visible) {
    gtk_stack_set_visible_child(stack_, GTK_WIDGET(view_));
  }
}

void GtkEngineView::grab_focus() { gtk_widget_grab_focus(GTK_WIDGET(view_)); }

// ---------------------------------------------------------------------------
// Input. UNTRUSTED JavaScript fallbacks, see the class comment.

void GtkEngineView::pointer_move(int x, int y) {
  pointer_x_ = x;
  pointer_y_ = y;
  // UNTRUSTED: hover effects driven by CSS :hover do not follow.
  run_javascript("(function(x,y){var e=document.elementFromPoint(x,y);if(e)"
                 "e.dispatchEvent(new MouseEvent('mousemove',{bubbles:true,clientX:x,"
                 "clientY:y}));})(" +
                 std::to_string(x) + "," + std::to_string(y) + ");");
}

void GtkEngineView::pointer_button(int x, int y, shell::MouseButton button, bool pressed) {
  if (button != shell::MouseButton::Left || pressed) {
    return;
  }
  // UNTRUSTED: click() runs the activation behaviour (links, buttons,
  // checkboxes) but grants no user activation (no fullscreen/popups).
  run_javascript("(function(x,y){var e=document.elementFromPoint(x,y);if(!e)return;"
                 "var f=e.closest('input,textarea,select,[contenteditable]');"
                 "if(f&&f.focus)f.focus();e.click();})(" +
                 std::to_string(x) + "," + std::to_string(y) + ");");
}

void GtkEngineView::scroll(int x, int y, int dx, int dy) {
  // Scrolls the nearest scrollable ancestor under the point, else the page.
  run_javascript(
      "(function(x,y,dx,dy){var e=document.elementFromPoint(x,y);"
      "while(e&&e!==document.body&&e!==document.documentElement){"
      "var s=getComputedStyle(e);"
      "if((dy&&/(auto|scroll)/.test(s.overflowY)&&e.scrollHeight>e.clientHeight)||"
      "(dx&&/(auto|scroll)/.test(s.overflowX)&&e.scrollWidth>e.clientWidth)){"
      "e.scrollBy(dx,dy);return;}e=e.parentElement;}"
      "window.scrollBy(dx,dy);})(" +
      std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(dx) + "," +
      std::to_string(dy) + ");");
}

void GtkEngineView::key(std::uint32_t key_code, std::uint32_t modifiers, bool pressed,
                        const std::string& text) {
  // UNTRUSTED: page handlers see the event, but no default action runs.
  run_javascript("(function(){var t=document.activeElement||document.body;"
                 "t.dispatchEvent(new KeyboardEvent('" +
                 std::string(pressed ? "keydown" : "keyup") +
                 "',{bubbles:true,cancelable:true,key:" + js_string(text) +
                 ",keyCode:" + std::to_string(key_code) +
                 ",shiftKey:" + ((modifiers & shell::kModShift) ? "true" : "false") +
                 ",ctrlKey:" + ((modifiers & shell::kModControl) ? "true" : "false") +
                 ",altKey:" + ((modifiers & shell::kModAlt) ? "true" : "false") + "}));})();");
}

void GtkEngineView::insert_text(const std::string& utf8) {
  run_javascript("document.execCommand('insertText', false, " + js_string(utf8) + ");");
}

void GtkEngineView::request_exit_fullscreen() {
  run_javascript("if (document.fullscreenElement) document.exitFullscreen();"
                 "else if (document.webkitFullscreenElement) document.webkitExitFullscreen();");
}

void GtkEngineView::run_javascript(const std::string& js) {
  webkit_web_view_evaluate_javascript(view_, js.c_str(), static_cast<gssize>(js.size()), nullptr,
                                      nullptr, nullptr, nullptr, nullptr);
}

void GtkEngineView::set_user_agent(const std::string& user_agent) {
  webkit_settings_set_user_agent(webkit_web_view_get_settings(view_), user_agent.c_str());
}

// ---------------------------------------------------------------------------
// Signals

void GtkEngineView::report_nav_state() {
  client_.nav_state_changed(webkit_web_view_can_go_back(view_),
                            webkit_web_view_can_go_forward(view_));
}

void GtkEngineView::on_load_changed(WebKitWebView* /*view*/, WebKitLoadEvent event, gpointer self) {
  auto* me = static_cast<GtkEngineView*>(self);
  switch (event) {
    case WEBKIT_LOAD_STARTED: me->client_.load_started(); break;
    case WEBKIT_LOAD_COMMITTED: me->report_nav_state(); break;
    case WEBKIT_LOAD_FINISHED:
      me->report_nav_state();
      me->client_.load_finished();
      break;
    case WEBKIT_LOAD_REDIRECTED: break;
  }
}

gboolean GtkEngineView::on_load_failed(WebKitWebView* /*view*/, WebKitLoadEvent /*event*/,
                                       const char* uri, GError* error, gpointer self) {
  auto* me = static_cast<GtkEngineView*>(self);
  // Cancelled loads (a new navigation, stop(), downloads) are not errors.
  if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) ||
      g_error_matches(error, WEBKIT_POLICY_ERROR,
                      WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE)) {
    return FALSE;
  }
  me->client_.load_failed(uri ? uri : "", error ? error->message : "unknown error");
  return FALSE;  // let WebKit show its error page as well
}

void GtkEngineView::on_notify_uri(GObject* /*object*/, GParamSpec* /*pspec*/, gpointer self) {
  auto* me = static_cast<GtkEngineView*>(self);
  const char* uri = webkit_web_view_get_uri(me->view_);
  me->client_.url_changed(uri ? uri : "");
  me->report_nav_state();
}

void GtkEngineView::on_notify_title(GObject* /*object*/, GParamSpec* /*pspec*/, gpointer self) {
  auto* me = static_cast<GtkEngineView*>(self);
  const char* title = webkit_web_view_get_title(me->view_);
  me->client_.title_changed(title ? title : "");
}

void GtkEngineView::on_back_forward_changed(WebKitBackForwardList* /*list*/,
                                            WebKitBackForwardListItem* /*added*/,
                                            gpointer /*removed*/, gpointer self) {
  static_cast<GtkEngineView*>(self)->report_nav_state();
}

// Returning FALSE lets WebKit run its default handling (the element covers
// the view, and WebKit fullscreens the toplevel window); the shell only
// needs to know so it can hide the chrome and route Circle to exit.
gboolean GtkEngineView::on_enter_fullscreen(WebKitWebView* /*view*/, gpointer self) {
  static_cast<GtkEngineView*>(self)->client_.fullscreen_entered();
  return FALSE;
}

gboolean GtkEngineView::on_leave_fullscreen(WebKitWebView* /*view*/, gpointer self) {
  static_cast<GtkEngineView*>(self)->client_.fullscreen_exited();
  return FALSE;
}

GtkWidget* GtkEngineView::on_create(WebKitWebView* /*view*/, WebKitNavigationAction* action,
                                    gpointer self) {
  WebKitURIRequest* request = webkit_navigation_action_get_request(action);
  const char* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
  static_cast<GtkEngineView*>(self)->client_.new_window_requested(uri ? uri : "");
  return nullptr;  // the shell opens the URL in a tab of its own
}

void GtkEngineView::on_script_message(WebKitUserContentManager* /*manager*/, JSCValue* value,
                                      gpointer self) {
  auto* me = static_cast<GtkEngineView*>(self);
  char* json_text = jsc_value_to_json(value, 0);
  if (!json_text) {
    return;
  }
  const auto msg = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
  if (msg.is_object() && msg.value("type", std::string()) == "focus") {
    me->client_.text_input_focused(msg.value("editable", false));
  } else {
    me->client_.script_message(json_text);
  }
  g_free(json_text);
}

}  // namespace prosperoweb::host
