// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fakes.h"
#include "prosperoweb/core/in_memory_storage.h"
#include "prosperoweb/core/session.h"
#include "prosperoweb/shell/shell_controller.h"

using namespace prosperoweb;
using core::Button;
using shell::ShellController;
using shell::test::FakeChromeUi;
using shell::test::FakeEngineFactory;
using shell::test::FakeEngineView;
using Focus = ShellController::Focus;
using json = nlohmann::json;

namespace {

constexpr const char* kHome = "https://home.example/";

struct Fixture {
  core::InMemoryStorage storage;
  FakeEngineFactory factory;
  FakeChromeUi chrome;
  std::unique_ptr<ShellController> shell;
  std::string diagnostics_url;

  void make(bool start = true) {
    ShellController::Config config;
    config.diagnostics_url = diagnostics_url;
    config.width = 1920;
    config.height = 1080;
    config.home_url = kHome;
    config.user_agent = "TestUA/1.0";
    config.now = [] { return std::int64_t{1000}; };
    shell = std::make_unique<ShellController>(factory, chrome, storage, config);
    if (start) {
      shell->start();
    }
  }

  FakeEngineView& view(std::size_t i) { return *factory.created.at(i); }
  FakeEngineView& active() { return *static_cast<FakeEngineView*>(shell->active_view()); }

  void press(Button b) {
    shell->on_button(b, true);
    shell->on_button(b, false);
  }
  void frame(float dt = 1.0f / 60.0f) { shell->tick(dt); }
  // Simulates the engine finishing a load of url with a title.
  void finish_load(FakeEngineView& v, const std::string& url, const std::string& title) {
    v.client().load_started();
    v.client().url_changed(url);
    v.client().title_changed(title);
    v.client().load_finished();
  }
};

}  // namespace

TEST_SUITE("shell.session") {
  TEST_CASE("start without a session opens the home page") {
    Fixture f;
    f.make();
    REQUIRE(f.factory.created.size() == 1);
    CHECK(f.view(0).called(std::string("load ") + kHome));
    CHECK(f.view(0).called("ua TestUA/1.0"));
    CHECK(f.view(0).visible);
    CHECK(f.shell->tabs().size() == 1);
    CHECK(f.chrome.renders >= 1);
    CHECK(f.chrome.last["focus"] == "page");
  }

  TEST_CASE("start restores the saved session, loading only the active tab") {
    Fixture f;
    {
      core::TabManager tabs;
      core::History history;
      core::Bookmarks bookmarks;
      tabs.open("https://one.example/");
      tabs.open("https://two.example/");
      bookmarks.add("https://one.example/", "One", 1);
      history.add("https://one.example/", "One", 1);
      core::Session(f.storage).save_all(tabs, history, bookmarks);
    }
    f.make();
    CHECK(f.shell->tabs().size() == 2);
    REQUIRE(f.factory.created.size() == 1);
    CHECK(f.view(0).called("load https://two.example/"));
    CHECK(f.shell->bookmarks().contains("https://one.example/"));
    CHECK(f.shell->history().size() == 1);
    CHECK(f.chrome.last["tabs"].size() == 2);

    // Switching to the other tab creates and loads its view lazily.
    f.press(Button::L1);
    REQUIRE(f.factory.created.size() == 2);
    CHECK(f.view(1).called("load https://one.example/"));
    CHECK(f.view(1).visible);
    CHECK_FALSE(f.view(0).visible);
  }

  TEST_CASE("tabs and history are saved a few seconds after a change") {
    Fixture f;
    f.make();
    f.frame(10.0f);  // the start-up session is saved as well
    f.shell->navigate("example.org");
    f.finish_load(f.view(0), "https://example.org/", "Example");
    f.frame(1.0f);
    {
      core::TabManager tabs;
      REQUIRE(core::Session(f.storage).load_tabs(tabs));
      CHECK(tabs.tabs()[0].url == kHome);  // not yet: debounced
    }
    f.frame(ShellController::kSessionSaveDelaySeconds);
    core::TabManager tabs;
    core::History history;
    REQUIRE(core::Session(f.storage).load_tabs(tabs));
    REQUIRE(core::Session(f.storage).load_history(history));
    CHECK(tabs.tabs()[0].url == "https://example.org/");
    CHECK(history.size() == 1);
  }

  TEST_CASE("shutdown saves tabs, history and bookmarks") {
    Fixture f;
    f.make();
    f.shell->navigate("example.org");
    f.finish_load(f.view(0), "https://example.org/", "Example");
    f.shell->open_url_in_new_tab("second.example");
    f.shell->shutdown();

    core::TabManager tabs;
    core::History history;
    core::Bookmarks bookmarks;
    CHECK(core::Session(f.storage).load_all(tabs, history, bookmarks));
    CHECK(tabs.size() == 2);
    CHECK(tabs.tabs()[0].url == "https://example.org/");
    CHECK(tabs.active()->url == "https://second.example");
    CHECK(history.size() == 1);
  }
}

