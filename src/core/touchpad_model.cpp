// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/touchpad_model.h"

#include <algorithm>
#include <cmath>

namespace prosperoweb::core {

TouchpadModel::Finger* TouchpadModel::find(int id) {
  for (Finger& f : fingers_) {
    if (f.id == id) {
      return &f;
    }
  }
  return nullptr;
}

Vec2 TouchpadModel::centroid() const {
  Vec2 c;
  for (const Finger& f : fingers_) {
    c.x += f.x;
    c.y += f.y;
  }
  const float n = static_cast<float>(std::max<std::size_t>(1, fingers_.size()));
  return {c.x / n, c.y / n};
}

void TouchpadModel::finger_down(int id, float x, float y, std::uint64_t now_ms) {
  if (find(id)) {
    finger_move(id, x, y, now_ms);
    return;
  }
  if (fingers_.empty()) {
    start_t_ = now_ms;
    start_x_ = x;
    start_y_ = y;
    travel_ = 0.0f;
    moving_ = pressed_;
    multi_ = false;
    pressed_during_ = pressed_;
  }
  fingers_.push_back({id, x, y, now_ms});
  if (fingers_.size() >= 2) {
    multi_ = true;
  }
}

void TouchpadModel::finger_move(int id, float x, float y, std::uint64_t now_ms) {
  Finger* f = find(id);
  if (!f) {
    finger_down(id, x, y, now_ms);
    return;
  }
  if (fingers_.size() >= 2) {
    const Vec2 before = centroid();
    *f = {id, x, y, now_ms};
    const Vec2 after = centroid();
    const float sign = natural_scroll_ ? -1.0f : 1.0f;
    out_.scroll.x += sign * (after.x - before.x) * kScrollGain;
    out_.scroll.y += sign * (after.y - before.y) * aspect_ * kScrollGain;
    return;
  }
  if (multi_) {
    *f = {id, x, y, now_ms};  // the rest of a scroll: the pointer stays
    return;
  }
  travel_ = std::max(travel_, std::hypot(x - start_x_, (y - start_y_) * aspect_));
  if (!moving_) {
    if (travel_ < kTapSlop && now_ms - start_t_ <= kTapMaxMs) {
      return;  // maybe a tap: keep the reference point at the start
    }
    // Past the slop: move by everything since the touch began.
    moving_ = true;
    move_pointer(x - start_x_, (y - start_y_) * aspect_, now_ms - start_t_);
  } else {
    move_pointer(x - f->x, (y - f->y) * aspect_, now_ms - f->t);
  }
  *f = {id, x, y, now_ms};
}

void TouchpadModel::move_pointer(float dx, float dy, std::uint64_t dt_ms) {
  const float dt = static_cast<float>(std::max<std::uint64_t>(1, dt_ms)) / 1000.0f;
  const float speed = std::hypot(dx, dy) / dt;
  const float t = std::clamp((speed - kAccelLowSpeed) / (kAccelHighSpeed - kAccelLowSpeed),
                             0.0f, 1.0f);
  const float gain = kPointerGain * (1.0f + (kMaxAcceleration - 1.0f) * t);
  out_.pointer.x += dx * gain;
  out_.pointer.y += dy * gain;
}

void TouchpadModel::finger_up(int id, std::uint64_t now_ms) {
  const auto it = std::find_if(fingers_.begin(), fingers_.end(),
                               [id](const Finger& f) { return f.id == id; });
  if (it == fingers_.end()) {
    return;
  }
  fingers_.erase(it);
  if (!fingers_.empty()) {
    return;
  }
  if (!multi_ && !pressed_during_ && now_ms - start_t_ <= kTapMaxMs && travel_ < kTapSlop) {
    out_.clicks++;
  }
}

void TouchpadModel::update(std::span<const TouchContact> contacts, std::uint64_t now_ms) {
  std::vector<int> lifted;
  for (const Finger& f : fingers_) {
    const bool present = std::any_of(contacts.begin(), contacts.end(),
                                     [&](const TouchContact& c) { return c.id == f.id; });
    if (!present) {
      lifted.push_back(f.id);
    }
  }
  for (int id : lifted) {
    finger_up(id, now_ms);
  }
  for (const TouchContact& c : contacts) {
    const Finger* f = find(c.id);
    if (!f) {
      finger_down(c.id, c.x, c.y, now_ms);
    } else if (f->x != c.x || f->y != c.y) {
      finger_move(c.id, c.x, c.y, now_ms);
    }
  }
}

void TouchpadModel::set_pressed(bool pressed) {
  pressed_ = pressed;
  if (pressed) {
    pressed_during_ = true;
    moving_ = true;
  }
}

void TouchpadModel::reset() {
  fingers_.clear();
  multi_ = false;
  moving_ = false;
  pressed_during_ = false;
  out_ = Output{};
}

TouchpadModel::Output TouchpadModel::take() {
  Output out = out_;
  out_ = Output{};
  return out;
}

}  // namespace prosperoweb::core
