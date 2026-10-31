// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>

#include "prosperoweb/core/storage.h"

namespace prosperoweb::core {

// Stores each key in "<dir>/<key>.json". Writes go to "<key>.json.tmp" first
// and are renamed into place, so a crash never leaves a truncated file.
// The directory (and missing parents) is created on first write.
// POSIX only: uses stdio, mkdir(2), fsync(2) and rename(2).
class FileStorage final : public Storage {
 public:
  explicit FileStorage(std::string dir);

  std::optional<std::string> read(const std::string& key) override;
  bool write(const std::string& key, const std::string& value) override;

  const std::string& dir() const { return dir_; }
  std::string path_for(const std::string& key) const;

 private:
  bool ensure_dir() const;

  std::string dir_;
};

}  // namespace prosperoweb::core