TEST_SUITE("shell.navigation") {
  TEST_CASE("address bar input is normalized and loaded in the active tab") {
    Fixture f;
    f.make();
    f.shell->on_chrome_event({{"type", "navigate"}, {"text", "  example.com  "}});
    CHECK(f.view(0).called("load https://example.com"));
    f.shell->on_chrome_event({{"type", "navigate"}, {"text", "ps5 browser"}});
    CHECK(f.view(0).called("load https://duckduckgo.com/?q=ps5%20browser"));
    f.shell->on_chrome_event({{"type", "navigate"}, {"text", "localhost:8080"}});
    CHECK(f.view(0).called("load http://localhost:8080"));
  }

  TEST_CASE("load events update tabs, chrome state and history") {
    Fixture f;
    f.make();
    f.view(0).client().load_started();
    f.frame();
    CHECK(f.chrome.last["nav"]["loading"] == true);
    CHECK(f.chrome.last["tabs"][0]["loading"] == true);
    f.view(0).client().url_changed("https://a.example/");
    f.view(0).client().title_changed("A page");
    f.view(0).client().nav_state_changed(true, false);
    f.view(0).client().load_finished();
    f.frame();
    CHECK(f.chrome.last["tabs"][0]["title"] == "A page");
    CHECK(f.chrome.last["address"]["text"] == "https://a.example/");
    CHECK(f.chrome.last["nav"]["back"] == true);
    CHECK(f.chrome.last["nav"]["loading"] == false);
    REQUIRE(f.shell->history().size() == 1);
    CHECK(f.shell->history().recent(1)[0].title == "A page");
  }

  TEST_CASE("load failure on the active tab shows an error notice") {
    Fixture f;
    f.make();
    f.view(0).client().load_failed("https://bad.example/", "Could not resolve host");
    f.frame();
    CHECK(f.chrome.last["notice"]["kind"] == "error");
    CHECK(f.shell->focus() == Focus::Notice);
    f.press(Button::Cross);
    CHECK(f.shell->focus() == Focus::Page);
  }

  TEST_CASE("Circle goes back only when possible") {
    Fixture f;
    f.make();
    f.press(Button::Circle);
    CHECK_FALSE(f.view(0).called("go_back"));
    f.view(0).client().nav_state_changed(true, false);
    f.press(Button::Circle);
    CHECK(f.view(0).count("go_back") == 1);
  }

  TEST_CASE("new window requests open a new tab") {
    Fixture f;
    f.make();
    f.view(0).client().new_window_requested("https://popup.example/");
    CHECK(f.shell->tabs().size() == 2);
    REQUIRE(f.factory.created.size() == 2);
    CHECK(f.view(1).called("load https://popup.example/"));
    CHECK(f.view(1).visible);
  }
}

TEST_SUITE("shell.tabs") {
  TEST_CASE("open, switch and close toggle view visibility") {
    Fixture f;
    f.make();
    CHECK(f.shell->open_url_in_new_tab("b.example"));
    REQUIRE(f.factory.created.size() == 2);
    CHECK_FALSE(f.view(0).visible);
    CHECK(f.view(1).visible);

    f.press(Button::L1);  // previous tab
    CHECK(f.view(0).visible);
    CHECK_FALSE(f.view(1).visible);
    f.press(Button::R1);  // next tab
    CHECK(f.view(1).visible);

    const auto first = f.shell->tabs().tabs()[0].id;
    f.shell->on_chrome_event({{"type", "tab_select"}, {"id", first}});
    CHECK(f.view(0).visible);

    const auto second = f.shell->tabs().tabs()[1].id;
    f.shell->on_chrome_event({{"type", "tab_close"}, {"id", second}});
    CHECK(f.shell->tabs().size() == 1);
    CHECK(f.shell->view_for(second) == nullptr);
  }

  TEST_CASE("a chrome event delivered as a JSON string is decoded") {
    Fixture f;
    f.make();
    CHECK(f.shell->open_url_in_new_tab("b.example"));
    const auto first = f.shell->tabs().tabs()[0].id;
    // What a frontend hands over when the chrome posts JSON.stringify(event).
    f.shell->on_chrome_event(json(json{{"type", "tab_select"}, {"id", first}}.dump()));
    CHECK(f.shell->tabs().active_id() == first);
    CHECK(f.view(0).visible);

    f.shell->on_chrome_event(json("{\"type\":\"tab_new\"}"));
    CHECK(f.shell->tabs().size() == 3);

    // Strings that are not an encoded object are ignored.
    f.shell->on_chrome_event(json("not json"));
    f.shell->on_chrome_event(json("\"tab_new\""));
    CHECK(f.shell->tabs().size() == 3);
  }

  TEST_CASE("closing the last tab opens the home page") {
    Fixture f;
    f.make();
    const auto id = *f.shell->tabs().active_id();
    f.shell->on_chrome_event({{"type", "tab_close"}, {"id", id}});
    CHECK(f.shell->tabs().size() == 1);
    CHECK(*f.shell->tabs().active_id() != id);
    REQUIRE(f.factory.created.size() == 2);
    CHECK(f.view(1).called(std::string("load ") + kHome));
    CHECK(f.view(1).visible);
  }

  TEST_CASE("NewTab when full shows the tab limit notice") {
    Fixture f;
    f.make();
    for (std::size_t i = 1; i < core::TabManager::kMaxTabs; ++i) {
      f.shell->on_chrome_event({{"type", "tab_new"}});
    }
    CHECK(f.shell->tabs().size() == core::TabManager::kMaxTabs);
    CHECK(f.shell->focus() == Focus::Page);
    f.shell->on_chrome_event({{"type", "tab_new"}});
    f.frame();
    CHECK(f.shell->tabs().size() == core::TabManager::kMaxTabs);
    CHECK(f.chrome.last["notice"]["title"] == "Tab limit reached");
    CHECK(f.shell->focus() == Focus::Notice);
  }
}

