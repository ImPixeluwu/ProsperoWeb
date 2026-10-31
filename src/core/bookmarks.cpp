// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperobrowser/core/bookmarks.h"

#include <algorithm>

namespace prosperobrowser::core {

bool Bookmarks::add(const std::string& url, const std::string& title, std::int64_t now) {
  if (contains(url)) {
    return false;
  }
  items_.push_back(Bookmark{url, title, now});
  return true;
}

bool Bookmarks::remove(const std::string& url) {
  return std::erase_if(items_, [&](const Bookmark& b) { return b.url == url; }) > 0;
}

bool Bookmarks::contains(const std::string& url) const {
  return std::any_of(items_.begin(), items_.end(),
                     [&](const Bookmark& b) { return b.url == url; });
}

nlohmann::json Bookmarks::to_json() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& b : items_) {
    arr.push_back({{"url", b.url}, {"title", b.title}, {"added_at", b.added_at}});
  }
  return arr;
}

bool Bookmarks::from_json(const nlohmann::json& j) {
  if (!j.is_array()) {
    return false;
  }
  Bookmarks restored;
  for (const auto& item : j) {
    if (!item.is_object() || !item.contains("url") || !item["url"].is_string()) {
      continue;
    }
    std::string title;
    if (item.contains("title") && item["title"].is_string()) {
      title = item["title"].get<std::string>();
    }
    std::int64_t added_at = 0;
    if (item.contains("added_at") && item["added_at"].is_number_integer()) {
      added_at = item["added_at"].get<std::int64_t>();
    }
    restored.add(item["url"].get<std::string>(), title, added_at);
  }
  items_ = std::move(restored.items_);
  return true;
}

}  // namespace prosperobrowser::core
