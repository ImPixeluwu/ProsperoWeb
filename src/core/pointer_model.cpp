// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/pointer_model.h"

#include <algorithm>
#include <cmath>

namespace prosperoweb::core {

PointerModel::PointerModel(float viewport_width, float viewport_height)
    : width_(std::max(1.0f, viewport_width)),
      height_(std::max(1.0f, viewport_height)),
      pos_{width_ / 2.0f, height_ / 2.0f} {}

void PointerModel::set_viewport(float width, float height) {
  width_ = std::max(1.0f, width);
  height_ = std::max(1.0f, height);
  pos_ = clamp(pos_);
}

void PointerModel::set_position(Vec2 pos) { pos_ = clamp(pos); }

Vec2 PointerModel::clamp(Vec2 p) const {
  return {std::clamp(p.x, 0.0f, width_ - 1.0f), std::clamp(p.y, 0.0f, height_ - 1.0f)};
}

Vec2 PointerModel::velocity(float stick_x, float stick_y, float max_speed) {
  const float mag = std::sqrt(stick_x * stick_x + stick_y * stick_y);
  if (!(mag > kDeadZone)) {  // also rejects NaN
    return {};
  }
  const float clamped = std::min(mag, 1.0f);
  const float t = (clamped - kDeadZone) / (1.0f - kDeadZone);
  const float speed = std::min(kBaseSpeed * kAccelScale * t * t, max_speed);
  return {stick_x / mag * speed, stick_y / mag * speed};
}

Vec2 PointerModel::update(float stick_x, float stick_y, float dt_seconds) {
  if (!(dt_seconds > 0.0f)) {
    return {};
  }
  const Vec2 v = velocity(stick_x, stick_y, kMaxSpeed);
  const Vec2 before = pos_;
  pos_ = clamp({pos_.x + v.x * dt_seconds, pos_.y + v.y * dt_seconds});
  return {pos_.x - before.x, pos_.y - before.y};
}

Vec2 PointerModel::scroll(float stick_x, float stick_y, float dt_seconds) {
  if (!(dt_seconds > 0.0f)) {
    return {};
  }
  // Same curve as the pointer, rescaled so full tilt hits kMaxScrollSpeed.
  const Vec2 v = velocity(stick_x, stick_y, kMaxSpeed);
  const float k = kMaxScrollSpeed / kMaxSpeed;
  return {v.x * k * dt_seconds, v.y * k * dt_seconds};
}

}  // namespace prosperoweb::core
