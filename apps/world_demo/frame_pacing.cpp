#include "internal/frame_pacing.h"
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/sort.hpp>
namespace ludus::world_demo
{
namespace
{
using platform::PaceOutcome;
/// Nearest-rank percentile of an ascending series: the smallest value with at least `percent`% at or below it.
[[nodiscard]] uint64 GetPercentile(const uint64* sorted, usize count, usize percent) noexcept
{
    const usize rank = (count * percent + 99) / 100;
    return sorted[rank == 0 ? 0 : rank - 1];
}
[[nodiscard]] IntervalDistribution ComputeDistribution(foundation::Array<uint64>& inoutValues) noexcept
{
    IntervalDistribution distribution;
    const usize count = inoutValues.GetSize();
    if (count == 0)
    {
        return distribution;
    }
    uint64* values = inoutValues.GetData();
    foundation::Sort(values, count, [](uint64 left, uint64 right) noexcept { return left < right; });
    // Mean of the integer quotient and remainder sums, so long runs cannot overflow the total.
    uint64 quotientSum = 0;
    uint64 remainderSum = 0;
    for (usize index = 0; index < count; ++index)
    {
        quotientSum += values[index] / count;
        remainderSum += values[index] % count;
    }
    distribution.Count = count;
    distribution.Minimum = values[0];
    distribution.Median = GetPercentile(values, count, 50);
    distribution.Percentile90 = GetPercentile(values, count, 90);
    distribution.Percentile99 = GetPercentile(values, count, 99);
    distribution.Maximum = values[count - 1];
    distribution.Mean = quotientSum + remainderSum / count;
    return distribution;
}
} // namespace
bool LoopPacing::Configure(uint32 framesPerSecondLimit) noexcept
{
    if (framesPerSecondLimit > MAXIMUM_FRAMES_PER_SECOND || applyPolicy(framesPerSecondLimit, mIsVisible) == false)
    {
        return false;
    }
    mFramesPerSecondLimit = framesPerSecondLimit;
    return true;
}
void LoopPacing::SetVisible(bool isVisible) noexcept
{
    if (isVisible == mIsVisible)
    {
        return;
    }
    // The active limit was validated by Configure() and the hidden rate is a valid constant, so this cannot fail.
    const bool isApplied = applyPolicy(mFramesPerSecondLimit, isVisible);
    LUDUS_ASSERT(isApplied == true, "A validated pacing policy must apply");
    (void)isApplied;
    mIsVisible = isVisible;
}
bool LoopPacing::applyPolicy(uint32 framesPerSecondLimit, bool isVisible) noexcept
{
    uint32 framesPerSecond = framesPerSecondLimit != 0 ? framesPerSecondLimit : IDLE_FRAMES_PER_SECOND;
    if (isVisible == false && framesPerSecond > HIDDEN_FRAMES_PER_SECOND)
    {
        framesPerSecond = HIDDEN_FRAMES_PER_SECOND;
    }
    // Rounded to the nearest nanosecond: a 60 Hz cap is 16,666,667 ns, about 20 ns/s slower than exact.
    const uint64 interval = (foundation::time::NANOSECONDS_PER_SECOND + framesPerSecond / 2) / framesPerSecond;
    // Configure() drops the schedule, so a policy change never inherits the previous phase.
    return mPacer.Configure(
    {
        .Interval = {interval},
        .MaximumLateness = {interval * MAXIMUM_LATE_INTERVALS},
    }) == platform::PacingStatus::Ok;
}
void LoopPacing::Reset() noexcept
{
    mPacer.Reset();
}
platform::PaceResult LoopPacing::EndFrame(bool hasPresented, const platform::PacingClock& clock) noexcept
{
    if (mFramesPerSecondLimit == 0 && mIsVisible == true && hasPresented == true)
    {
        // The FIFO swapchain paced this frame. Drop the idle schedule so the next
        // idle frame anchors afresh instead of inheriting a stale phase. A hidden
        // window is never left to presentation pacing: its presents need not block.
        mPacer.Reset();
        return {};
    }
    return mPacer.Pace(clock);
}
bool PacingReport::Reserve(usize frames) noexcept
{
    mRecords.Clear();
    mDropped = 0;
    return frames != 0 && frames <= MAXIMUM_PACING_RECORDS && mRecords.TryEnsureCapacity(frames) == true;
}
void PacingReport::Record(const PacingRecord& record) noexcept
{
    if (mRecords.GetSize() >= mRecords.GetCapacity() || mRecords.TryAdd(record) == false)
    {
        ++mDropped;
    }
}
bool PacingReport::Summarize(PacingSummary& outSummary) const noexcept
{
    const usize count = mRecords.GetSize();
    foundation::Array<uint64> lateness;
    foundation::Array<uint64> frameStart;
    foundation::Array<uint64> presentReturn;
    if (lateness.TryEnsureCapacity(count) == false || frameStart.TryEnsureCapacity(count) == false ||
        presentReturn.TryEnsureCapacity(count) == false)
    {
        return false;
    }
    PacingSummary summary;
    summary.Frames = count;
    for (usize index = 0; index < count; ++index)
    {
        const PacingRecord& record = mRecords[index];
        const PaceOutcome outcome = record.Pace.Outcome;
        summary.EarlyWakes += record.Pace.EarlyWakeCount;
        switch (outcome)
        {
            case PaceOutcome::Waited:
                ++summary.Waited;
                (void)lateness.TryAdd(record.Pace.Lateness.Nanoseconds);
                break;
            case PaceOutcome::Late:
                ++summary.Late;
                break;
            case PaceOutcome::Rescheduled:
                ++summary.Rescheduled;
                break;
            case PaceOutcome::Disabled:
                ++summary.PresentationPaced;
                break;
            default:
                ++summary.Other;
                break;
        }
        if (index == 0)
        {
            continue;
        }
        const PacingRecord& previous = mRecords[index - 1];
        const bool isPaced = outcome != PaceOutcome::Disabled && previous.Pace.Outcome != PaceOutcome::Disabled;
        if (isPaced == true && record.Pace.Wake.Nanoseconds >= previous.Pace.Wake.Nanoseconds)
        {
            (void)frameStart.TryAdd(record.Pace.Wake.Nanoseconds - previous.Pace.Wake.Nanoseconds);
        }
        if (record.HasPresented == true && previous.HasPresented == true &&
            record.PresentReturn.Nanoseconds >= previous.PresentReturn.Nanoseconds)
        {
            (void)presentReturn.TryAdd(record.PresentReturn.Nanoseconds - previous.PresentReturn.Nanoseconds);
        }
    }
    summary.WakeLateness = ComputeDistribution(lateness);
    summary.FrameStart = ComputeDistribution(frameStart);
    summary.PresentReturn = ComputeDistribution(presentReturn);
    outSummary = summary;
    return true;
}
} // namespace ludus::world_demo
