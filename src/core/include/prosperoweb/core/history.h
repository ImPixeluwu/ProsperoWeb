// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace prosperoweb::core {

struct HistoryEntry {
  std::string url;
  std::string title;
  std::int64_t visited_at = 0;  // unix seconds
  std::uint32_t visit_count = 0;
};

// Browsing history keyed by URL, capped at kMaxEntries.
class History {
 public:
  static constexpr std::size_t kMaxEntries = 2000;

  // Records a visit. A repeated URL bumps visit_count and updates the visit
  // time (and the title, if the new one is non-empty). When the cap is
  // exceeded the entry with the oldest visit time is evicted.
  void add(const std::string& url, const std::string& title, std::int64_t now);

  // Most recently visited first.
  std::vector<HistoryEntry> recent(std::size_t n) const;

  // Case-insensitive substring match over url and title (ASCII and basic
  // Cyrillic folding). Ordered by visit_count desc, then visited_at desc.
  std::vector<HistoryEntry> search(std::string_view query, std::size_t n) const;

  bool remove(const std::string& url);
  void clear() { entries_.clear(); }
  std::size_t size() const { return entries_.size(); }
  // Entries in least-recently-touched-first order.
  const std::vector<HistoryEntry>& entries() const { return entries_; }

  // JSON array of {"url", "title", "visited_at", "visit_count"}.
  nlohmann::json to_json() const;
  // Replaces the history. Invalid items are skipped. Returns false (and
  // leaves state unchanged) if j is not an array.
  bool from_json(const nlohmann::json& j);

 private:
  void evict_if_needed();

  std::vector<HistoryEntry> entries_;
};

}  // namespace prosperoweb::core
