#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/time/time.hpp>
#include <ludus/platform/frame_pacing.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

// Every policy case drives synthetic time: the injected wait advances a counter
// instead of sleeping, so pacing policy is exact and independent of host load.
// Only the system-clock case sleeps, and it checks lower bounds alone.

using namespace ludus::foundation;
using ludus::platform::FramePacer;
using ludus::platform::PaceOutcome;
using ludus::platform::PaceResult;
using ludus::platform::PacingClock;
using ludus::platform::PacingPolicy;
using ludus::platform::PacingStatus;
using ludus::platform::PacingWaitStatus;
using time::Duration;
using time::Timestamp;

namespace
{
/// A large epoch, so arithmetic is checked away from zero as on a long-running host.
constexpr uint64 EPOCH_NANOSECONDS = uint64{1} << 62;
/// 60 Hz rounded to whole nanoseconds.
constexpr uint64 INTERVAL_60HZ_NANOSECONDS = 16'666'667;
/// Frame work shorter than one interval.
constexpr uint64 FRAME_WORK_NANOSECONDS = 5'000'000;
/// The lateness bound used by most cases: four 60 Hz intervals.
constexpr uint64 MAXIMUM_LATENESS_NANOSECONDS = 4 * INTERVAL_60HZ_NANOSECONDS;
/// A short real wait for the system-clock case: 2 ms.
constexpr uint64 SYSTEM_WAIT_NANOSECONDS = 2'000'000;
/// Real waits issued in a row, so the native adapter reuses its per-thread state.
constexpr uint32 SYSTEM_WAIT_ATTEMPTS = 4;

/// Scripted clock. A completed wait advances time by the request plus an
/// oversleep; interruptions and short wakes advance it by less.
struct SyntheticClock final
{
    uint64 Now = EPOCH_NANOSECONDS;
    uint64 OversleepNanoseconds = 0;
    /// Number of upcoming waits that return Interrupted after InterruptAdvanceNanoseconds.
    uint32 InterruptCount = 0;
    uint64 InterruptAdvanceNanoseconds = 0;
    /// Number of upcoming waits that report Completed but wake ShortWakeNanoseconds early.
    uint32 ShortWakeCount = 0;
    uint64 ShortWakeNanoseconds = 0;
    /// Status returned without advancing time when not Completed.
    PacingWaitStatus ForcedStatus = PacingWaitStatus::Completed;
    /// When nonzero, the next read after a wait jumps back by this amount.
    uint64 RegressAfterWaitNanoseconds = 0;
    uint32 ReadCount = 0;
    uint32 WaitCount = 0;
    uint64 LastRequestNanoseconds = 0;
};

Timestamp ReadSyntheticNow(void* contextOrNull) noexcept
{
    SyntheticClock* clock = static_cast<SyntheticClock*>(contextOrNull);
    ++clock->ReadCount;
    return {clock->Now};
}

PacingWaitStatus WaitSynthetic(void* contextOrNull, Duration interval) noexcept
{
    SyntheticClock* clock = static_cast<SyntheticClock*>(contextOrNull);
    ++clock->WaitCount;
    clock->LastRequestNanoseconds = interval.Nanoseconds;
    if (clock->ForcedStatus != PacingWaitStatus::Completed)
    {
        return clock->ForcedStatus;
    }
    if (clock->RegressAfterWaitNanoseconds != 0)
    {
        clock->Now -= clock->RegressAfterWaitNanoseconds;
        clock->RegressAfterWaitNanoseconds = 0;
        return PacingWaitStatus::Completed;
    }
    if (clock->InterruptCount != 0)
    {
        --clock->InterruptCount;
        clock->Now += clock->InterruptAdvanceNanoseconds;
        return PacingWaitStatus::Interrupted;
    }
    if (clock->ShortWakeCount != 0)
    {
        --clock->ShortWakeCount;
        clock->Now += interval.Nanoseconds - clock->ShortWakeNanoseconds;
        return PacingWaitStatus::Completed;
    }
    clock->Now += interval.Nanoseconds + clock->OversleepNanoseconds;
    return PacingWaitStatus::Completed;
}

[[nodiscard]] PacingClock MakeClock(SyntheticClock& inoutClock) noexcept
{
    return
    {
        .ReadNowOrNull = ReadSyntheticNow,
        .WaitForOrNull = WaitSynthetic,
        .ContextOrNull = &inoutClock,
    };
}

[[nodiscard]] FramePacer MakePacer(uint64 maximumLatenessNanoseconds = MAXIMUM_LATENESS_NANOSECONDS)
{
    FramePacer pacer;
    REQUIRE(pacer.Configure(
    {
        .Interval = {INTERVAL_60HZ_NANOSECONDS},
        .MaximumLateness = {maximumLatenessNanoseconds},
    }) == PacingStatus::Ok);
    return pacer;
}

[[nodiscard]] uint64 GetSlot(uint64 index) noexcept
{
    return EPOCH_NANOSECONDS + index * INTERVAL_60HZ_NANOSECONDS;
}
} // namespace

