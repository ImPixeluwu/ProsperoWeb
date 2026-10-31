// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_runtime_env.h"

#include <cstdlib>

#include "ps5_log.h"

namespace ps5browser::frontend {

namespace {

// Keeps a value the user (or a parent loader) already exported.
void set_default(const char* name, const std::string& value) {
  if (setenv(name, value.c_str(), /*overwrite=*/0) != 0) {
    log_printf("setenv(%s) failed", name);
    return;
  }
  const char* effective = std::getenv(name);
  log_printf("env %s=%s", name, effective ? effective : "(null)");
}

}  // namespace

std::string system_language_tag(const std::string& language) {
  if (language.empty() || language == "en") {
    return "en-US";
  }
  if (language == "ru") {
    return "ru-RU";
  }
  return language;
}

void setup_runtime_env(const std::string& base_dir, const std::string& language) {
  const std::string fonts = base_dir + "/fonts";
  set_default("FONTCONFIG_FILE", fonts + "/fonts.conf");
  set_default("FONTCONFIG_PATH", fonts);
  set_default("PS5BROWSER_CA_BUNDLE", base_dir + "/cacert.pem");
  set_default("SYSTEM_LANGUAGE", system_language_tag(language));
}

}  // namespace ps5browser::frontend
