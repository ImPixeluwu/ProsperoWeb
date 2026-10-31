// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace prosperoweb::core {

struct Tab {
  std::uint64_t id = 0;
  std::string url;
  std::string title;
  bool loading = false;
  bool can_go_back = false;
  bool can_go_forward = false;
};

}  // namespace prosperoweb::core
