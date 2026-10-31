// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hardware decoder's background probe (hwdec_probe.h): every call logged
// before and after, failures reported, a call that hangs past the limit
// turns the decoder off without the probe ever being joined.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "hwdec_probe.h"

using prosperoweb::video::HwdecProbe;
using State = HwdecProbe::State;
using namespace std::chrono_literals;

namespace {

struct Lines {
  std::mutex lock;
  std::vector<std::string> lines;
  bool has(const std::string& text) {
    std::lock_guard<std::mutex> guard(lock);
    for (const auto& line : lines) {
      if (line.find(text) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
};

// A call that blocks until released (shared: it outlives the test's scope
// when the probe thread is still in it).
struct Gate {
  std::mutex lock;
  std::condition_variable cv;
  bool open = false;
  void release() {
    std::lock_guard<std::mutex> guard(lock);
    open = true;
    cv.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> guard(lock);
    cv.wait(guard, [this] { return open; });
  }
};

// The probe's threads may log after a test ends: they hold the lines.
HwdecProbe::Log sink(const std::shared_ptr<Lines>& lines) {
  return [lines](const std::string& line) {
    std::lock_guard<std::mutex> guard(lines->lock);
    lines->lines.push_back(line);
  };
}

}  // namespace

TEST_SUITE("hwdec_probe") {
  TEST_CASE("every call succeeds: ready, each call logged before and after") {
    auto log = std::make_shared<Lines>();
    auto probe = HwdecProbe::create(2000ms, sink(log));
    CHECK(probe->state() == State::NotStarted);
    CHECK(probe->wait(1ms) == State::NotStarted);
    const bool started = probe->start([](HwdecProbe& p) {
      const int a = p.call("QueryComputeMemoryInfo", "this_size 0x18", [](std::string& detail) {
        detail = "cpu_gpu_size 0x10000";
        return 0;
      });
      const int b = p.call("AllocateComputeQueue", "pipe 0", [](std::string&) { return 0; });
      return a == 0 && b == 0;
    });
    CHECK(started);
    CHECK(probe->wait(5s) == State::Ready);
    CHECK(probe->reason().empty());
    CHECK(log->has("hwdec: QueryComputeMemoryInfo(this_size 0x18) ..."));
    CHECK(log->has("hwdec: QueryComputeMemoryInfo(this_size 0x18) -> 0 [cpu_gpu_size 0x10000] ("));
    CHECK(log->has("hwdec: AllocateComputeQueue(pipe 0) -> 0 ("));
    CHECK_FALSE(log->has("did not return"));
    CHECK(log->has("hwdec: probe done: hardware decoding ready"));
  }

  TEST_CASE("a refused call: unavailable with its reason, the code logged") {
    auto log = std::make_shared<Lines>();
    auto probe = HwdecProbe::create(2000ms, sink(log));
    probe->start([](HwdecProbe& p) {
      const int rc = p.call("AllocateComputeQueue", "", [](std::string&) {
        return static_cast<int>(0x811D0111u);
      });
      if (rc != 0) {
        p.fail("compute queue 0x811d0111");
        return false;
      }
      return true;
    });
    CHECK(probe->wait(5s) == State::Unavailable);
    CHECK(probe->reason() == "compute queue 0x811d0111");
    CHECK(log->has("hwdec: AllocateComputeQueue() -> 0x811d0111 ("));
    CHECK(log->has("hwdec: probe done: hardware decoding off (compute queue 0x811d0111)"));
  }

  TEST_CASE("a body that fails without a reason still says why") {
    auto probe = HwdecProbe::create(2000ms, [](const std::string&) {});
    probe->start([](HwdecProbe&) { return false; });
    CHECK(probe->wait(5s) == State::Unavailable);
    CHECK_FALSE(probe->reason().empty());
  }

  TEST_CASE("only the first start runs") {
    auto probe = HwdecProbe::create(2000ms, [](const std::string&) {});
    std::atomic<int> runs{0};
    CHECK(probe->start([&runs](HwdecProbe&) {
      ++runs;
      return true;
    }));
    CHECK_FALSE(probe->start([&runs](HwdecProbe&) {
      ++runs;
      return true;
    }));
    CHECK(probe->wait(5s) == State::Ready);
    CHECK(runs.load() == 1);
  }

  TEST_CASE("a call that hangs: stuck after the limit, never waited for") {
    auto log = std::make_shared<Lines>();
    auto gate = std::make_shared<Gate>();
    auto after = std::make_shared<std::atomic<int>>(0);
    auto probe = HwdecProbe::create(100ms, sink(log));
    CHECK(probe->limit_text() == "100 ms");
    const auto began = std::chrono::steady_clock::now();
    probe->start([gate, after](HwdecProbe& p) {
      p.call("QueryComputeMemoryInfo", "", [](std::string&) { return 0; });
      const int rc = p.call("AllocateComputeQueue", "pipe 0", [gate](std::string&) {
        gate->wait();
        return 0;
      });
      // Given up meanwhile: what the call returns says so, and later calls
      // do not run.
      after->store(rc);
      p.call("QueryDecoderMemoryInfo", "", [after](std::string&) {
        after->store(1);
        return 0;
      });
      return true;
    });
    CHECK(probe->wait(5s) == State::Stuck);
    const auto waited = std::chrono::steady_clock::now() - began;
    CHECK(waited >= 100ms);
    CHECK(waited < 3s);
    CHECK(probe->reason() == "AllocateComputeQueue did not return in 100 ms");
    CHECK(log->has("hwdec: AllocateComputeQueue did not return in 100 ms, hardware decoding "
                   "disabled"));
    CHECK(log->has("hwdec: AllocateComputeQueue(pipe 0) ..."));
    CHECK_FALSE(log->has("hwdec: AllocateComputeQueue(pipe 0) -> "));

    // The call returns late: the probe stays off.
    gate->release();
    for (int i = 0; i < 200 && after->load() == 0; ++i) {
      std::this_thread::sleep_for(5ms);
    }
    CHECK(after->load() == HwdecProbe::kAbandoned);
    CHECK(probe->state() == State::Stuck);
    for (int i = 0; i < 200 && !log->has("too late"); ++i) {
      std::this_thread::sleep_for(5ms);
    }
    CHECK(log->has("hwdec: AllocateComputeQueue(pipe 0) -> 0 ("));
    CHECK(log->has("too late: hardware decoding stays off"));
    CHECK_FALSE(log->has("QueryDecoderMemoryInfo"));
  }

  TEST_CASE("slow calls each within the limit do not trip the watchdog") {
    auto log = std::make_shared<Lines>();
    auto probe = HwdecProbe::create(150ms, sink(log));
    probe->start([](HwdecProbe& p) {
      for (int i = 0; i < 4; ++i) {
        const int rc = p.call("QueryDecoderMemoryInfo", std::to_string(i), [](std::string&) {
          std::this_thread::sleep_for(60ms);
          return 0;
        });
        if (rc != 0) {
          return false;
        }
      }
      return true;
    });
    CHECK(probe->wait(5s) == State::Ready);
    CHECK_FALSE(log->has("did not return"));
  }

  TEST_CASE("waiting while the probe runs returns at the deadline") {
    auto gate = std::make_shared<Gate>();
    auto probe = HwdecProbe::create(10000ms, [](const std::string&) {});
    CHECK(probe->limit_text() == "10 s");
    probe->start([gate](HwdecProbe& p) {
      return p.call("AllocateComputeQueue", "", [gate](std::string&) {
               gate->wait();
               return 0;
             }) == 0;
    });
    CHECK(probe->wait(50ms) == State::Probing);
    CHECK(probe->reason().empty());
    gate->release();
    CHECK(probe->wait(5s) == State::Ready);
  }
}