TEST_SUITE("shell.input") {
  TEST_CASE("Cross clicks at the pointer with trusted pointer events") {
    Fixture f;
    f.make();
    f.shell->on_sticks(1.0f, 0.0f, 0.0f, 0.0f);
    f.frame(0.1f);
    f.shell->on_sticks(0.0f, 0.0f, 0.0f, 0.0f);
    const auto p = f.shell->pointer();
    CHECK(p.x > 960.0f);
    CHECK(f.view(0).called_prefix("pointer_move "));
    f.frame();
    REQUIRE_FALSE(f.chrome.pointer_calls.empty());
    CHECK(f.chrome.pointer_calls.back().visible);
    CHECK(f.chrome.pointer_calls.back().x == static_cast<int>(p.x));

    f.view(0).calls.clear();
    f.press(Button::Cross);
    const int x = static_cast<int>(p.x);
    const int y = static_cast<int>(p.y) - ShellController::kChromeBandHeight;
    const std::string at = std::to_string(x) + "," + std::to_string(y);
    REQUIRE(f.view(0).calls.size() == 3);
    CHECK(f.view(0).calls[0] == "pointer_move " + at);
    CHECK(f.view(0).calls[1] == "pointer_button " + at + " 1 down");
    CHECK(f.view(0).calls[2] == "pointer_button " + at + " 1 up");
    CHECK_FALSE(f.view(0).called_prefix("js "));
  }

  TEST_CASE("Cross over the chrome band clicks the chrome, not the page") {
    Fixture f;
    f.make();
    f.shell->on_mouse_move(100.0f, 40.0f);
    f.view(0).calls.clear();
    f.press(Button::Cross);
    CHECK(f.view(0).calls.empty());
    REQUIRE(f.chrome.clicks.size() == 1);
    CHECK(f.chrome.clicks[0] == std::make_pair(100, 40));
  }

  // The console path: no mouse, the left stick drives the pointer up into
  // the band, Cross clicks there, and the chrome's event comes back.
  TEST_CASE("stick pointer moved into the chrome band: Cross reaches ChromeUi::click") {
    Fixture f;
    f.make();
    f.shell->set_viewport(1920, 1080);
    f.shell->on_sticks(0.0f, -1.0f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i) {
      f.frame(0.05f);
    }
    f.shell->on_sticks(0.0f, 0.0f, 0.0f, 0.0f);
    const auto p = f.shell->pointer();
    CHECK(p.y == doctest::Approx(0.0f));
    REQUIRE(p.y < static_cast<float>(ShellController::kChromeBandHeight));

    f.view(0).calls.clear();
    f.press(Button::Cross);
    CHECK_FALSE(f.view(0).called_prefix("pointer_button "));
    REQUIRE(f.chrome.clicks.size() == 1);
    CHECK(f.chrome.clicks[0] ==
          std::make_pair(static_cast<int>(p.x), static_cast<int>(p.y)));

    f.shell->on_chrome_event({{"type", "tab_new"}});
    CHECK(f.shell->tabs().size() == 2);
  }

  TEST_CASE("Cross at the last band row goes to the chrome, the next row to the page") {
    Fixture f;
    f.make();
    const float band = static_cast<float>(ShellController::kChromeBandHeight);
    f.shell->on_mouse_move(300.0f, band - 1.0f);
    f.press(Button::Cross);
    REQUIRE(f.chrome.clicks.size() == 1);
    CHECK(f.chrome.clicks[0] == std::make_pair(300, ShellController::kChromeBandHeight - 1));

    f.view(0).calls.clear();
    f.shell->on_mouse_move(300.0f, band);
    f.press(Button::Cross);
    CHECK(f.chrome.clicks.size() == 1);
    CHECK(f.view(0).called("pointer_button 300,0 1 down"));
  }

  TEST_CASE("with the chrome hidden, Cross at the top row clicks the page") {
    Fixture f;
    f.make();
    f.press(Button::R3);
    f.frame();
    REQUIRE(f.shell->content_top() == 0);
    f.shell->on_mouse_move(100.0f, 40.0f);
    f.view(0).calls.clear();
    f.press(Button::Cross);
    CHECK(f.chrome.clicks.empty());
    CHECK(f.view(0).called("pointer_button 100,40 1 down"));
  }

  TEST_CASE("D-pad and triggers scroll, with auto-repeat while held") {
    Fixture f;
    f.make();
    f.press(Button::Down);
    CHECK(f.view(0).called("scroll 0,120"));
    f.press(Button::R2);
    CHECK(f.view(0).called_prefix("scroll 0,8"));  // 85% of 984 px

    f.view(0).calls.clear();
    f.shell->on_button(Button::Up, true);
    CHECK(f.view(0).count("scroll 0,-120") == 1);
    f.frame(0.5f);  // 300 ms delay + repeats every 60 ms
    CHECK(f.view(0).count("scroll 0,-120") >= 3);
    f.shell->on_button(Button::Up, false);
    const auto n = f.view(0).count("scroll 0,-120");
    f.frame(0.5f);
    CHECK(f.view(0).count("scroll 0,-120") == n);
  }

  TEST_CASE("right stick scrolls the page") {
    Fixture f;
    f.make();
    f.shell->on_sticks(0.0f, 0.0f, 0.0f, 1.0f);
    f.frame(0.1f);
    CHECK(f.view(0).called_prefix("scroll 0,"));
  }

  TEST_CASE("mouse use hides the gamepad pointer until the stick moves") {
    Fixture f;
    f.make();
    f.shell->on_mouse_move(10.0f, 500.0f);
    f.frame();
    CHECK(f.chrome.last["pointer"]["visible"] == false);
    f.shell->on_sticks(0.0f, 1.0f, 0.0f, 0.0f);
    f.frame();
    CHECK(f.chrome.last["pointer"]["visible"] == true);
  }
}

