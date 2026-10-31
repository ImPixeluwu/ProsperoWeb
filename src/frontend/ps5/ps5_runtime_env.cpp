// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_runtime_env.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <sys/stat.h>

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

void setup_runtime_env(const std::string& base_dir) {
  const std::string fonts = base_dir + "/fonts";
  const std::string tmp = base_dir + "/tmp";
  set_default("FONTCONFIG_FILE", fonts + "/fonts.conf");
  set_default("FONTCONFIG_PATH", fonts);
  set_default("PS5BROWSER_CA_BUNDLE", base_dir + "/cacert.pem");
  set_default("SYSTEM_LANGUAGE", system_language_tag(""));
  set_default("TMPDIR", tmp);
  set_default("HOME", base_dir);
  if (mkdir(tmp.c_str(), 0755) != 0 && errno != EEXIST) {
    log_printf("mkdir(%s) failed: %s", tmp.c_str(), std::strerror(errno));
  }
}

void apply_language_env(const std::string& language) {
  if (language.empty()) {
    return;
  }
  const std::string tag = system_language_tag(language);
  if (setenv("SYSTEM_LANGUAGE", tag.c_str(), /*overwrite=*/1) != 0) {
    log_printf("setenv(SYSTEM_LANGUAGE) failed");
    return;
  }
  log_printf("env SYSTEM_LANGUAGE=%s (config.json)", tag.c_str());
}

}  // namespace ps5browser::frontend
