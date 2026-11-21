// Clip players: simulation-tick control state, event traversal and root-motion deltas.
//
// Forward traversal delivers events in (old, new], reverse traversal reversible events in
// [new, old); loop crossings split into ordered per-cycle intervals, and each event carries the
// cycle containing it. Root motion composes inverse(R(old)) * cycleEnd^n * R(new), never
// subtracting positions across a wrap. See docs/architecture/skeletal-animation.md, "Playback
// synchronization and events" and "Root motion and locomotion quality".

#include <ludus/animation/clip_player.hpp>

#include "internal/root_math.hpp"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::animation
{
using foundation::float64;
using foundation::int64;
using foundation::uint64;
using foundation::usize;
namespace math = foundation::math;

namespace
{
/// Committed timeline position of a player.
struct Cursor final
{
    float64 TimeSeconds = 0;
    int64 Cycle = 0;
    bool IsCompleted = false;
};

/// Identities stamped on every delivered event.
struct EventIdentity final
{
    uint64 InstanceId = 0;
    uint64 ActivationId = 0;
};

/// Candidate result of one traversal, committed only after every step succeeded.
struct Traversal final
{
    Cursor Next{};
    int64 CyclesCrossed = 0;
    bool HasCompleted = false;
    ClipTraversalDirection Direction = ClipTraversalDirection::Forward;
};

/// Cycle-local event window. Forward windows are (Lower, Upper]; reverse windows are [Lower, Upper).
/// A missing bound covers the whole cycle on that side.
struct CycleWindow final
{
    bool HasLower = false;
    float64 Lower = 0;
    bool HasUpper = false;
    float64 Upper = 0;
};

bool IsInWindow(float64 timeSeconds, const CycleWindow& window, ClipTraversalDirection direction) noexcept
{
    if (direction == ClipTraversalDirection::Forward)
    {
        return (window.HasLower == false || timeSeconds > window.Lower) &&
               (window.HasUpper == false || timeSeconds <= window.Upper);
    }
    return (window.HasLower == false || timeSeconds >= window.Lower) &&
           (window.HasUpper == false || timeSeconds < window.Upper);
}

bool IsEligible(const ClipEventRecord& event, ClipTraversalDirection direction) noexcept
{
    return direction == ClipTraversalDirection::Forward || event.IsReversible == true;
}

usize CountInWindow(foundation::BorrowedView<const ClipEventRecord> events,
                    const CycleWindow& window,
                    ClipTraversalDirection direction) noexcept
{
    usize count = 0;
    for (const ClipEventRecord& event : events)
    {
        if (IsEligible(event, direction) == true && IsInWindow(event.TimeSeconds, window, direction) == true)
        {
            ++count;
        }
    }
    return count;
}

/// Window of cycle `cycle` when traversing from `from` to `to` (cycles and cycle-local times).
CycleWindow MakeWindow(const Cursor& from, const Cursor& to, int64 cycle, ClipTraversalDirection direction) noexcept
{
    if (direction == ClipTraversalDirection::Forward)
    {
        return
        {
            .HasLower = cycle == from.Cycle,
            .Lower = from.TimeSeconds,
            .HasUpper = cycle == to.Cycle,
            .Upper = to.TimeSeconds,
        };
    }
    return
    {
        .HasLower = cycle == to.Cycle,
        .Lower = to.TimeSeconds,
        .HasUpper = cycle == from.Cycle,
        .Upper = from.TimeSeconds,
    };
}

/// Count the events a traversal delivers without iterating the crossed full cycles.
bool TryCountEvents(foundation::BorrowedView<const ClipEventRecord> events,
                    const Cursor& from,
                    const Traversal& traversal,
                    usize& outCount) noexcept
{
    const ClipTraversalDirection direction = traversal.Direction;
    const Cursor& to = traversal.Next;
    if (from.Cycle == to.Cycle)
    {
        outCount = CountInWindow(events, MakeWindow(from, to, from.Cycle, direction), direction);
        return true;
    }
    const usize first = CountInWindow(events, MakeWindow(from, to, from.Cycle, direction), direction);
    const usize last = CountInWindow(events, MakeWindow(from, to, to.Cycle, direction), direction);
    const usize perCycle = CountInWindow(events, {}, direction);
    const int64 crossed = traversal.CyclesCrossed < 0 ? -traversal.CyclesCrossed : traversal.CyclesCrossed;
    usize middleCycles = 0;
    usize middle = 0;
    usize total = 0;
    return foundation::TryIntegerCast(crossed - 1, middleCycles) == true &&
           foundation::TryMultiply(middleCycles, perCycle, middle) == true &&
           foundation::TryAdd(first, last, total) == true && foundation::TryAdd(total, middle, outCount) == true;
}

void WriteCycleEvents(foundation::BorrowedView<const ClipEventRecord> events,
                      const CycleWindow& window,
                      int64 cycle,
                      ClipTraversalDirection direction,
                      const EventIdentity& identity,
                      foundation::BorrowedView<ClipEvent> outEvents,
                      usize& inoutWritten) noexcept
{
    const usize count = events.GetSize();
    for (usize step = 0; step < count; ++step)
    {
        // Forward delivery is ascending (time, identity); reverse delivery is descending.
        const usize index = direction == ClipTraversalDirection::Forward ? step : count - 1 - step;
        const ClipEventRecord& event = events[index];
        if (IsEligible(event, direction) == false || IsInWindow(event.TimeSeconds, window, direction) == false)
        {
            continue;
        }
        outEvents[inoutWritten] =
        {
            .InstanceId = identity.InstanceId,
            .ActivationId = identity.ActivationId,
            .Cycle = cycle,
            .EventId = event.EventId,
            .TimeSeconds = event.TimeSeconds,
            .Direction = direction,
        };
        ++inoutWritten;
    }
}

/// Write the counted events in delivery order. A cycle without eligible events is skipped
/// arithmetically, so the work is bounded by the written count and the two partial cycles.
usize WriteEvents(foundation::BorrowedView<const ClipEventRecord> events,
                  const Cursor& from,
                  const Traversal& traversal,
                  const EventIdentity& identity,
                  foundation::BorrowedView<ClipEvent> outEvents) noexcept
{
    const ClipTraversalDirection direction = traversal.Direction;
    const Cursor& to = traversal.Next;
    usize written = 0;
    const int64 step = direction == ClipTraversalDirection::Forward ? 1 : -1;
    const bool hasFullCycleEvents = CountInWindow(events, {}, direction) != 0;
    int64 cycle = from.Cycle;
    while (true)
    {
        WriteCycleEvents(events,
                         MakeWindow(from, to, cycle, direction),
                         cycle,
                         direction,
                         identity,
                         outEvents,
                         written);
        if (cycle == to.Cycle)
        {
            break;
        }
        const bool isNextLast = cycle + step == to.Cycle;
        cycle = hasFullCycleEvents == true || isNextLast == true ? cycle + step : to.Cycle;
    }
    return written;
}

/// Advance a cursor on a looping timeline by a signed timeline delta.
AnimationStatus
TryAdvanceLoop(float64 durationSeconds, const Cursor& from, float64 timelineDelta, Traversal& outTraversal) noexcept
{
    const float64 unwrapped = from.TimeSeconds + timelineDelta;
    const float64 cycles = math::Floor(unwrapped / durationSeconds);
    if (math::IsFinite(cycles) == false || math::Abs(cycles) > static_cast<float64>(MAX_CLIP_TRAVERSAL_CYCLES))
    {
        return AnimationStatus::OutOfRange;
    }
    int64 crossed = static_cast<int64>(cycles);
    float64 time = unwrapped - cycles * durationSeconds;
    // Repair rounding at a cycle boundary without moving the position by a whole cycle.
    if (time < 0)
    {
        time += durationSeconds;
        --crossed;
        if (time >= durationSeconds)
        {
            time = 0;
            ++crossed;
        }
    }
    else if (time >= durationSeconds)
    {
        time -= durationSeconds;
        ++crossed;
        time = time < 0 ? 0.0 : time;
    }
    int64 cycle = 0;
    if (foundation::TryAdd(from.Cycle, crossed, cycle) == false)
    {
        return AnimationStatus::OutOfRange;
    }
    outTraversal.Next = { .TimeSeconds = time, .Cycle = cycle, .IsCompleted = false };
    outTraversal.CyclesCrossed = crossed;
    outTraversal.HasCompleted = false;
    return AnimationStatus::Success;
}

/// Advance a cursor on a clamped timeline, latching completion at the endpoint it reaches.
void AdvanceClamp(float64 durationSeconds, const Cursor& from, float64 timelineDelta, Traversal& outTraversal) noexcept
{
    float64 time = from.TimeSeconds + timelineDelta;
    time = time > durationSeconds ? durationSeconds : time;
    time = time < 0 ? 0.0 : time;
    const bool isAtEndpoint = (timelineDelta > 0 && time >= durationSeconds) || (timelineDelta < 0 && time <= 0);
    outTraversal.Next = { .TimeSeconds = time, .Cycle = from.Cycle, .IsCompleted = isAtEndpoint };
    outTraversal.CyclesCrossed = 0;
    outTraversal.HasCompleted = isAtEndpoint;
}

AnimationStatus
TryComputeRootDelta(const Clip& clip, const Cursor& from, const Traversal& traversal, RootTransform& outDelta) noexcept
{
    const RootMotionMode mode = clip.GetRootMode();
    if (mode == RootMotionMode::None)
    {
        outDelta = RootTransform{};
        return AnimationStatus::Success;
    }
    RootTransform start{};
    RootTransform end{};
    if (TrySampleClipRoot(clip, from.TimeSeconds, start) != AnimationStatus::Success ||
        TrySampleClipRoot(clip, traversal.Next.TimeSeconds, end) != AnimationStatus::Success)
    {
        return AnimationStatus::InvalidArgument;
    }
    detail::RigidTransformD path = detail::ConvertToRigid(end);
    if (traversal.CyclesCrossed != 0)
    {
        path = detail::Compose(detail::RaiseToPower(detail::ConvertToRigid(clip.GetRootEnd()), traversal.CyclesCrossed),
                               path);
    }
    detail::RigidTransformD delta = detail::Compose(detail::Invert(detail::ConvertToRigid(start)), path);
    if (mode == RootMotionMode::Planar)
    {
        // Planar composition is planar exactly; remove rounding residue outside the plane.
        delta.Translation.Y = 0;
        delta.Rotation.X = 0;
        delta.Rotation.Z = 0;
    }
    RootTransform rounded{};
    if (detail::TryConvertToRootTransform(delta, rounded) == false)
    {
        return AnimationStatus::OutOfRange;
    }
    outDelta = rounded;
    return AnimationStatus::Success;
}

/// Plan a traversal seek that lands exactly on its target within the current cycle; a loop
/// target at the duration enters the next cycle at its start.
AnimationStatus TryPlanSeekTraversal(const Clip& clip,
                                     const Cursor& from,
                                     float64 targetTimeSeconds,
                                     Traversal& inoutTraversal) noexcept
{
    const float64 duration = clip.GetDurationSeconds();
    if (clip.GetWrapMode() == ClipWrapMode::Clamp)
    {
        // Decide the endpoint from the target itself: from + (target - from) may round onto or
        // off an endpoint the target does not reach.
        const bool isForward = targetTimeSeconds > from.TimeSeconds;
        const bool isAtEndpoint =
            (isForward == true && targetTimeSeconds == duration) || (isForward == false && targetTimeSeconds == 0);
        inoutTraversal.Next = { .TimeSeconds = targetTimeSeconds, .Cycle = from.Cycle, .IsCompleted = isAtEndpoint };
        inoutTraversal.CyclesCrossed = 0;
        inoutTraversal.HasCompleted = isAtEndpoint;
        return AnimationStatus::Success;
    }
    const bool isWrap = targetTimeSeconds == duration;
    int64 cycle = from.Cycle;
    if (isWrap == true && foundation::TryAdd(from.Cycle, int64{1}, cycle) == false)
    {
        return AnimationStatus::OutOfRange;
    }
    inoutTraversal.Next =
    {
        .TimeSeconds = isWrap == true ? 0.0 : targetTimeSeconds,
        .Cycle = cycle,
        .IsCompleted = false,
    };
    inoutTraversal.CyclesCrossed = isWrap == true ? 1 : 0;
    inoutTraversal.HasCompleted = false;
    return AnimationStatus::Success;
}

/// Plan, count, check and write one traversal; nothing is written unless it all fits.
/// A seek passes its target so rounding of from + delta cannot miss it or cross a cycle.
AnimationStatus TryTraverse(const Clip& clip,
                            const Cursor& from,
                            float64 timelineDelta,
                            const float64* seekTargetOrNull,
                            const EventIdentity& identity,
                            foundation::BorrowedView<ClipEvent> outEvents,
                            Traversal& outTraversal,
                            ClipAdvanceResult& outResult) noexcept
{
    if (timelineDelta == 0)
    {
        outTraversal =
        {
            .Next = from,
            .CyclesCrossed = 0,
            .HasCompleted = false,
            .Direction = ClipTraversalDirection::Forward,
        };
        outResult = ClipAdvanceResult{};
        return AnimationStatus::Success;
    }
    Traversal traversal{};
    traversal.Direction = timelineDelta > 0 ? ClipTraversalDirection::Forward : ClipTraversalDirection::Reverse;
    AnimationStatus planStatus = AnimationStatus::Success;
    if (seekTargetOrNull != nullptr)
    {
        planStatus = TryPlanSeekTraversal(clip, from, *seekTargetOrNull, traversal);
    }
    else if (clip.GetWrapMode() == ClipWrapMode::Loop)
    {
        planStatus = TryAdvanceLoop(clip.GetDurationSeconds(), from, timelineDelta, traversal);
    }
    else
    {
        AdvanceClamp(clip.GetDurationSeconds(), from, timelineDelta, traversal);
    }
    if (planStatus != AnimationStatus::Success)
    {
        return planStatus;
    }
    const foundation::BorrowedView<const ClipEventRecord> events = clip.GetEvents();
    usize eventCount = 0;
    if (TryCountEvents(events, from, traversal, eventCount) == false || eventCount > outEvents.GetSize())
    {
        return AnimationStatus::EventCapacityExceeded;
    }
    if (eventCount != 0 && outEvents.GetDataOrNull() == nullptr)
    {
        return AnimationStatus::InvalidArgument;
    }
    ClipAdvanceResult result{};
    const AnimationStatus status = TryComputeRootDelta(clip, from, traversal, result.RootDelta);
    if (status != AnimationStatus::Success)
    {
        return status;
    }
    result.EventCount = eventCount == 0 ? 0 : WriteEvents(events, from, traversal, identity, outEvents);
    LUDUS_ASSERT(result.EventCount == eventCount);
    result.CyclesCrossed = traversal.CyclesCrossed;
    result.HasCompleted = traversal.HasCompleted;
    outTraversal = traversal;
    outResult = result;
    return AnimationStatus::Success;
}

AnimationStatus ValidatePlayerClip(const Clip& clip, const ClipPlayer& player) noexcept
{
    if (clip.IsPrepared() == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    if (player.IsActive() == false)
    {
        return AnimationStatus::NotActive;
    }
    return clip.GetFingerprint() == player.GetClipFingerprint() ? AnimationStatus::Success
                                                                : AnimationStatus::IncompatibleClip;
}

bool IsTimelinePositionValid(const Clip& clip, float64 timeSeconds) noexcept
{
    return math::IsFinite(timeSeconds) == true && timeSeconds >= 0 && timeSeconds <= clip.GetDurationSeconds();
}

/// The canonical cycle-local spelling of a timeline position: a loop maps its duration to zero.
float64 CanonicalizeTime(const Clip& clip, float64 timeSeconds) noexcept
{
    if (clip.GetWrapMode() == ClipWrapMode::Loop && timeSeconds == clip.GetDurationSeconds())
    {
        return 0;
    }
    return timeSeconds == 0 ? 0.0 : timeSeconds;
}
} // namespace

bool ClipPlayer::IsActive() const noexcept
{
    return mIsActive;
}

bool ClipPlayer::IsPaused() const noexcept
{
    return mIsPaused;
}

bool ClipPlayer::IsCompleted() const noexcept
{
    return mIsCompleted;
}

uint64 ClipPlayer::GetInstanceId() const noexcept
{
    return mInstanceId;
}

uint64 ClipPlayer::GetActivationId() const noexcept
{
    return mActivationId;
}

foundation::Fingerprint128 ClipPlayer::GetClipFingerprint() const noexcept
{
    return mClipFingerprint;
}

float64 ClipPlayer::GetTimeSeconds() const noexcept
{
    return mTimeSeconds;
}

float64 ClipPlayer::GetPreviousTimeSeconds() const noexcept
{
    return mPreviousTimeSeconds;
}

int64 ClipPlayer::GetCycle() const noexcept
{
    return mCycle;
}

float64 ClipPlayer::GetRate() const noexcept
{
    return mRate;
}

AnimationStatus ClipPlayer::TrySetRate(float64 rate) noexcept
{
    if (mIsActive == false)
    {
        return AnimationStatus::NotActive;
    }
    if (math::IsFinite(rate) == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    mRate = rate;
    return AnimationStatus::Success;
}

void ClipPlayer::Pause() noexcept
{
    mIsPaused = mIsActive == true ? true : mIsPaused;
}

void ClipPlayer::Resume() noexcept
{
    mIsPaused = mIsActive == true ? false : mIsPaused;
}

void ClipPlayer::Stop() noexcept
{
    mIsActive = false;
}

AnimationStatus
TryActivateClipPlayer(const Clip& clip, const ClipActivation& activation, ClipPlayer& outPlayer) noexcept
{
    if (clip.IsPrepared() == false || activation.InstanceId == 0 || activation.ActivationId == 0 ||
        IsTimelinePositionValid(clip, activation.StartTimeSeconds) == false || math::IsFinite(activation.Rate) == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    ClipPlayer player;
    player.mClipFingerprint = clip.GetFingerprint();
    player.mInstanceId = activation.InstanceId;
    player.mActivationId = activation.ActivationId;
    player.mTimeSeconds = CanonicalizeTime(clip, activation.StartTimeSeconds);
    player.mPreviousTimeSeconds = player.mTimeSeconds;
    player.mCycle = 0;
    player.mRate = activation.Rate;
    player.mIsActive = true;
    outPlayer = player;
    return AnimationStatus::Success;
}

AnimationStatus TryAdvanceClipPlayer(const Clip& clip,
                                     float64 deltaSeconds,
                                     foundation::BorrowedView<ClipEvent> outEvents,
                                     ClipPlayer& inoutPlayer,
                                     ClipAdvanceResult& outResult) noexcept
{
    const AnimationStatus clipStatus = ValidatePlayerClip(clip, inoutPlayer);
    if (clipStatus != AnimationStatus::Success)
    {
        return clipStatus;
    }
    if (math::IsFinite(deltaSeconds) == false || deltaSeconds < 0)
    {
        return AnimationStatus::InvalidArgument;
    }
    if (inoutPlayer.mIsPaused == true || inoutPlayer.mIsCompleted == true)
    {
        inoutPlayer.mPreviousTimeSeconds = inoutPlayer.mTimeSeconds;
        outResult = ClipAdvanceResult{};
        return AnimationStatus::Success;
    }
    const float64 timelineDelta = inoutPlayer.mRate * deltaSeconds;
    if (math::IsFinite(timelineDelta) == false)
    {
        return AnimationStatus::OutOfRange;
    }
    const Cursor from
    {
        .TimeSeconds = inoutPlayer.mTimeSeconds,
        .Cycle = inoutPlayer.mCycle,
        .IsCompleted = false,
    };
    const EventIdentity identity{ .InstanceId = inoutPlayer.mInstanceId, .ActivationId = inoutPlayer.mActivationId };
    Traversal traversal{};
    ClipAdvanceResult result{};
    const AnimationStatus status =
        TryTraverse(clip, from, timelineDelta, nullptr, identity, outEvents, traversal, result);
    if (status != AnimationStatus::Success)
    {
        return status;
    }
    inoutPlayer.mPreviousTimeSeconds = from.TimeSeconds;
    inoutPlayer.mTimeSeconds = traversal.Next.TimeSeconds;
    inoutPlayer.mCycle = traversal.Next.Cycle;
    inoutPlayer.mIsCompleted = traversal.Next.IsCompleted;
    outResult = result;
    return AnimationStatus::Success;
}

AnimationStatus TrySeekClipPlayer(const Clip& clip,
                                  float64 targetTimeSeconds,
                                  ClipSeekPolicy policy,
                                  foundation::BorrowedView<ClipEvent> outEvents,
                                  ClipPlayer& inoutPlayer,
                                  ClipAdvanceResult& outResult) noexcept
{
    const AnimationStatus clipStatus = ValidatePlayerClip(clip, inoutPlayer);
    if (clipStatus != AnimationStatus::Success)
    {
        return clipStatus;
    }
    if (IsTimelinePositionValid(clip, targetTimeSeconds) == false ||
        (policy != ClipSeekPolicy::Scrub && policy != ClipSeekPolicy::Traverse))
    {
        return AnimationStatus::InvalidArgument;
    }
    if (policy == ClipSeekPolicy::Scrub)
    {
        // Like Traverse, a loop's duration is the start of the next cycle, never the current one.
        int64 cycle = inoutPlayer.mCycle;
        if (clip.GetWrapMode() == ClipWrapMode::Loop && targetTimeSeconds == clip.GetDurationSeconds() &&
            foundation::TryAdd(inoutPlayer.mCycle, int64{1}, cycle) == false)
        {
            return AnimationStatus::OutOfRange;
        }
        inoutPlayer.mCycle = cycle;
        inoutPlayer.mTimeSeconds = CanonicalizeTime(clip, targetTimeSeconds);
        inoutPlayer.mPreviousTimeSeconds = inoutPlayer.mTimeSeconds;
        inoutPlayer.mIsCompleted = false;
        outResult = ClipAdvanceResult{};
        return AnimationStatus::Success;
    }
    const Cursor from
    {
        .TimeSeconds = inoutPlayer.mTimeSeconds,
        .Cycle = inoutPlayer.mCycle,
        .IsCompleted = false,
    };
    const EventIdentity identity{ .InstanceId = inoutPlayer.mInstanceId, .ActivationId = inoutPlayer.mActivationId };
    Traversal traversal{};
    ClipAdvanceResult result{};
    const float64 target = targetTimeSeconds == 0 ? 0.0 : targetTimeSeconds;
    const AnimationStatus status =
        TryTraverse(clip, from, target - from.TimeSeconds, &target, identity, outEvents, traversal, result);
    if (status != AnimationStatus::Success)
    {
        return status;
    }
    inoutPlayer.mPreviousTimeSeconds = from.TimeSeconds;
    inoutPlayer.mTimeSeconds = traversal.Next.TimeSeconds;
    inoutPlayer.mCycle = traversal.Next.Cycle;
    inoutPlayer.mIsCompleted = traversal.Next.IsCompleted;
    outResult = result;
    return AnimationStatus::Success;
}
} // namespace ludus::animation
