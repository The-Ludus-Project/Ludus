#include <ludus/foundation/time/timers.hpp>

// Thanks to Noel Llopis, "The Clock: Keeping Your Finger on the Pulse of the
// Game", Game Programming Gems 4, 1.3, pp. 27-34: independent pause state and
// one shared frame sample. This is original code using explicit value samples,
// integer intervals and visible clamp loss; no registered timer graph or smoothing.
// Detailed review: docs/architecture/high-resolution-time.md, Gems review.

namespace ludus::foundation::time
{
void Stopwatch::Reset() noexcept
{
    mAnchor = {};
    mAccumulated = {};
    mState = StopwatchState::Stopped;
}

void Stopwatch::Start(Timestamp now) noexcept
{
    mAnchor = now;
    mAccumulated = {};
    mState = StopwatchState::Running;
}

TimeStatus Stopwatch::Read(Timestamp now, Duration& out) const noexcept
{
    if (mState == StopwatchState::Stopped)
    {
        return TimeStatus::InvalidState;
    }
    if (mState == StopwatchState::Paused)
    {
        out = mAccumulated;
        return TimeStatus::Ok;
    }
    Duration elapsed;
    const auto status = TryElapsed(mAnchor, now, elapsed);
    return status == TimeStatus::Ok ? TryAdd(mAccumulated, elapsed, out) : status;
}

TimeStatus Stopwatch::Pause(Timestamp now) noexcept
{
    if (mState != StopwatchState::Running)
    {
        return TimeStatus::InvalidState;
    }
    Duration elapsed;
    const auto status = Read(now, elapsed);
    if (status == TimeStatus::Ok)
    {
        mAccumulated = elapsed;
        mAnchor = now;
        mState = StopwatchState::Paused;
    }
    return status;
}

TimeStatus Stopwatch::Resume(Timestamp now) noexcept
{
    if (mState != StopwatchState::Paused)
    {
        return TimeStatus::InvalidState;
    }
    if (now.Nanoseconds < mAnchor.Nanoseconds)
    {
        return TimeStatus::ClockRegression;
    }
    mAnchor = now;
    mState = StopwatchState::Running;
    return TimeStatus::Ok;
}

TimeStatus Deadline::Arm(Timestamp now, Duration delay) noexcept
{
    Timestamp due;
    const auto status = TryAdd(now, delay, due);
    if (status == TimeStatus::Ok)
    {
        mDue = due;
        mArmed = true;
    }
    return status;
}

void FrameClock::Reset() noexcept
{
    mPrevious = {};
    mHasBaseline = false;
}

FrameSample FrameClock::Sample(Timestamp now) noexcept
{
    if (mHasBaseline == false)
    {
        mPrevious = now;
        mHasBaseline = true;
        return { .IsBaseline = true };
    }
    Duration elapsed;
    const auto status = TryElapsed(mPrevious, now, elapsed);
    if (status != TimeStatus::Ok)
    {
        return { .Status = status };
    }
    mPrevious = now;
    if (elapsed.Nanoseconds > mMaximumDelta.Nanoseconds)
    {
        return
        {
            .Elapsed = mMaximumDelta,
            .Discarded = {elapsed.Nanoseconds - mMaximumDelta.Nanoseconds},
        };
    }
    return { .Elapsed = elapsed };
}
} // namespace ludus::foundation::time
