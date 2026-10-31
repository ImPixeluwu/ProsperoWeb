// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ps5browser/core/bookmarks.h"
#include "ps5browser/core/drm_policy.h"
#include "ps5browser/core/fullscreen_controller.h"
#include "ps5browser/core/gamepad.h"
#include "ps5browser/core/gamepad_mapper.h"
#include "ps5browser/core/history.h"
#include "ps5browser/core/on_screen_keyboard.h"
#include "ps5browser/core/pointer_model.h"
#include "ps5browser/core/repeat_timer.h"
#include "ps5browser/core/session.h"
#include "ps5browser/core/storage.h"
#include "ps5browser/core/tab_manager.h"
#include "ps5browser/shell/chrome_ui.h"
#include "ps5browser/shell/engine_view.h"

namespace ps5browser::shell {

// Engine-independent browser logic: tabs, navigation, gamepad handling,
// on-screen keyboard, fullscreen, menu, DRM notices and session persistence.
// The frontend feeds it input (on_button, on_sticks, on_mouse_*, tick) and
// chrome events, and implements EngineFactory/EngineView and ChromeUi.
class ShellController {
 public:
  // Height of the top band (tab strip + address bar) in window pixels.
  static constexpr int kChromeBandHeight = 96;
  // D-pad scroll step in px.
  static constexpr int kScrollStep = 120;
  // Fraction of the viewport scrolled by L2/R2.
  static constexpr float kPageScrollFraction = 0.85f;
  // How long after a Cross click a text_input_focused(true) opens the OSK.
  static constexpr float kClickFocusWindowSeconds = 1.0f;
  static constexpr std::size_t kMenuHistoryItems = 30;
  // Tabs and history are saved at most this long after they change, so a
  // console app kill (PS button, crash) loses little; shutdown() saves too.
  static constexpr float kSessionSaveDelaySeconds = 5.0f;

  struct Config {
    int width = 1920;
    int height = 1080;
    std::string home_url = "about:blank";
    std::string user_agent;
    std::string language = "en";  // "en" or "ru"
    // Unix time source for history/bookmarks; defaults to time(nullptr).
    std::function<std::int64_t()> now;
  };

  enum class Focus : std::uint8_t { Page, Chrome, Osk, Menu, Notice };
  enum class OskTarget : std::uint8_t { Address, Page };
  enum class MenuSection : std::uint8_t { Main, Bookmarks, History };

  ShellController(EngineFactory& factory, ChromeUi& chrome, core::Storage& storage,
                  Config config);
  ~ShellController();

  ShellController(const ShellController&) = delete;
  ShellController& operator=(const ShellController&) = delete;

  // Restores the saved session (only the active tab is loaded right away,
  // the others load when first activated) or opens the home page.
  void start();
  // Saves tabs, history and bookmarks (tabs and history are also saved
  // from tick(), kSessionSaveDelaySeconds after a change).
  void shutdown();

  // Gamepad input.
  void on_button(core::Button button, bool pressed);
  // Left stick moves the pointer, right stick scrolls; values in [-1, 1].
  void on_sticks(float lx, float ly, float rx, float ry);
  // Advances pointer motion, scrolling and key repeat; renders the chrome
  // if anything changed.
  void tick(float dt_seconds);

  // Host mouse passthrough (window coordinates). The real mouse goes to the
  // engine directly on the host; these keep the pointer model in sync and
  // hide the gamepad pointer while the mouse is in use.
  void on_mouse_move(float x, float y);

  // Events from the HTML chrome:
  //   {type:'ready'}                       chrome loaded, re-render
  //   {type:'navigate', text}              address bar submit
  //   {type:'nav', action:'back'|'forward'|'reload'|'stop'}
  //   {type:'tab_select', id} {type:'tab_close', id} {type:'tab_new'}
  //   {type:'menu_open'} {type:'menu_select', index} {type:'menu_close'}
  //   {type:'notice_dismiss', proceed}
  //   {type:'osk_key', row, col} {type:'osk_commit'} {type:'osk_cancel'}
  //   {type:'address_focus'} {type:'address_blur'}
  // event is a JSON object; a JSON string holding such an object is decoded
  // first. Anything else is ignored.
  void on_chrome_event(const nlohmann::json& event);

  // Opens url in a new tab (shows the tab-limit notice when full).
  bool open_url_in_new_tab(const std::string& url);
  // Navigates the active tab to address-bar input (normalized).
  void navigate(const std::string& input);

  void set_viewport(int width, int height);

  // A window-space rectangle.
  struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };
  // Window areas where the chrome may draw right now: the top band, the OSK
  // panel, the whole window under a menu or notice, and the gamepad pointer.
  // Compositing frontends blend the chrome over the page only inside these
  // (published as "chrome_rects" in chrome_state()).
  std::vector<Rect> chrome_rects() const;

  // Builds the chrome state document (see ChromeUi).
  nlohmann::json chrome_state() const;
  // Renders the chrome now, regardless of the dirty flag.
  void render_now();