TEST_SUITE("shell.pointer") {
  // Moves the pointer with the left stick for n frames of dt.
  void move_stick(Fixture& f, float lx, float ly, int frames, float dt = 1.0f / 60.0f) {
    f.shell->on_sticks(lx, ly, 0.0f, 0.0f);
    for (int i = 0; i < frames; ++i) {
      f.frame(dt);
    }
    f.shell->on_sticks(0.0f, 0.0f, 0.0f, 0.0f);
  }

  std::size_t moves(const FakeEngineView& v) {
    return static_cast<std::size_t>(std::count_if(v.calls.begin(), v.calls.end(), [](const std::string& c) {
      return c.starts_with("pointer_move ");
    }));
  }

  TEST_CASE("pointer motion does not re-render the chrome") {
    Fixture f;
    f.make();
    f.frame();
    const int renders = f.chrome.renders;
    f.chrome.pointer_calls.clear();
    move_stick(f, 1.0f, 0.0f, 30);
    CHECK(f.chrome.renders == renders);
    CHECK(f.chrome.pointer_calls.size() == 30);  // one ring update per frame
    const auto p = f.shell->pointer();
    CHECK(f.chrome.pointer_calls.back().x == static_cast<int>(p.x));
    CHECK(f.chrome.pointer_calls.back().y == static_cast<int>(p.y));
  }

  TEST_CASE("a still pointer sends nothing to the page or the chrome") {
    Fixture f;
    f.make();
    move_stick(f, 1.0f, 0.0f, 5);
    f.view(0).calls.clear();
    f.chrome.pointer_calls.clear();
    const int renders = f.chrome.renders;
    for (int i = 0; i < 120; ++i) {
      f.frame();
    }
    CHECK(moves(f.view(0)) == 0);
    CHECK(f.chrome.pointer_calls.empty());
    CHECK(f.chrome.renders == renders);
  }

  TEST_CASE("page moves are sent at most once per frame") {
    Fixture f;
    f.make();
    f.view(0).calls.clear();
    move_stick(f, 1.0f, 1.0f, 20);
    CHECK(moves(f.view(0)) <= 20);
    CHECK(moves(f.view(0)) >= 10);
  }

  TEST_CASE("sub-pixel motion is coalesced until the integer position changes") {
    Fixture f;
    f.make();
    f.frame();
    f.view(0).calls.clear();
    // About 18 px/s: a third of a pixel per frame.
    move_stick(f, 0.235f, 0.0f, 30);
    const std::size_t sent = moves(f.view(0));
    CHECK(sent >= 5);
    CHECK(sent <= 15);
    // The pointer did move; whatever reached the page is its integer position.
    if (sent > 0) {
      const auto p = f.shell->pointer();
      const int y = static_cast<int>(p.y) - ShellController::kChromeBandHeight;
      CHECK(f.view(0).called("pointer_move " + std::to_string(static_cast<int>(p.x)) + "," +
                             std::to_string(y)));
    }
  }

  TEST_CASE("a click does not repeat the move it already sent") {
    Fixture f;
    f.make();
    move_stick(f, 1.0f, 0.0f, 3);
    f.press(Button::Cross);
    f.view(0).calls.clear();
    f.frame();
    CHECK(moves(f.view(0)) == 0);
  }

  TEST_CASE("switching tabs sends the position to the new page on the next move") {
    Fixture f;
    f.make();
    move_stick(f, 1.0f, 0.0f, 3);
    f.shell->on_chrome_event({{"type", "tab_new"}});
    REQUIRE(f.factory.created.size() == 2);
    f.frame();
    move_stick(f, 1.0f, 0.0f, 1);
    CHECK(moves(f.view(1)) == 1);
  }

  TEST_CASE("over the chrome band no move goes to the page") {
    Fixture f;
    f.make();
    f.view(0).calls.clear();
    move_stick(f, 0.0f, -1.0f, 60, 0.05f);
    REQUIRE(f.shell->pointer().y < static_cast<float>(ShellController::kChromeBandHeight));
    f.view(0).calls.clear();
    move_stick(f, 1.0f, 0.0f, 5);
    CHECK(moves(f.view(0)) == 0);
  }

  TEST_CASE("native pointer: no chrome rect, state marked native, no ring updates") {
    Fixture f;
    ShellController::Config config;
    config.home_url = kHome;
    config.native_pointer = true;
    f.shell = std::make_unique<ShellController>(f.factory, f.chrome, f.storage, config);
    f.shell->start();
    f.frame();
    CHECK(f.shell->pointer_visible());
    CHECK(f.chrome.last["pointer"]["native"] == true);
    const auto p = f.shell->pointer();
    for (const auto& r : f.shell->chrome_rects()) {
      CHECK_FALSE((static_cast<int>(p.y) >= r.y && static_cast<int>(p.y) < r.y + r.height));
    }
    move_stick(f, 1.0f, 0.0f, 10);
    CHECK(f.chrome.pointer_calls.empty());
    CHECK(moves(f.view(0)) > 0);  // the page still gets hover moves
  }

  TEST_CASE("hiding the pointer for the menu is reported once") {
    Fixture f;
    f.make();
    move_stick(f, 1.0f, 0.0f, 2);
    f.chrome.pointer_calls.clear();
    f.press(Button::Options);
    f.frame();
    f.frame();
    CHECK_FALSE(f.shell->pointer_visible());
    CHECK(f.chrome.last["pointer"]["visible"] == false);
    CHECK(f.chrome.pointer_calls.empty());  // the render carried it
  }
}

TEST_SUITE("shell.touchpad") {
  using core::TouchContact;

  void touch(Fixture& f, std::vector<TouchContact> contacts, std::uint64_t t) {
    f.shell->on_touchpad(contacts, t);
    f.frame();
  }

  TEST_CASE("a one-finger slide moves the pointer and hovers the page") {
    Fixture f;
    f.make();
    f.frame();
    const auto start = f.shell->pointer();
    f.view(0).calls.clear();
    touch(f, {{0, 0.3f, 0.5f}}, 0);
    for (int i = 1; i <= 10; ++i) {
      touch(f, {{0, 0.3f + 0.02f * static_cast<float>(i), 0.5f}}, static_cast<std::uint64_t>(i) * 50);
    }
    const auto p = f.shell->pointer();
    CHECK(p.x > start.x + 100.0f);
    CHECK(p.y == doctest::Approx(start.y));
    CHECK(f.view(0).called_prefix("pointer_move "));
    CHECK_FALSE(f.view(0).called_prefix("pointer_button "));
  }

  TEST_CASE("a tap clicks at the pointer") {
    Fixture f;
    f.make();
    f.view(0).calls.clear();
    touch(f, {{0, 0.5f, 0.5f}}, 1000);
    touch(f, {}, 1080);
    CHECK(f.view(0).called_prefix("pointer_button "));
    CHECK(f.view(0).count("pointer_button 960,444 1 down") == 1);
    CHECK(f.view(0).count("pointer_button 960,444 1 up") == 1);
  }

  TEST_CASE("two fingers scroll the page") {
    Fixture f;
    f.make();
    f.view(0).calls.clear();
    touch(f, {{0, 0.4f, 0.7f}, {1, 0.6f, 0.7f}}, 0);
    touch(f, {{0, 0.4f, 0.5f}, {1, 0.6f, 0.5f}}, 16);
    CHECK(f.view(0).called_prefix("scroll 0,"));
    CHECK_FALSE(f.view(0).called_prefix("scroll 0,-"));  // fingers up: content up
    touch(f, {}, 100);
    CHECK_FALSE(f.view(0).called_prefix("pointer_button "));
  }

  TEST_CASE("the pad's button presses, drags and releases on the page") {
    Fixture f;
    f.make();
    f.frame();
    f.view(0).calls.clear();
    touch(f, {{0, 0.5f, 0.5f}}, 0);
    f.shell->on_button(Button::Touchpad, true);
    CHECK(f.view(0).called("pointer_button 960,444 1 down"));
    CHECK_FALSE(f.view(0).called_prefix("pointer_button 960,444 1 up"));
    touch(f, {{0, 0.6f, 0.5f}}, 100);  // drag
    f.shell->on_button(Button::Touchpad, false);
    const auto p = f.shell->pointer();
    const std::string at = std::to_string(static_cast<int>(p.x)) + "," +
                           std::to_string(static_cast<int>(p.y) - ShellController::kChromeBandHeight);
    CHECK(p.x > 960.0f);
    CHECK(f.view(0).called("pointer_move " + at));
    CHECK(f.view(0).called("pointer_button " + at + " 1 up"));
    touch(f, {}, 200);  // lifting after a press is not another click
    CHECK(f.view(0).count("pointer_button " + at + " 1 down") == 0);
    CHECK(f.shell->tabs().size() == 1);  // no longer opens a new tab
  }

  TEST_CASE("the touchpad is ignored outside the page") {
    Fixture f;
    f.make();
    f.press(Button::Options);  // menu
    const auto before = f.shell->pointer();
    touch(f, {{0, 0.2f, 0.5f}}, 0);
    touch(f, {{0, 0.8f, 0.5f}}, 50);
    touch(f, {}, 100);
    CHECK(f.shell->pointer().x == doctest::Approx(before.x));
    CHECK(f.shell->focus() == Focus::Menu);
  }

  TEST_CASE("the touchpad button still switches the OSK layout") {
    Fixture f;
    f.make();
    f.press(Button::Triangle);
    REQUIRE(f.shell->focus() == Focus::Osk);
    const std::string layout = f.shell->chrome_state()["osk"]["layout"];
    f.press(Button::Touchpad);
    CHECK(f.shell->chrome_state()["osk"]["layout"] != layout);
  }
}

