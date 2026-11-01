// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <deque>
#include <optional>

#include "FFmpegPresenterPolicy.h"

using namespace WebCore::FFmpegMedia;

namespace {

struct Frame {
    double pts;
    double duration;
};

struct Simulation {
    unsigned shown { 0 };
    unsigned dropped { 0 };
    unsigned steps { 0 };
    double maxLateness { 0 };
    PresenterStats stats;
};

// Drives the presenter rules the way FFmpegVideoPresenter::run() does, with
// a fake clock: frames decoded ahead, the clock advanced by the waits the
// policy asks for, plus an optional jump (an audio clock catching up).
Simulation simulate(double fps, double seconds, double jumpAt = -1, double jumpBy = 0)
{
    Simulation result;
    std::deque<Frame> queue;
    double interval = 1 / fps;
    int count = static_cast<int>(fps * seconds + 0.5);
    for (int i = 0; i < count; ++i)
        queue.push_back({ i * interval, interval });
    double clock = 0;
    bool jumped = false;
    while (!queue.empty() && clock < seconds + 1) {
        ++result.steps;
        std::optional<double> untilDue;
        while (!queue.empty()) {
            std::optional<double> nextPts;
            if (queue.size() > 1)
                nextPts = queue[1].pts;
            auto decision = decidePresentation(queue.front().pts, queue.front().duration, nextPts, clock, false);
            if (decision.action == PresentAction::Wait) {
                untilDue = decision.untilDue;
                break;
            }
            queue.pop_front();
            result.stats.record(decision);
            if (decision.action == PresentAction::Drop) {
                ++result.dropped;
                continue;
            }
            ++result.shown;
            result.maxLateness = std::max(result.maxLateness, decision.lateness);
        }
        clock += presenterWaitSeconds(untilDue, 1, true);
        if (jumpAt >= 0 && !jumped && clock >= jumpAt) {
            jumped = true;
            clock += jumpBy;
        }
    }
    return result;
}

} // namespace

