// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_chrome_ui.h"

#include <WebKit/WKEvent.h>

namespace ps5browser::frontend {

Ps5ChromeUi::Ps5ChromeUi(WKContextRef context, WKWebsiteDataStoreRef data_store,
                         const std::string& chrome_url, EventHandler on_event)
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

  // The chrome is our own trusted UI; no page scripts are injected, and
  // fullscreen is irrelevant for it.
  web_ = std::make_unique<WkWebView>(context, data_store, "chrome",
                                     std::vector<std::string>{},
                                     /*fullscreen_enabled=*/false, std::move(cb));
  web_->set_size(1920, 1080);
  web_->set_visible(true);
  web_->load_url(chrome_url);
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
