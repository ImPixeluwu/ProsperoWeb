// SPDX-License-Identifier: GPL-3.0-or-later
//
// EngineView / EngineFactory implemented on the WebKit PlayStation C API.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <WebKit/WKBase.h>

#include "prosperoweb/shell/engine_view.h"
#include "wk_web_view.h"

namespace prosperoweb::frontend {

// One web-content view (one per tab).
class Ps5EngineView final : public shell::EngineView {
 public:
  Ps5EngineView(shell::EngineViewClient& client, WKContextRef context,
                WKWebsiteDataStoreRef data_store,
                const std::vector<std::string>& user_scripts,
                const std::string& user_agent);
  ~Ps5EngineView() override = default;

  // shell::EngineView
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

  // Compositor access for the frame loop.
  WkWebView& web() { return *web_; }

 private:
  shell::EngineViewClient& client_;
  shell::PageCursor cursor_ = shell::PageCursor::Pointer;
  std::unique_ptr<WkWebView> web_;
  int buttons_logged_ = 0;  // console diagnostics: the first presses/releases
};

// Creates web-content views sharing one WKContext and website data store.
class Ps5EngineFactory final : public shell::EngineFactory {
 public:
  // base_dir is the persistence root (/data/prosperoweb). user_scripts are
  // injected into every content view (EME detector, focus reporter, gamepad
  // bridge).
  Ps5EngineFactory(const std::string& base_dir,
                   std::vector<std::string> user_scripts,
                   std::string user_agent);
  ~Ps5EngineFactory() override;

  std::unique_ptr<shell::EngineView> create(shell::EngineViewClient& client) override;

  // The chrome view shares the content context and website data store.
  WKContextRef context() const { return context_; }
  WKWebsiteDataStoreRef data_store() const { return data_store_; }

 private:
  WKContextRef context_ = nullptr;
  WKWebsiteDataStoreRef data_store_ = nullptr;
  std::vector<std::string> user_scripts_;
  std::string user_agent_;
};

}  // namespace prosperoweb::frontend
