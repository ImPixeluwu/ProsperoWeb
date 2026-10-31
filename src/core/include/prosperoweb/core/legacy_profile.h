// SPDX-License-Identifier: GPL-3.0-or-later
//
// Carry-over from the project's former names, newest first: ProsperoBrowser
// (/data/prosperobrowser, PROSPEROBROWSER_*) and ps5browser
// (/data/ps5browser, PS5BROWSER_*). Kept for one release after the rename.
#pragma once

#include <string>
#include <vector>

namespace prosperoweb::core {

inline constexpr const char* kEnvPrefix = "PROSPEROWEB_";

// Environment prefixes of the earlier names, newest first.
std::vector<std::string> legacy_env_prefixes();

// "PROSPEROBROWSER_X" or "PS5BROWSER_X" -> "PROSPEROWEB_X"; "" for any
// other name (including a bare prefix).
std::string renamed_env_name(const std::string& name);

// The names that set the same variable as the legacy `name` and win over
// it: the new name, then the names with a newer legacy prefix. Empty for a
// name that is not legacy.
std::vector<std::string> env_names_preferred_over(const std::string& name);

// For every legacy variable in the process environment whose new name is
// not set, exports the new name with the same value; when several legacy
// names map to one variable, the newest prefix wins. Returns the legacy
// names that were adopted, sorted. POSIX (environ, setenv).
std::vector<std::string> adopt_legacy_env();

// Profile entries (files or directories) carried over from an old install
// directory.
std::vector<std::string> legacy_profile_items();

// Name of the marker file written to the new profile directory once the
// carry-over has run, so it never runs again.
inline constexpr const char* kLegacyMigrationMarker = ".migrated-legacy-profile";

struct LegacyMigration {
  bool ran = false;                  // no marker, and a source had a profile
  std::string from_dir;              // that source
  int copied = 0;                    // top-level items copied
  std::vector<std::string> failed;   // "<item>: <reason>"
};

// Takes the first of from_dirs (newest first) that holds at least one
// legacy_profile_items() entry and copies (never moves) each entry that is
// missing in to_dir, directories recursively (regular files and directories
// only), then writes the marker into to_dir. Older sources are not mixed in.
// Does nothing if no source has a profile or the marker is already there.
// In the copied config.json, any of from_dirs at the start of a JSON string
// or after file:// is replaced with to_dir (chrome_dir from
// config.json.sample). to_dir is created if missing. A file is written under
// a temporary name and renamed into place; a directory is copied in place.
// The marker is only written when every item copied, so a failed file is
// retried on the next start (a partially copied directory then counts as
// present and is kept as is).
LegacyMigration migrate_legacy_profile(const std::vector<std::string>& from_dirs,
                                       const std::string& to_dir);

}  // namespace prosperoweb::core
