// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "fakes.h"
#include "prosperoweb/core/in_memory_storage.h"
#include "prosperoweb/shell/shell_controller.h"

using namespace prosperoweb;
using core::Button;
using core::RemoteAction;
using core::RemoteCommand;
using shell::ShellController;
using shell::test::FakeChromeUi;
using shell::test::FakeEngineFactory;
using shell::test::FakeEngineView;
using shell::test::FakeRemoteControl;
using Focus = ShellController::Focus;
using json = nlohmann::json;

namespace {

constexpr const char* kHome = "https://home.example/";

struct RemoteFixture {
  core::InMemoryStorage storage;
  FakeEngineFactory factory;
  FakeChromeUi chrome;
  FakeRemoteControl remote;
  std::unique_ptr<ShellController> shell;

  explicit RemoteFixture(bool with_remote = true) {
    ShellController::Config config;
    config.home_url = kHome;
    config.now = [] { return std::int64_t{1000}; };
    if (with_remote) config.remote_control = &remote;
    shell = std::make_unique<ShellController>(factory, chrome, storage, config);
    shell->start();
    frame();
  }

  FakeEngineView& active() { return *static_cast<FakeEngineView*>(shell->active_view()); }
  void frame() { shell->tick(1.0f / 60.0f); }
  void send(const RemoteCommand& c) { shell->remote_command(c); }

  // Index of the main-menu entry whose label starts with prefix, or -1.
  int menu_index(const std::string& prefix) {
    const auto& items = chrome.last["menu"]["items"];
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (items[i]["label"].get<std::string>().rfind(prefix, 0) == 0) return static_cast<int>(i);
    }
    return -1;
  }
  void open_menu() {
    shell->on_chrome_event({{"type", "menu_open"}});
    frame();
  }
  void select(int index) {
    REQUIRE(index >= 0);
    shell->on_chrome_event({{"type", "menu_select"}, {"index", index}});
    frame();
  }
};

RemoteCommand cmd(RemoteCommand::Type type) {
  RemoteCommand c;
  c.type = type;
  return c;
}

RemoteCommand action(RemoteAction a) {
  RemoteCommand c = cmd(RemoteCommand::Type::Action);
  c.action = a;
  return c;
}

}  // namespace

