// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <optional>
#include <string>

#include "prosperobrowser/core/storage.h"

namespace prosperobrowser::core {

// Storage backed by a std::map; intended for tests.
class InMemoryStorage final : public Storage {
 public:
  std::optional<std::string> read(const std::string& key) override;
  bool write(const std::string& key, const std::string& value) override;

  const std::map<std::string, std::string>& data() const { return data_; }

 private:
  std::map<std::string, std::string> data_;
};

}  // namespace prosperobrowser::core
