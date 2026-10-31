// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/legacy_profile.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

extern char** environ;

namespace prosperoweb::core {

namespace {

bool exists(const std::string& path) {
  struct stat st {};
  return ::lstat(path.c_str(), &st) == 0;
}

bool is_dir(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string errno_text(const std::string& what) {
  return what + ": " + std::strerror(errno);
}

// mkdir -p.
bool make_dirs(const std::string& path) {
  for (std::size_t pos = path.find('/', 1); pos != std::string::npos;
       pos = path.find('/', pos + 1)) {
    const std::string prefix = path.substr(0, pos);
    if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST) {
      return false;
    }
  }
  return (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) && is_dir(path);
}

// Copies a regular file to `to` through `to.tmp`. Empty string on success.
std::string copy_file(const std::string& from, const std::string& to, mode_t mode) {
  const int in = ::open(from.c_str(), O_RDONLY);
  if (in < 0) {
    return errno_text("open " + from);
  }
  const std::string tmp = to + ".tmp";
  const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
  if (out < 0) {
    const std::string error = errno_text("create " + tmp);
    ::close(in);
    return error;
  }
  std::string error;
  char buf[64 * 1024];
  for (;;) {
    const ssize_t n = ::read(in, buf, sizeof buf);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = errno_text("read " + from);
      break;
    }
    ssize_t done = 0;
    while (done < n) {
      const ssize_t w = ::write(out, buf + done, static_cast<std::size_t>(n - done));
      if (w < 0) {
        if (errno == EINTR) {
          continue;
        }
        error = errno_text("write " + tmp);
        break;
      }
      done += w;
    }
    if (!error.empty()) {
      break;
    }
  }
  ::close(in);
  if (::close(out) != 0 && error.empty()) {
    error = errno_text("close " + tmp);
  }
  if (error.empty() && ::rename(tmp.c_str(), to.c_str()) != 0) {
    error = errno_text("rename " + tmp);
  }
  if (!error.empty()) {
    ::unlink(tmp.c_str());
  }
  return error;
}

// Rewrites from_dir to to_dir in a copied config.json, where it starts a
// path or a file:// URL ("chrome_dir": "/data/ps5browser/chrome" from
// config.json.sample, a file:///data/ps5browser/... home_url).
std::string rewrite_config_paths(const std::string& path, const std::string& from_dir,
                                 const std::string& to_dir) {
  const int in = ::open(path.c_str(), O_RDONLY);
  if (in < 0) {
    return errno_text("open " + path);
  }
  std::string text;
  char buf[4096];
  ssize_t n = 0;
  while ((n = ::read(in, buf, sizeof buf)) > 0) {
    text.append(buf, static_cast<std::size_t>(n));
  }
  ::close(in);
  if (n < 0) {
    return errno_text("read " + path);
  }
  std::string out;
  bool changed = false;
  for (std::size_t pos = 0; pos < text.size();) {
    const std::size_t hit = text.find(from_dir, pos);
    const std::size_t end = hit == std::string::npos ? hit : hit + from_dir.size();
    if (hit == std::string::npos) {
      out.append(text, pos, std::string::npos);
      break;
    }
    out.append(text, pos, hit - pos);
    const bool starts_path = (hit > 0 && text[hit - 1] == '"') ||
                             (hit >= 7 && text.compare(hit - 7, 7, "file://") == 0);
    const bool ends_path = end < text.size() && (text[end] == '/' || text[end] == '"');
    if (starts_path && ends_path) {
      out += to_dir;
      changed = true;
    } else {
      out.append(text, hit, from_dir.size());
    }
    pos = end;
  }
  if (!changed) {
    return "";
  }
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return errno_text("create " + tmp);
  }
  const bool written = ::write(fd, out.data(), out.size()) == static_cast<ssize_t>(out.size());
  ::close(fd);
  if (!written || ::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return errno_text("rewrite " + path);
  }
  return "";
}

