// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process environment the WebKit runtime reads (fontconfig, TLS roots,
// language, temp/home directories). The defaults are set in main() before
// the role dispatch, so the UI, WebProcess and NetworkProcess all get them
// even if the spawner's environment copy does not reach a child; the UI
// then applies the config.json language, which the spawner forwards.
#pragma once

#include <string>

namespace prosperoweb::frontend {

// Install and persistence root on the console.
inline constexpr const char* kPs5DataDir = "/data/prosperoweb";

// Install root of the releases named ps5browser; its profile is copied into
// kPs5DataDir once (prosperoweb/core/legacy_profile.h).
inline constexpr const char* kLegacyDataDir = "/data/ps5browser";

// Maps a config.json language ("en", "ru", or already a tag like "de-DE")
// to the BCP 47 tag WebKit expects in SYSTEM_LANGUAGE.
std::string system_language_tag(const std::string& language);

// Sets FONTCONFIG_FILE, FONTCONFIG_PATH, PROSPEROWEB_CA_BUNDLE,
// SYSTEM_LANGUAGE (en-US), TMPDIR and HOME for files under base_dir, unless
// already set, then exports base_dir/config.json "env" (overriding), then
// defaults JSC_useJIT and JSC_useWasm to false, and creates the temp
// directory. Any process role.
void setup_runtime_env(const std::string& base_dir);

// UI role: SYSTEM_LANGUAGE from config.json (overrides the default above).
void apply_language_env(const std::string& language);

}  // namespace prosperoweb::frontend
