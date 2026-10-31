// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperobrowser/core/tab_manager.h"

namespace prosperobrowser::core {

std::optional<std::uint64_t> TabManager::open(const std::string& url) {
  if (full()) {
    return std::nullopt;
  }
  Tab tab;
  tab.id = next_id_++;
  tab.url = url;
  tabs_.push_back(std::move(tab));
  active_id_ = tabs_.back().id;
  return active_id_;
}

bool TabManager::close(std::uint64_t id) {
  const auto idx = index_of(id);
  if (!idx) {
    return false;
  }
  tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(*idx));
  if (active_id_ != id) {
    return true;
  }
  if (tabs_.empty()) {
    active_id_.reset();
  } else if (*idx < tabs_.size()) {
    active_id_ = tabs_[*idx].id;  // right neighbour moved into this slot
  } else {
    active_id_ = tabs_[*idx - 1].id;
  }
  return true;
}

bool TabManager::activate(std::uint64_t id) {
  if (!index_of(id)) {
    return false;
  }
  active_id_ = id;
  return true;
}

void TabManager::next() { step(1); }

void TabManager::prev() { step(-1); }

void TabManager::step(int delta) {
  if (tabs_.empty()) {
    return;
  }
  const auto n = static_cast<long>(tabs_.size());
  long cur = 0;
  if (active_id_) {
    if (const auto idx = index_of(*active_id_)) {
      cur = static_cast<long>(*idx);
    }
  }
  const long target = ((cur + delta) % n + n) % n;
  active_id_ = tabs_[static_cast<std::size_t>(target)].id;
}

const Tab* TabManager::active() const {
  return active_id_ ? find(*active_id_) : nullptr;
}

const Tab* TabManager::find(std::uint64_t id) const {
  for (const auto& t : tabs_) {
    if (t.id == id) {
      return &t;
    }
  }
  return nullptr;
}

Tab* TabManager::find_mutable(std::uint64_t id) {
  return const_cast<Tab*>(static_cast<const TabManager*>(this)->find(id));
}

std::optional<std::size_t> TabManager::index_of(std::uint64_t id) const {
  for (std::size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].id == id) {
      return i;
    }
  }
  return std::nullopt;
}

bool TabManager::update_title(std::uint64_t id, const std::string& title) {
  Tab* t = find_mutable(id);
  if (t == nullptr) {
    return false;
  }
  t->title = title;
  return true;
}

bool TabManager::update_url(std::uint64_t id, const std::string& url) {
  Tab* t = find_mutable(id);
  if (t == nullptr) {
    return false;
  }
  t->url = url;
  return true;
}

bool TabManager::set_loading(std::uint64_t id, bool loading) {
  Tab* t = find_mutable(id);
  if (t == nullptr) {
    return false;
  }
  t->loading = loading;
  return true;
}

bool TabManager::set_nav_state(std::uint64_t id, bool can_go_back, bool can_go_forward) {
  Tab* t = find_mutable(id);
  if (t == nullptr) {
    return false;
  }
  t->can_go_back = can_go_back;
  t->can_go_forward = can_go_forward;
  return true;
}

nlohmann::json TabManager::to_json() const {
  nlohmann::json tabs = nlohmann::json::array();
  for (const auto& t : tabs_) {
    tabs.push_back({{"url", t.url}, {"title", t.title}});
  }
  nlohmann::json j = {{"tabs", std::move(tabs)}};
  if (active_id_) {
    if (const auto idx = index_of(*active_id_)) {
      j["active"] = *idx;
    }
  }
  return j;
}

bool TabManager::from_json(const nlohmann::json& j) {
  if (!j.is_object() || !j.contains("tabs") || !j["tabs"].is_array()) {
    return false;
  }
  std::vector<Tab> restored;
  for (const auto& item : j["tabs"]) {
    if (restored.size() >= kMaxTabs) {
      break;
    }
    if (!item.is_object() || !item.contains("url") || !item["url"].is_string()) {
      continue;
    }
    Tab t;
    t.id = next_id_++;
    t.url = item["url"].get<std::string>();
    if (item.contains("title") && item["title"].is_string()) {
      t.title = item["title"].get<std::string>();
    }
    restored.push_back(std::move(t));
  }
  tabs_ = std::move(restored);
  active_id_.reset();
  if (!tabs_.empty()) {
    std::size_t idx = 0;
    if (j.contains("active") && j["active"].is_number_unsigned()) {
      idx = j["active"].get<std::size_t>();
    }
    active_id_ = tabs_[idx < tabs_.size() ? idx : 0].id;
  }
  return true;
}

}  // namespace prosperobrowser::core
