// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace prosperoweb::core {

struct Bookmark {
  std::string url;
  std::string title;
  std::int64_t added_at = 0;  // unix seconds
};

// Flat bookmark list in insertion order, unique by URL.
class Bookmarks {
 public:
  // Returns false if the URL is already bookmarked (the list is unchanged).
  bool add(const std::string& url, const std::string& title, std::int64_t now);
  bool remove(const std::string& url);
  bool contains(const std::string& url) const;
  const std::vector<Bookmark>& list() const { return items_; }
  std::size_t size() const { return items_.size(); }

  // JSON array of {"url", "title", "added_at"}.
  nlohmann::json to_json() const;
  // Replaces the list. Invalid and duplicate items are skipped. Returns false
  // (and leaves state unchanged) if j is not an array.
  bool from_json(const nlohmann::json& j);

 private:
  std::vector<Bookmark> items_;
};

}  // namespace prosperoweb::core
