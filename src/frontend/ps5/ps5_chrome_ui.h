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
  // file:// URL of chrome.html; when it is empty or fails to load, a small
  // built-in fallback chrome is shown instead. The chrome view spans the
  // whole window (width x height) and paints over a transparent background.
  // on_event receives decoded chrome events.
  Ps5ChromeUi(WKContextRef context, WKWebsiteDataStoreRef data_store,
              const std::string& chrome_url, int width, int height,
              EventHandler on_event);
  ~Ps5ChromeUi() override = default;

  // shell::ChromeUi
  void render(const nlohmann::json& state) override;
  void click(int x, int y) override;

  WkWebView& web() { return *web_; }

 private:
  void load_fallback();

  EventHandler on_event_;
  std::unique_ptr<WkWebView> web_;
  bool fallback_loaded_ = false;
  int clicks_logged_ = 0;
  bool first_event_logged_ = false;
};

}  // namespace ps5browser::frontend
