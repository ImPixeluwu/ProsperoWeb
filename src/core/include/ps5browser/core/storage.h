// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ps5browser::core {

// Key/value persistence used by Session. Keys are short identifiers
// ([A-Za-z0-9_-]+); values are opaque strings (JSON documents in practice).
class Storage {
 public:
  virtual ~Storage();

  virtual std::optional<std::string> read(const std::string& key) = 0;
  virtual bool write(const std::string& key, const std::string& value) = 0;

  // True if key is non-empty and consists only of [A-Za-z0-9_-].
  static bool is_valid_key(std::string_view key);
};

}  // namespace ps5browser::core