TEST_SUITE("shell.osk") {
  TEST_CASE("Triangle edits the address with the OSK and navigates on commit") {
    Fixture f;
    f.make();
    f.press(Button::Triangle);
    f.frame();
    CHECK(f.shell->focus() == Focus::Osk);
    CHECK(f.chrome.last["osk"]["open"] == true);
    CHECK(f.chrome.last["osk"]["target"] == "address");
    CHECK(f.chrome.last["osk"]["text"] == kHome);
    CHECK(f.chrome.last["address"]["editing"] == true);
    CHECK(f.chrome.last["osk"]["rows"].size() == 5);

    // Replace the text: clear it, type "q" via cursor and Cross, commit.
    for (std::size_t i = 0; i < std::string(kHome).size(); ++i) {
      f.press(Button::Square);  // backspace
    }
    f.press(Button::Down);  // row 1 starts with "q"
    f.press(Button::Cross);
    f.frame();
    CHECK(f.chrome.last["osk"]["text"] == "q");
    f.press(Button::Options);  // confirm
    CHECK(f.shell->focus() == Focus::Page);
    CHECK(f.view(0).called("load https://duckduckgo.com/?q=q"));
  }

  TEST_CASE("Square opens the OSK for the page field and inserts text on commit") {
    Fixture f;
    f.make();
    f.press(Button::Square);
    CHECK(f.shell->focus() == Focus::Osk);
    f.shell->on_chrome_event({{"type", "osk_key"}, {"row", 1}, {"col", 4}});  // "t"
    f.press(Button::Triangle);                                                // space
    f.press(Button::Options);
    CHECK(f.view(0).called("insert_text t "));
    CHECK(f.shell->focus() == Focus::Page);
  }

  TEST_CASE("Circle cancels the OSK without side effects") {
    Fixture f;
    f.make();
    f.press(Button::Square);
    f.press(Button::Cross);
    f.press(Button::Circle);
    CHECK(f.shell->focus() == Focus::Page);
    CHECK_FALSE(f.view(0).called_prefix("insert_text"));
  }

  TEST_CASE("a text field focused right after a click opens the OSK") {
    Fixture f;
    f.make();
    f.view(0).client().text_input_focused(true);  // not caused by our click
    CHECK(f.shell->focus() == Focus::Page);
    f.press(Button::Cross);
    f.view(0).client().text_input_focused(true);
    CHECK(f.shell->focus() == Focus::Osk);
    f.frame();
    CHECK(f.chrome.last["osk"]["target"] == "page");
  }
}

