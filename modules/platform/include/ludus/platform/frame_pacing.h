#pragma once

// Native frame pacing policy. The contract (absolute monotonic targets, an
// operating-system wait rechecked after every wake, drift-free target advance,
// a bounded lateness reset, no browser waits and no spin) is owned by
// docs/architecture/high-resolution-time.md, "Native frame pacing".
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/time/time.hpp>

namespace ludus::platform
{
/// Longest pacing interval Configure() accepts: one second. One Pace() call never
/// requests more than two intervals of waiting, so a misconfigured or regressed
/// clock can never block the frame loop for longer than two seconds.
inline constexpr foundation::uint64 MAXIMUM_PACING_INTERVAL_NANOSECONDS = foundation::time::NANOSECONDS_PER_SECOND;

/// Operating-system waits one Pace() call may issue before it gives up and starts
/// the frame early. Bounds the loop when a wait keeps returning before its target.
inline constexpr foundation::uint32 MAXIMUM_PACING_WAIT_ATTEMPTS = 8;

/// @brief Result of one wait request made through a PacingClock.
enum class PacingWaitStatus : foundation::uint8
{
    /// The wait returned after its requested interval. It may have returned late.
    Completed,
    /// The wait returned early, for example after a signal. The pacer rereads the clock.
    Interrupted,
    /// The target has no wait primitive. Browser builds always report this.
    Unsupported,
    /// The operating system rejected the request.
    Failed,
};

/// @brief Injected monotonic time source and wait primitive for FramePacer.
///
/// Production loops use GetSystemPacingClock(). Unit tests supply a synthetic
/// clock whose wait advances simulated time, so pacing policy is tested without
/// sleeping. Both functions are called only on the thread that calls Pace().
struct PacingClock final
{
    /// Return the current monotonic reading in the FoundationTime domain. Null makes Pace() report InvalidClock.
    foundation::time::Timestamp (*ReadNowOrNull)(void* contextOrNull) noexcept = nullptr;
    /// Block for about `interval` (never zero). May return early or late; the
    /// pacer rereads the clock after every return. Null makes Pace() report InvalidClock.
    PacingWaitStatus (*WaitForOrNull)(void* contextOrNull, foundation::time::Duration interval) noexcept = nullptr;
    /// Opaque state passed to both functions; owned by the caller and kept alive across Pace().
    void* ContextOrNull = nullptr;
};

/// @brief Return the native pacing clock: FoundationTime Now() and a relative operating-system wait.
///
/// Each wait covers the interval remaining until the target, computed from a
/// fresh Now() reading on every attempt. The absolute target therefore stays in
/// the FoundationTime domain and lateness never accumulates across frames. macOS
/// waits on a one-shot `kevent64(2)` timer marked `NOTE_CRITICAL`, which opts out
/// of the kernel's timer-coalescing leeway (up to 1 ms for a windowed application
/// and several milliseconds for a background process on the measured host); if
/// the thread's queue cannot be created it falls back to `nanosleep(2)`. Linux
/// sleeps with `nanosleep(2)`. A wake can still be late by scheduler latency, and
/// on Linux by timer slack. A signal returns Interrupted. Browser builds return a
/// clock whose wait always reports Unsupported: browsers pace with presentation
/// callbacks and must never block or busy-wait. Other targets have no adapter and
/// also report Unsupported.
///
/// The returned value carries no state, so it may be copied and used from any
/// thread. On macOS each thread that waits lazily opens one kqueue descriptor,
/// reused by its later waits and closed when the thread exits.
/// @note Zero leeway keeps the kernel from batching this wake with other timers.
/// A loop that should save power while hidden or idle lowers its pacing rate.
/// @return A clock whose function pointers are never null and whose context is null.
[[nodiscard]] PacingClock GetSystemPacingClock() noexcept;

/// @brief Application-chosen pacing schedule.
struct PacingPolicy final
{
    /// Target interval between frame starts. Configure() accepts 1 ns to one second;
    /// a default-constructed FramePacer has zero, which disables pacing.
    foundation::time::Duration Interval;
    /// Lateness at which a missed schedule is abandoned instead of skipped. A frame
    /// that starts at most this late keeps the target phase; a later one re-anchors
    /// the schedule at its actual start. Zero re-anchors after any lateness.
    foundation::time::Duration MaximumLateness;
};

/// @brief Configure() result.
enum class PacingStatus : foundation::uint8
{
    /// The policy was applied and the schedule was dropped.
    Ok,
    /// The interval was zero or longer than MAXIMUM_PACING_INTERVAL_NANOSECONDS; the previous policy and schedule are
    /// kept.
    InvalidConfiguration,
};

/// @brief What one FramePacer::Pace() call did.
enum class PaceOutcome : foundation::uint8
{
    /// The pacer has no interval (default construction). The clock was not read and nothing waited.
    Disabled,
    /// The clock functions were null. Nothing was read or waited and the schedule is unchanged.
    InvalidClock,
    /// No schedule existed (first call, after Reset() or Configure()). The frame starts now and anchors the schedule.
    Baseline,
    /// The frame was early. The pacer waited, rechecking after each wake, until the target was reached.
    Waited,
    /// The frame reached its target with no wait, late by at most MaximumLateness (possibly
    /// zero). It starts at once; missed targets are skipped and the phase is kept.
    Late,
    /// The frame was later than MaximumLateness. It starts at once and the schedule is re-anchored at its start.
    Rescheduled,
    /// A wait reported Unsupported or Failed. The frame starts early; the phase is kept.
    WaitFailed,
    /// MAXIMUM_PACING_WAIT_ATTEMPTS waits all returned early. The frame starts early; the phase is kept.
    WaitLimit,
    /// The clock read below the previous reading, or the target was more than two
    /// intervals away. Nothing waited; the schedule is re-anchored at this reading.
    ClockRegression,
    /// The next target cannot be represented. Nothing waited and the schedule is dropped.
    Overflow,
};

/// @brief Diagnostics for one Pace() call. All timestamps share the injected clock's domain.
struct PaceResult final
{
    /// What the call did.
    PaceOutcome Outcome = PaceOutcome::Disabled;
    /// The frame-start target this call honored. Equals Wake when the schedule was (re-)anchored.
    foundation::time::Timestamp Target;
    /// The final clock reading: when the frame actually starts.
    foundation::time::Timestamp Wake;
    /// Wake minus Target when the frame started at or after its target, otherwise zero.
    foundation::time::Duration Lateness;
    /// Whole targets after Target that had already passed. Late skips them; Rescheduled abandons them.
    foundation::uint64 SkippedTargets = 0;
    /// Wait requests issued through the clock.
    foundation::uint32 WaitCount = 0;
    /// Waits that returned (Completed or Interrupted) before the target was reached.
    foundation::uint32 EarlyWakeCount = 0;
};

/// @brief Owner-local native frame pacer that waits for absolute monotonic targets.
///
/// Call Pace() once per frame, before the frame reads its own timestamp. The
/// first call anchors a schedule; each later call waits until the next target
/// and advances that target from the previous target, never from the wake time,
/// so oversleeping one frame does not shift later frames. A late frame skips
/// missed targets while it is within the policy's maximum lateness and re-anchors
/// the schedule beyond it, so stalls never produce a burst of back-to-back frames.
///
/// The pacer never spins: every wait is an operating-system wait for the
/// remaining interval. A scheduled target is at most one interval away; after a
/// failed or limited wait started a frame early, at most two. It does not
/// replace presentation blocking. Swapchain queue depth, vsync and GPU completion
/// remain graphics policy, and browsers pace with presentation callbacks.
///
/// Values are allocation-free and need caller synchronization if shared. Call
/// Reset() on lifecycle changes that should not inherit the old phase: startup,
/// suspension and session replacement.
class FramePacer final
{
public:
    /// @brief Apply a pacing policy and drop the schedule.
    /// @param policy Interval within [1 ns, MAXIMUM_PACING_INTERVAL_NANOSECONDS] and any maximum lateness.
    /// @return Ok, or InvalidConfiguration with the previous policy and schedule unchanged.
    [[nodiscard]] PacingStatus Configure(const PacingPolicy& policy) noexcept;

