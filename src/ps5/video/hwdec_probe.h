// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bringing up the UI's hardware video decoder off the startup path. The
// first console run of hardware decoding never got past the decoder's first
// call: the UI stopped right after the library loaded, with no splash and
// no WebKit. So the probe (library, compute queue, memory answers) runs on a
// thread of its own, every platform call through call(), which logs it
// before and after with its arguments and result, and a watchdog gives up on
// the decoder when one call has not returned within the limit: hardware
// decoding is then off for the session (streams are refused and the
// WebProcess decodes in software) and the stuck thread is left alone, never
// joined.
//
// Platform-free (the calls are functions handed in), so the host tests
// drive it with calls that succeed, fail or hang.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace prosperoweb::video {

// How long one platform call of the probe may take.
inline constexpr std::chrono::milliseconds kHwdecCallLimit{3000};

class HwdecProbe : public std::enable_shared_from_this<HwdecProbe> {
 public:
  enum class State {
    NotStarted,
    Probing,
    Ready,        // the body succeeded
    Unavailable,  // the body failed (a call refused, a resource missing)
    Stuck,        // a call did not return within the limit
  };
  using Log = std::function<void(const std::string&)>;
  // Returns true when the decoder is usable; failure reasons go to fail().
  using Body = std::function<bool(HwdecProbe&)>;
  // A platform call: returns its result code; may append what it returned
  // (sizes, addresses) to `detail` for the log.
  using Call = std::function<int(std::string& detail)>;

  // What call() returns when the probe was given up while the call ran.
  static constexpr int kAbandoned = -0x7ffffff0;

  static std::shared_ptr<HwdecProbe> create(std::chrono::milliseconds call_limit, Log log);

  // Runs `body` on a detached thread, watched by a detached watchdog. Only
  // the first start does anything; false otherwise.
  bool start(Body body);

  // Within the body: runs `fn` as the named call, logging
  // "hwdec: <name>(<args>) ..." before it and
  // "hwdec: <name>(<args>) -> 0x<rc> [<detail>] (<n> ms)" after it. When the
  // probe has been given up meanwhile it returns kAbandoned.
  int call(const char* name, const std::string& args, const Call& fn);
  // Within the body: why it is about to return false.
  void fail(const std::string& reason);

  State state() const;
  // Until the probe is no longer Probing or `max` passed; the state then.
  State wait(std::chrono::milliseconds max) const;
  // Why the decoder is not available ("" while Ready or Probing).
  std::string reason() const;
  // "3 s" / "250 ms": the limit as the log writes it.
  std::string limit_text() const;

 private:
  HwdecProbe(std::chrono::milliseconds call_limit, Log log);
  void run_body(const Body& body);
  void watch();
  void finish(State state, const std::string& reason);

  using Clock = std::chrono::steady_clock;
  const std::chrono::milliseconds limit_;
  const Log log_;
  mutable std::mutex lock_;
  mutable std::condition_variable changed_;
  State state_ = State::NotStarted;
  std::string reason_;
  // The call running now (empty when none) and when it began; `serial`
  // tells the watchdog whether the call it timed is still the same one.
  std::string current_;
  Clock::time_point current_since_{};
  std::uint64_t serial_ = 0;
};

const char* to_string(HwdecProbe::State state);

}  // namespace prosperoweb::video
