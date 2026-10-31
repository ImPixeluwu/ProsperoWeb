// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "fakes.h"
#include "prosperobrowser/core/in_memory_storage.h"
#include "prosperobrowser/shell/shell_controller.h"

using namespace prosperobrowser;
using core::Button;
using shell::ShellController;
using shell::TextInputKind;
using shell::test::FakeChromeUi;
using shell::test::FakeEngineFactory;
using shell::test::FakeEngineView;
using shell::test::FakeTextInputProvider;
using Focus = ShellController::Focus;

namespace {

constexpr const char* kHome = "https://home.example/";
constexpr const char* kSelectPrefix = "js (function(){var e=document.activeElement;";

struct Fixture {
  core::InMemoryStorage storage;
  FakeEngineFactory factory;
  FakeChromeUi chrome;
  FakeTextInputProvider provider;
  std::unique_ptr<ShellController> shell;

  explicit Fixture(bool with_provider = true) {
    ShellController::Config config;
    config.home_url = kHome;
    config.now = [] { return std::int64_t{1000}; };
    config.text_input = with_provider ? &provider : nullptr;
    shell = std::make_unique<ShellController>(factory, chrome, storage, config);
    shell->start();
  }

  FakeEngineView& view() { return *factory.created.at(0); }
  void press(Button b) {
    shell->on_button(b, true);
    shell->on_button(b, false);
  }
  void frame() { shell->tick(1.0f / 60.0f); }
  // What core::kFocusReporterScript posts when a field gains focus.
  void focus_field(const std::string& type, const nlohmann::json& value) {
    view().client().script_message(
        nlohmann::json{{"type", "focus_info"}, {"inputType", type}, {"value", value}}.dump());
    view().client().text_input_focused(true);
  }
};

}  // namespace

