// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace prosperoweb::core {

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

// Virtual mouse pointer driven by the left stick; right stick scrolls.
//
// Stick input (x, y in [-1, 1]) is processed as follows:
//  - magnitudes below kDeadZone are ignored;
//  - the remaining range is rescaled to t in [0, 1];
//  - speed = kBaseSpeed * kAccelScale * t^2 px/s, capped at kMaxSpeed, so
//    small deflections give precise slow movement and full deflection
//    reaches the cap.
// The pointer is clamped to [0, width - 1] x [0, height - 1].
class PointerModel {
 public:
  static constexpr float kDeadZone = 0.15f;
  static constexpr float kBaseSpeed = 600.0f;   // px/s
  static constexpr float kAccelScale = 3.0f;
  static constexpr float kMaxSpeed = 1800.0f;   // px/s
  static constexpr float kMaxScrollSpeed = 2400.0f;  // px/s at full tilt

  PointerModel(float viewport_width, float viewport_height);

  // Resizes the viewport and re-clamps the pointer.
  void set_viewport(float width, float height);
  void set_position(Vec2 pos);
  Vec2 position() const { return pos_; }

  // Moves the pointer for one frame; returns the delta actually applied
  // after clamping.
  Vec2 update(float stick_x, float stick_y, float dt_seconds);

  // Scroll delta in px for one frame (positive y scrolls down). Uses the same
  // dead zone and quadratic curve, capped at kMaxScrollSpeed.
  static Vec2 scroll(float stick_x, float stick_y, float dt_seconds);

  // Velocity in px/s for a stick position (exposed for tests and tuning).
  static Vec2 velocity(float stick_x, float stick_y, float max_speed);

 private:
  Vec2 clamp(Vec2 p) const;

  float width_;
  float height_;
  Vec2 pos_;
};

}  // namespace prosperoweb::core
