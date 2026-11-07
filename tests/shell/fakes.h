// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "prosperoweb/shell/chrome_ui.h"
#include "prosperoweb/shell/engine_view.h"
#include "prosperoweb/shell/remote.h"
#include "prosperoweb/shell/text_input.h"

namespace prosperoweb::shell::test {

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
  void pointer(int x, int y, bool visible) override {
    pointer_calls.push_back({x, y, visible});
  }

  struct PointerCall {
    int x = 0;
    int y = 0;
    bool visible = false;
  };

  nlohmann::json last;
  int renders = 0;
  std::vector<PointerCall> pointer_calls;
  std::vector<std::pair<int, int>> clicks;
};

// Records requests; the test completes them with finish_*(). With
// refuse = true request_text() fails to start; with sync_error set the
// request completes with that error from inside request_text().
class FakeTextInputProvider final : public TextInputProvider {
 public:
  bool request_text(const TextInputRequest& request, Completion done) override {
    requests.push_back(request);
    if (refuse) {
      return false;
    }
    done_ = std::move(done);
    if (sync_error) {
      complete({TextInputResult::Outcome::Error, "", *sync_error});
    }
    return true;
  }
  void poll() override { ++polls; }
  bool active() const override { return static_cast<bool>(done_); }

  void finish_done(const std::string& text) {
    complete({TextInputResult::Outcome::Done, text, 0});
  }
  void finish_cancelled() { complete({TextInputResult::Outcome::Cancelled, "", 0}); }
  void finish_error(std::int64_t code) { complete({TextInputResult::Outcome::Error, "", code}); }

  std::vector<TextInputRequest> requests;
  bool refuse = false;
  std::optional<std::int64_t> sync_error;
  int polls = 0;

 private:
  void complete(const TextInputResult& result) {
    Completion done = std::move(done_);
    done_ = nullptr;
    if (done) {
      done(result);
    }
  }
  Completion done_;
};

// A remote control without sockets; tests set `devices` to simulate phones
// pairing and `fail` to make start() fail.
class FakeRemoteControl final : public RemoteControl {
 public:
  std::optional<Info> start(std::string& error) override {
    ++starts;
    if (fail) {
      error = "port in use";
      return std::nullopt;
    }
    on = true;
    pin = std::to_string(100000 + starts);
    return info();
  }
  void stop() override {
    on = false;
    devices = 0;
  }
  bool running() const override { return on; }
  std::optional<Info> info() const override {
    if (!on) return std::nullopt;
    return Info{"http://192.0.2.1:17780", pin};
  }
  std::size_t paired_devices() const override { return devices; }
  void disconnect_all() override {
    devices = 0;
    ++disconnects;
  }

  bool on = false;
  bool fail = false;
  std::size_t devices = 0;
  int starts = 0;
  int disconnects = 0;
  std::string pin;
};

}  // namespace prosperoweb::shell::test
