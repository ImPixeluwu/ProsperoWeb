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

// Rewrites from_dir to to_dir in config.json text, where it starts a path
// or a file:// URL ("chrome_dir": "/data/prosperobrowser/chrome" from
// config.json.sample, a file:///data/ps5browser/... home_url). True if
// anything changed.
bool rewrite_root(std::string& text, const std::string& from_dir, const std::string& to_dir) {
  std::string out;
  bool changed = false;
  for (std::size_t pos = 0; pos < text.size();) {
    const std::size_t hit = text.find(from_dir, pos);
    if (hit == std::string::npos) {
      out.append(text, pos, std::string::npos);
      break;
    }
    const std::size_t end = hit + from_dir.size();
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
  if (changed) {
    text = std::move(out);
  }
  return changed;
}

// Points every old install root in a copied config.json at to_dir.
std::string rewrite_config_paths(const std::string& path, const std::vector<std::string>& from_dirs,
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
  bool changed = false;
  for (const auto& from_dir : from_dirs) {
    changed = rewrite_root(text, from_dir, to_dir) || changed;
  }
  if (!changed) {
    return "";
  }
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return errno_text("create " + tmp);
  }
  const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
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

std::vector<std::string> legacy_env_prefixes() { return {"PROSPEROBROWSER_", "PS5BROWSER_"}; }

namespace {

// Index of name's prefix in legacy_env_prefixes(), or -1. A bare prefix
// does not count.
int legacy_rank(const std::string& name) {
  const auto prefixes = legacy_env_prefixes();
  for (std::size_t i = 0; i < prefixes.size(); ++i) {
    const std::string& prefix = prefixes[i];
    if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

}  // namespace

std::string renamed_env_name(const std::string& name) {
  const int rank = legacy_rank(name);
  if (rank < 0) {
    return "";
  }
  return kEnvPrefix + name.substr(legacy_env_prefixes()[static_cast<std::size_t>(rank)].size());
}

std::vector<std::string> env_names_preferred_over(const std::string& name) {
  const int rank = legacy_rank(name);
  if (rank < 0) {
    return {};
  }
  const auto prefixes = legacy_env_prefixes();
  const std::string suffix = name.substr(prefixes[static_cast<std::size_t>(rank)].size());
  std::vector<std::string> names{kEnvPrefix + suffix};
  for (int i = 0; i < rank; ++i) {
    names.push_back(prefixes[static_cast<std::size_t>(i)] + suffix);
  }
  return names;
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
    if (legacy_rank(name) >= 0) {
      legacy.emplace_back(std::move(name), eq + 1);
    }
  }
  // Newest prefix first, so it is the one adopted when several are set.
  std::stable_sort(legacy.begin(), legacy.end(), [](const auto& a, const auto& b) {
    return legacy_rank(a.first) < legacy_rank(b.first);
  });
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

LegacyMigration migrate_legacy_profile(const std::vector<std::string>& from_dirs,
                                       const std::string& to_dir) {
  LegacyMigration result;
  const std::string marker = to_dir + "/" + kLegacyMigrationMarker;
  if (exists(marker)) {
    return result;
  }
  const auto items = legacy_profile_items();
  for (const auto& dir : from_dirs) {
    if (is_dir(dir) && std::any_of(items.begin(), items.end(), [&](const std::string& item) {
          return exists(dir + "/" + item);
        })) {
      result.from_dir = dir;
      break;
    }
  }
  if (result.from_dir.empty()) {
    return result;
  }
  result.ran = true;
  if (!make_dirs(to_dir)) {
    result.failed.push_back(errno_text("mkdir " + to_dir));
    return result;
  }
  for (const auto& item : items) {
    const std::string from = result.from_dir + "/" + item;
    const std::string to = to_dir + "/" + item;
    if (!exists(from) || exists(to)) {
      continue;
    }
    std::string error = copy_tree(from, to);
    if (error.empty() && item == "config.json") {
      error = rewrite_config_paths(to, from_dirs, to_dir);
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
      const std::string text = "copied from " + result.from_dir + "\n";
      if (::write(fd, text.data(), text.size()) < 0) {
        result.failed.push_back(errno_text("write " + marker));
      }
      ::close(fd);
    }
  }
  return result;
}

}  // namespace prosperoweb::core
