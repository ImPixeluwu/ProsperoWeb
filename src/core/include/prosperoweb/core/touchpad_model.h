// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "prosperoweb/core/pointer_model.h"

namespace prosperoweb::core {

// One finger on the touchpad. x and y are normalized to [0, 1] across the
// pad (0, 0 = top-left); id stays the same while the finger stays down.
struct TouchContact {
  int id = 0;
  float x = 0.0f;
  float y = 0.0f;
};

// The DualSense touchpad as a laptop-style trackpad (pure, no I/O):
//  - one finger moves the pointer relatively, with acceleration: the gain is
//    kPointerGain px per pad width below kAccelLowSpeed and rises linearly to
//    kMaxAcceleration times that at kAccelHighSpeed (pad widths per second);
//    motion starts once the finger travelled kTapSlop, or after kTapMaxMs,
//    so a tap does not nudge the pointer;
//  - a tap (one finger, up within kTapMaxMs, travel < kTapSlop, no press of
//    the pad's button meanwhile) is a click;
//  - two fingers scroll (kScrollGain px per pad width). By default the
//    direction is the classic mouse-wheel one: moving the fingers down
//    scrolls the page down. set_natural_scroll(true) makes the content
//    follow the fingers instead. The pointer stays put until every finger
//    is lifted;
//  - while the pad's button is held (set_pressed), motion has no slop, so a
//    press-and-drag drags.
// Distances along y are scaled by the pad's aspect ratio (height / width),
// so equal finger travel moves the pointer equally in both directions.
class TouchpadModel {
 public:
  // false (default): fingers down scroll the page down; true: the content
  // follows the fingers (touch-screen style).
  void set_natural_scroll(bool natural) { natural_scroll_ = natural; }
  bool natural_scroll() const { return natural_scroll_; }

  static constexpr float kPointerGain = 900.0f;      // px per pad width
  static constexpr float kMaxAcceleration = 3.0f;
  static constexpr float kAccelLowSpeed = 0.25f;     // pad widths per second
  static constexpr float kAccelHighSpeed = 2.5f;     // pad widths per second
  static constexpr float kScrollGain = 1600.0f;      // px per pad width
  static constexpr std::uint64_t kTapMaxMs = 180;
  static constexpr float kTapSlop = 0.02f;           // pad widths
  static constexpr float kDefaultAspect = 1080.0f / 1920.0f;  // DualSense

  struct Output {
    Vec2 pointer;  // px
    Vec2 scroll;   // px, positive y scrolls the content down
    int clicks = 0;
    bool empty() const {
      return pointer.x == 0.0f && pointer.y == 0.0f && scroll.x == 0.0f && scroll.y == 0.0f &&
             clicks == 0;
    }
  };

  explicit TouchpadModel(float aspect = kDefaultAspect) : aspect_(aspect) {}

  // Pad height / width (e.g. its resolution 1920 x 1080 -> 0.5625).
  void set_aspect(float aspect) { aspect_ = aspect; }
  float aspect() const { return aspect_; }

  // Finger events. A move or up of an unknown id is a down / ignored.
  void finger_down(int id, float x, float y, std::uint64_t now_ms);
  void finger_move(int id, float x, float y, std::uint64_t now_ms);
  void finger_up(int id, std::uint64_t now_ms);

  // Every finger now on the pad (what the frontend reads each frame), turned
  // into the events above: lifted fingers first, then moves and new fingers.
  void update(std::span<const TouchContact> contacts, std::uint64_t now_ms);

  // The pad's physical button. A touch during which it was pressed is not a
  // tap (the press is the click); while held, motion has no slop.
  void set_pressed(bool pressed);

  // Drops every finger without a tap (controller gone, focus changed).
  void reset();

  // What the gestures produced since the last take().
  Output take();

  std::size_t fingers() const { return fingers_.size(); }
  // True while the current touch is (or was) a two-finger gesture.
  bool multi_finger() const { return multi_; }

 private:
  struct Finger {
    int id = 0;
    float x = 0.0f;
    float y = 0.0f;
    std::uint64_t t = 0;
  };

  Finger* find(int id);
  Vec2 centroid() const;
  void move_pointer(float dx, float dy, std::uint64_t dt_ms);

  float aspect_;
  std::vector<Finger> fingers_;
  bool pressed_ = false;

  // The current touch: from the first finger down to the last finger up.
  std::uint64_t start_t_ = 0;
  float start_x_ = 0.0f;
  float start_y_ = 0.0f;
  float travel_ = 0.0f;    // the farthest the first finger got from its start
  bool moving_ = false;    // past the slop: motion moves the pointer
  bool multi_ = false;     // two or more fingers were down
  bool pressed_during_ = false;

  Output out_;
  bool natural_scroll_ = false;
};

}  // namespace prosperoweb::core
