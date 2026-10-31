// SPDX-License-Identifier: GPL-3.0-or-later
//
// Carry-over from the project's former name (ps5browser): the console
// profile in /data/ps5browser and PS5BROWSER_* environment variables.
// Kept for one release after the rename.
#pragma once

#include <string>
#include <vector>

namespace prosperoweb::core {

inline constexpr const char* kLegacyEnvPrefix = "PS5BROWSER_";
inline constexpr const char* kEnvPrefix = "PROSPEROWEB_";

// "PS5BROWSER_X" -> "PROSPEROWEB_X"; "" for any other name (including
// the bare prefix).
std::string renamed_env_name(const std::string& name);

// For every PS5BROWSER_X in the process environment whose PROSPEROWEB_X
// is not set, exports PROSPEROWEB_X with the same value. Returns the
// legacy names that were adopted, sorted. POSIX (environ, setenv).
std::vector<std::string> adopt_legacy_env();

// Profile entries (files or directories) carried over from the old
// install directory.
std::vector<std::string> legacy_profile_items();

// Name of the marker file written to the new profile directory once the
// carry-over has run, so it never runs again.
inline constexpr const char* kLegacyMigrationMarker = ".migrated-from-ps5browser";

struct LegacyMigration {
  bool ran = false;                  // from_dir existed and no marker was found
  int copied = 0;                    // top-level items copied
  std::vector<std::string> failed;   // "<item>: <reason>"
};

// Copies (never moves) each legacy_profile_items() entry that exists in
// from_dir and is missing in to_dir, directories recursively (regular files
// and directories only), then writes the marker into to_dir. Does nothing
// if from_dir does not exist or the marker is already there. In the copied
// config.json, from_dir at the start of a JSON string or after file:// is
// replaced with to_dir (chrome_dir from config.json.sample). to_dir is
// created if missing. A file is written under a temporary name and renamed
// into place; a directory is copied in place. The marker is only written
// when every item copied, so a failed file is retried on the next start (a
// partially copied directory then counts as present and is kept as is).
LegacyMigration migrate_legacy_profile(const std::string& from_dir, const std::string& to_dir);

}  // namespace prosperoweb::core
