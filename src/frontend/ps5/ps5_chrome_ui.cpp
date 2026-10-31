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
    if (doc.is_discarded()) {
      return;
    }
    const std::string type = doc.is_object() ? doc.value("type", std::string()) : std::string();
    if (type == "click_trace") {
      // chrome.js reports what a click activated.
      log_printf("chrome: click at %d,%d -> %s (%s)", doc.value("x", -1), doc.value("y", -1),
                 doc.value("handler", std::string("?")).c_str(),
                 doc.value("target", std::string("?")).c_str());
      return;
    }
    if (!first_event_logged_) {
      first_event_logged_ = true;
      log_printf("chrome: first event from the chrome: %s", type.c_str());
    }
    on_event_(doc);
  };
  cb.load_finished = []() { log_line("chrome: navigation finished"); };
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
    log_printf("chrome: loading %s", chrome_url.c_str());
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
  // Every attempt is logged; what it activated follows as a click_trace line
  // once the chrome's script message arrives.
  log_printf("chrome: click attempt at %d,%d", x, y);
  // Window coordinates == chrome-local CSS pixels (the chrome view spans the
  // whole surface at scale 1). The trusted move keeps :hover right; the
  // activation goes through chrome.js click(x, y) (elementFromPoint +
  // element.click(), the path the host frontend uses), which does not depend
  // on WebCore turning a synthesized press/release into a click and handles
  // the address field. This is our own UI: user activation is not needed.
  web_->pointer_move(x, y);
  web_->run_javascript("window.ps5chrome && window.ps5chrome.click(" + std::to_string(x) + "," +
                       std::to_string(y) + ")");
  if (fallback_loaded_) {
    log_printf("chrome: click at %d,%d -> fallback chrome (no controls)", x, y);
  }
}

}  // namespace ps5browser::frontend
