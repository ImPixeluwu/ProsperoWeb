// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "prosperoweb/core/bookmarks.h"
#include "prosperoweb/core/history.h"
#include "prosperoweb/core/site_zoom.h"
#include "prosperoweb/core/site_user_agents.h"
#include "prosperoweb/core/storage.h"
#include "prosperoweb/core/tab_manager.h"
#include "prosperoweb/core/userscripts.h"

namespace prosperoweb::core {

// Loads and saves browser state through a Storage under the keys
// kTabsKey, kHistoryKey, kBookmarksKey, kZoomKey, kUserAgentsKey and
// kUserscriptsKey. Does not own anything.
class Session {
 public:
  static constexpr const char* kTabsKey = "tabs";
  static constexpr const char* kHistoryKey = "history";
  static constexpr const char* kBookmarksKey = "bookmarks";
  static constexpr const char* kZoomKey = "zoom";
  static constexpr const char* kUserAgentsKey = "user_agents";
  static constexpr const char* kUserscriptsKey = "userscripts";

  explicit Session(Storage& storage) : storage_(storage) {}

  // Each load returns false if the key is missing or the stored document is
  // malformed; the target object is left unchanged in that case.
  bool load_tabs(TabManager& tabs);
  bool load_history(History& history);
  bool load_bookmarks(Bookmarks& bookmarks);
  bool load_zoom(SiteZoom& zoom);
  bool load_user_agents(SiteUserAgents& user_agents);
  bool load_userscripts(UserscriptStore& userscripts);

  bool save_tabs(const TabManager& tabs);
  bool save_history(const History& history);
  bool save_bookmarks(const Bookmarks& bookmarks);
  bool save_zoom(const SiteZoom& zoom);
  bool save_user_agents(const SiteUserAgents& user_agents);
  bool save_userscripts(const UserscriptStore& userscripts);

  // Convenience wrappers; return true only if every part succeeded.
  bool load_all(TabManager& tabs, History& history, Bookmarks& bookmarks);
  bool save_all(const TabManager& tabs, const History& history, const Bookmarks& bookmarks);

 private:
  template <typename T>
  bool load(const char* key, T& target);
  bool save(const char* key, const nlohmann::json& doc);

  Storage& storage_;
};

}  // namespace prosperoweb::core
