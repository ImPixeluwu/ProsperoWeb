// SPDX-License-Identifier: GPL-3.0-or-later
//
// ChromeUi implemented as a second WKView loading the HTML chrome.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <WebKit/WKBase.h>
#include <nlohmann/json.hpp>

#include "ps5browser/shell/chrome_ui.h"
#include "wk_web_view.h"

namespace ps5browser::frontend {

class Ps5ChromeUi final : public shell::ChromeUi {
 public:
  using EventHandler = std::function<void(const nlohmann::json&)>;

  // context/data_store are shared with the content views. chrome_url is the
  // file:// URL of chrome.html. on_event receives decoded chrome events.
  Ps5ChromeUi(WKContextRef context, WKWebsiteDataStoreRef data_store,
              const std::string& chrome_url, EventHandler on_event);
  ~Ps5ChromeUi() override = default;

  // shell::ChromeUi
  void render(const nlohmann::json& state) override;
  void click(int x, int y) override;

  WkWebView& web() { return *web_; }

 private:
  EventHandler on_event_;
  std::unique_ptr<WkWebView> web_;
};

}  // namespace ps5browser::frontend