TEST_SUITE("media_presenter") {
  TEST_CASE("early frames wait for their pts") {
    auto decision = decidePresentation(1.0, 0.04, 1.04, 0.9, false);
    CHECK(decision.action == PresentAction::Wait);
    CHECK(decision.untilDue == doctest::Approx(0.1));
    // Within the early tolerance a frame is due.
    CHECK(decidePresentation(1.0, 0.04, 1.04, 0.996, false).action == PresentAction::Show);
    CHECK(decidePresentation(1.0, 0.04, 1.04, 1.0, false).action == PresentAction::Show);
  }

  TEST_CASE("late frames are shown up to 1.5 intervals") {
    // 25 fps: 40 ms per frame, drop threshold 60 ms.
    auto oneInterval = decidePresentation(1.0, 0.04, 1.04, 1.04, false);
    CHECK(oneInterval.action == PresentAction::Show);
    CHECK(oneInterval.lateness == doctest::Approx(0.04));
    CHECK(decidePresentation(1.0, 0.04, 1.04, 1.059, false).action == PresentAction::Show);
    auto tooLate = decidePresentation(1.0, 0.04, 1.04, 1.061, false);
    CHECK(tooLate.action == PresentAction::Drop);
    CHECK(tooLate.lateness == doctest::Approx(0.061));
  }

  TEST_CASE("the last decoded frame is shown however late") {
    CHECK(decidePresentation(1.0, 0.04, std::nullopt, 3.0, false).action == PresentAction::Show);
    CHECK(decidePresentation(1.0, 0.04, std::nullopt, 3.0, false).lateness == doctest::Approx(2.0));
  }

  TEST_CASE("paused or seeking shows the first frame at once") {
    auto decision = decidePresentation(5.0, 0.04, 5.04, 4.0, true);
    CHECK(decision.action == PresentAction::Show);
    CHECK(decision.lateness == 0);
  }

  TEST_CASE("resume from hidden: holding the queue keeps decode near the clock") {
    // A 2 s GOP. The page was hidden at pts 1930 with the clock there; it is
    // shown 5 s later, so the audio clock is at 1935. Video buffered to 1979.
    const double hold = 1930, bufferedEnd = 1979, clock = 1935, gop = 2;
    // Old behaviour (drain while hidden): decode resumes at the buffered end,
    // ~46 s ahead of the clock -> the player never reaches it, frozen.
    double aheadOld = resumeDecodeAheadOfClock(true, hold, bufferedEnd, clock, gop);
    CHECK(aheadOld > 40);
    CHECK_FALSE(resumeConverges(aheadOld, /*reachSeconds*/ 2));
    // The fix (hold the queue): decode resumes at the hold point, within a
    // GOP of it, at or just past the clock that advanced while hidden.
    double aheadNew = resumeDecodeAheadOfClock(false, hold, bufferedEnd, clock, gop);
    CHECK(aheadNew <= gop);
    CHECK(resumeConverges(aheadNew, gop));
    // Hidden longer than a GOP: the clock passes the hold point, so decode
    // resumes behind the clock and the drop-behind catches up.
    CHECK(resumeDecodeAheadOfClock(false, 1930, 1979, 1960, gop) < 0);
  }

  TEST_CASE("frame interval") {
    CHECK(presentationInterval(1.0, 0.0417, 1.1) == doctest::Approx(0.0417));
    CHECK(presentationInterval(1.0, 0, 1.02) == doctest::Approx(0.02));
    CHECK(presentationInterval(1.0, 0, std::nullopt) == doctest::Approx(1.0 / 30));
    CHECK(presentationInterval(1.0, 10, std::nullopt) == doctest::Approx(0.25));
    CHECK(presentationInterval(1.0, 0, 1.0) == doctest::Approx(1.0 / 30));
    CHECK(presentationInterval(1.0, 0.0001, std::nullopt) == doctest::Approx(1.0 / 240));
    // Without a duration, the gap to the next frame sets the threshold.
    CHECK(decidePresentation(1.0, 0, 1.02, 1.029, false).action == PresentAction::Show);
    CHECK(decidePresentation(1.0, 0, 1.02, 1.031, false).action == PresentAction::Drop);
  }

  TEST_CASE("sleep until the next frame, bounded") {
    CHECK(presenterWaitSeconds(std::nullopt, 1, false) == doctest::Approx(presenterIdleSeconds));
    CHECK(presenterWaitSeconds(0.5, 1, false) == doctest::Approx(presenterIdleSeconds));
    CHECK(presenterWaitSeconds(std::nullopt, 1, true) == doctest::Approx(presenterPollSeconds));
    CHECK(presenterWaitSeconds(0.5, 1, true) == doctest::Approx(presenterMaxWaitSeconds));
    CHECK(presenterWaitSeconds(0.006, 1, true) == doctest::Approx(0.006));
    CHECK(presenterWaitSeconds(0.006, 2, true) == doctest::Approx(0.003));
    CHECK(presenterWaitSeconds(0.0, 1, true) == doctest::Approx(presenterMinWaitSeconds));
  }

  TEST_CASE("stats line") {
    PresenterStats stats;
    CHECK_FALSE(stats.intervalElapsed(10.0, 5.0));
    stats.record({ PresentAction::Show, 0, 0.004 });
    stats.record({ PresentAction::Show, 0, 0.012 });
    stats.record({ PresentAction::Drop, 0, 0.1 });
    stats.record({ PresentAction::Wait, 0.02, 0 });
    CHECK(stats.due() == 3);
    CHECK(stats.shown() == 2);
    CHECK(stats.lateDropped() == 1);
    CHECK(stats.line() == "presenter: due 3 shown 2 late-dropped 1 max lateness 12.0 ms");
    CHECK_FALSE(stats.intervalElapsed(14.9, 5.0));
    CHECK(stats.intervalElapsed(15.0, 5.0));
    stats.reset(15.0);
    CHECK(stats.due() == 0);
    CHECK(stats.line() == "presenter: due 0 shown 0 late-dropped 0 max lateness 0.0 ms");
    CHECK_FALSE(stats.intervalElapsed(19.0, 5.0));
  }

  TEST_CASE("steady 24 fps shows every frame on time") {
    auto run = simulate(24, 5);
    CHECK(run.shown == 120);
    CHECK(run.dropped == 0);
    CHECK(run.maxLateness < presenterMaxWaitSeconds + 1e-9);
    CHECK(run.stats.due() == 120);
    CHECK(run.stats.lateDropped() == 0);
  }

  TEST_CASE("a clock jump drops only what is beyond 1.5 intervals") {
    // 24 fps (41.7 ms): a 200 ms jump makes about 5 frames due at once; the
    // ones more than 62.5 ms late are dropped, the rest shown.
    auto run = simulate(24, 5, 2.0, 0.2);
    CHECK(run.dropped >= 3);
    CHECK(run.dropped <= 4);
    CHECK(run.shown + run.dropped == 120);
    CHECK(run.maxLateness <= 1.5 / 24 + 1e-9);
    // A 50 ms hiccup (just over one interval) drops nothing.
    auto small = simulate(24, 5, 2.0, 0.05);
    CHECK(small.dropped == 0);
    CHECK(small.shown == 120);
  }

  TEST_CASE("60 fps content at the poll limits") {
    auto run = simulate(60, 2);
    CHECK(run.shown == 120);
    CHECK(run.dropped == 0);
  }
}