TEST_CASE("A default pacer is disabled and never touches the clock", "[pacing]")
{
    SyntheticClock synthetic;
    FramePacer pacer;
    const PaceResult result = pacer.Pace(MakeClock(synthetic));
    CHECK(result.Outcome == PaceOutcome::Disabled);
    CHECK(synthetic.ReadCount == 0);
    CHECK(synthetic.WaitCount == 0);
    CHECK(pacer.IsScheduled() == false);
}

TEST_CASE("Configuration validates the interval and preserves the previous policy", "[pacing]")
{
    FramePacer pacer = MakePacer();
    SyntheticClock synthetic;
    CHECK(pacer.Pace(MakeClock(synthetic)).Outcome == PaceOutcome::Baseline);
    CHECK(pacer.Configure({ .Interval = {0}, .MaximumLateness = {} }) == PacingStatus::InvalidConfiguration);
    CHECK(pacer.Configure(
    {
        .Interval = {ludus::platform::MAXIMUM_PACING_INTERVAL_NANOSECONDS + 1},
        .MaximumLateness = {},
    }) == PacingStatus::InvalidConfiguration);
    CHECK(pacer.GetPolicy().Interval.Nanoseconds == INTERVAL_60HZ_NANOSECONDS);
    CHECK(pacer.IsScheduled() == true);

    CHECK(pacer.Configure({ .Interval = {1}, .MaximumLateness = {} }) == PacingStatus::Ok);
    CHECK(pacer.IsScheduled() == false);
    CHECK(pacer.Configure(
    {
        .Interval = {ludus::platform::MAXIMUM_PACING_INTERVAL_NANOSECONDS},
        .MaximumLateness = {},
    }) == PacingStatus::Ok);
    CHECK(pacer.GetPolicy().Interval.Nanoseconds == ludus::platform::MAXIMUM_PACING_INTERVAL_NANOSECONDS);
}

TEST_CASE("Null clock functions are rejected without changing the schedule", "[pacing]")
{
    FramePacer pacer = MakePacer();
    SyntheticClock synthetic;
    REQUIRE(pacer.Pace(MakeClock(synthetic)).Outcome == PaceOutcome::Baseline);
    const Timestamp next = pacer.GetNextTarget();
    PacingClock missingWait = MakeClock(synthetic);
    missingWait.WaitForOrNull = nullptr;
    CHECK(pacer.Pace(missingWait).Outcome == PaceOutcome::InvalidClock);
    CHECK(pacer.Pace(PacingClock{}).Outcome == PaceOutcome::InvalidClock);
    CHECK(pacer.GetNextTarget() == next);
    CHECK(synthetic.ReadCount == 1);
}