TEST_SUITE("shell.text_input") {
  TEST_CASE("Triangle asks the provider for a URL and navigates on done") {
    Fixture f;
    f.press(Button::Triangle);
    REQUIRE(f.provider.requests.size() == 1);
    CHECK(f.provider.requests[0].kind == TextInputKind::Url);
    CHECK(f.provider.requests[0].initial == kHome);
    CHECK_FALSE(f.provider.requests[0].title.empty());
    CHECK(f.shell->text_input_active());
    CHECK(f.shell->focus() == Focus::Page);  // no built-in OSK
    f.frame();
    CHECK(f.provider.polls == 1);
    CHECK(f.chrome.last["osk"]["open"] == false);
    CHECK(f.chrome.last["text_input"]["active"] == true);
    CHECK(f.chrome.last["text_input"]["target"] == "address");

    f.provider.finish_done("example.org");
    CHECK_FALSE(f.shell->text_input_active());
    CHECK(f.view().called_prefix("load https://example.org"));
    CHECK(f.shell->focus() == Focus::Page);
    f.frame();
    CHECK(f.provider.polls == 1);  // not polled once idle
    CHECK(f.chrome.last["text_input"]["active"] == false);
  }

  TEST_CASE("Square on a reported field pre-fills and replaces its value") {
    Fixture f;
    f.focus_field("search", "old query");
    f.press(Button::Square);
    REQUIRE(f.provider.requests.size() == 1);
    CHECK(f.provider.requests[0].kind == TextInputKind::Search);
    CHECK(f.provider.requests[0].initial == "old query");
    f.provider.finish_done("new query");
    CHECK(f.view().called_prefix(kSelectPrefix));
    CHECK(f.view().called("insert_text new query"));
    CHECK(f.view().calls.back() == "insert_text new query");  // selection first
  }

  TEST_CASE("Square without a field report inserts at the caret") {
    Fixture f;
    f.press(Button::Square);
    REQUIRE(f.provider.requests.size() == 1);
    CHECK(f.provider.requests[0].kind == TextInputKind::Text);
    CHECK(f.provider.requests[0].initial.empty());
    f.provider.finish_done("abc");
    CHECK_FALSE(f.view().called_prefix(kSelectPrefix));
    CHECK(f.view().called("insert_text abc"));
  }

  TEST_CASE("an emptied field is cleared instead of inserting nothing") {
    Fixture f;
    f.focus_field("text", "something");
    f.press(Button::Square);
    f.provider.finish_done("");
    CHECK(f.view().called_prefix(kSelectPrefix));
    CHECK(f.view().called("js document.execCommand('delete', false)"));
    CHECK_FALSE(f.view().called_prefix("insert_text"));
  }

  TEST_CASE("a password field is not pre-filled") {
    Fixture f;
    f.focus_field("password", nullptr);
    f.press(Button::Square);
    REQUIRE(f.provider.requests.size() == 1);
    CHECK(f.provider.requests[0].kind == TextInputKind::Password);
    CHECK(f.provider.requests[0].initial.empty());
    f.provider.finish_done("hunter2");
    CHECK_FALSE(f.view().called_prefix(kSelectPrefix));
    CHECK(f.view().called("insert_text hunter2"));
  }

  TEST_CASE("cancel changes nothing") {
    Fixture f;
    const auto calls_before = f.view().calls.size();
    f.press(Button::Triangle);
    f.provider.finish_cancelled();
    CHECK_FALSE(f.shell->text_input_active());
    CHECK(f.shell->focus() == Focus::Page);
    CHECK(f.view().calls.size() == calls_before);
    f.frame();
    CHECK(f.chrome.last["osk"]["open"] == false);
  }

  TEST_CASE("an error falls back to the built-in OSK with the same text") {
    Fixture f;
    f.press(Button::Triangle);
    f.provider.finish_error(0x80bc0001);
    CHECK(f.shell->focus() == Focus::Osk);
    f.frame();
    CHECK(f.chrome.last["osk"]["open"] == true);
    CHECK(f.chrome.last["osk"]["target"] == "address");
    CHECK(f.chrome.last["osk"]["text"] == kHome);
    f.press(Button::Options);  // commit the unchanged URL through the OSK
    CHECK(f.view().count(std::string("load ") + kHome) == 2);
  }

  TEST_CASE("the OSK fallback keeps replace semantics for a reported field") {
    Fixture f;
    f.focus_field("text", "abc");
    f.press(Button::Square);
    f.provider.finish_error(-1);
    REQUIRE(f.shell->focus() == Focus::Osk);
    f.frame();
    CHECK(f.chrome.last["osk"]["text"] == "abc");
    f.press(Button::Options);
    CHECK(f.view().called_prefix(kSelectPrefix));
    CHECK(f.view().calls.back() == "insert_text abc");
  }

  TEST_CASE("a provider that cannot start falls back immediately") {
    Fixture f;
    f.provider.refuse = true;
    f.press(Button::Triangle);
    CHECK(f.provider.requests.size() == 1);
    CHECK_FALSE(f.shell->text_input_active());
    CHECK(f.shell->focus() == Focus::Osk);
  }

  TEST_CASE("a synchronous error also falls back") {
    Fixture f;
    f.provider.sync_error = 7;
    f.press(Button::Square);
    CHECK_FALSE(f.shell->text_input_active());
    CHECK(f.shell->focus() == Focus::Osk);
  }

  TEST_CASE("gamepad input is ignored while the provider is active") {
    Fixture f;
    f.press(Button::Triangle);
    f.press(Button::Square);    // would request again / open the OSK
    f.press(Button::Options);   // would open the menu
    f.shell->on_sticks(1.0f, 0.0f, 0.0f, 0.0f);
    const auto before = f.shell->pointer();
    f.frame();
    CHECK(f.provider.requests.size() == 1);
    CHECK(f.shell->focus() == Focus::Page);
    CHECK(f.shell->pointer().x == before.x);
    f.provider.finish_cancelled();
    f.press(Button::Options);
    CHECK(f.shell->focus() == Focus::Menu);
  }

  TEST_CASE("a click that focuses a field opens the provider with its value") {
    Fixture f;
    f.focus_field("text", "x");  // focus not caused by our click
    CHECK(f.provider.requests.empty());
    f.press(Button::Cross);
    f.focus_field("email", "me@example.org");
    REQUIRE(f.provider.requests.size() == 1);
    CHECK(f.provider.requests[0].kind == TextInputKind::Email);
    CHECK(f.provider.requests[0].initial == "me@example.org");
    CHECK(f.shell->focus() == Focus::Page);
  }

  TEST_CASE("a page result is dropped after switching tabs") {
    Fixture f;
    f.focus_field("text", "a");
    f.press(Button::Square);
    f.shell->open_url_in_new_tab("https://other.example/");
    f.provider.finish_done("typed");
    CHECK_FALSE(f.view().called_prefix("insert_text"));
    CHECK_FALSE(f.factory.created.at(1)->called_prefix("insert_text"));
  }

  TEST_CASE("blur forgets the field, so the next Square inserts") {
    Fixture f;
    f.focus_field("text", "keep");
    f.view().client().text_input_focused(false);
    f.press(Button::Square);
    REQUIRE(f.provider.requests.size() == 1);
    CHECK(f.provider.requests[0].initial.empty());
  }

  TEST_CASE("without a provider the built-in OSK is pre-filled from the field") {
    Fixture f(/*with_provider=*/false);
    f.focus_field("text", "hello");
    f.press(Button::Square);
    CHECK(f.shell->focus() == Focus::Osk);
    CHECK(f.provider.requests.empty());
    f.frame();
    CHECK(f.chrome.last["osk"]["text"] == "hello");
  }

  TEST_CASE("kind mapping from input types") {
    CHECK(shell::text_input_kind_from_type("url") == TextInputKind::Url);
    CHECK(shell::text_input_kind_from_type("search") == TextInputKind::Search);
    CHECK(shell::text_input_kind_from_type("number") == TextInputKind::Number);
    CHECK(shell::text_input_kind_from_type("tel") == TextInputKind::Text);
    CHECK(shell::text_input_kind_from_type("textarea") == TextInputKind::Text);
    CHECK(std::string(shell::text_input_kind_name(TextInputKind::Password)) == "password");
  }
}
