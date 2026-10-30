// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <nlohmann/json.hpp>

namespace ps5browser::shell {

// The browser chrome (tab strip, address bar, overlays). The frontend renders
// it as HTML (src/chrome-ui/chrome.html, window.ps5chrome.render(state)) and
// forwards events from it to ShellController::on_chrome_event().
//
// The state document is the single source of truth for the chrome:
//   {
//     "tabs": [{"id", "title", "url", "active", "loading"}],
//     "address": {"text", "editing"},
//     "nav": {"back", "fwd", "loading"},
//     "chrome_visible": bool,
//     "fullscreen_state": "normal" | "chrome_hidden" | "element",
//     "content": {"x", "y", "width", "height"},   // where the page view goes
//     "pointer": {"x", "y", "visible"},           // window coordinates
//     "osk": {"open", "layout", "shift", "text", "target": "address" | "page",
//             "cursor": {"row", "col"},
//             "rows": [[{"label", "code", "width"}]]},
//     "notice": {"kind": "drm" | "error" | "info" | null, "title", "text"},
//     "menu": {"open", "section": "main" | "bookmarks" | "history",
//              "items": [{"label", "url", "selected"}], "selected"},
//     "focus": "page" | "chrome" | "osk" | "menu" | "notice"
//   }
class ChromeUi {
 public:
  virtual ~ChromeUi() = default;
  virtual void render(const nlohmann::json& state) = 0;
  // Gamepad Cross while the pointer is over the chrome band: activate the
  // chrome element under (x, y) in window coordinates. The chrome is our own
  // UI, so a synthesized click is fine here (page input never goes this way).
  virtual void click(int /*x*/, int /*y*/) {}
};

}  // namespace ps5browser::shell
