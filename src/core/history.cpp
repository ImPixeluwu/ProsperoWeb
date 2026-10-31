// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperobrowser/core/history.h"

#include <algorithm>

#include "text_utils.h"

namespace prosperobrowser::core {

void History::add(const std::string& url, const std::string& title, std::int64_t now) {
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const HistoryEntry& e) { return e.url == url; });
  if (it != entries_.end()) {
    HistoryEntry e = std::move(*it);
    entries_.erase(it);
    e.visit_count += 1;
    e.visited_at = now;
    if (!title.empty()) {
      e.title = title;
    }
    entries_.push_back(std::move(e));
    return;
  }
  entries_.push_back(HistoryEntry{url, title, now, 1});
  evict_if_needed();
}

void History::evict_if_needed() {
  while (entries_.size() > kMaxEntries) {
    // min_element returns the first minimum, i.e. the least recently touched
    // entry among those with the oldest timestamp.
    auto oldest = std::min_element(
        entries_.begin(), entries_.end(),
        [](const HistoryEntry& a, const HistoryEntry& b) { return a.visited_at < b.visited_at; });
    entries_.erase(oldest);
  }
}

std::vector<HistoryEntry> History::recent(std::size_t n) const {
  std::vector<HistoryEntry> out(entries_.rbegin(), entries_.rend());
  std::stable_sort(out.begin(), out.end(), [](const HistoryEntry& a, const HistoryEntry& b) {
    return a.visited_at > b.visited_at;
  });
  if (out.size() > n) {
    out.resize(n);
  }
  return out;
}

std::vector<HistoryEntry> History::search(std::string_view query, std::size_t n) const {
  const std::string needle = detail::fold_case(detail::trim(query));
  std::vector<HistoryEntry> out;
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (needle.empty() || detail::fold_case(it->url).find(needle) != std::string::npos ||
        detail::fold_case(it->title).find(needle) != std::string::npos) {
      out.push_back(*it);
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const HistoryEntry& a, const HistoryEntry& b) {
    if (a.visit_count != b.visit_count) {
      return a.visit_count > b.visit_count;
    }
    return a.visited_at > b.visited_at;
  });
  if (out.size() > n) {
    out.resize(n);
  }
  return out;
}

bool History::remove(const std::string& url) {
  const auto before = entries_.size();
  std::erase_if(entries_, [&](const HistoryEntry& e) { return e.url == url; });
  return entries_.size() != before;
}

nlohmann::json History::to_json() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& e : entries_) {
    arr.push_back({{"url", e.url},
                   {"title", e.title},
                   {"visited_at", e.visited_at},
                   {"visit_count", e.visit_count}});
  }
  return arr;
}

bool History::from_json(const nlohmann::json& j) {
  if (!j.is_array()) {
    return false;
  }
  std::vector<HistoryEntry> restored;
  for (const auto& item : j) {
    if (!item.is_object() || !item.contains("url") || !item["url"].is_string() ||
        !item.contains("visited_at") || !item["visited_at"].is_number_integer()) {
      continue;
    }
    HistoryEntry e;
    e.url = item["url"].get<std::string>();
    e.visited_at = item["visited_at"].get<std::int64_t>();
    if (item.contains("title") && item["title"].is_string()) {
      e.title = item["title"].get<std::string>();
    }
    e.visit_count = 1;
    if (item.contains("visit_count") && item["visit_count"].is_number_unsigned()) {
      e.visit_count = std::max<std::uint32_t>(1, item["visit_count"].get<std::uint32_t>());
    }
    const bool duplicate = std::any_of(restored.begin(), restored.end(),
                                       [&](const HistoryEntry& r) { return r.url == e.url; });
    if (!duplicate) {
      restored.push_back(std::move(e));
    }
  }
  entries_ = std::move(restored);
  evict_if_needed();
  return true;
}

}  // namespace prosperobrowser::core
