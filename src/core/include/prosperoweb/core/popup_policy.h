// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prosperoweb::core {

// What made the page ask for a new window, as reported by the engine.
enum class PopupOrigin : std::uint8_t {
  Link,    // a click on <a target="_blank"> (or a link with a modifier)
  Form,    // a form submitted into a new window
  Script,  // window.open() and anything the engine reports as "other"
};

struct PopupRequest {
  std::string opener_url;  // URL of the page that asked for the window
  std::string target_url;  // URL of the new window; may be empty or about:blank
  bool user_gesture = false;  // the request came from a real click or key press
  PopupOrigin origin = PopupOrigin::Script;
};

struct PopupBlockerConfig {
  bool enabled = true;                     // on by default
  std::vector<std::string> allowed_sites;  // sites (see site_of) that may always open windows
};

enum class PopupDecision : std::uint8_t { Allow, Block };

// Decides whether a page may open a new window (the shell turns an allowed
// window into a tab). The rules, in order:
//   1. blocker off, or the opener's site is in allowed_sites: allow;
//   2. no user gesture: block (timers, onload, redirect chains);
//   3. a link or a form the user activated: allow (target="_blank" links);
//   4. window.open() from a user gesture: allow only when the target is on
//      the opener's site. A click that opens another site through script is
//      the usual pop-under, so it is blocked, and so is an empty or
//      about:blank target (the shell opens tabs without an opener, so a
//      script could not fill it in anyway).
PopupDecision decide_popup(const PopupBlockerConfig& config, const PopupRequest& request);

// The site of a host: its registrable domain, approximated without the
// Public Suffix List. Lower-cased, without a trailing dot. "www.example.com"
// and "m.example.com" give "example.com"; two-letter country domains with a
// common second level keep three labels ("news.bbc.co.uk" gives "bbc.co.uk").
// IP literals and single-label hosts are returned as they are.
std::string site_of(std::string_view host);

// site_of() of the URL's host, or "" for URLs without one (about:, data:).
std::string site_of_url(std::string_view url);

// Adds the URL's site to allowed_sites. Returns false if the URL has no site
// or the site is already allowed.
bool allow_popups_for(PopupBlockerConfig& config, std::string_view url);

}  // namespace prosperoweb::core
