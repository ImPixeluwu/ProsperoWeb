// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_config.h"

#include <nlohmann/json.hpp>

#include "prosperoweb/core/storage.h"

namespace prosperoweb::frontend {

Ps5Config Ps5Config::load(core::Storage& storage) {
  Ps5Config cfg;
  auto raw = storage.read("config");
  if (!raw) {
    return cfg;
  }

  nlohmann::json doc = nlohmann::json::parse(*raw, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) {
    return cfg;
  }

  if (auto it = doc.find("home_url"); it != doc.end() && it->is_string()) {
    cfg.home_url = it->get<std::string>();
  }
  if (auto it = doc.find("user_agent"); it != doc.end() && it->is_string()) {
    cfg.user_agent = it->get<std::string>();
  }
  if (auto it = doc.find("language"); it != doc.end() && it->is_string()) {
    cfg.language = it->get<std::string>();
  }
  if (auto it = doc.find("chrome_dir"); it != doc.end() && it->is_string()) {
    cfg.chrome_dir = it->get<std::string>();
  }
  if (auto it = doc.find("osk"); it != doc.end() && it->is_string()) {
    cfg.osk = it->get<std::string>();
  }
  if (auto it = doc.find("width"); it != doc.end() && it->is_number_integer()) {
    cfg.width = it->get<int>();
  }
  if (auto it = doc.find("height"); it != doc.end() && it->is_number_integer()) {
    cfg.height = it->get<int>();
  }

  return cfg;
}

}  // namespace prosperoweb::frontend
