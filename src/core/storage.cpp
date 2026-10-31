// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperobrowser/core/storage.h"

namespace prosperobrowser::core {

Storage::~Storage() = default;

bool Storage::is_valid_key(std::string_view key) {
  if (key.empty()) {
    return false;
  }
  for (char c : key) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

}  // namespace prosperobrowser::core
