#include "viewport_pacing.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/time/time.hpp>

namespace ludus::editor::tests
{
using namespace foundation;
namespace
{
constexpr uint64 PERCENT = 100;
constexpr uint64 P95_PERCENT = 95;

/// Median of values; sorts them in place. Zero when empty.
uint64 SortAndGetMedian(Array<uint64>& inoutValues) noexcept
{
    if (inoutValues.IsEmpty() == true)
    {
        return 0;
    }
    Sort(inoutValues.GetData(), inoutValues.GetSize(), [](uint64 left, uint64 right) { return left < right; });
    return inoutValues[inoutValues.GetSize() / 2];
}
/// Nearest-rank percentile of sorted, nonempty values.
uint64 GetSortedPercentile(const Array<uint64>& values, uint64 percent) noexcept
{
    const uint64 rank = (percent * values.GetSize() + PERCENT - 1) / PERCENT;
    return values[rank == 0 ? 0 : static_cast<usize>(rank - 1)];
}
uint64 GetLarger(uint64 left, uint64 right) noexcept
{
    return left < right ? right : left;
}
} // namespace

bool SummarizeViewportPacingWindow(BorrowedView<const uint64> presentedTicks,
                                   const ViewportPacingWindow& window,
                                   ViewportPacingWindowSummary& outSummary)
{
    if (window.EndTicks <= window.BeginTicks)
    {
        return false;
    }
    Array<uint64> intervals;
    if (intervals.TryEnsureCapacity(presentedTicks.GetSize()) == false)
    {
        return false;
    }
    ViewportPacingWindowSummary summary;
    uint64 previous = window.BeginTicks;
    for (const uint64 ticks : presentedTicks)
    {
        if (ticks < window.BeginTicks || ticks >= window.EndTicks)
        {
            continue;
        }
        if (summary.Images > 0)
        {
            if (intervals.TryAdd(ticks - previous) == false)
            {
                return false;
            }
        }
        summary.LongestGap = GetLarger(summary.LongestGap, ticks - previous);
        previous = ticks;
        ++summary.Images;
    }
    summary.LongestGap = GetLarger(summary.LongestGap, window.EndTicks - previous);
    if (intervals.IsEmpty() == false)
    {
        summary.MedianInterval = SortAndGetMedian(intervals);
        summary.P95Interval = GetSortedPercentile(intervals, P95_PERCENT);
    }
    outSummary = summary;
    return true;
}

bool EvaluateViewportPacing(BorrowedView<const uint64> presentedTicks,
                            BorrowedView<const ViewportPacingWindow> windows,
                            ViewportPacingVerdict& outVerdict)
{
    Array<uint64> idleImages;
    Array<uint64> inputImages;
    Array<uint64> idleGaps;
    Array<uint64> inputGaps;
    ViewportPacingVerdict verdict;
    verdict.IsInputSustained = true;
    verdict.HasProgress = true;
    for (const ViewportPacingWindow& window : windows)
    {
        ViewportPacingWindowSummary summary;
        if (SummarizeViewportPacingWindow(presentedTicks, window, summary) == false)
        {
            return false;
        }
        const bool isInput = window.Kind == ViewportPacingWindowKind::Input;
        if (isInput == true)
        {
            verdict.IsInputSustained = verdict.IsInputSustained == true && window.InputCount > MIN_PACING_INPUT_EVENTS;
            verdict.HasProgress = verdict.HasProgress == true && summary.Images > MIN_PACING_WINDOW_IMAGES;
        }
        Array<uint64>& images = isInput == true ? inputImages : idleImages;
        Array<uint64>& gaps = isInput == true ? inputGaps : idleGaps;
        if (images.TryAdd(summary.Images) == false || gaps.TryAdd(summary.LongestGap) == false)
        {
            return false;
        }
    }
    if (idleImages.IsEmpty() == true || inputImages.IsEmpty() == true)
    {
        return false;
    }
    verdict.IdleMedianImages = SortAndGetMedian(idleImages);
    verdict.InputMedianImages = SortAndGetMedian(inputImages);
    verdict.IdleMedianLongestGap = SortAndGetMedian(idleGaps);
    verdict.InputMedianLongestGap = SortAndGetMedian(inputGaps);
    verdict.IsRatioJudged = verdict.IdleMedianImages >= PACING_IDLE_IMAGE_FLOOR;
    verdict.IsThroughputKept =
        verdict.IsRatioJudged == false || verdict.InputMedianImages * PACING_THROUGHPUT_DENOMINATOR >=
                                              verdict.IdleMedianImages * PACING_THROUGHPUT_NUMERATOR;
    verdict.InputLongestGapLimit = GetLarger(time::Milliseconds(PACING_GAP_FLOOR_MILLISECONDS).Nanoseconds,
                                             verdict.IdleMedianLongestGap * PACING_GAP_IDLE_MULTIPLE);
    verdict.IsCadenceKept = verdict.InputMedianLongestGap <= verdict.InputLongestGapLimit;
    outVerdict = verdict;
    return true;
}

bool IsViewportPacingAccepted(const ViewportPacingVerdict& verdict) noexcept
{
    return verdict.IsInputSustained == true && verdict.HasProgress == true && verdict.IsThroughputKept == true &&
           verdict.IsCadenceKept == true;
}
} // namespace ludus::editor::tests