// Copies a file or a directory tree. Other file types are skipped.
std::string copy_tree(const std::string& from, const std::string& to) {
  struct stat st {};
  if (::lstat(from.c_str(), &st) != 0) {
    return errno_text("stat " + from);
  }
  if (S_ISREG(st.st_mode)) {
    return copy_file(from, to, st.st_mode);
  }
  if (!S_ISDIR(st.st_mode)) {
    return "";
  }
  if (::mkdir(to.c_str(), st.st_mode & 0777) != 0 && errno != EEXIST) {
    return errno_text("mkdir " + to);
  }
  DIR* dir = ::opendir(from.c_str());
  if (dir == nullptr) {
    return errno_text("opendir " + from);
  }
  std::vector<std::string> names;
  while (const dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") {
      names.push_back(name);
    }
  }
  ::closedir(dir);
  for (const auto& name : names) {
    if (std::string error = copy_tree(from + "/" + name, to + "/" + name); !error.empty()) {
      return error;
    }
  }
  return "";
}

}  // namespace

std::string renamed_env_name(const std::string& name) {
  const std::size_t prefix = std::strlen(kLegacyEnvPrefix);
  if (name.size() <= prefix || name.compare(0, prefix, kLegacyEnvPrefix) != 0) {
    return "";
  }
  return kEnvPrefix + name.substr(prefix);
}

std::vector<std::string> adopt_legacy_env() {
  // Collect first: setenv() may reallocate environ.
  std::vector<std::pair<std::string, std::string>> legacy;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const char* eq = std::strchr(*entry, '=');
    if (eq == nullptr) {
      continue;
    }
    std::string name(*entry, static_cast<std::size_t>(eq - *entry));
    if (!renamed_env_name(name).empty()) {
      legacy.emplace_back(std::move(name), eq + 1);
    }
  }
  std::vector<std::string> adopted;
  for (const auto& [name, value] : legacy) {
    const std::string renamed = renamed_env_name(name);
    if (std::getenv(renamed.c_str()) == nullptr &&
        ::setenv(renamed.c_str(), value.c_str(), /*overwrite=*/0) == 0) {
      adopted.push_back(name);
    }
  }
  std::sort(adopted.begin(), adopted.end());
  return adopted;
}

std::vector<std::string> legacy_profile_items() {
  return {"tabs.json", "history.json", "bookmarks.json", "config.json",
          "storage",   "localstorage", "indexeddb",      "cache"};
}

LegacyMigration migrate_legacy_profile(const std::string& from_dir, const std::string& to_dir) {
  LegacyMigration result;
  const std::string marker = to_dir + "/" + kLegacyMigrationMarker;
  if (!is_dir(from_dir) || exists(marker)) {
    return result;
  }
  result.ran = true;
  if (!make_dirs(to_dir)) {
    result.failed.push_back(errno_text("mkdir " + to_dir));
    return result;
  }
  for (const auto& item : legacy_profile_items()) {
    const std::string from = from_dir + "/" + item;
    const std::string to = to_dir + "/" + item;
    if (!exists(from) || exists(to)) {
      continue;
    }
    std::string error = copy_tree(from, to);
    if (error.empty() && item == "config.json") {
      error = rewrite_config_paths(to, from_dir, to_dir);
    }
    if (!error.empty()) {
      result.failed.push_back(item + ": " + error);
      continue;
    }
    result.copied++;
  }
  // Only a complete carry-over is final; a failed item is retried next start.
  if (result.failed.empty()) {
    const int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      result.failed.push_back(errno_text("create " + marker));
    } else {
      static constexpr char kText[] = "copied from the ps5browser profile\n";
      if (::write(fd, kText, sizeof kText - 1) < 0) {
        result.failed.push_back(errno_text("write " + marker));
      }
      ::close(fd);
    }
  }
  return result;
}

}  // namespace prosperoweb::core
