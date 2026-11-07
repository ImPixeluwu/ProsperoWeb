// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/addons_config.h"

#include <algorithm>

namespace prosperoweb::core {

const std::vector<std::string>& AddonsConfig::all_sponsorblock_categories() {
  // The standard SponsorBlock categories (https://wiki.sponsor.ajay.app).
  static const std::vector<std::string> kCategories = {
      "sponsor",      "intro",   "outro",         "selfpromo",
      "interaction",  "preview", "music_offtopic", "filler"};
  return kCategories;
}

bool AddonsConfig::is_sponsorblock_category(const std::string& c) {
  const auto& all = all_sponsorblock_categories();
  return std::find(all.begin(), all.end(), c) != all.end();
}

AddonsConfig AddonsConfig::from_json(const nlohmann::json& addons) {
  AddonsConfig cfg;
  if (!addons.is_object()) {
    return cfg;
  }

  if (auto it = addons.find("adblock"); it != addons.end() && it->is_object()) {
    const auto& ab = *it;
    if (auto v = ab.find("enabled"); v != ab.end() && v->is_boolean()) {
      cfg.adblock.enabled = v->get<bool>();
    }
    if (auto v = ab.find("cookie_banners"); v != ab.end() && v->is_boolean()) {
      cfg.adblock.cookie_banners = v->get<bool>();
    }
    if (auto v = ab.find("auto_update"); v != ab.end() && v->is_boolean()) {
      cfg.adblock.auto_update = v->get<bool>();
    }
    if (auto v = ab.find("last_updated"); v != ab.end() && v->is_number_integer()) {
      cfg.adblock.last_updated = v->get<std::int64_t>();
    }
  }

  if (auto it = addons.find("sponsorblock"); it != addons.end() && it->is_object()) {
    const auto& sb = *it;
    if (auto v = sb.find("enabled"); v != sb.end() && v->is_boolean()) {
      cfg.sponsorblock.enabled = v->get<bool>();
    }
    if (auto v = sb.find("categories"); v != sb.end() && v->is_array()) {
      std::vector<std::string> cats;
      for (const auto& c : *v) {
        if (c.is_string() && is_sponsorblock_category(c.get<std::string>())) {
          const auto name = c.get<std::string>();
          if (std::find(cats.begin(), cats.end(), name) == cats.end()) {
            cats.push_back(name);
          }
        }
      }
      cfg.sponsorblock.categories = std::move(cats);
    }
  }

  if (auto it = addons.find("popups"); it != addons.end() && it->is_object()) {
    const auto& pp = *it;
    if (auto v = pp.find("enabled"); v != pp.end() && v->is_boolean()) {
      cfg.popups.enabled = v->get<bool>();
    }
    if (auto v = pp.find("allowed_sites"); v != pp.end() && v->is_array()) {
      std::vector<std::string> sites;
      for (const auto& s : *v) {
        if (!s.is_string()) {
          continue;
        }
        // Normalise hand-edited entries ("www.Example.com." -> "example.com").
        std::string site = site_of(s.get<std::string>());
        if (!site.empty() && std::find(sites.begin(), sites.end(), site) == sites.end()) {
          sites.push_back(std::move(site));
        }
      }
      cfg.popups.allowed_sites = std::move(sites);
    }
  }

  return cfg;
}

AddonsConfig AddonsConfig::from_config_root(const nlohmann::json& root) {
  if (root.is_object()) {
    if (auto it = root.find("addons"); it != root.end()) {
      return from_json(*it);
    }
  }
  return AddonsConfig{};
}

nlohmann::json AddonsConfig::to_json() const {
  nlohmann::json out;
  out["adblock"] = {
      {"enabled", adblock.enabled},
      {"cookie_banners", adblock.cookie_banners},
      {"auto_update", adblock.auto_update},
      {"last_updated", adblock.last_updated},
  };
  out["sponsorblock"] = {
      {"enabled", sponsorblock.enabled},
      {"categories", sponsorblock.categories},
  };
  out["popups"] = {
      {"enabled", popups.enabled},
      {"allowed_sites", popups.allowed_sites},
  };
  return out;
}

}  // namespace prosperoweb::core