    /// @brief Drop the schedule. The next Pace() is a Baseline that starts at once.
    void Reset() noexcept;

    /// @brief Wait for this frame's target, then advance the schedule.
    ///
    /// Reads the clock, waits while it is before the target (rereading after every
    /// wake or interruption), and returns once the target is reached, a wait fails
    /// or the attempt limit is hit. See PaceOutcome for the schedule change in each case.
    /// @param clock Time source and wait primitive; both functions must be nonnull.
    /// @return Diagnostics for this frame; the schedule changes as described by its Outcome.
    [[nodiscard]] PaceResult Pace(const PacingClock& clock) noexcept;

    /// @brief Return the active policy. A default-constructed pacer has a zero interval.
    [[nodiscard]] LUDUS_INLINE const PacingPolicy& GetPolicy() const noexcept
    {
        return mPolicy;
    }

    /// @brief Return whether a schedule is anchored, so the next Pace() is not a Baseline.
    [[nodiscard]] LUDUS_INLINE bool IsScheduled() const noexcept
    {
        return mIsScheduled;
    }

    /// @brief Return the target the next Pace() waits for. Meaningful only while IsScheduled().
    [[nodiscard]] LUDUS_INLINE foundation::time::Timestamp GetNextTarget() const noexcept
    {
        return mNextTarget;
    }

private:
    [[nodiscard]] bool anchor(foundation::time::Timestamp start) noexcept;
    PacingPolicy mPolicy;
    foundation::time::Timestamp mNextTarget;
    foundation::time::Timestamp mLastReading;
    bool mIsScheduled = false;
};
} // namespace ludus::platform
