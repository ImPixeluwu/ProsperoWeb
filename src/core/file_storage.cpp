// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/file_storage.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

namespace prosperoweb::core {

namespace {

bool make_dir(const std::string& path) {
  if (path.empty()) {
    return true;
  }
  if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }
  return false;
}

}  // namespace

FileStorage::FileStorage(std::string dir) : dir_(std::move(dir)) {
  while (dir_.size() > 1 && dir_.back() == '/') {
    dir_.pop_back();
  }
}

std::string FileStorage::path_for(const std::string& key) const {
  return dir_ + "/" + key + ".json";
}

bool FileStorage::ensure_dir() const {
  // mkdir -p: create every prefix ending before a '/'.
  for (std::size_t pos = dir_.find('/', 1); pos != std::string::npos;
       pos = dir_.find('/', pos + 1)) {
    if (!make_dir(dir_.substr(0, pos))) {
      return false;
    }
  }
  return make_dir(dir_);
}

std::optional<std::string> FileStorage::read(const std::string& key) {
  if (!is_valid_key(key)) {
    return std::nullopt;
  }
  std::FILE* f = std::fopen(path_for(key).c_str(), "rb");
  if (f == nullptr) {
    return std::nullopt;
  }
  std::string out;
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    out.append(buf, n);
  }
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  if (failed) {
    return std::nullopt;
  }
  return out;
}

bool FileStorage::write(const std::string& key, const std::string& value) {
  if (!is_valid_key(key) || !ensure_dir()) {
    return false;
  }
  const std::string final_path = path_for(key);
  const std::string tmp_path = final_path + ".tmp";
  std::FILE* f = std::fopen(tmp_path.c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  bool ok = std::fwrite(value.data(), 1, value.size(), f) == value.size();
  ok = std::fflush(f) == 0 && ok;
  ok = ::fsync(::fileno(f)) == 0 && ok;
  ok = std::fclose(f) == 0 && ok;
  if (!ok || std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    std::remove(tmp_path.c_str());
    return false;
  }
  return true;
}

}  // namespace prosperoweb::core
