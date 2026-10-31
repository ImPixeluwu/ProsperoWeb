// SPDX-License-Identifier: GPL-3.0-or-later
#include "title_paths.h"

namespace prosperoweb::spawn {

namespace {

// ShadowMountPlus README.md at 30c5a2b7, "Scan paths" (defaults).
constexpr const char* kScanPaths[] = {
    "/data/homebrew",       "/data/etaHEN/games",       "/mnt/ext0/homebrew",
    "/mnt/ext0/etaHEN/games", "/mnt/ext1/homebrew",     "/mnt/ext1/etaHEN/games",
    "/mnt/usb0/homebrew",   "/mnt/usb0/etaHEN/games",   "/mnt/usb1/homebrew",
    "/mnt/usb1/etaHEN/games", "/mnt/usb2/homebrew",     "/mnt/usb3/homebrew",
    "/mnt/usb4/homebrew",   "/mnt/usb5/homebrew",       "/mnt/usb6/homebrew",
    "/mnt/usb7/homebrew",
};

std::string strip_slash(std::string path) {
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

}  // namespace

std::vector<std::string> title_dir_candidates(const std::string& title_id, const char* override) {
  std::vector<std::string> out;
  if (override && override[0]) {
    out.push_back(strip_slash(override));
  }
  for (const char* base : kScanPaths) {
    out.push_back(std::string(base) + "/" + title_id);
  }
  return out;
}

TitleLayout resolve_title_layout(
    const std::string& app0, const std::string& title_id, const char* override,
    const std::function<bool(const std::string&, const std::string&)>& same_title) {
  TitleLayout layout;
  if (override && override[0]) {
    layout.ui_root = layout.child_root = strip_slash(override);
    layout.how = "PROSPEROWEB_TITLE_DIR";
    return layout;
  }
  for (const std::string& dir : title_dir_candidates(title_id, nullptr)) {
    if (same_title(dir, app0)) {
      layout.ui_root = layout.child_root = dir;
      layout.how = "install folder " + dir + " (same eboot.bin as " + app0 + ")";
      return layout;
    }
  }
  layout.ui_root = app0;
  layout.child_root = "/mnt/sandbox/" + title_id + "_000" + app0;
  layout.how = "no install folder under the ShadowMountPlus scan paths; helpers use the "
               "sandbox path";
  return layout;
}

std::string rebase_path(const std::string& value, const std::string& from, const std::string& to) {
  if (from.empty() || from == to || value.compare(0, from.size(), from) != 0) {
    return value;
  }
  if (value.size() != from.size() && value[from.size()] != '/') {
    return value;  // "/app0x" is not under "/app0"
  }
  return to + value.substr(from.size());
}

std::vector<std::string> rebase_env(const std::vector<std::string>& env, const std::string& from,
                                    const std::string& to) {
  static const std::string kFile = "file://";
  std::vector<std::string> out;
  out.reserve(env.size());
  for (const std::string& entry : env) {
    const size_t eq = entry.find('=');
    if (eq == std::string::npos) {
      out.push_back(entry);
      continue;
    }
    std::string value = entry.substr(eq + 1);
    if (value.compare(0, kFile.size(), kFile) == 0) {
      value = kFile + rebase_path(value.substr(kFile.size()), from, to);
    } else {
      value = rebase_path(value, from, to);
    }
    out.push_back(entry.substr(0, eq + 1) + value);
  }
  return out;
}

}  // namespace prosperoweb::spawn