  // Introspection (frontends and tests).
  Focus focus() const { return focus_; }
  core::Mode mode() const;
  const core::TabManager& tabs() const { return tabs_; }
  const core::History& history() const { return history_; }
  const core::Bookmarks& bookmarks() const { return bookmarks_; }
  const core::FullscreenController& fullscreen() const { return fullscreen_; }
  const core::OnScreenKeyboard& osk() const { return osk_; }
  core::Vec2 pointer() const { return pointer_.position(); }
  // Window y of the content view's top edge: the chrome band height while
  // the chrome is visible, else 0. The content view is width x (height -
  // content_top()) and page coordinates are window coordinates minus this.
  int content_top() const;
  EngineView* active_view();
  EngineView* view_for(std::uint64_t tab_id);
  bool quit_requested() const { return quit_requested_; }
  // True while the active page reports a focused editable element; hosts
  // with a physical keyboard use it to stop treating letters as buttons.
  bool text_input_focused() const { return text_input_focused_; }

 private:
  class TabClient;
  friend class TabClient;

  struct TabEntry {
    std::unique_ptr<TabClient> client;
    std::unique_ptr<EngineView> view;
    bool loaded = false;  // a URL was sent to the view
  };

  struct Notice {
    std::string kind;  // "drm", "error", "info"
    std::string title;
    std::string text;
  };

  struct PendingNavigation {
    std::uint64_t tab_id = 0;
    std::string url;
  };

  struct MenuItem {
    std::string label;
    std::string url;
    std::function<void()> run;
  };

  // Engine callbacks (per tab).
  void on_url_changed(std::uint64_t id, const std::string& url);
  void on_title_changed(std::uint64_t id, const std::string& title);
  void on_load_started(std::uint64_t id);
  void on_load_finished(std::uint64_t id);
  void on_load_failed(std::uint64_t id, const std::string& url, const std::string& error);
  void on_nav_state(std::uint64_t id, bool back, bool fwd);
  void on_fullscreen_entered(std::uint64_t id);
  void on_fullscreen_exited(std::uint64_t id);
  void on_script_message(std::uint64_t id, const std::string& json);
  void on_new_window(std::uint64_t id, const std::string& url);
  void on_text_input_focused(std::uint64_t id, bool focused);

  // Actions.
  void run_action(core::Action action);
  void run_page_action(core::Action action);
  void run_chrome_action(core::Action action);
  void run_menu_action(core::Action action);
  void run_osk_action(core::Action action);
  void handle_notice_button(core::Button button);

  // Tabs and engines.
  std::optional<std::uint64_t> open_tab(const std::string& url, bool load_now);
  void close_tab(std::uint64_t id);
  void activate_tab(std::uint64_t id);
  void after_tab_switch();
  TabEntry& ensure_entry(std::uint64_t id);
  void load_in_tab(std::uint64_t id, const std::string& url);
  void load_in_tab_now(std::uint64_t id, const std::string& url);
  void apply_geometry();
  void sync_visibility();

  // Input helpers.
  void click_at_pointer();
  void scroll_active(int dx, int dy);
  core::Vec2 to_view(core::Vec2 window_pos) const;

  // Overlays.
  void open_osk(OskTarget target, const std::string& text);
  void close_osk();
  void commit_osk();
  void open_menu();
  void show_menu_section(MenuSection section);
  void close_menu();
  void activate_menu_item(std::size_t index);
  void show_notice(Notice notice);
  void dismiss_notice(bool proceed);
  void toggle_bookmark();
  void toggle_fullscreen();
  void exit_fullscreen();
  void set_focus(Focus focus);

  std::int64_t now() const;
  core::Language language() const;
  void mark_dirty() { dirty_ = true; }
  void mark_session_dirty() { session_dirty_ = true; }
  void save_session_if_due(float dt_seconds);

  EngineFactory& factory_;
  ChromeUi& chrome_;
  core::Session session_;
  Config config_;

  core::TabManager tabs_;
  core::History history_;
  core::Bookmarks bookmarks_;
  core::GamepadMapper mapper_;
  core::PointerModel pointer_;
  core::RepeatTimer repeat_;
  core::OnScreenKeyboard osk_;
  core::FullscreenController fullscreen_;
  core::DrmPolicy drm_;

  std::map<std::uint64_t, TabEntry> entries_;
  std::optional<std::uint64_t> visible_tab_;

  Focus focus_ = Focus::Page;
  Focus focus_before_notice_ = Focus::Page;
  OskTarget osk_target_ = OskTarget::Address;
  bool osk_open_ = false;
  bool menu_open_ = false;
  MenuSection menu_section_ = MenuSection::Main;
  std::vector<MenuItem> menu_items_;
  std::size_t menu_selected_ = 0;
  std::optional<Notice> notice_;
  std::optional<PendingNavigation> pending_nav_;

  // Held button for auto-repeat.
  std::optional<core::Button> held_button_;
  float repeat_carry_ms_ = 0.0f;

  float sticks_[4] = {0, 0, 0, 0};
  core::Vec2 scroll_carry_;
  bool pointer_hidden_by_mouse_ = false;
  float since_click_ = -1.0f;  // seconds since the last Cross click, <0 = none
  bool text_input_focused_ = false;
  bool quit_requested_ = false;
  bool dirty_ = true;
  bool session_dirty_ = false;
  float since_session_change_ = 0.0f;
};

}  // namespace ps5browser::shell
