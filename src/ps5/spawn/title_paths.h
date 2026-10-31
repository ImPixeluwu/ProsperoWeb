// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the native title's files are, as the UI and as its helpers see them.
//
// The native title (PRWB00001/eboot.bin) runs in its own sandbox and sees
// its folder as /app0. The spawn broker, the WebProcess and the
// NetworkProcess are payloads started through elfldr and see the real file
// system, where /app0 does not exist. Anything the UI hands to them (the
// payload ELF to run, the fonts, the CA bundle, the chrome page URL) must
// therefore name the title folder by a path valid for them.
//
// ShadowMountPlus mounts /data (and /mnt/usbN, /mnt/extN) into a homebrew
// title's sandbox with writable nullfs (README.md at 30c5a2b7, "Runtime
// config"), so when the title is installed as a plain folder under one of
// its scan paths the same path works on both sides. That folder is found
// by looking for <scan path>/<title id> holding the same eboot.bin as
// /app0. Otherwise (an image mount, an unknown folder) the helpers get the
// sandbox's app0 seen from outside, /mnt/sandbox/<title id>_000/app0, and
// the UI keeps reading /app0.
//
// Platform-independent: compiled for the PS5 and the host (tests/ps5).
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace prosperoweb::spawn {

struct TitleLayout {
  std::string ui_root;     // the title folder as the UI reads it
  std::string child_root;  // the same folder as the broker and children see it
  std::string how;         // one line for the log: how it was found
};

// ShadowMountPlus's default scan paths (README.md at 30c5a2b7, "Scan
// paths"), each joined with title_id, after override (PROSPEROWEB_TITLE_DIR,
// may be null or empty) when it is set.
std::vector<std::string> title_dir_candidates(const std::string& title_id, const char* override);

// The layout for a title whose sandbox root is app0 ("/app0"). same_title
// (candidate, app0) tells whether a candidate folder holds the running
// title; the first candidate that does is used for both sides. An override
// is trusted without the check.
TitleLayout resolve_title_layout(
    const std::string& app0, const std::string& title_id, const char* override,
    const std::function<bool(const std::string&, const std::string&)>& same_title);

// value with a leading from-root (on a path boundary) replaced by to-root;
// unchanged otherwise or when the roots are equal. "/app0/data/x" with
// "/app0" -> "/data/homebrew/PRWB00001" gives "/data/homebrew/PRWB00001/data/x".
std::string rebase_path(const std::string& value, const std::string& from, const std::string& to);

// Rebases the value of every NAME=VALUE entry (rebase_path), and every
// "file://" URL value, from one root to the other.
std::vector<std::string> rebase_env(const std::vector<std::string>& env, const std::string& from,
                                    const std::string& to);

}  // namespace prosperoweb::spawn