TEST_SUITE("shell.osk.caret") {
  // Opens the address OSK on kHome ("https://home.example/", 21 chars).
  void open_address_osk(Fixture& f) {
    f.make();
    f.press(Button::Triangle);
    f.frame();
    REQUIRE(f.shell->focus() == Focus::Osk);
  }

  TEST_CASE("the caret starts at the end and is published as caret_index") {
    Fixture f;
    open_address_osk(f);
    CHECK(f.chrome.last["osk"]["caret_index"] == 21);
    f.press(Button::L1);
    f.press(Button::L1);
    f.frame();
    CHECK(f.chrome.last["osk"]["caret_index"] == 19);
    f.press(Button::R1);
    f.frame();
    CHECK(f.chrome.last["osk"]["caret_index"] == 20);
    CHECK(f.chrome.last["osk"]["cursor"]["row"] == 0);  // key grid untouched
  }

  TEST_CASE("typing at the caret edits the middle of the address") {
    Fixture f;
    open_address_osk(f);
    for (int i = 0; i < 8; ++i) f.press(Button::L1);  // after "https://home."
    f.press(Button::Square);                           // drops the "."
    f.frame();
    CHECK(f.chrome.last["osk"]["text"] == "https://homeexample/");
    CHECK(f.chrome.last["osk"]["caret_index"] == 12);
    f.shell->on_chrome_event({{"type", "osk_key"}, {"row", 3}, {"col", 8}});  // "."
    f.frame();
    CHECK(f.chrome.last["osk"]["text"] == "https://home.example/");
    CHECK(f.chrome.last["osk"]["caret_index"] == 13);
    CHECK(f.chrome.last["address"]["text"] == "https://home.example/");
  }

  TEST_CASE("Touchpad switches the layout and the OSK rows show the new keys") {
    Fixture f;
    open_address_osk(f);
    f.press(Button::Touchpad);
    f.frame();
    CHECK(f.chrome.last["osk"]["layout"] == "symbols");
    const auto& special = f.chrome.last["osk"]["rows"][4];
    REQUIRE(special.size() == 8);
    CHECK(special[2]["code"] == "clear");
    CHECK(special[3]["code"] == "left");
    CHECK(special[5]["code"] == "right");
    f.press(Button::L2);
    f.frame();
    CHECK(f.chrome.last["osk"]["shift"] == true);
  }

  TEST_CASE("the Clear key empties the address in one press") {
    Fixture f;
    open_address_osk(f);
    f.shell->on_chrome_event({{"type", "osk_key"}, {"row", 4}, {"col", 2}});
    f.frame();
    CHECK(f.chrome.last["osk"]["text"] == "");
    CHECK(f.chrome.last["osk"]["caret_index"] == 0);
  }

  TEST_CASE("holding Square repeats Backspace after 350 ms, every 50 ms") {
    Fixture f;
    open_address_osk(f);
    f.shell->on_button(Button::Square, true);
    CHECK(f.shell->osk().text().size() == 20);  // fires once on press
    f.frame(0.34f);
    CHECK(f.shell->osk().text().size() == 20);  // still inside the delay
    f.frame(0.02f);                               // 360 ms
    CHECK(f.shell->osk().text().size() == 19);
    f.frame(0.1f);  // 460 ms: repeats at 400 and 450
    CHECK(f.shell->osk().text().size() == 17);
    f.shell->on_button(Button::Square, false);
    f.frame(0.5f);
    CHECK(f.shell->osk().text().size() == 17);  // release stops the repeat
  }

  TEST_CASE("a long Square hold deletes faster but never clears everything") {
    Fixture f;
    f.make();
    f.press(Button::Square);  // page-field OSK
    REQUIRE(f.shell->focus() == Focus::Osk);
    for (int i = 0; i < 80; ++i) f.press(Button::Triangle);  // 80 spaces
    REQUIRE(f.shell->osk().text().size() == 80);
    f.shell->on_button(Button::Square, true);
    for (int i = 0; i < 100; ++i) f.frame(0.02f);  // 2 s
    const auto left = f.shell->osk().text().size();
    CHECK(left > 30);   // 1 + (2000 - 350) / 50 + 1 = 35 deleted
    CHECK(left < 50);
    f.shell->on_button(Button::Square, false);
  }

  TEST_CASE("holding L1 or R1 repeats caret moves") {
    Fixture f;
    open_address_osk(f);
    f.shell->on_button(Button::L1, true);
    f.frame(0.42f);  // press + repeats at 350 and 400
    f.shell->on_button(Button::L1, false);
    CHECK(f.shell->osk().caret() == 18);
    f.shell->on_button(Button::R1, true);
    f.frame(0.36f);  // press + repeat at 350
    f.shell->on_button(Button::R1, false);
    CHECK(f.shell->osk().caret() == 20);
  }

  TEST_CASE("holding the D-pad repeats key-grid moves at the OSK rate") {
    Fixture f;
    open_address_osk(f);
    f.shell->on_button(Button::Right, true);
    CHECK(f.shell->osk().cursor().col == 1);
    f.frame(0.3f);
    CHECK(f.shell->osk().cursor().col == 1);  // OSK delay is 350 ms, not 300
    f.frame(0.11f);                            // 410 ms: repeats at 350 and 400
    CHECK(f.shell->osk().cursor().col == 3);
    f.shell->on_button(Button::Right, false);
    f.frame(0.2f);
    CHECK(f.shell->osk().cursor().col == 3);
  }

  TEST_CASE("closing the OSK while a key is held stops the repeat") {
    Fixture f;
    open_address_osk(f);
    f.shell->on_button(Button::Square, true);
    f.press(Button::Circle);  // cancel
    CHECK(f.shell->focus() == Focus::Page);
    const auto calls = f.view(0).calls.size();
    f.frame(1.0f);
    CHECK(f.view(0).calls.size() == calls);  // no scrolls or other page actions
    f.shell->on_button(Button::Square, false);
  }
}

TEST_SUITE("shell.fullscreen") {
  TEST_CASE("R3 hides and shows the chrome and resizes the view") {
    Fixture f;
    f.make();
    CHECK(f.view(0).height == 1080 - ShellController::kChromeBandHeight);
    f.press(Button::R3);
    f.frame();
    CHECK(f.chrome.last["chrome_visible"] == false);
    CHECK(f.chrome.last["fullscreen_state"] == "chrome_hidden");
    CHECK(f.chrome.last["content"]["y"] == 0);
    CHECK(f.view(0).height == 1080);
    f.press(Button::R3);
    f.frame();
    CHECK(f.chrome.last["chrome_visible"] == true);
    CHECK(f.view(0).height == 1080 - ShellController::kChromeBandHeight);
  }

  TEST_CASE("page-initiated fullscreen: Circle asks the engine to exit") {
    Fixture f;
    f.make();
    f.view(0).client().nav_state_changed(true, false);
    f.view(0).client().fullscreen_entered();
    f.frame();
    CHECK(f.chrome.last["fullscreen_state"] == "element");
    CHECK(f.chrome.last["chrome_visible"] == false);
    // The page gets the whole window: no band offset, 1920x1080.
    CHECK(f.shell->content_top() == 0);
    CHECK(f.chrome.last["content"]["y"] == 0);
    CHECK(f.view(0).width == 1920);
    CHECK(f.view(0).height == 1080);
    f.press(Button::Circle);
    CHECK(f.view(0).called("request_exit_fullscreen"));
    CHECK_FALSE(f.view(0).called("go_back"));
    f.view(0).client().fullscreen_exited();
    f.frame();
    CHECK(f.chrome.last["fullscreen_state"] == "normal");
    CHECK(f.chrome.last["chrome_visible"] == true);
    CHECK(f.shell->content_top() == ShellController::kChromeBandHeight);
    CHECK(f.view(0).height == 1080 - ShellController::kChromeBandHeight);
  }

  TEST_CASE("page leaving fullscreen by itself restores the previous state") {
    Fixture f;
    f.make();
    f.press(Button::R3);  // chrome hidden
    f.view(0).client().fullscreen_entered();
    f.press(Button::R3);  // R3 during element fullscreen exits it
    CHECK(f.view(0).count("request_exit_fullscreen") == 1);
    f.view(0).client().fullscreen_entered();
    f.view(0).client().fullscreen_exited();
    f.frame();
    CHECK(f.chrome.last["fullscreen_state"] == "normal");
  }
}

