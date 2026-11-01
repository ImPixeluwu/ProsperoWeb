// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decision of whether the ad-block filter lists are stale enough to
// refresh over the network at startup / on the daily cadence. Pure so the
// host tests can pin the policy; the on-device updater (ps5_addons.cpp)
// calls it.
#pragma once

#include <cstdint>

namespace prosperoweb::core {

// Default staleness threshold: 24 hours.
inline constexpr std::int64_t kFilterRefreshThresholdSeconds = 24 * 60 * 60;

// True when a background refresh should run now:
//   - the ad blocker is enabled, and
//   - auto-update is enabled, and
//   - the cached lists have never been updated (last_updated <= 0) or are
//     older than `threshold` seconds relative to `now`.
// A last_updated in the future (clock skew) is treated as fresh.
inline bool should_refresh_filters(bool adblock_enabled, bool auto_update,
                                   std::int64_t last_updated, std::int64_t now,
                                   std::int64_t threshold = kFilterRefreshThresholdSeconds) {
  if (!adblock_enabled || !auto_update) {
    return false;
  }
  if (last_updated <= 0) {
    return true;
  }
  if (last_updated > now) {
    return false;
  }
  return (now - last_updated) >= threshold;
}

}  // namespace prosperoweb::core
