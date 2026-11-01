// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "fakes.h"
#include "prosperoweb/core/in_memory_storage.h"
#include "prosperoweb/shell/addons.h"
#include "prosperoweb/shell/shell_controller.h"

using namespace prosperoweb;
using shell::ShellController;
using shell::test::FakeChromeUi;
using shell::test::FakeEngineFactory;
using json = nlohmann::json;

namespace {

// Records the last applied add-on state.
class FakeAddonsBackend final : public shell::AddonsBackend {
 public:
  void set_adblock(bool enabled, bool cookie_banners) override {
    adblock_enabled = enabled;
    cookie = cookie_banners;
    ++adblock_calls;
  }
  void set_sponsorblock(bool enabled, const std::vector<std::string>& categories) override {
    sb_enabled = enabled;
    sb_categories = categories;
    ++sb_calls;
  }
  void update_filters_now() override { ++updates; }
  std::int64_t filters_last_updated() const override { return last_updated; }

  bool adblock_enabled = false;
  bool cookie = false;
  bool sb_enabled = false;
  std::vector<std::string> sb_categories;
  int adblock_calls = 0;
  int sb_calls = 0;
  int updates = 0;
  std::int64_t last_updated = 0;
};

struct Fixture {
  core::InMemoryStorage storage;
  FakeEngineFactory factory;
  FakeChromeUi chrome;
  FakeAddonsBackend backend;
  core::AddonsConfig persisted;
  bool persisted_set = false;
  std::unique_ptr<ShellController> shell;

  void make() {
    ShellController::Config config;
    config.width = 1920;
    config.height = 1080;
    config.home_url = "https://home.example/";
    config.now = [] { return std::int64_t{1000}; };
    config.addons_backend = &backend;
    config.on_addons_changed = [this](const core::AddonsConfig& c) {
      persisted = c;
      persisted_set = true;
    };
    shell = std::make_unique<ShellController>(factory, chrome, storage, config);
    shell->start();
  }

  void event(const json& e) {
    shell->on_chrome_event(e);
    shell->render_now();  // refresh chrome.last like a frame tick would
  }

  // Finds the index of a menu item whose label starts with `prefix`.
  long menu_index(const std::string& prefix) {
    const auto& items = chrome.last["menu"]["items"];
    for (std::size_t i = 0; i < items.size(); ++i) {
      const std::string label = items[i]["label"].get<std::string>();
      if (label.rfind(prefix, 0) == 0) {
        return static_cast<long>(i);
      }
    }
    return -1;
  }
  void select(const std::string& prefix) {
    long i = menu_index(prefix);
    REQUIRE(i >= 0);
    event({{"type", "menu_select"}, {"index", i}});
  }
  std::string label_of(const std::string& prefix) {
    long i = menu_index(prefix);
    REQUIRE(i >= 0);
    return chrome.last["menu"]["items"][static_cast<std::size_t>(i)]["label"].get<std::string>();
  }
};

}  // namespace

TEST_SUITE("shell.addons") {
  TEST_CASE("the main menu shows quick toggles reflecting the initial state") {
    Fixture f;
    f.make();
    f.event({{"type", "menu_open"}});
    CHECK(f.label_of("Ad blocker: ") == "Ad blocker: On");       // default on
    CHECK(f.label_of("SponsorBlock: ") == "SponsorBlock: Off");  // default off
  }

  TEST_CASE("toggling the ad blocker applies and persists") {
    Fixture f;
    f.make();
    f.event({{"type", "menu_open"}});
    f.select("Ad blocker: ");
    CHECK(f.backend.adblock_enabled == false);
    REQUIRE(f.persisted_set);
    CHECK(f.persisted.adblock.enabled == false);
    // The label updated in place and the menu stayed open.
    CHECK(f.label_of("Ad blocker: ") == "Ad blocker: Off");
    CHECK(f.chrome.last["menu"]["open"] == true);
  }

  TEST_CASE("the Add-ons section exposes sub-options and categories") {
    Fixture f;
    f.make();
    f.event({{"type", "menu_open"}});
    f.select("Add-ons");
    CHECK(f.chrome.last["menu"]["section"] == "addons");
    // Cookie banners off by default.
    CHECK(f.label_of("Block cookie banners") == "Block cookie banners (EasyList-Cookie): Off");
    f.select("Block cookie banners");
    CHECK(f.backend.cookie == true);
    CHECK(f.persisted.adblock.cookie_banners == true);
  }

  TEST_CASE("sponsorblock category toggles feed the backend") {
    Fixture f;
    f.make();
    f.event({{"type", "menu_open"}});
    f.select("Add-ons");
    // Enable SponsorBlock, then add the "intro" category.
    f.select("SponsorBlock: ");
    CHECK(f.backend.sb_enabled == true);
    CHECK(f.backend.sb_categories == std::vector<std::string>{"sponsor"});  // default
    f.select("  intro");
    CHECK(f.backend.sb_categories == std::vector<std::string>{"sponsor", "intro"});
    // Removing sponsor leaves only intro.
    f.select("  sponsor");
    CHECK(f.backend.sb_categories == std::vector<std::string>{"intro"});
  }

  TEST_CASE("update now calls the backend; timestamp refreshes the label") {
    Fixture f;
    f.make();
    f.event({{"type", "menu_open"}});
    f.select("Add-ons");
    f.select("Update filter lists now");
    CHECK(f.backend.updates == 1);
    CHECK(f.label_of("Filter lists updated: ") == "Filter lists updated: never");
    // A completed update at t=1000 (now()) shows "just now".
    f.shell->on_filters_updated(1000);
    f.shell->render_now();
    CHECK(f.label_of("Filter lists updated: ") == "Filter lists updated: just now");
    CHECK(f.persisted.adblock.last_updated == 1000);
  }

  TEST_CASE("no backend -> no Add-ons menu entries") {
    core::InMemoryStorage storage;
    FakeEngineFactory factory;
    FakeChromeUi chrome;
    ShellController::Config config;
    config.home_url = "https://home.example/";
    config.now = [] { return std::int64_t{1000}; };
    ShellController shell(factory, chrome, storage, config);
    shell.start();
    shell.on_chrome_event({{"type", "menu_open"}});
    shell.render_now();
    const auto& items = chrome.last["menu"]["items"];
    for (const auto& it : items) {
      CHECK(it["label"].get<std::string>().rfind("Ad blocker", 0) != 0);
    }
  }
}
