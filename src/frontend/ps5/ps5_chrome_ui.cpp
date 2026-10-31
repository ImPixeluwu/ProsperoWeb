// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_chrome_ui.h"

#include <WebKit/WKEvent.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPagePrivatePlayStation.h>

#include "ps5_log.h"
#include "wk_helpers.h"

namespace ps5browser::frontend {

namespace {

// Shown when chrome.html is missing or fails to load: the active tab's title
// and URL in the band, a pointer ring, and a hint to reinstall the chrome.
// Gamepad shortcuts keep working; the controls of the real chrome do not.
constexpr const char* kFallbackChromeHtml = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><style>
html,body{margin:0;height:100%;overflow:hidden;background:transparent;
  font:22px sans-serif;color:#eef0f4}
#band{position:fixed;left:0;top:0;right:0;height:96px;background:#121418;
  padding:12px 24px;box-sizing:border-box}
#title{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#url{color:#9aa2b1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#warn{position:fixed;right:24px;top:12px;color:#ffb547;font-size:18px}
#pointer{position:fixed;left:0;top:0;width:36px;height:36px;margin:-18px 0 0 -18px;
  border-radius:50%;border:3px solid #fff;background:rgba(61,139,255,.25);box-sizing:border-box}
</style></head><body>
<div id="band"><div id="title"></div><div id="url"></div>
<div id="warn">chrome.html missing: reinstall /data/ps5browser/chrome</div></div>
<div id="pointer" hidden></div>
<script>
window.ps5chrome = {
  render: function (s) {
    var t = (s.tabs || []).filter(function (x) { return x.active; })[0] || {};
    document.getElementById('band').hidden = !s.chrome_visible;
    document.getElementById('title').textContent = t.title || '';
    document.getElementById('url').textContent = (s.address || {}).text || t.url || '';
    var p = s.pointer || {}, n = document.getElementById('pointer');
    n.hidden = !p.visible;
    n.style.transform = 'translate(' + Math.round(p.x || 0) + 'px,' + Math.round(p.y || 0) + 'px)';
  },
  click: function () {}
};
try { window.webkit.messageHandlers.chrome.postMessage({ type: 'ready' }); } catch (e) {}
</script></body></html>)HTML";

}  // namespace

Ps5ChromeUi::Ps5ChromeUi(WKContextRef context, WKWebsiteDataStoreRef data_store,
                         const std::string& chrome_url, int width, int height,
                         EventHandler on_event)
    : on_event_(std::move(on_event)) {
  WkWebView::Callbacks cb;
  cb.script_message = [this](const std::string& json) {
    if (!on_event_) {
      return;
    }
    nlohmann::json doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_discarded()) {
      on_event_(doc);
    }
  };
  cb.load_failed = [this](const std::string& url, const std::string& error) {
    log_printf("chrome: loading %s failed: %s", url.c_str(), error.c_str());
    load_fallback();
  };

  // The chrome is our own trusted UI; no page scripts are injected, and
  // fullscreen is irrelevant for it.
  web_ = std::make_unique<WkWebView>(context, data_store, "chrome",
                                     std::vector<std::string>{},
                                     /*fullscreen_enabled=*/false, std::move(cb));
  // The chrome is composed over the page: keep what it does not paint
  // transparent (chrome.css leaves html/body transparent) instead of white.
  WKPageSetDrawsTransparentBackground(web_->page(), true);
  web_->set_size(width, height);
  web_->set_visible(true);
  if (chrome_url.empty()) {
    load_fallback();
  } else {
    web_->load_url(chrome_url);
  }
}

void Ps5ChromeUi::load_fallback() {
  if (fallback_loaded_) {
    return;
  }
  fallback_loaded_ = true;
  log_line("chrome: using the built-in fallback chrome");
  auto html = make_wk_string(kFallbackChromeHtml);
  WKPageLoadHTMLString(web_->page(), html.get(), nullptr);
}

void Ps5ChromeUi::render(const nlohmann::json& state) {
  web_->run_javascript("window.ps5chrome && window.ps5chrome.render(" + state.dump() +
                       ")");
}

void Ps5ChromeUi::click(int x, int y) {
  // Window coordinates == chrome-local coordinates (the chrome view spans the
  // whole surface). A synthesized click is acceptable here: this is our own UI
  // and page input never goes through this path.
  web_->pointer_move(x, y);
  web_->pointer_button(x, y, kWKEventMouseButtonLeftButton, true);
  web_->pointer_button(x, y, kWKEventMouseButtonLeftButton, false);
}

}  // namespace ps5browser::frontend
