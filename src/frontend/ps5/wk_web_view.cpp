// SPDX-License-Identifier: GPL-3.0-or-later
#include "wk_web_view.h"

#include <nlohmann/json.hpp>

#include <WebKit/WKArray.h>
#include <WebKit/WKContext.h>
#include <WebKit/WKDictionary.h>
#include <WebKit/WKNumber.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPageFullScreenClient.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKPageStateClient.h>
#include <WebKit/WKPageUIClient.h>
#include <WebKit/WKPreferencesRef.h>
#include <WebKit/WKPreferencesRefPrivate.h>
#include <WebKit/WKScriptMessageRef.h>
#include <WebKit/WKString.h>
#include <WebKit/WKType.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKURLRequest.h>
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>
#include <WebKit/WKUserScriptInjectionTime.h>

#include <WebKit/WKEventPlayStation.h>
#include <WebKit/WKNavigationActionRef.h>
#include <WebKit/WKPagePrivatePlayStation.h>
#include <WebKit/WKView.h>
#include <WebKit/WKViewClient.h>

#include "wk_helpers.h"

namespace ps5browser::frontend {

namespace {

WkWebView* self_of(const void* ci) {
  return const_cast<WkWebView*>(static_cast<const WkWebView*>(ci));
}

// Converts a WKTypeRef (produced by WKScriptMessageGetBody) into JSON.
nlohmann::json wk_value_to_json(WKTypeRef value) {
  if (!value) {
    return nullptr;
  }
  WKTypeID id = WKGetTypeID(value);
  if (id == WKStringGetTypeID()) {
    return to_std_string(static_cast<WKStringRef>(value));
  }
  if (id == WKBooleanGetTypeID()) {
    return static_cast<bool>(WKBooleanGetValue(static_cast<WKBooleanRef>(value)));
  }
  if (id == WKDoubleGetTypeID()) {
    return WKDoubleGetValue(static_cast<WKDoubleRef>(value));
  }
  if (id == WKUInt64GetTypeID()) {
    return WKUInt64GetValue(static_cast<WKUInt64Ref>(value));
  }
  if (id == WKArrayGetTypeID()) {
    auto arr = static_cast<WKArrayRef>(value);
    nlohmann::json out = nlohmann::json::array();
    for (size_t i = 0, n = WKArrayGetSize(arr); i < n; i++) {
      out.push_back(wk_value_to_json(WKArrayGetItemAtIndex(arr, i)));
    }
    return out;
  }
  if (id == WKDictionaryGetTypeID()) {
    auto dict = static_cast<WKDictionaryRef>(value);
    nlohmann::json out = nlohmann::json::object();
    WKArrayRef keys = WKDictionaryCopyKeys(dict);
    for (size_t i = 0, n = WKArrayGetSize(keys); i < n; i++) {
      auto key = static_cast<WKStringRef>(WKArrayGetItemAtIndex(keys, i));
      out[to_std_string(key)] = wk_value_to_json(WKDictionaryGetItemForKey(dict, key));
    }
    WKRelease(keys);
    return out;
  }
  return nullptr;
}

// --- navigation client ----------------------------------------------------

void nav_did_start(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci) {
  auto& cb = self_of(ci)->callbacks();
  if (cb.load_started) {
    cb.load_started();
  }
}

void nav_did_finish(WKPageRef, WKNavigationRef, WKTypeRef, const void* ci) {
  auto& cb = self_of(ci)->callbacks();
  if (cb.load_finished) {
    cb.load_finished();
  }
}

void nav_did_fail(WKPageRef page, WKNavigationRef, WKErrorRef error,
                  WKTypeRef, const void* ci) {
  auto& cb = self_of(ci)->callbacks();
  if (!cb.load_failed) {
    return;
  }
  std::string url = consume_wk_url(WKPageCopyActiveURL(page));
  std::string desc;
  if (error) {
    WKStringRef s = WKErrorCopyLocalizedDescription(error);
    desc = to_std_string(s);
    if (s) {
      WKRelease(s);
    }
  }
  cb.load_failed(url, desc);
}

// --- state client (title/url/nav state/loading) ---------------------------

void state_did_change_title(const void* ci) {
  // The WkWebView stashes its page in the callbacks' owner; we fetch the
  // current title from the page recorded in the UI client. Simpler: the state
  // client's clientInfo is the WkWebView, and we read the page from it.
  auto* self = self_of(ci);
  auto& cb = self->callbacks();
  if (!cb.title_changed || !self->page()) {
    return;
  }
  WKStringRef t = WKPageCopyTitle(self->page());
  cb.title_changed(to_std_string(t));
  if (t) {
    WKRelease(t);
  }
}

void state_did_change_url(const void* ci) {
  auto* self = self_of(ci);
  auto& cb = self->callbacks();
  if (cb.url_changed && self->page()) {
    cb.url_changed(consume_wk_url(WKPageCopyActiveURL(self->page())));
  }
}

void state_did_change_nav(const void* ci) {
  auto* self = self_of(ci);
  auto& cb = self->callbacks();
  if (cb.nav_state_changed && self->page()) {
    cb.nav_state_changed(WKPageCanGoBack(self->page()),
                         WKPageCanGoForward(self->page()));
  }
}

// --- UI client (popups) ----------------------------------------------------

WKPageRef ui_create_new_page(WKPageRef, WKPageConfigurationRef,
                             WKNavigationActionRef action,
                             WKWindowFeaturesRef, const void* ci) {
  auto& cb = self_of(ci)->callbacks();
  if (cb.create_new_page && action) {
    WKURLRequestRef req = WKNavigationActionCopyRequest(action);
    if (req) {
      std::string url = consume_wk_url(WKURLRequestCopyURL(req));
      WKRelease(req);
      if (!url.empty()) {
        cb.create_new_page(url);
      }
    }
  }
  // We never let the engine open a window; the shell opens a tab instead.
  return nullptr;
}

// --- script message handler ------------------------------------------------

void script_message(WKScriptMessageRef message, WKCompletionListenerRef reply,
                     const void* ci) {
  auto& cb = self_of(ci)->callbacks();
  if (cb.script_message && message) {
    // A string body is passed through as is (it is expected to hold JSON
    // already); any other value is serialised once. Re-encoding a string
    // would hand the receiver a JSON string instead of the object inside it.
    WKTypeRef body = WKScriptMessageGetBody(message);
    if (body && WKGetTypeID(body) == WKStringGetTypeID()) {
      cb.script_message(to_std_string(static_cast<WKStringRef>(body)));
    } else {
      cb.script_message(wk_value_to_json(body).dump());
    }
  }
  if (reply) {
    WKCompletionListenerComplete(reply, nullptr);
  }
}

WKRect to_wk_rect(const DirtyRect& r) {
  return WKRectMake(r.x, r.y, r.width, r.height);
}

}  // namespace

// --- view-client trampolines (static members) ------------------------------

void WkWebView::s_needs_display(WKViewRef, WKRect rect, const void* ci) {
  auto* self = self_of(ci);
  DirtyRect d{true, static_cast<int>(rect.origin.x), static_cast<int>(rect.origin.y),
              static_cast<int>(rect.size.width), static_cast<int>(rect.size.height)};
  if (d.width <= 0 || d.height <= 0) {
    return;
  }
  // Several invalidations can arrive between two frames; keep their union.
  self->dirty_ = union_of(self->dirty_, d);
  if (self->cb_.needs_display) {
    self->cb_.needs_display(d);
  }
}

void WkWebView::s_enter_fullscreen(WKViewRef, const void* ci) {
  auto& cb = self_of(ci)->callbacks();
  if (cb.enter_fullscreen) {
    cb.enter_fullscreen();
  }
}

void WkWebView::s_exit_fullscreen(WKViewRef, const void* ci) {
  auto& cb = self_of(ci)->callbacks();
  if (cb.exit_fullscreen) {
    cb.exit_fullscreen();
  }
}

// --- construction -----------------------------------------------------------

WkWebView::WkWebView(WKContextRef context, WKWebsiteDataStoreRef data_store,
                     const std::string& message_handler_name,
                     const std::vector<std::string>& user_scripts,
                     bool fullscreen_enabled, Callbacks callbacks)
    : cb_(std::move(callbacks)) {
  content_controller_ = WKUserContentControllerCreate();
  for (const auto& src : user_scripts) {
    auto s = make_wk_string(src);
    WKUserScriptRef script = WKUserScriptCreateWithSource(
        s.get(), kWKInjectAtDocumentStart, /*forMainFrameOnly=*/false);
    WKUserContentControllerAddUserScript(content_controller_, script);
    WKRelease(script);
  }
  {
    auto name = make_wk_string(message_handler_name);
    WKUserContentControllerAddScriptMessageHandler(content_controller_, name.get(),
                                                   &script_message, this);
  }

  preferences_ = WKPreferencesCreate();
  WKPreferencesSetFullScreenEnabled(preferences_, fullscreen_enabled);
  // Phase 1 is the non-accelerated raster path (WKPagePaint + SDL framebuffer).
  WKPreferencesSetAcceleratedCompositingEnabled(preferences_, false);
  WKPreferencesSetJavaScriptCanOpenWindowsAutomatically(preferences_, true);

  WKPageConfigurationRef config = WKPageConfigurationCreate();
  WKPageConfigurationSetContext(config, context);
  WKPageConfigurationSetPreferences(config, preferences_);
  WKPageConfigurationSetUserContentController(config, content_controller_);
  if (data_store) {
    WKPageConfigurationSetWebsiteDataStore(config, data_store);
  }

  view_ = WKViewCreate(config);
  WKRelease(config);
  page_ = WKViewGetPage(view_);

  WKViewClientV0 view_client{};
  view_client.base.version = 0;
  view_client.base.clientInfo = this;
  view_client.setViewNeedsDisplay = &WkWebView::s_needs_display;
  view_client.enterFullScreen = &WkWebView::s_enter_fullscreen;
  view_client.exitFullScreen = &WkWebView::s_exit_fullscreen;
  WKViewSetViewClient(view_, &view_client.base);

  WKPageNavigationClientV0 nav_client{};
  nav_client.base.version = 0;
  nav_client.base.clientInfo = this;
  nav_client.didStartProvisionalNavigation = &nav_did_start;
  nav_client.didFinishNavigation = &nav_did_finish;
  nav_client.didFailNavigation = &nav_did_fail;
  nav_client.didFailProvisionalNavigation = &nav_did_fail;
  WKPageSetPageNavigationClient(page_, &nav_client.base);

  WKPageStateClientV0 state_client{};
  state_client.base.version = 0;
  state_client.base.clientInfo = this;
  state_client.didChangeTitle = &state_did_change_title;
  state_client.didChangeActiveURL = &state_did_change_url;
  state_client.didChangeCanGoBack = &state_did_change_nav;
  state_client.didChangeCanGoForward = &state_did_change_nav;
  WKPageSetPageStateClient(page_, &state_client.base);

  WKPageUIClientV19 ui_client{};
  ui_client.base.version = 19;
  ui_client.base.clientInfo = this;
  ui_client.createNewPage = &ui_create_new_page;
  WKPageSetPageUIClient(page_, &ui_client.base);
}

WkWebView::~WkWebView() {
  // The view owns the page; release the pieces we created.
  if (view_) {
    WKRelease(view_);
  }
  if (preferences_) {
    WKRelease(preferences_);
  }
  if (content_controller_) {
    WKRelease(content_controller_);
  }
}

// --- navigation / geometry --------------------------------------------------

void WkWebView::load_url(const std::string& url) {
  WKURLRef u = WKURLCreateWithUTF8CString(url.c_str());
  WKPageLoadURL(page_, u);
  WKRelease(u);
}

void WkWebView::go_back() { WKPageGoBack(page_); }
void WkWebView::go_forward() { WKPageGoForward(page_); }
void WkWebView::reload() { WKPageReload(page_); }
void WkWebView::stop() { WKPageStopLoading(page_); }

void WkWebView::set_size(int width, int height) {
  width_ = width;
  height_ = height;
  WKViewSetSize(view_, WKSizeMake(width, height));
}

void WkWebView::set_visible(bool visible) {
  WKViewSetVisible(view_, visible);
  WKViewSetActive(view_, visible);
  WKViewSetFocus(view_, visible);
}

// --- input ------------------------------------------------------------------

void WkWebView::pointer_move(int x, int y) {
  WKMouseEvent e = WKMouseEventMake(kWKEventMouseMove, kWKEventMouseButtonNoButton,
                                    WKPointMake(x, y), 0, 0);
  WKPageHandleMouseEvent(page_, e);
}

void WkWebView::pointer_button(int x, int y, int wk_mouse_button, bool pressed) {
  WKMouseEvent e = WKMouseEventMake(pressed ? kWKEventMouseDown : kWKEventMouseUp,
                                    static_cast<WKEventMouseButton>(wk_mouse_button),
                                    WKPointMake(x, y), 1, 0);
  WKPageHandleMouseEvent(page_, e);
}

void WkWebView::scroll(int x, int y, int dx, int dy) {
  WKWheelEvent e = WKWheelEventMake(kWKEventWheel, WKPointMake(x, y),
                                    WKSizeMake(dx, dy), WKSizeMake(0, 0), 0);
  WKPageHandleWheelEvent(page_, e);
}

void WkWebView::key(std::uint32_t key_code, std::uint32_t wk_modifiers,
                    bool pressed, const std::string& text) {
  WKKeyboardEvent e = WKKeyboardEventMake(
      pressed ? kWKEventKeyDown : kWKEventKeyUp, kWKInputTypeNormal, text.c_str(),
      static_cast<uint32_t>(text.size()), nullptr,
      static_cast<int32_t>(key_code), 0, 0, wk_modifiers);
  WKPageHandleKeyboardEvent(page_, e);
}

void WkWebView::run_javascript(const std::string& js) {
  auto s = make_wk_string(js);
  WKPageEvaluateJavaScriptInMainFrame(page_, s.get(), nullptr, nullptr);
}

void WkWebView::set_user_agent(const std::string& user_agent) {
  auto s = make_wk_string(user_agent);
  WKPageSetCustomUserAgent(page_, s.get());
}

void WkWebView::request_exit_fullscreen() { WKViewRequestExitFullScreen(view_); }

// --- painting ---------------------------------------------------------------

void WkWebView::paint(unsigned char* argb, int width, int height,
                      const DirtyRect& rect) {
  if (!argb) {
    return;
  }
  WKRect r = rect.valid ? to_wk_rect(rect)
                        : WKRectMake(0, 0, width, height);
  WKPagePaint(page_, argb, WKSizeMake(width, height), r);
}

DirtyRect WkWebView::take_dirty() {
  DirtyRect d = dirty_;
  dirty_ = DirtyRect{};
  return d;
}

}  // namespace ps5browser::frontend
