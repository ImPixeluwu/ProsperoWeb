// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ps5browser/shell/chrome_ui.h"
#include "ps5browser/shell/engine_view.h"

namespace ps5browser::shell::test {

// Records every call as a short string, e.g. "load https://a.example".
class FakeEngineView final : public EngineView {
 public:
  explicit FakeEngineView(EngineViewClient& client) : client_(client) {}

  void load(const std::string& url) override { log("load " + url); }
  void go_back() override { log("go_back"); }
  void go_forward() override { log("go_forward"); }
  void reload() override { log("reload"); }
  void stop() override { log("stop"); }
  void set_size(int w, int h) override {
    width = w;
    height = h;
  }
  void set_visible(bool v) override {
    visible = v;
    log(v ? "show" : "hide");
  }
  void pointer_move(int x, int y) override {
    log("pointer_move " + std::to_string(x) + "," + std::to_string(y));
  }
  void pointer_button(int x, int y, MouseButton b, bool pressed) override {
    log(std::string("pointer_button ") + std::to_string(x) + "," + std::to_string(y) + " " +
        std::to_string(static_cast<int>(b)) + (pressed ? " down" : " up"));
  }
  void scroll(int x, int y, int dx, int dy) override {
    (void)x;
    (void)y;
    log("scroll " + std::to_string(dx) + "," + std::to_string(dy));
  }
  void key(std::uint32_t code, std::uint32_t, bool pressed, const std::string&) override {
    log("key " + std::to_string(code) + (pressed ? " down" : " up"));
  }
  void insert_text(const std::string& utf8) override { log("insert_text " + utf8); }
  void request_exit_fullscreen() override { log("request_exit_fullscreen"); }
  void run_javascript(const std::string& js) override { log("js " + js); }
  void set_user_agent(const std::string& ua) override { log("ua " + ua); }

  bool called(const std::string& entry) const {
    return std::find(calls.begin(), calls.end(), entry) != calls.end();
  }
  bool called_prefix(const std::string& prefix) const {
    return std::any_of(calls.begin(), calls.end(),
                       [&](const std::string& c) { return c.starts_with(prefix); });
  }
  std::size_t count(const std::string& entry) const {
    return static_cast<std::size_t>(std::count(calls.begin(), calls.end(), entry));
  }

  EngineViewClient& client() { return client_; }

  std::vector<std::string> calls;
  bool visible = false;
  int width = 0;
  int height = 0;

 private:
  void log(std::string s) { calls.push_back(std::move(s)); }
  EngineViewClient& client_;
};

class FakeEngineFactory final : public EngineFactory {
 public:
  std::unique_ptr<EngineView> create(EngineViewClient& client) override {
    auto view = std::make_unique<FakeEngineView>(client);
    created.push_back(view.get());
    return view;
  }
  // Views in creation order; pointers stay valid while the tab is open.
  std::vector<FakeEngineView*> created;
};

class FakeChromeUi final : public ChromeUi {
 public:
  void render(const nlohmann::json& state) override {
    last = state;
    ++renders;
  }
  void click(int x, int y) override { clicks.emplace_back(x, y); }

  nlohmann::json last;
  int renders = 0;
  std::vector<std::pair<int, int>> clicks;
};

}  // namespace ps5browser::shell::test
