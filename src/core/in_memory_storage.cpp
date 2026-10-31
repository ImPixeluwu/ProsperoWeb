// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/in_memory_storage.h"

namespace prosperoweb::core {

std::optional<std::string> InMemoryStorage::read(const std::string& key) {
  const auto it = data_.find(key);
  if (it == data_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool InMemoryStorage::write(const std::string& key, const std::string& value) {
  if (!is_valid_key(key)) {
    return false;
  }
  data_[key] = value;
  return true;
}

}  // namespace prosperoweb::core
