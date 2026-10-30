// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ps5browser/core/tab.h"

namespace ps5browser::core {

// Ordered list of tabs with a single active tab.
//
// Tab ids are assigned monotonically and never reused for the lifetime of a
// TabManager instance, including across from_json() calls.
class TabManager {
 public:
  static constexpr std::size_t kMaxTabs = 8;

  // Opens a tab at the end of the strip and makes it active.
  // Returns std::nullopt when kMaxTabs tabs are already open.
  std::optional<std::uint64_t> open(const std::string& url);

  // Closes a tab. If it was active, the nearest right neighbour becomes
  // active, else the left one. Closing the last tab leaves no tabs.
  bool close(std::uint64_t id);

  bool activate(std::uint64_t id);
  void next();  // cyclic
  void prev();  // cyclic

  const Tab* active() const;
  std::optional<std::uint64_t> active_id() const { return active_id_; }
  const Tab* find(std::uint64_t id) const;
  const std::vector<Tab>& tabs() const { return tabs_; }
  std::size_t size() const { return tabs_.size(); }
  bool empty() const { return tabs_.empty(); }
  bool full() const { return tabs_.size() >= kMaxTabs; }

  bool update_title(std::uint64_t id, const std::string& title);
  bool update_url(std::uint64_t id, const std::string& url);
  bool set_loading(std::uint64_t id, bool loading);
  bool set_nav_state(std::uint64_t id, bool can_go_back, bool can_go_forward);

  // Session restore format: {"active": <index>, "tabs": [{"url", "title"}]}.
  // Transient state (loading, navigation flags) is not persisted.
  nlohmann::json to_json() const;
  // Replaces all tabs; restored tabs get fresh ids. Invalid entries are
  // skipped, extra entries beyond kMaxTabs are dropped. Returns false if the
  // document is not an object with a "tabs" array (state is left unchanged).
  bool from_json(const nlohmann::json& j);

 private:
  Tab* find_mutable(std::uint64_t id);
  std::optional<std::size_t> index_of(std::uint64_t id) const;
  void step(int delta);

  std::vector<Tab> tabs_;
  std::optional<std::uint64_t> active_id_;
  std::uint64_t next_id_ = 1;
};

}  // namespace ps5browser::core
