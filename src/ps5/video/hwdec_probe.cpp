// SPDX-License-Identifier: GPL-3.0-or-later
#include "hwdec_probe.h"

#include <cstdio>
#include <thread>
#include <utility>

namespace prosperoweb::video {

const char* to_string(HwdecProbe::State state) {
  switch (state) {
    case HwdecProbe::State::NotStarted:
      return "not started";
    case HwdecProbe::State::Probing:
      return "probing";
    case HwdecProbe::State::Ready:
      return "ready";
    case HwdecProbe::State::Unavailable:
      return "unavailable";
    case HwdecProbe::State::Stuck:
      return "stuck";
  }
  return "?";
}

std::shared_ptr<HwdecProbe> HwdecProbe::create(std::chrono::milliseconds call_limit, Log log) {
  return std::shared_ptr<HwdecProbe>(new HwdecProbe(call_limit, std::move(log)));
}

HwdecProbe::HwdecProbe(std::chrono::milliseconds call_limit, Log log)
    : limit_(call_limit), log_(std::move(log)) {}

std::string HwdecProbe::limit_text() const {
  const long long ms = limit_.count();
  char text[32];
  if (ms >= 1000 && ms % 1000 == 0) {
    std::snprintf(text, sizeof(text), "%lld s", ms / 1000);
  } else {
    std::snprintf(text, sizeof(text), "%lld ms", ms);
  }
  return text;
}

bool HwdecProbe::start(Body body) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (state_ != State::NotStarted) {
      return false;
    }
    state_ = State::Probing;
  }
  // Both threads hold the probe: a stuck one may outlive everything else
  // (it is never joined, so the process can still exit).
  auto self = shared_from_this();
  std::thread([self, body = std::move(body)] { self->run_body(body); }).detach();
  std::thread([self] { self->watch(); }).detach();
  return true;
}

void HwdecProbe::run_body(const Body& body) {
  const bool ok = body(*this);
  finish(ok ? State::Ready : State::Unavailable, std::string());
}

void HwdecProbe::fail(const std::string& reason) {
  std::lock_guard<std::mutex> lock(lock_);
  if (reason_.empty()) {
    reason_ = reason;
  }
}

void HwdecProbe::finish(State state, const std::string& reason) {
  std::lock_guard<std::mutex> lock(lock_);
  if (state_ != State::Probing) {
    return;  // given up already: Stuck stays
  }
  state_ = state;
  if (state == State::Ready) {
    reason_.clear();
  } else if (reason_.empty()) {
    reason_ = reason.empty() ? "hardware decoder unavailable" : reason;
  }
  log_(state == State::Ready ? std::string("hwdec: probe done: hardware decoding ready")
                             : "hwdec: probe done: hardware decoding off (" + reason_ + ")");
  changed_.notify_all();
}

int HwdecProbe::call(const char* name, const std::string& args, const Call& fn) {
  const std::string what = std::string(name) + "(" + args + ")";
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (state_ != State::Probing) {
      return kAbandoned;
    }
    current_ = name;
    current_since_ = Clock::now();
    ++serial_;
    changed_.notify_all();
  }
  log_("hwdec: " + what + " ...");
  const auto began = Clock::now();
  std::string detail;
  const int rc = fn(detail);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
  bool abandoned;
  {
    std::lock_guard<std::mutex> lock(lock_);
    abandoned = state_ != State::Probing;
    current_.clear();
    changed_.notify_all();
  }
  char tail[96];
  std::snprintf(tail, sizeof(tail), " -> %#x", static_cast<unsigned>(rc));
  std::string line = "hwdec: " + what + tail;
  if (!detail.empty()) {
    line += " [" + detail + "]";
  }
  std::snprintf(tail, sizeof(tail), " (%lld ms)", static_cast<long long>(ms));
  line += tail;
  if (abandoned) {
    line += ", too late: hardware decoding stays off";
  }
  log_(line);
  return abandoned ? kAbandoned : rc;
}

void HwdecProbe::watch() {
  std::unique_lock<std::mutex> lock(lock_);
  while (state_ == State::Probing) {
    if (current_.empty()) {
      changed_.wait(lock);
      continue;
    }
    const std::uint64_t serial = serial_;
    const auto deadline = current_since_ + limit_;
    if (changed_.wait_until(lock, deadline, [&] {
          return state_ != State::Probing || serial_ != serial || current_.empty();
        })) {
      continue;
    }
    // The same call, still running past its limit.
    // Logged before anyone waiting hears of it.
    reason_ = current_ + " did not return in " + limit_text();
    log_("hwdec: " + reason_ + ", hardware decoding disabled");
    state_ = State::Stuck;
    changed_.notify_all();
    return;
  }
}

HwdecProbe::State HwdecProbe::state() const {
  std::lock_guard<std::mutex> lock(lock_);
  return state_;
}

HwdecProbe::State HwdecProbe::wait(std::chrono::milliseconds max) const {
  std::unique_lock<std::mutex> lock(lock_);
  changed_.wait_for(lock, max, [&] { return state_ != State::Probing; });
  return state_;
}

std::string HwdecProbe::reason() const {
  std::lock_guard<std::mutex> lock(lock_);
  return state_ == State::Ready || state_ == State::Probing ? std::string() : reason_;
}

}  // namespace prosperoweb::video
