// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process environment the WebKit runtime reads (fontconfig, TLS roots,
// language). Set in the UI process before WebKit is initialised; the
// spawner copies the environment into the WebProcess and NetworkProcess.
#pragma once

#include <string>

namespace ps5browser::frontend {

// Maps a config.json language ("en", "ru", or already a tag like "de-DE")
// to the BCP 47 tag WebKit expects in SYSTEM_LANGUAGE.
std::string system_language_tag(const std::string& language);

// Sets FONTCONFIG_FILE, FONTCONFIG_PATH, PS5BROWSER_CA_BUNDLE and
// SYSTEM_LANGUAGE for files under base_dir, unless already set.
void setup_runtime_env(const std::string& base_dir, const std::string& language);

}  // namespace ps5browser::frontend