TEST_CASE("Targets advance from the previous target, not from the wake", "[pacing]")
{
    FramePacer pacer = MakePacer();
    SyntheticClock synthetic;
    const PacingClock clock = MakeClock(synthetic);
    const PaceResult baseline = pacer.Pace(clock);
    CHECK(baseline.Outcome == PaceOutcome::Baseline);
    CHECK(baseline.Wake.Nanoseconds == EPOCH_NANOSECONDS);
    CHECK(synthetic.WaitCount == 0);

    // Every wake oversleeps by 2 ms. A relative sleep after each frame would drift
    // by 600 * 2 ms; absolute targets stay on the original grid.
    synthetic.OversleepNanoseconds = 2'000'000;
    for (uint64 frame = 1; frame <= 600; ++frame)
    {
        synthetic.Now += FRAME_WORK_NANOSECONDS;
        const PaceResult result = pacer.Pace(clock);
        REQUIRE(result.Outcome == PaceOutcome::Waited);
        CHECK(result.Target.Nanoseconds == GetSlot(frame));
        CHECK(result.Wake.Nanoseconds == GetSlot(frame) + 2'000'000);
        CHECK(result.Lateness.Nanoseconds == 2'000'000);
        CHECK(result.WaitCount == 1);
        // The previous frame started 2 ms late (except after the zero-wait baseline), leaving less to wait.
        const uint64 previousLateness = frame == 1 ? 0 : 2'000'000;
        CHECK(synthetic.LastRequestNanoseconds ==
              INTERVAL_60HZ_NANOSECONDS - FRAME_WORK_NANOSECONDS - previousLateness);
    }
    CHECK(pacer.GetNextTarget().Nanoseconds == GetSlot(601));
}

