// SPDX-License-Identifier: GPL-3.0-or-later
//
// D-pad link navigation: with the "D-pad: jump between links" option on, a
// D-pad press moves the pointer to the nearest clickable element in that
// direction instead of scrolling, so Cross clicks it.
//
// The shell runs spatial_targets_script() in the page; the script posts
// {type: 'spatial_targets', dir, targets: [[x, y, width, height], ...]} with
// the visible, unobscured clickable elements in view CSS pixels, and the
// shell picks one with pick_spatial_target().
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "prosperoweb/core/pointer_model.h"  // Vec2

namespace prosperoweb::core {

enum class NavDirection : std::uint8_t { Up, Down, Left, Right };

struct NavRect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

// "up", "down", "left", "right".
std::string_view direction_name(NavDirection dir);
std::optional<NavDirection> direction_from_name(std::string_view name);

// The best target in direction dir from origin, or nullopt when there is
// none. A target must lie beyond origin in that direction (its centre at
// least a few pixels past it) and must not contain origin, so a press always
// leaves the current element. Among those, the lowest score wins:
//   distance along dir to the target's near edge
//   + 4 * distance across dir from origin to the target's span,
// so an element in the same row (or column) beats a closer diagonal one.
// Empty rectangles are ignored.
std::optional<std::size_t> pick_spatial_target(Vec2 origin, NavDirection dir,
                                               std::span<const NavRect> targets);

// The page script that reports the clickable elements for a press in dir:
// links, buttons, form fields, ARIA widgets, [onclick] and focusable
// [tabindex] elements of the top document that are inside the viewport,
// visible, enabled and not covered by another element at their centre
// (clipped to the viewport, at most 400).
std::string spatial_targets_script(NavDirection dir);

}  // namespace prosperoweb::core