TEST_SUITE("shell.remote") {
  TEST_CASE("no menu entry without a remote control") {
    RemoteFixture f(/*with_remote=*/false);
    f.open_menu();
    CHECK(f.menu_index("Remote control") == -1);
  }

  TEST_CASE("menu: switching on shows the URL and PIN; a paired phone closes it") {
    RemoteFixture f;
    f.open_menu();
    CHECK(f.chrome.last["menu"]["items"][f.menu_index("Remote control")]["label"] ==
          "Remote control: Off");
    CHECK(f.menu_index("Disconnect phones") == -1);
    f.select(f.menu_index("Remote control"));
    CHECK(f.remote.on);
    CHECK(f.shell->focus() == Focus::Notice);
    CHECK(f.chrome.last["notice"]["kind"] == "info");
    const std::string text = f.chrome.last["notice"]["text"];
    CHECK(text.find("http://192.0.2.1:17780") != std::string::npos);
    CHECK(text.find(f.remote.pin) != std::string::npos);
    CHECK(text.find("not encrypted") != std::string::npos);

    f.remote.devices = 1;
    f.frame();
    CHECK(f.shell->focus() == Focus::Page);
    CHECK_FALSE(f.chrome.last["notice"]["kind"].is_string());

    f.open_menu();
    CHECK(f.chrome.last["menu"]["items"][f.menu_index("Remote control")]["label"] ==
          "Remote control: On");
    f.select(f.menu_index("Disconnect phones"));
    CHECK(f.remote.disconnects == 1);
    CHECK(f.remote.on);

    f.open_menu();
    f.select(f.menu_index("Show remote PIN"));
    CHECK(f.shell->focus() == Focus::Notice);
    f.shell->on_chrome_event({{"type", "notice_dismiss"}, {"proceed", true}});

    f.open_menu();
    f.select(f.menu_index("Remote control"));
    CHECK_FALSE(f.remote.on);
    CHECK(f.chrome.last["menu"]["items"][f.menu_index("Remote control")]["label"] ==
          "Remote control: Off");
  }

  TEST_CASE("a phone pairing while another notice is up does not close it") {
    RemoteFixture f;
    f.open_menu();
    f.select(f.menu_index("Remote control"));
    f.shell->on_chrome_event({{"type", "notice_dismiss"}, {"proceed", true}});
    f.frame();
    // A later error notice stays until the user dismisses it.
    f.active().client().load_failed("https://x.example/", "boom");
    f.remote.devices = 1;
    f.frame();
    CHECK(f.shell->focus() == Focus::Notice);
  }

  TEST_CASE("start failure shows an error") {
    RemoteFixture f;
    f.remote.fail = true;
    f.open_menu();
    f.select(f.menu_index("Remote control"));
    CHECK(f.chrome.last["notice"]["kind"] == "error");
    CHECK(f.chrome.last["notice"]["text"] == "port in use");
  }

  TEST_CASE("open a URL here or in a new tab") {
    RemoteFixture f;
    auto open = cmd(RemoteCommand::Type::OpenUrl);
    open.text = "example.com";
    f.send(open);
    CHECK(f.active().called("load https://example.com"));
    open.text = "second.example";
    open.new_tab = true;
    f.send(open);
    CHECK(f.shell->tabs().size() == 2);
    CHECK(f.active().called("load https://second.example"));
  }

  TEST_CASE("pointer, click and scroll") {
    RemoteFixture f;
    const core::Vec2 start = f.shell->pointer();
    auto move = cmd(RemoteCommand::Type::Pointer);
    move.dx = 40;
    move.dy = 25;
    f.send(move);
    f.frame();
    CHECK(f.shell->pointer().x == doctest::Approx(start.x + 40));
    CHECK(f.shell->pointer().y == doctest::Approx(start.y + 25));
    const int x = static_cast<int>(start.x + 40);
    const int y = static_cast<int>(start.y + 25) - f.shell->content_top();
    const std::string at = std::to_string(x) + "," + std::to_string(y);
    CHECK(f.active().called("pointer_move " + at));
    f.send(cmd(RemoteCommand::Type::Click));
    CHECK(f.active().called("pointer_button " + at + " 1 down"));
    CHECK(f.active().called("pointer_button " + at + " 1 up"));
    auto scroll = cmd(RemoteCommand::Type::Scroll);
    scroll.dy = 300;
    f.send(scroll);
    CHECK(f.active().called("scroll 0,300"));
  }

  TEST_CASE("text goes into the focused field; submit presses Enter") {
    RemoteFixture f;
    auto text = cmd(RemoteCommand::Type::Text);
    text.text = "hola mundo";
    f.send(text);
    CHECK(f.active().called("insert_text hola mundo"));
    CHECK_FALSE(f.active().called_prefix("js (function(){var t=document.activeElement"));
    text.text = "";
    text.submit = true;
    f.send(text);
    CHECK(f.active().called_prefix("js (function(){var t=document.activeElement"));
    CHECK(f.active().count("insert_text hola mundo") == 1);
  }

  TEST_CASE("text typed into the open on-screen keyboard") {
    RemoteFixture f;
    f.shell->on_button(Button::Triangle, true);  // address bar: the built-in OSK
    f.shell->on_button(Button::Triangle, false);
    f.frame();
    REQUIRE(f.shell->focus() == Focus::Osk);
    auto text = cmd(RemoteCommand::Type::Text);
    text.text = "example.org";
    text.submit = true;
    f.send(text);
    CHECK(f.shell->focus() == Focus::Page);
    CHECK(f.active().called("load https://example.org"));
  }

  TEST_CASE("actions") {
    RemoteFixture f;
    f.send(action(RemoteAction::Reload));
    CHECK(f.active().called("reload"));
    f.send(action(RemoteAction::NewTab));
    CHECK(f.shell->tabs().size() == 2);
    const auto second = f.shell->tabs().active_id();
    f.send(action(RemoteAction::PrevTab));
    CHECK(f.shell->tabs().active_id() != second);
    CHECK(f.shell->remote_status()["active_tab"] == 0);
    f.send(action(RemoteAction::CloseTab));
    CHECK(f.shell->tabs().size() == 1);

    f.send(action(RemoteAction::Menu));
    f.frame();
    CHECK(f.shell->focus() == Focus::Menu);
    // Clicks pick the selected menu entry; Escape leaves the menu.
    f.send(action(RemoteAction::Escape));
    f.frame();
    CHECK(f.shell->focus() == Focus::Page);
    f.send(action(RemoteAction::Menu));
    f.send(action(RemoteAction::Menu));
    CHECK(f.shell->focus() == Focus::Page);
    // A page action closes the menu first.
    f.send(action(RemoteAction::Menu));
    f.send(action(RemoteAction::Forward));
    CHECK(f.shell->focus() == Focus::Page);
    CHECK(f.active().called("go_forward"));
  }

  TEST_CASE("pointer and scroll are ignored outside the page") {
    RemoteFixture f;
    f.send(action(RemoteAction::Menu));
    const core::Vec2 before = f.shell->pointer();
    auto move = cmd(RemoteCommand::Type::Pointer);
    move.dx = 50;
    f.send(move);
    CHECK(f.shell->pointer().x == doctest::Approx(before.x));
  }

  TEST_CASE("status") {
    RemoteFixture f;
    f.active().client().url_changed("https://example.com/a");
    f.active().client().title_changed("Example");
    const json s = f.shell->remote_status();
    CHECK(s["url"] == "https://example.com/a");
    CHECK(s["title"] == "Example");
    CHECK(s["tabs"] == 1);
    CHECK(s["active_tab"] == 0);
    CHECK(s["focus"] == "page");
  }
}
