// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include "prosperoweb/shell/engine_view.h"

namespace prosperoweb::host {

// EngineView on top of a WebKitGTK 6.0 WebKitWebView living in a GtkStack.
//
// Limitation of the host: WebKitGTK has no public API to inject input
// events, so pointer_*(), scroll() and key() are implemented with
// JavaScript (elementFromPoint().click(), scrollBy(), dispatched
// KeyboardEvents). Those events are UNTRUSTED (isTrusted == false, no user
// activation), unlike the real engine events the PS5 port must deliver.
// On the host the real mouse and keyboard reach the page directly through
// GTK, so this fallback only serves gamepad-driven testing.
class GtkEngineView final : public shell::EngineView {
 public:
  GtkEngineView(shell::EngineViewClient& client, WebKitNetworkSession* session, GtkStack* stack);
  ~GtkEngineView() override;

  GtkEngineView(const GtkEngineView&) = delete;
  GtkEngineView& operator=(const GtkEngineView&) = delete;

  void load(const std::string& url) override;
  void go_back() override;
  void go_forward() override;
  void reload() override;
  void stop() override;
  void set_size(int width, int height) override;
  void set_visible(bool visible) override;
  void pointer_move(int x, int y) override;
  void pointer_button(int x, int y, shell::MouseButton button, bool pressed) override;
  void scroll(int x, int y, int dx, int dy) override;
  void key(std::uint32_t key_code, std::uint32_t modifiers, bool pressed,
           const std::string& text) override;
  void insert_text(const std::string& utf8) override;
  void request_exit_fullscreen() override;
  void run_javascript(const std::string& js) override;
  void set_user_agent(const std::string& user_agent) override;

  WebKitWebView* web_view() const { return view_; }
  void grab_focus();

 private:
  static void on_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer self);
  static gboolean on_load_failed(WebKitWebView* view, WebKitLoadEvent event, const char* uri,
                                 GError* error, gpointer self);
  static void on_notify_uri(GObject* object, GParamSpec* pspec, gpointer self);
  static void on_notify_title(GObject* object, GParamSpec* pspec, gpointer self);
  static void on_back_forward_changed(WebKitBackForwardList* list, WebKitBackForwardListItem* added,
                                      gpointer removed, gpointer self);
  static gboolean on_enter_fullscreen(WebKitWebView* view, gpointer self);
  static gboolean on_leave_fullscreen(WebKitWebView* view, gpointer self);
  static GtkWidget* on_create(WebKitWebView* view, WebKitNavigationAction* action, gpointer self);
  static void on_script_message(WebKitUserContentManager* manager, JSCValue* value, gpointer self);

  void report_nav_state();

  shell::EngineViewClient& client_;
  GtkStack* stack_;
  WebKitUserContentManager* content_manager_ = nullptr;
  WebKitWebView* view_ = nullptr;
  int pointer_x_ = 0;
  int pointer_y_ = 0;
};

}  // namespace prosperoweb::host