TEST_SUITE("shell.drm") {
  TEST_CASE("EME script message shows the notice once per origin") {
    Fixture f;
    f.make();
    const std::string msg =
        R"({"type":"eme","keySystem":"com.widevine.alpha","origin":"https://video.example"})";
    f.view(0).client().script_message(msg);
    f.frame();
    CHECK(f.chrome.last["notice"]["kind"] == "drm");
    CHECK(f.shell->focus() == Focus::Notice);
    f.press(Button::Cross);
    f.frame();
    CHECK(f.chrome.last["notice"]["kind"].is_null());
    f.view(0).client().script_message(msg);
    CHECK(f.shell->focus() == Focus::Page);
    // Clear Key needs no CDM: no notice.
    f.view(0).client().script_message(
        R"({"type":"eme","keySystem":"org.w3.clearkey","origin":"https://other.example"})");
    CHECK(f.shell->focus() == Focus::Page);
    // Garbage is ignored.
    f.view(0).client().script_message("not json");
    CHECK(f.shell->focus() == Focus::Page);
  }

  TEST_CASE("navigating to a known Widevine-only host shows the notice first") {
    Fixture f;
    f.make();
    f.shell->navigate("https://www.netflix.com/browse");
    CHECK_FALSE(f.view(0).called_prefix("load https://www.netflix.com"));
    CHECK(f.shell->focus() == Focus::Notice);
    f.press(Button::Up);  // other buttons do nothing while the notice is up
    CHECK(f.shell->focus() == Focus::Notice);
    f.press(Button::Cross);
    CHECK(f.view(0).called("load https://www.netflix.com/browse"));
    // Second visit to the same origin: no notice.
    f.shell->navigate("https://www.netflix.com/title/1");
    CHECK(f.view(0).called("load https://www.netflix.com/title/1"));
    CHECK(f.shell->focus() == Focus::Page);
  }

  TEST_CASE("Circle on the pre-navigation notice cancels the navigation") {
    Fixture f;
    f.make();
    f.shell->navigate("kinopoisk.ru");
    f.press(Button::Circle);
    CHECK_FALSE(f.view(0).called_prefix("load https://kinopoisk.ru"));
    CHECK(f.shell->focus() == Focus::Page);
  }
}

TEST_SUITE("shell.menu") {
  TEST_CASE("Options menu adds and removes a bookmark") {
    Fixture f;
    f.make();
    f.finish_load(f.view(0), "https://page.example/", "Page");
    f.press(Button::Options);
    f.frame();
    CHECK(f.shell->focus() == Focus::Menu);
    CHECK(f.chrome.last["menu"]["open"] == true);
    CHECK(f.chrome.last["menu"]["section"] == "main");
    CHECK(f.chrome.last["menu"]["items"][2]["label"] == "Add bookmark");
    f.press(Button::Down);
    f.press(Button::Down);
    f.frame();
    CHECK(f.chrome.last["menu"]["selected"] == 2);
    f.press(Button::Cross);
    CHECK(f.shell->bookmarks().contains("https://page.example/"));
    CHECK(f.shell->focus() == Focus::Page);
    CHECK(f.storage.data().count("bookmarks") == 1);

    // The bookmarks section lists it and opens it.
    f.press(Button::Options);
    f.shell->on_chrome_event({{"type", "menu_select"}, {"index", 0}});
    f.frame();
    CHECK(f.chrome.last["menu"]["section"] == "bookmarks");
    CHECK(f.chrome.last["menu"]["items"][0]["label"] == "Page");
    f.press(Button::Circle);  // back to main
    f.frame();
    CHECK(f.chrome.last["menu"]["section"] == "main");
    CHECK(f.chrome.last["menu"]["items"][2]["label"] == "Remove bookmark");
    f.shell->on_chrome_event({{"type", "menu_select"}, {"index", 2}});
    CHECK_FALSE(f.shell->bookmarks().contains("https://page.example/"));
  }

  TEST_CASE("history section loads the selected entry") {
    Fixture f;
    f.make();
    f.finish_load(f.view(0), "https://visited.example/", "Visited");
    f.press(Button::Options);
    f.press(Button::Down);
    f.press(Button::Cross);  // History
    f.frame();
    CHECK(f.chrome.last["menu"]["section"] == "history");
    f.view(0).calls.clear();
    f.press(Button::Cross);
    CHECK(f.view(0).called("load https://visited.example/"));
    CHECK(f.shell->focus() == Focus::Page);
  }

  TEST_CASE("Circle closes the menu") {
    Fixture f;
    f.make();
    f.press(Button::Options);
    f.press(Button::Circle);
    CHECK(f.shell->focus() == Focus::Page);
    f.frame();
    CHECK(f.chrome.last["menu"]["open"] == false);
  }
}

TEST_SUITE("shell.chrome_events") {
  TEST_CASE("chrome rects cover the band, the OSK, modals and the pointer") {
    Fixture f;
    f.make();
    auto has = [](const std::vector<ShellController::Rect>& rects, int x, int y) {
      for (const auto& r : rects) {
        if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) return true;
      }
      return false;
    };
    const core::Vec2 p = f.shell->pointer();
    auto rects = f.shell->chrome_rects();
    CHECK(has(rects, 10, 10));                                             // band
    CHECK(has(rects, static_cast<int>(p.x), static_cast<int>(p.y)));      // pointer
    CHECK_FALSE(has(rects, 10, 1000));                                     // plain page
    CHECK(f.shell->chrome_state()["chrome_rects"].size() == rects.size());

    f.press(Button::Triangle);  // OSK on the address bar hides the pointer
    rects = f.shell->chrome_rects();
    CHECK(has(rects, 10, 1000));
    CHECK_FALSE(has(rects, static_cast<int>(p.x), static_cast<int>(p.y)));
    f.press(Button::Circle);

    f.press(Button::Options);  // menu: the whole window
    rects = f.shell->chrome_rects();
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].width == 1920);
    CHECK(rects[0].height == 1080);
  }

  TEST_CASE("nav buttons and address focus") {
    Fixture f;
    f.make();
    f.shell->on_chrome_event({{"type", "nav"}, {"action", "reload"}});
    f.shell->on_chrome_event({{"type", "nav"}, {"action", "forward"}});
    CHECK(f.view(0).called("reload"));
    CHECK(f.view(0).called("go_forward"));
    f.shell->on_chrome_event({{"type", "address_focus"}});
    CHECK(f.shell->focus() == Focus::Chrome);
    f.shell->on_chrome_event({{"type", "address_blur"}});
    CHECK(f.shell->focus() == Focus::Page);
    f.shell->on_chrome_event({{"type", "tab_new"}});
    CHECK(f.shell->tabs().size() == 2);
    f.shell->on_chrome_event(json::array());  // ignored
    f.shell->on_chrome_event({{"type", "unknown"}});
    CHECK(f.shell->tabs().size() == 2);
  }

  TEST_CASE("Diagnostics menu item opens the probe page in a new tab") {
    Fixture f;
    f.diagnostics_url = "file:///data/prosperoweb/chrome/diag.html";
    f.make();
    f.press(Button::Options);
    f.frame();
    REQUIRE(f.chrome.last["menu"]["items"].size() == 10);
    CHECK(f.chrome.last["menu"]["items"][7]["label"] == "Diagnostics");
    CHECK(f.chrome.last["menu"]["items"][9]["label"] == "Quit");
    f.shell->on_chrome_event({{"type", "menu_select"}, {"index", 7}});
    CHECK(f.shell->tabs().size() == 2);
    CHECK(f.active().called("load file:///data/prosperoweb/chrome/diag.html"));
    CHECK(f.shell->focus() == Focus::Page);
  }

  TEST_CASE("no Diagnostics menu item without a diagnostics URL") {
    Fixture f;
    f.make();
    f.press(Button::Options);
    f.frame();
    for (const auto& item : f.chrome.last["menu"]["items"]) {
      CHECK(item["label"] != "Diagnostics");
    }
  }

  TEST_CASE("Quit menu item requests exit") {
    Fixture f;
    f.make();
    f.press(Button::Options);
    f.shell->on_chrome_event({{"type", "menu_select"}, {"index", 8}});
    CHECK(f.shell->quit_requested());
  }
}

