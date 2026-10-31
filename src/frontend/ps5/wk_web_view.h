// SPDX-License-Identifier: GPL-3.0-or-later
//
// A thin C++ wrapper around one WebKit PlayStation WKView + its WKPage: it
// installs the view/navigation/state/UI clients and a user-content script
// message handler, forwards everything through std::function callbacks, and
// exposes the input, navigation and WKPagePaint() surface the frontend needs.
//
// Both the web-content views (Ps5EngineView) and the HTML chrome
// (Ps5ChromeUi) are built on this.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <WebKit/WKBase.h>
#include <WebKit/WKGeometry.h>

namespace ps5browser::frontend {

// A dirty rectangle in view-local pixels.
struct DirtyRect {
  bool valid = false;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Smallest rectangle covering both a and b (an invalid rect is empty).
inline DirtyRect union_of(const DirtyRect& a, const DirtyRect& b) {
  if (!a.valid) {
    return b;
  }
  if (!b.valid) {
    return a;
  }
  const int x0 = a.x < b.x ? a.x : b.x;
  const int y0 = a.y < b.y ? a.y : b.y;
  const int x1 = (a.x + a.width) > (b.x + b.width) ? a.x + a.width : b.x + b.width;
  const int y1 = (a.y + a.height) > (b.y + b.height) ? a.y + a.height : b.y + b.height;
  return DirtyRect{true, x0, y0, x1 - x0, y1 - y0};
}

class WkWebView {
 public:
  struct Callbacks {
    std::function<void(const DirtyRect&)> needs_display;
    std::function<void()> enter_fullscreen;
    std::function<void()> exit_fullscreen;
    std::function<void()> load_started;
    std::function<void()> load_finished;
    std::function<void(const std::string& url, const std::string& error)> load_failed;
    std::function<void(const std::string& url)> url_changed;
    std::function<void(const std::string& title)> title_changed;
    std::function<void(bool can_back, bool can_forward)> nav_state_changed;
    std::function<void(const std::string& url)> create_new_page;
    // Body of a window.webkit.messageHandlers.<name>.postMessage(), as JSON.
    std::function<void(const std::string& json)> script_message;
  };

  // context/data_store are shared and owned by the caller (the factory).
  // message_handler_name is the script-message handler to register (e.g.
  // "ps5browser" or "chrome"). user_scripts are injected at document start in
  // all frames. fullscreen_enabled toggles the HTML5 Fullscreen API.
  WkWebView(WKContextRef context, WKWebsiteDataStoreRef data_store,
            const std::string& message_handler_name,
            const std::vector<std::string>& user_scripts,
            bool fullscreen_enabled, Callbacks callbacks);
  ~WkWebView();

  WkWebView(const WkWebView&) = delete;
  WkWebView& operator=(const WkWebView&) = delete;

  WKViewRef view() const { return view_; }
  WKPageRef page() const { return page_; }

  // Accessor for the C-API trampolines (which receive this as clientInfo).
  Callbacks& callbacks() { return cb_; }

  // Navigation.
  void load_url(const std::string& url);
  void go_back();
  void go_forward();
  void reload();
  void stop();

  // Geometry / visibility.
  void set_size(int width, int height);
  int width() const { return width_; }
  int height() const { return height_; }
  void set_visible(bool visible);

  // Trusted input (real engine events).
  void pointer_move(int x, int y);
  void pointer_button(int x, int y, int wk_mouse_button, bool pressed);
  void scroll(int x, int y, int dx, int dy);
  // keysym is a WPE keysym (WPE_KEY_*), which is what the PlayStation
  // WKPageHandleKeyboardEvent expects in virtualKeyCode.
  void key(std::uint32_t keysym, std::uint32_t wk_modifiers, bool pressed,
           const std::string& text);

  void run_javascript(const std::string& js);
  void set_user_agent(const std::string& user_agent);
  void request_exit_fullscreen();

  // Paints the dirty area into an ARGB32 (little-endian: B,G,R,A bytes) buffer
  // of size width*height*4. rect is clamped to the surface by WebKit.
  void paint(unsigned char* argb, int width, int height, const DirtyRect& rect);

  // Returns and clears the pending dirty rect: the union of every
  // needs_display rect since the last call.
  DirtyRect take_dirty();

 private:
  // C-API trampolines (clientInfo == this).
  static void s_needs_display(WKViewRef, WKRect, const void*);
  static void s_enter_fullscreen(WKViewRef, const void*);
  static void s_exit_fullscreen(WKViewRef, const void*);
  static void s_began_enter_fullscreen(WKViewRef, WKRect, WKRect, const void*);
  static void s_began_exit_fullscreen(WKViewRef, WKRect, WKRect, const void*);
  void notify_fullscreen_exited(const char* how);

  Callbacks cb_;
  WKViewRef view_ = nullptr;
  WKPageRef page_ = nullptr;
  WKUserContentControllerRef content_controller_ = nullptr;
  WKPreferencesRef preferences_ = nullptr;
  DirtyRect dirty_;
  int width_ = 1920;
  int height_ = 1080;
  bool in_fullscreen_ = false;
  // Diagnostics: the view's name in log.txt and one-shot "first" events.
  std::string name_;
  bool needs_display_logged_ = false;
  bool paint_logged_ = false;
};

}  // namespace ps5browser::frontend
