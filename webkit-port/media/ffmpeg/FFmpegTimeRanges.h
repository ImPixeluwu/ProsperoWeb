// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sorted, non-overlapping set of half-open [start, end) time ranges in
// seconds. Engine-independent; the demuxer records what it has demuxed here
// and the player converts it into PlatformTimeRanges for buffered().

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace WebCore {
namespace FFmpegMedia {

class TimeRangeSet {
public:
    struct Range {
        double start;
        double end;
    };

    // Ranges closer than this are merged: demuxed packets of one stream are
    // contiguous but timestamps carry rounding error.
    static constexpr double mergeTolerance = 0.05;

    void add(double start, double end)
    {
        if (!std::isfinite(start) || !std::isfinite(end) || end <= start)
            return;
        Range added { start, end };
        std::vector<Range> merged;
        merged.reserve(m_ranges.size() + 1);
        bool inserted = false;
        for (auto& range : m_ranges) {
            if (range.end + mergeTolerance < added.start) {
                merged.push_back(range);
                continue;
            }
            if (added.end + mergeTolerance < range.start) {
                if (!inserted) {
                    merged.push_back(added);
                    inserted = true;
                }
                merged.push_back(range);
                continue;
            }
            added.start = std::min(added.start, range.start);
            added.end = std::max(added.end, range.end);
        }
        if (!inserted)
            merged.push_back(added);
        m_ranges = std::move(merged);
    }

    void clear() { m_ranges.clear(); }
    bool isEmpty() const { return m_ranges.empty(); }
    size_t size() const { return m_ranges.size(); }
    const Range& operator[](size_t i) const { return m_ranges[i]; }
    const std::vector<Range>& ranges() const { return m_ranges; }

    bool contains(double time) const
    {
        for (auto& range : m_ranges) {
            if (time >= range.start - mergeTolerance && time < range.end)
                return true;
        }
        return false;
    }

    // Seconds of contiguous data available from `time` on (0 if `time` is
    // not buffered). Drives the HAVE_FUTURE_DATA/HAVE_ENOUGH_DATA decision.
    double bufferedAhead(double time) const
    {
        for (auto& range : m_ranges) {
            if (time >= range.start - mergeTolerance && time < range.end)
                return range.end - time;
        }
        return 0;
    }

    double totalDuration() const
    {
        double total = 0;
        for (auto& range : m_ranges)
            total += range.end - range.start;
        return total;
    }

private:
    std::vector<Range> m_ranges;
};

} // namespace FFmpegMedia
} // namespace WebCore