TEST_SUITE("shell.cursor") {
  using shell::PageCursor;

  void stick(Fixture& f, float lx, int frames) {
    f.shell->on_sticks(lx, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < frames; ++i) {
      f.frame();
    }
    f.shell->on_sticks(0.0f, 0.0f, 0.0f, 0.0f);
  }

  void idle(Fixture& f, float seconds) {
    for (float t = 0.0f; t < seconds; t += 0.1f) {
      f.frame(0.1f);
    }
  }

  TEST_CASE("cursor: none from the page hides the pointer, any other shows it") {
    Fixture f;
    f.make();
    f.frame();
    CHECK(f.shell->pointer_visible());
    f.view(0).client().cursor_changed(PageCursor::None);
    f.frame();
    CHECK(f.shell->page_cursor() == PageCursor::None);
    CHECK_FALSE(f.shell->pointer_visible());
    CHECK(f.chrome.pointer_calls.back().visible == false);
    // Moving alone does not bring it back: the page decides.
    stick(f, 1.0f, 5);
    CHECK_FALSE(f.shell->pointer_visible());
    f.view(0).client().cursor_changed(PageCursor::Hand);
    f.frame();
    CHECK(f.shell->pointer_visible());
    CHECK(f.chrome.pointer_calls.back().visible == true);
  }

  TEST_CASE("the cursor is per tab") {
    Fixture f;
    f.make();
    f.view(0).client().cursor_changed(PageCursor::None);
    CHECK_FALSE(f.shell->pointer_visible());
    f.press(Button::Options);
    f.shell->on_chrome_event(json{{"type", "menu_close"}});
    REQUIRE(f.shell->open_url_in_new_tab("https://b.example/"));
    f.frame();
    CHECK(f.shell->page_cursor() == PageCursor::Pointer);
    CHECK(f.shell->pointer_visible());
    // A background tab's cursor changes do not affect the active one.
    f.view(0).client().cursor_changed(PageCursor::Hand);
    f.view(1).client().cursor_changed(PageCursor::None);
    CHECK_FALSE(f.shell->pointer_visible());
  }

  TEST_CASE("element fullscreen hides an idle pointer after 3 s") {
    Fixture f;
    f.make();
    f.view(0).client().fullscreen_entered();
    f.frame();
    CHECK(f.shell->pointer_visible());
    idle(f, ShellController::kFullscreenPointerHideSeconds - 0.5f);
    CHECK(f.shell->pointer_visible());
    idle(f, 1.0f);
    CHECK_FALSE(f.shell->pointer_visible());
    // The next stick input shows it at once.
    stick(f, 1.0f, 1);
    CHECK(f.shell->pointer_visible());
    idle(f, ShellController::kFullscreenPointerHideSeconds + 0.5f);
    CHECK_FALSE(f.shell->pointer_visible());
    // So does a button (D-pad scroll).
    f.shell->on_button(Button::Up, true);
    f.shell->on_button(Button::Up, false);
    f.frame();
    CHECK(f.shell->pointer_visible());
  }

  TEST_CASE("outside element fullscreen an idle pointer stays") {
    Fixture f;
    f.make();
    idle(f, 10.0f);
    CHECK(f.shell->pointer_visible());
    f.press(Button::R3);  // chrome hidden, not element fullscreen
    idle(f, 10.0f);
    CHECK(f.shell->pointer_visible());
    // Leaving element fullscreen shows a pointer hidden by idling.
    f.view(0).client().fullscreen_entered();
    idle(f, 5.0f);
    CHECK_FALSE(f.shell->pointer_visible());
    f.view(0).client().fullscreen_exited();
    f.frame();
    CHECK(f.shell->pointer_visible());
  }
}
