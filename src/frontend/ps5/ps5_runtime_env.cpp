// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_runtime_env.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/stat.h>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/legacy_profile.h"
#include "ps5_log.h"

namespace prosperoweb::frontend {

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

// config.json "env": {"NAME": "value", ...}, exported over the defaults
// below. Every role runs this, and the UI's environment is forwarded to
// the children as well, so a value reaches all processes either way.
void apply_config_env(const std::string& base_dir) {
  std::ifstream in(base_dir + "/config.json");
  if (!in) {
    return;
  }
  std::stringstream text;
  text << in.rdbuf();
  const nlohmann::json doc =
      nlohmann::json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) {
    return;
  }
  const auto env = doc.find("env");
  if (env == doc.end() || !env->is_object()) {
    return;
  }
  for (const auto& [key, value] : env->items()) {
    // A config.json carried over from ProsperoBrowser or ps5browser may
    // still use the old names; the new name (or a newer old one) wins.
    std::string name = key;
    if (const std::string renamed = core::renamed_env_name(key); !renamed.empty()) {
      const auto preferred = core::env_names_preferred_over(key);
      const auto winner = std::find_if(preferred.begin(), preferred.end(), [&](const auto& other) {
        return env->contains(other);
      });
      if (winner != preferred.end()) {
        log_printf("env %s: deprecated name in config.json, ignored (%s is set)", key.c_str(),
                   winner->c_str());
        continue;
      }
      log_printf("env %s: deprecated name in config.json, use %s", key.c_str(), renamed.c_str());
      name = renamed;
    }
    std::string text_value;
    if (value.is_string()) {
      text_value = value.get<std::string>();
    } else if (value.is_boolean()) {
      text_value = value.get<bool>() ? "true" : "false";
    } else if (value.is_number()) {
      text_value = value.dump();
    } else {
      log_printf("env %s: config.json value is not a string, number or bool; ignored",
                 name.c_str());
      continue;
    }
    if (name.empty() || name.find('=') != std::string::npos ||
        setenv(name.c_str(), text_value.c_str(), /*overwrite=*/1) != 0) {
      log_printf("env %s: cannot set from config.json", name.c_str());
      continue;
    }
    log_printf("env %s=%s (config.json)", name.c_str(), text_value.c_str());
  }
}

}  // namespace

std::string config_env_value(const std::string& base_dir, const std::string& name) {
  std::ifstream in(base_dir + "/config.json");
  if (!in) {
    return "";
  }
  std::stringstream text;
  text << in.rdbuf();
  const nlohmann::json doc =
      nlohmann::json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) {
    return "";
  }
  const auto env = doc.find("env");
  if (env == doc.end() || !env->is_object()) {
    return "";
  }
  const auto value = env->find(name);
  if (value == env->end()) {
    return "";
  }
  if (value->is_string()) {
    return value->get<std::string>();
  }
  if (value->is_boolean()) {
    return value->get<bool>() ? "true" : "false";
  }
  return value->is_number() ? value->dump() : "";
}

std::string system_language_tag(const std::string& language) {
  if (language.empty() || language == "en") {
    return "en-US";
  }
  if (language == "ru") {
    return "ru-RU";
  }
  return language;
}

namespace {
std::string& assets_dir_slot() {
  static auto* dir = new std::string(kPs5DataDir);  // never destroyed
  return *dir;
}
}  // namespace

const std::string& runtime_assets_dir() { return assets_dir_slot(); }

void setup_runtime_env(const std::string& base_dir, const std::string& assets_dir) {
  assets_dir_slot() = assets_dir;
  const std::string fonts = assets_dir + "/fonts";
  const std::string tmp = base_dir + "/tmp";
  if (mkdir(base_dir.c_str(), 0755) != 0 && errno != EEXIST) {
    log_printf("mkdir(%s) failed: %s", base_dir.c_str(), std::strerror(errno));
  }
  set_default("FONTCONFIG_FILE", fonts + "/fonts.conf");
  set_default("FONTCONFIG_PATH", fonts);
  set_default("PROSPEROWEB_CA_BUNDLE", assets_dir + "/cacert.pem");
  set_default("SYSTEM_LANGUAGE", system_language_tag(""));
  set_default("TMPDIR", tmp);
  set_default("HOME", base_dir);
  // The UI's audio server (ps5_audio_server.h); the WebProcess inherits it
  // and forwards media audio there.
  set_default("PROSPEROWEB_AUDIO_PORT", "17772");
  // The UI's video overlay server (ps5_video_overlay.h); WebProcess media
  // players send their frames there instead of painting them into the page.
  set_default("PROSPEROWEB_VIDEO_PORT", "17773");
  apply_config_env(base_dir);
  // JavaScriptCore is built with the baseline/DFG JIT and WebAssembly; both
  // are on by default (verified on a console: about 1.7x faster than the
  // interpreter, no crashes while browsing). config.json "env" or the
  // environment can still turn them off ("false"). JSC reads JSC_<option>
  // from the environment when it initialises its options.
  set_default("JSC_useJIT", "true");
  set_default("JSC_useWasm", "true");
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

}  // namespace prosperoweb::frontend
