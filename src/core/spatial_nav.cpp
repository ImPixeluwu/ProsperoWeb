// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/spatial_nav.h"

#include <algorithm>
#include <limits>

namespace prosperoweb::core {

namespace {

// A target's centre must be at least this far past the origin.
constexpr float kMinAdvance = 4.0f;
// Weight of the cross-axis offset in the score.
constexpr float kCrossWeight = 4.0f;

constexpr const char* kScriptBody = R"JS((function (dir) {
  'use strict';
  var SELECTOR = 'a[href],button,input:not([type=hidden]),select,textarea,summary,' +
      '[role=button],[role=link],[role=tab],[role=menuitem],[role=option],' +
      '[role=checkbox],[role=radio],[role=switch],[onclick],' +
      '[tabindex]:not([tabindex="-1"]),[contenteditable=""],[contenteditable="true"]';
  var vw = window.innerWidth, vh = window.innerHeight, out = [];
  var els = document.querySelectorAll(SELECTOR);
  for (var i = 0; i < els.length && out.length < 400; i++) {
    var e = els[i];
    if (e.disabled) continue;
    var r = e.getBoundingClientRect();
    if (r.width < 4 || r.height < 4) continue;
    if (r.right <= 0 || r.bottom <= 0 || r.left >= vw || r.top >= vh) continue;
    var s = window.getComputedStyle(e);
    if (s.visibility === 'hidden' || s.display === 'none' || Number(s.opacity) === 0) continue;
    var x = Math.max(0, r.left), y = Math.max(0, r.top);
    var w = Math.min(vw, r.right) - x, h = Math.min(vh, r.bottom) - y;
    var top = document.elementFromPoint(x + w / 2, y + h / 2);
    if (!top || (top !== e && !e.contains(top) && !top.contains(e))) continue;
    out.push([Math.round(x), Math.round(y), Math.round(w), Math.round(h)]);
  }
  try {
    window.webkit.messageHandlers.prosperoweb.postMessage(
        { type: 'spatial_targets', dir: dir, targets: out });
  } catch (err) {
  }
})()JS";

// Distance from v to the interval [lo, hi]; 0 inside it.
float distance_to_span(float v, float lo, float hi) {
  if (v < lo) return lo - v;
  if (v > hi) return v - hi;
  return 0.0f;
}

}  // namespace

std::string_view direction_name(NavDirection dir) {
  switch (dir) {
    case NavDirection::Up: return "up";
    case NavDirection::Down: return "down";
    case NavDirection::Left: return "left";
    case NavDirection::Right: return "right";
  }
  return "down";
}

std::optional<NavDirection> direction_from_name(std::string_view name) {
  if (name == "up") return NavDirection::Up;
  if (name == "down") return NavDirection::Down;
  if (name == "left") return NavDirection::Left;
  if (name == "right") return NavDirection::Right;
  return std::nullopt;
}

std::optional<std::size_t> pick_spatial_target(Vec2 origin, NavDirection dir,
                                               std::span<const NavRect> targets) {
  std::optional<std::size_t> best;
  float best_score = std::numeric_limits<float>::max();
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const NavRect& r = targets[i];
    if (r.width <= 0.0f || r.height <= 0.0f) {
      continue;
    }
    const float left = r.x, right = r.x + r.width, top = r.y, bottom = r.y + r.height;
    if (origin.x >= left && origin.x <= right && origin.y >= top && origin.y <= bottom) {
      continue;  // the element under the pointer: move away from it
    }
    const float cx = r.x + r.width / 2.0f;
    const float cy = r.y + r.height / 2.0f;
    float along = 0.0f;
    float across = 0.0f;
    switch (dir) {
      case NavDirection::Right:
        if (cx < origin.x + kMinAdvance) continue;
        along = std::max(0.0f, left - origin.x);
        across = distance_to_span(origin.y, top, bottom);
        break;
      case NavDirection::Left:
        if (cx > origin.x - kMinAdvance) continue;
        along = std::max(0.0f, origin.x - right);
        across = distance_to_span(origin.y, top, bottom);
        break;
      case NavDirection::Down:
        if (cy < origin.y + kMinAdvance) continue;
        along = std::max(0.0f, top - origin.y);
        across = distance_to_span(origin.x, left, right);
        break;
      case NavDirection::Up:
        if (cy > origin.y - kMinAdvance) continue;
        along = std::max(0.0f, origin.y - bottom);
        across = distance_to_span(origin.x, left, right);
        break;
    }
    const float score = along + kCrossWeight * across;
    if (score < best_score) {
      best_score = score;
      best = i;
    }
  }
  return best;
}

std::string spatial_targets_script(NavDirection dir) {
  // The body ends with "})(" so the direction is the only argument.
  return std::string(kScriptBody) + "'" + std::string(direction_name(dir)) + "');";
}

}  // namespace prosperoweb::core
