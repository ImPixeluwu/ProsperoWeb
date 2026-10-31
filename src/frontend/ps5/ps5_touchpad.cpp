// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_touchpad.h"

#include <algorithm>
#include <cstring>

#include "ps5_log.h"

// libScePad (on SDL2's link line). The state layout is the one SDL's PS5
// driver passes to the same call (SDL src/joystick/ps5/SDL_ps5joystick.h,
// PS5_PadData / PS5_PadTouchData).
extern "C" {
int scePadGetHandle(int user_id, int type, int index);
int scePadReadState(int handle, void* data);
int scePadGetControllerInformation(int handle, void* info);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
}

namespace prosperobrowser::frontend {

namespace {

constexpr int kPadPortStandard = 0;  // PS5_PAD_PORT_TYPE_STANDARD
constexpr std::uint64_t kResolveIntervalMs = 1000;

struct PadTouch {
  std::uint16_t x;
  std::uint16_t y;
  std::uint8_t finger;
  std::uint8_t pad[3];
};

struct PadTouchData {
  std::uint8_t fingers;
  std::uint8_t pad1[3];
  std::uint32_t pad2;
  PadTouch touch[2];
};

struct PadData {
  std::uint32_t buttons;
  std::uint8_t sticks[4];
  std::uint8_t analog_buttons[2];
  std::uint16_t padding;
  float quat[4];
  float vel[3];
  float accel[3];
  PadTouchData touch;
  std::uint8_t connected;
  std::uint64_t timestamp;
  std::uint8_t ext[16];
  std::uint8_t count;
  std::uint8_t unknown[15];
};

// Room past the known layout in case the library writes more.
union PadState {
  PadData data;
  std::uint8_t bytes[512];
};

}  // namespace

bool Ps5Touchpad::resolve_handle(std::uint64_t now_ms) {
  if (handle_ >= 0) {
    return true;
  }
  if (now_ms < next_resolve_ms_) {
    return false;
  }
  next_resolve_ms_ = now_ms + kResolveIntervalMs;
  int users[4] = {-1, -1, -1, -1};
  if (sceUserServiceGetLoginUserIdList(users) != 0) {
    return false;
  }
  for (int user : users) {
    if (user == -1) {
      continue;
    }
    // SDL opened the pad with scePadOpen(user, standard, 0); this finds it.
    const int handle = scePadGetHandle(user, kPadPortStandard, 0);
    if (handle >= 0) {
      handle_ = handle;
      read_resolution();
      return true;
    }
  }
  return false;
}

void Ps5Touchpad::read_resolution() {
  if (resolution_read_) {
    return;
  }
  resolution_read_ = true;
  // ScePadControllerInformation starts with ScePadTouchPadInformation
  // {float pixelDensity; uint16 resolution x, y}; trusted only when sane.
  alignas(8) std::uint8_t info[256] = {};
  if (scePadGetControllerInformation(handle_, info) != 0) {
    return;
  }
  std::uint16_t w = 0;
  std::uint16_t h = 0;
  std::memcpy(&w, info + 4, sizeof w);
  std::memcpy(&h, info + 6, sizeof h);
  if (w >= 500 && w <= 4096 && h >= 200 && h <= 4096) {
    width_ = w;
    height_ = h;
    resolution_from_info_ = true;
  }
}

void Ps5Touchpad::read(SDL_GameController* pad, std::uint64_t now_ms,
                       std::vector<core::TouchContact>& contacts) {
  contacts.clear();
  if (!pad) {
    return;
  }
  if (SDL_GameControllerGetNumTouchpads(pad) > 0) {
    if (!source_logged_) {
      source_logged_ = true;
      log_printf("touchpad: source sdl, resolution %dx%d (default, SDL reports 0..1)", width_,
                 height_);
    }
    const int n = SDL_GameControllerGetNumTouchpadFingers(pad, 0);
    for (int i = 0; i < n; i++) {
      Uint8 state = 0;
      float x = 0.0f;
      float y = 0.0f;
      float pressure = 0.0f;
      if (SDL_GameControllerGetTouchpadFinger(pad, 0, i, &state, &x, &y, &pressure) == 0 &&
          state) {
        contacts.push_back({i, x, y});
      }
    }
    return;
  }

  if (!resolve_handle(now_ms)) {
    return;
  }
  PadState state{};
  if (scePadReadState(handle_, &state) != 0) {
    handle_ = -1;  // gone or reopened: look it up again
    return;
  }
  if (!source_logged_) {
    source_logged_ = true;
    log_printf("touchpad: source scePad, resolution %dx%d (%s)", width_, height_,
               resolution_from_info_ ? "controller info" : "default");
  }
  const PadTouchData& touch = state.data.touch;
  const int fingers = std::min<int>(touch.fingers, 2);
  for (int i = 0; i < fingers; i++) {
    const PadTouch& t = touch.touch[i];
    // A coordinate past the assumed resolution means it was wrong: grow it.
    if (t.x >= width_ || t.y >= height_) {
      width_ = std::max<int>(width_, t.x + 1);
      height_ = std::max<int>(height_, t.y + 1);
      if (!grow_logged_) {
        grow_logged_ = true;
        log_printf("touchpad: saw %u,%u, resolution now %dx%d", t.x, t.y, width_, height_);
      }
    }
    contacts.push_back({t.finger, static_cast<float>(t.x) / static_cast<float>(width_),
                        static_cast<float>(t.y) / static_cast<float>(height_)});
  }
}

}  // namespace prosperobrowser::frontend