TEST_CASE("Interruptions and early wakes are rechecked against the absolute target", "[pacing]")
{
    FramePacer pacer = MakePacer();
    SyntheticClock synthetic;
    const PacingClock clock = MakeClock(synthetic);
    REQUIRE(pacer.Pace(clock).Outcome == PaceOutcome::Baseline);

    SECTION("Signals interrupt the wait twice")
    {
        synthetic.InterruptCount = 2;
        synthetic.InterruptAdvanceNanoseconds = 3'000'000;
        const PaceResult result = pacer.Pace(clock);
        CHECK(result.Outcome == PaceOutcome::Waited);
        CHECK(result.WaitCount == 3);
        CHECK(result.EarlyWakeCount == 2);
        CHECK(synthetic.LastRequestNanoseconds == INTERVAL_60HZ_NANOSECONDS - 6'000'000);
        CHECK(result.Wake.Nanoseconds == GetSlot(1));
        CHECK(result.Lateness.Nanoseconds == 0);
    }
    SECTION("A completed wait that wakes before the target waits again")
    {
        synthetic.ShortWakeCount = 1;
        synthetic.ShortWakeNanoseconds = 50'000;
        const PaceResult result = pacer.Pace(clock);
        CHECK(result.Outcome == PaceOutcome::Waited);
        CHECK(result.WaitCount == 2);
        CHECK(result.EarlyWakeCount == 1);
        CHECK(synthetic.LastRequestNanoseconds == 50'000);
        CHECK(result.Wake.Nanoseconds == GetSlot(1));
    }
    SECTION("Persistent early wakes stop at the attempt limit and keep the phase")
    {
        synthetic.InterruptCount = 1000;
        synthetic.InterruptAdvanceNanoseconds = 1;
        const PaceResult result = pacer.Pace(clock);
        CHECK(result.Outcome == PaceOutcome::WaitLimit);
        CHECK(result.WaitCount == ludus::platform::MAXIMUM_PACING_WAIT_ATTEMPTS);
        CHECK(result.EarlyWakeCount == ludus::platform::MAXIMUM_PACING_WAIT_ATTEMPTS);
        CHECK(result.Lateness.Nanoseconds == 0);
        CHECK(pacer.GetNextTarget().Nanoseconds == GetSlot(2));

        // The early start leaves the next target less than two intervals away; it is honored, not
        // mistaken for a regression.
        synthetic.InterruptCount = 0;
        const PaceResult next = pacer.Pace(clock);
        CHECK(next.Outcome == PaceOutcome::Waited);
        CHECK(next.Wake.Nanoseconds == GetSlot(2));
    }
}

TEST_CASE("A failed or unsupported wait starts the frame early without spinning", "[pacing]")
{
    const PacingWaitStatus status = GENERATE(PacingWaitStatus::Failed, PacingWaitStatus::Unsupported);
    FramePacer pacer = MakePacer();
    SyntheticClock synthetic;
    const PacingClock clock = MakeClock(synthetic);
    REQUIRE(pacer.Pace(clock).Outcome == PaceOutcome::Baseline);
    synthetic.ForcedStatus = status;
    synthetic.Now += FRAME_WORK_NANOSECONDS;
    const PaceResult result = pacer.Pace(clock);
    CHECK(result.Outcome == PaceOutcome::WaitFailed);
    CHECK(result.WaitCount == 1);
    CHECK(result.Wake.Nanoseconds == EPOCH_NANOSECONDS + FRAME_WORK_NANOSECONDS);
    CHECK(result.Target.Nanoseconds == GetSlot(1));
    CHECK(pacer.GetNextTarget().Nanoseconds == GetSlot(2));
}

TEST_CASE("Lateness within the bound skips missed targets and keeps the phase", "[pacing]")
{
    FramePacer pacer = MakePacer();
    SyntheticClock synthetic;
    const PacingClock clock = MakeClock(synthetic);
    REQUIRE(pacer.Pace(clock).Outcome == PaceOutcome::Baseline);

    // A 2.5-interval frame: one full target passes after the one it missed.
    synthetic.Now += INTERVAL_60HZ_NANOSECONDS * 5 / 2;
    const PaceResult late = pacer.Pace(clock);
    CHECK(late.Outcome == PaceOutcome::Late);
    CHECK(late.WaitCount == 0);
    CHECK(late.Target.Nanoseconds == GetSlot(1));
    CHECK(late.Lateness.Nanoseconds == synthetic.Now - GetSlot(1));
    CHECK(late.SkippedTargets == 1);
    CHECK(pacer.GetNextTarget().Nanoseconds == GetSlot(3));

    // No burst: the next frame waits for the next slot on the original grid.
    const PaceResult next = pacer.Pace(clock);
    CHECK(next.Outcome == PaceOutcome::Waited);
    CHECK(next.Wake.Nanoseconds == GetSlot(3));

    // Arriving exactly on a target is a zero-lateness start with no wait.
    synthetic.Now = GetSlot(4);
    const PaceResult exact = pacer.Pace(clock);
    CHECK(exact.Outcome == PaceOutcome::Late);
    CHECK(exact.Lateness.Nanoseconds == 0);
    CHECK(exact.SkippedTargets == 0);
    CHECK(pacer.GetNextTarget().Nanoseconds == GetSlot(5));

    // Exactly at the bound still keeps the phase.
    synthetic.Now = GetSlot(5) + MAXIMUM_LATENESS_NANOSECONDS;
    const PaceResult bound = pacer.Pace(clock);
    CHECK(bound.Outcome == PaceOutcome::Late);
    CHECK(bound.SkippedTargets == 4);
    CHECK(pacer.GetNextTarget().Nanoseconds == GetSlot(10));
}

TEST_CASE("Lateness beyond the bound re-anchors the schedule", "[pacing]")
{
    FramePacer pacer = MakePacer();
    SyntheticClock synthetic;
    const PacingClock clock = MakeClock(synthetic);
    REQUIRE(pacer.Pace(clock).Outcome == PaceOutcome::Baseline);

    synthetic.Now = GetSlot(1) + MAXIMUM_LATENESS_NANOSECONDS + 1;
    const uint64 stalled = synthetic.Now;
    const PaceResult result = pacer.Pace(clock);
    CHECK(result.Outcome == PaceOutcome::Rescheduled);
    CHECK(result.WaitCount == 0);
    CHECK(result.SkippedTargets == 4);
    CHECK(pacer.GetNextTarget().Nanoseconds == stalled + INTERVAL_60HZ_NANOSECONDS);

    for (uint64 frame = 1; frame <= 3; ++frame)
    {
        synthetic.Now += FRAME_WORK_NANOSECONDS;
        const PaceResult paced = pacer.Pace(clock);
        CHECK(paced.Outcome == PaceOutcome::Waited);
        CHECK(paced.Wake.Nanoseconds == stalled + frame * INTERVAL_60HZ_NANOSECONDS);
    }

    SECTION("A zero bound re-anchors after any lateness")
    {
        FramePacer strict = MakePacer(0);
        REQUIRE(strict.Pace(clock).Outcome == PaceOutcome::Baseline);
        synthetic.Now += INTERVAL_60HZ_NANOSECONDS + 1;
        CHECK(strict.Pace(clock).Outcome == PaceOutcome::Rescheduled);
        CHECK(strict.GetNextTarget().Nanoseconds == synthetic.Now + INTERVAL_60HZ_NANOSECONDS);
    }
}

TEST_CASE("Clock regressions re-anchor instead of waiting on a distant target", "[pacing]")
{
    FramePacer pacer = MakePacer();
    SyntheticClock synthetic;
    const PacingClock clock = MakeClock(synthetic);
    REQUIRE(pacer.Pace(clock).Outcome == PaceOutcome::Baseline);

    SECTION("A reading below the previous one")
    {
        synthetic.Now -= 1;
        const PaceResult result = pacer.Pace(clock);
        CHECK(result.Outcome == PaceOutcome::ClockRegression);
        CHECK(result.WaitCount == 0);
        CHECK(pacer.GetNextTarget().Nanoseconds == synthetic.Now + INTERVAL_60HZ_NANOSECONDS);
        // The regressed reading becomes the new baseline; pacing resumes from it.
        CHECK(pacer.Pace(clock).Outcome == PaceOutcome::Waited);
    }
    SECTION("A reading that regresses during a wait")
    {
        synthetic.RegressAfterWaitNanoseconds = 1'000'000;
        const PaceResult result = pacer.Pace(clock);
        CHECK(result.Outcome == PaceOutcome::ClockRegression);
        CHECK(result.WaitCount == 1);
        CHECK(result.Wake.Nanoseconds == EPOCH_NANOSECONDS - 1'000'000);
        CHECK(pacer.GetNextTarget().Nanoseconds == result.Wake.Nanoseconds + INTERVAL_60HZ_NANOSECONDS);
    }
}

TEST_CASE("An unrepresentable target drops the schedule", "[pacing]")
{
    FramePacer pacer = MakePacer();
    SyntheticClock synthetic;
    synthetic.Now = ~uint64{0} - 1;
    const PaceResult result = pacer.Pace(MakeClock(synthetic));
    CHECK(result.Outcome == PaceOutcome::Overflow);
    CHECK(pacer.IsScheduled() == false);
    CHECK(synthetic.WaitCount == 0);
}

TEST_CASE("Lifecycle changes do not produce bursts or inherit a stale phase", "[pacing][lifecycle]")
{
    FramePacer pacer = MakePacer();
    SyntheticClock synthetic;
    const PacingClock clock = MakeClock(synthetic);
    REQUIRE(pacer.Pace(clock).Outcome == PaceOutcome::Baseline);
    for (uint32 frame = 0; frame < 10; ++frame)
    {
        synthetic.Now += FRAME_WORK_NANOSECONDS;
        REQUIRE(pacer.Pace(clock).Outcome == PaceOutcome::Waited);
    }

    SECTION("Suspension with an explicit reset resumes from a baseline")
    {
        // A hidden or suspended window stops calling Pace for 30 s; resume resets.
        synthetic.Now += 30 * time::NANOSECONDS_PER_SECOND;
        pacer.Reset();
        const PaceResult resumed = pacer.Pace(clock);
        CHECK(resumed.Outcome == PaceOutcome::Baseline);
        CHECK(resumed.SkippedTargets == 0);
        const uint64 anchor = synthetic.Now;
        synthetic.Now += FRAME_WORK_NANOSECONDS;
        const PaceResult next = pacer.Pace(clock);
        CHECK(next.Outcome == PaceOutcome::Waited);
        CHECK(next.Wake.Nanoseconds == anchor + INTERVAL_60HZ_NANOSECONDS);
    }
    SECTION("Suspension without a reset is bounded by the lateness policy")
    {
        synthetic.Now += 30 * time::NANOSECONDS_PER_SECOND;
        const PaceResult resumed = pacer.Pace(clock);
        CHECK(resumed.Outcome == PaceOutcome::Rescheduled);
        CHECK(resumed.WaitCount == 0);
        // The ~1800 abandoned targets never run as back-to-back frames.
        CHECK(resumed.SkippedTargets ==
              (30 * time::NANOSECONDS_PER_SECOND - INTERVAL_60HZ_NANOSECONDS) / INTERVAL_60HZ_NANOSECONDS);
        synthetic.Now += FRAME_WORK_NANOSECONDS;
        CHECK(pacer.Pace(clock).Outcome == PaceOutcome::Waited);
    }
    SECTION("Session replacement after a long load starts a fresh schedule")
    {
        // The replacement's 400 ms activation frame is far beyond the bound.
        synthetic.Now += 400'000'000;
        const PaceResult loaded = pacer.Pace(clock);
        CHECK(loaded.Outcome == PaceOutcome::Rescheduled);
        // An explicit reset at activation gives the same zero-wait start.
        pacer.Reset();
        CHECK(pacer.Pace(clock).Outcome == PaceOutcome::Baseline);
        synthetic.Now += FRAME_WORK_NANOSECONDS;
        CHECK(pacer.Pace(clock).Outcome == PaceOutcome::Waited);
    }
    SECTION("Reconfiguring drops the old phase")
    {
        REQUIRE(pacer.Configure({ .Interval = {2 * INTERVAL_60HZ_NANOSECONDS}, .MaximumLateness = {} }) ==
                PacingStatus::Ok);
        const PaceResult result = pacer.Pace(clock);
        CHECK(result.Outcome == PaceOutcome::Baseline);
        CHECK(pacer.GetNextTarget().Nanoseconds == synthetic.Now + 2 * INTERVAL_60HZ_NANOSECONDS);
    }
}

TEST_CASE("The system clock is a stateless native adapter", "[pacing]")
{
    const PacingClock clock = ludus::platform::GetSystemPacingClock();
    CHECK(clock.ReadNowOrNull != nullptr);
    CHECK(clock.WaitForOrNull != nullptr);
    CHECK(clock.ContextOrNull == nullptr);
    // Reads the real clock but never waits: monotonic readings only.
    const Timestamp first = clock.ReadNowOrNull(nullptr);
    const Timestamp second = clock.ReadNowOrNull(nullptr);
    CHECK(second.Nanoseconds >= first.Nanoseconds);
}

TEST_CASE("The system wait never completes before its interval", "[pacing]")
{
    const PacingClock clock = ludus::platform::GetSystemPacingClock();
#if LUDUS_TARGET_OS == LUDUS_OS_MACOS || LUDUS_TARGET_OS == LUDUS_OS_LINUX
    // Lower bounds only: a loaded host may wake late, but a completed wait never wakes early.
    for (uint32 attempt = 0; attempt < SYSTEM_WAIT_ATTEMPTS; ++attempt)
    {
        const Timestamp before = clock.ReadNowOrNull(nullptr);
        const PacingWaitStatus status = clock.WaitForOrNull(nullptr, {SYSTEM_WAIT_NANOSECONDS});
        const Timestamp after = clock.ReadNowOrNull(nullptr);
        REQUIRE((status == PacingWaitStatus::Completed || status == PacingWaitStatus::Interrupted));
        if (status == PacingWaitStatus::Completed)
        {
            CHECK(after.Nanoseconds - before.Nanoseconds >= SYSTEM_WAIT_NANOSECONDS);
        }
    }
#else
    CHECK(clock.WaitForOrNull(nullptr, {SYSTEM_WAIT_NANOSECONDS}) == PacingWaitStatus::Unsupported);
#endif
}
