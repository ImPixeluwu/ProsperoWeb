// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/session.h"

namespace prosperoweb::core {

template <typename T>
bool Session::load(const char* key, T& target) {
  const auto raw = storage_.read(key);
  if (!raw) {
    return false;
  }
  const auto doc = nlohmann::json::parse(*raw, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded()) {
    return false;
  }
  return target.from_json(doc);
}

bool Session::save(const char* key, const nlohmann::json& doc) {
  // Replace invalid UTF-8 (e.g. from a broken page title) instead of throwing.
  return storage_.write(key, doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

bool Session::load_tabs(TabManager& tabs) { return load(kTabsKey, tabs); }
bool Session::load_history(History& history) { return load(kHistoryKey, history); }
bool Session::load_bookmarks(Bookmarks& bookmarks) { return load(kBookmarksKey, bookmarks); }

bool Session::save_tabs(const TabManager& tabs) { return save(kTabsKey, tabs.to_json()); }
bool Session::save_history(const History& history) {
  return save(kHistoryKey, history.to_json());
}
bool Session::save_bookmarks(const Bookmarks& bookmarks) {
  return save(kBookmarksKey, bookmarks.to_json());
}

bool Session::load_all(TabManager& tabs, History& history, Bookmarks& bookmarks) {
  const bool t = load_tabs(tabs);
  const bool h = load_history(history);
  const bool b = load_bookmarks(bookmarks);
  return t && h && b;
}

bool Session::save_all(const TabManager& tabs, const History& history,
                       const Bookmarks& bookmarks) {
  const bool t = save_tabs(tabs);
  const bool h = save_history(history);
  const bool b = save_bookmarks(bookmarks);
  return t && h && b;
}

}  // namespace prosperoweb::core
