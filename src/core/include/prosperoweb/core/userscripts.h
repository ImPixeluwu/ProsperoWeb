// SPDX-License-Identifier: GPL-3.0-or-later
//
// User scripts in the Greasemonkey / Tampermonkey format: a JavaScript file
// with a "// ==UserScript==" metadata block. The user installs them by
// opening a *.user.js URL; the shell keeps them in a UserscriptStore (saved
// by Session as "userscripts") and hands the enabled ones, wrapped by
// userscript_wrapper(), to the engine as document-start user scripts in
// every frame. Each wrapper checks the page URL itself, so the engine needs
// no per-site logic.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace prosperoweb::core {

enum class RunAt : std::uint8_t { DocumentStart, DocumentEnd, DocumentIdle };

struct UserscriptMeta {
  std::string name;
  std::string namespace_;
  std::string version;
  std::string description;
  std::vector<std::string> matches;          // @match (Chrome match patterns)
  std::vector<std::string> includes;         // @include (globs or /regex/)
  std::vector<std::string> excludes;         // @exclude (globs or /regex/)
  std::vector<std::string> exclude_matches;  // @exclude-match
  std::vector<std::string> grants;           // @grant, as written
  RunAt run_at = RunAt::DocumentEnd;         // @run-at; Greasemonkey's default
  bool noframes = false;                     // @noframes

  // namespace + "/" + name: two scripts with the same id are one script
  // (installing the second updates the first).
  std::string id() const;
};

// Parses the metadata block. nullopt when the source has no
// "// ==UserScript==" ... "// ==/UserScript==" block or no @name.
std::optional<UserscriptMeta> parse_userscript_meta(std::string_view source);

// ECMAScript regex source (case-insensitive use) for a Chrome match pattern
// ("*://*.example.com/*", "https://example.com/path*", "<all_urls>"),
// matched against the URL without its fragment. "" when the pattern is
// invalid. The host part allows any port.
std::string match_pattern_to_regex(std::string_view pattern);
// The same for an @include / @exclude value: "*" is any run of characters
// and the glob must match the whole URL; "/.../" is used as a regex as is.
std::string include_glob_to_regex(std::string_view glob);

// Whether the script runs on url (by its @match / @include / @exclude*
// rules; with neither @match nor @include it runs on every http(s) page).
bool userscript_applies(const UserscriptMeta& meta, std::string_view url);

// The script ready for the engine: the URL check, the GM_* subset (GM_info,
// GM_addStyle, GM_getValue/setValue/deleteValue/listValues on localStorage,
// GM_openInTab, GM_xmlhttpRequest on fetch(), GM_log, unsafeWindow and the
// promise-based GM.* versions) and the @run-at timing around the source.
std::string userscript_wrapper(const UserscriptMeta& meta, std::string_view source);

// The installed user scripts, in install order.
class UserscriptStore {
 public:
  struct Entry {
    std::string source;
    bool enabled = true;
    UserscriptMeta meta;  // parsed from source
  };

  const std::vector<Entry>& scripts() const { return scripts_; }
  std::size_t size() const { return scripts_.size(); }

  // Installs (or updates: same id) a script; returns its index, or nullopt
  // when the source has no valid metadata block. An update keeps the
  // enabled state.
  std::optional<std::size_t> install(std::string source);
  // The index of the installed script with meta's id, if any.
  std::optional<std::size_t> find(const UserscriptMeta& meta) const;
  bool remove(std::size_t index);
  bool set_enabled(std::size_t index, bool enabled);

  // The wrapped sources of the enabled scripts, for the engine.
  std::vector<std::string> injected_sources() const;
  // How many enabled scripts run on url.
  std::size_t count_for_url(std::string_view url) const;

  // [{"source": ..., "enabled": bool}, ...]
  nlohmann::json to_json() const;
  // Entries without a valid metadata block are skipped. False (and nothing
  // changed) when j is not an array.
  bool from_json(const nlohmann::json& j);

 private:
  std::vector<Entry> scripts_;
};

// Whether url points at a user script to offer for installation: an http(s)
// URL whose path ends in ".user.js".
bool is_userscript_url(std::string_view url);

// Page script for a loaded *.user.js URL: fetches the document's own URL
// (falling back to its text) and posts
// {type: 'userscript_source', url, source} to the "prosperoweb" handler.
std::string userscript_source_script();

// Sources longer than this are not installed.
inline constexpr std::size_t kMaxUserscriptBytes = 1024 * 1024;

}  // namespace prosperoweb::core
