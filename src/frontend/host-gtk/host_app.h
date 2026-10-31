// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtk/gtk.h>
#include <webkit/webkit.h>

#include "prosperobrowser/core/gamepad.h"
#include "prosperobrowser/core/storage.h"
#include "prosperobrowser/shell/chrome_ui.h"
#include "prosperobrowser/shell/engine_view.h"
#include "prosperobrowser/shell/shell_controller.h"

namespace prosperobrowser::host {

class GtkEngineView;
class Gamepad;

struct Options {
  int width = 1920;
  int height = 1080;
  std::string profile_dir;   // empty: $XDG_DATA_HOME/prosperobrowser
  std::string url;           // opened in the active tab after start
  std::string screenshot;    // PNG path; empty: no screenshot
  int after_ms = 3000;
  std::string demo;          // empty: off; otherwise chrome.html?demo=<demo>
  std::string language = "en";
  std::string user_agent;
  std::string chrome_dir;    // empty: autodetect
  // Debug/automation: gamepad buttons pressed one by one after start.
  std::vector<std::string> input;
  int input_delay_ms = 1500;
  int input_step_ms = 250;
};

// The WebKitGTK host: one window with a GtkOverlay holding the content views
// (a GtkStack, one WebKitWebView per tab) under a transparent chrome
// WebKitWebView that renders src/chrome-ui/chrome.html.
class HostApp final : public shell::ChromeUi, public shell::EngineFactory {
 public:
  explicit HostApp(Options options);
  ~HostApp() override;

  // Runs the main loop; returns the process exit code.
  int run();

  // shell::ChromeUi
  void render(const nlohmann::json& state) override;
  void pointer(int x, int y, bool visible) override;
  void click(int x, int y) override;
  // shell::EngineFactory
  std::unique_ptr<shell::EngineView> create(shell::EngineViewClient& client) override;

 private:
  void build_window();
  void build_chrome_view(const std::string& chrome_dir);
  void flush_chrome();
  void update_chrome_targeting(double pointer_y);
  void quit();

  bool on_key(guint keyval, bool pressed);
  void update_key_sticks();
  void run_next_input();
  void take_screenshot();
  bool save_window_png(const std::string& path);
  static bool save_x11_png(const std::string& path);

  static gboolean on_tick(gpointer self);
  static gboolean on_key_pressed(GtkEventControllerKey* controller, guint keyval, guint keycode,
                                 GdkModifierType state, gpointer self);
  static void on_key_released(GtkEventControllerKey* controller, guint keyval, guint keycode,
                              GdkModifierType state, gpointer self);
  static void on_motion(GtkEventControllerMotion* controller, double x, double y, gpointer self);
  static gboolean on_close_request(GtkWindow* window, gpointer self);
  static void on_chrome_message(WebKitUserContentManager* manager, JSCValue* value,
                                gpointer self);
  static void on_chrome_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer self);
  static gboolean on_input_timer(gpointer self);
  static gboolean on_screenshot_timer(gpointer self);

  Options options_;
  GMainLoop* loop_ = nullptr;
  GtkWindow* window_ = nullptr;
  GtkOverlay* overlay_ = nullptr;
  GtkStack* stack_ = nullptr;
  WebKitWebView* chrome_view_ = nullptr;
  WebKitNetworkSession* session_ = nullptr;

  std::unique_ptr<core::Storage> storage_;
  std::unique_ptr<shell::ShellController> shell_;
  std::unique_ptr<Gamepad> gamepad_;

  bool chrome_loaded_ = false;
  std::string pending_state_;
  std::string focus_ = "page";
  int band_height_ = 0;
  double last_pointer_y_ = 1e9;

  std::set<guint> held_keys_;
  gint64 last_tick_us_ = 0;
  guint tick_source_ = 0;
  std::size_t next_input_ = 0;
  std::optional<core::Button> input_release_;
  int exit_code_ = 0;
  bool quitting_ = false;
};

}  // namespace prosperobrowser::host
