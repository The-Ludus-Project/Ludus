// Runtime timing telemetry: aggregation over synthetic time and the bounded
// timing record carried on Status (docs/architecture/high-resolution-time.md,
// "Runtime timing telemetry"), and the host pacing policy's hidden rate ("Native
// frame pacing"). No case sleeps: the pacer's injected wait advances a counter,
// so totals and window statistics are exact.

#include "internal/host_pacing.h"
#include "internal/protocol_codec.h"
#include "internal/timing_telemetry.h"

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/platform/frame_pacing.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace ludus::runtime::game_host;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;
using ludus::foundation::time::Duration;
using ludus::foundation::time::FrameClock;
using ludus::foundation::time::FrameSample;
using ludus::foundation::time::Timestamp;
using ludus::foundation::time::TimeStatus;
using ludus::platform::FramePacer;
using ludus::platform::PaceOutcome;
using ludus::platform::PaceResult;
using ludus::platform::PacingClock;
using ludus::platform::PacingStatus;
using ludus::platform::PacingWaitStatus;
using protocol::Message;

namespace
{
/// A large epoch, so arithmetic runs far from zero as on a long-running host.
constexpr uint64 EPOCH_NANOSECONDS = uint64{1} << 62;
/// The host's 60 Hz pacing interval, in whole nanoseconds.
constexpr uint64 INTERVAL_NANOSECONDS = 16'666'667;
/// The host's 10 Hz pacing interval while its window cannot be seen.
constexpr uint64 HIDDEN_INTERVAL_NANOSECONDS = 100'000'000;
/// One millisecond, for frame work and lifecycle offsets.
constexpr uint64 MILLISECOND_NANOSECONDS = 1'000'000;
/// Constant wake lateness of the synthetic operating-system wait.
constexpr uint64 OVERSLEEP_NANOSECONDS = 1'000'000;
/// The host's diagnostic frame clamp.
constexpr uint64 FRAME_DELTA_NANOSECONDS = 250'000'000;
/// Frames recorded by the steady-rate cases: longer than one window.
constexpr uint32 STEADY_FRAMES = 200;

/// Wait that advances synthetic time by the request plus a constant oversleep.
struct SyntheticClock final
{
    uint64 Now = EPOCH_NANOSECONDS;
    uint32 WaitCount = 0;
};

Timestamp ReadSyntheticNow(void* contextOrNull) noexcept
{
    return {static_cast<SyntheticClock*>(contextOrNull)->Now};
}

PacingWaitStatus WaitSynthetic(void* contextOrNull, Duration interval) noexcept
{
    SyntheticClock* clock = static_cast<SyntheticClock*>(contextOrNull);
    clock->Now += interval.Nanoseconds + OVERSLEEP_NANOSECONDS;
    ++clock->WaitCount;
    return PacingWaitStatus::Completed;
}

/// The returned clock advances `inoutClock` on every wait; keep it alive while the clock is used.
[[nodiscard]] PacingClock MakeClock(SyntheticClock& inoutClock) noexcept
{
    return { .ReadNowOrNull = ReadSyntheticNow, .WaitForOrNull = WaitSynthetic, .ContextOrNull = &inoutClock };
}

/// The host loop's order: pace, then sample the frame clock at the frame start.
void RunFrame(FramePacer& inoutPacer,
              FrameClock& inoutFrames,
              TimingTelemetry& inoutTiming,
              const PacingClock& clock,
              const SyntheticClock& syntheticClock)
{
    const PaceResult pace = inoutPacer.Pace(clock);
    inoutTiming.Record(inoutFrames.Sample(Timestamp{syntheticClock.Now}), pace);
}

[[nodiscard]] FramePacer MakeHostPacer()
{
    FramePacer pacer;
    REQUIRE(pacer.Configure({ .Interval = {INTERVAL_NANOSECONDS}, .MaximumLateness = {4 * INTERVAL_NANOSECONDS} }) ==
            PacingStatus::Ok);
    return pacer;
}

[[nodiscard]] Message RoundTrip(const Message& record)
{
    Message parsed;
    REQUIRE(Message::Parse(record.Serialize(), parsed));
    return parsed;
}

[[nodiscard]] TimingReport MakeValidReport() noexcept
{
    TimingReport report;
    report.Frames = 10;
    report.Baselines = 1;
    report.PacingIntervalNanoseconds = INTERVAL_NANOSECONDS;
    report.PaceWaited = 9;
    report.PaceOther = 1;
    report.LastOutcome = PaceOutcome::Waited;
    report.WindowFrames = 9;
    report.WindowIntervalMinimumNanoseconds = INTERVAL_NANOSECONDS;
    report.WindowIntervalMeanNanoseconds = INTERVAL_NANOSECONDS;
    report.WindowIntervalMaximumNanoseconds = INTERVAL_NANOSECONDS + 1;
    report.WindowLatenessMeanNanoseconds = OVERSLEEP_NANOSECONDS;
    report.WindowLatenessMaximumNanoseconds = OVERSLEEP_NANOSECONDS;
    return report;
}

/// Encode a valid report, then replace one field with a raw value.
[[nodiscard]] Message MakeRecordWithUint(std::string_view key, uint64 value)
{
    Message record;
    WriteTimingRecord(MakeValidReport(), record);
    record.SetUint(key, value);
    return record;
}
} // namespace

TEST_CASE("timing telemetry aggregates a paced loop over synthetic time", "[timing]")
{
    SyntheticClock syntheticClock;
    const PacingClock clock = MakeClock(syntheticClock);
    FramePacer pacer = MakeHostPacer();
    FrameClock frames{Duration{FRAME_DELTA_NANOSECONDS}};
    TimingTelemetry timing;
    timing.SetPacingInterval(pacer.GetPolicy().Interval);
    for (uint32 i = 0; i < STEADY_FRAMES; ++i)
    {
        RunFrame(pacer, frames, timing, clock, syntheticClock);
        timing.RecordTick();
    }

    const TimingReport report = timing.GetReport();
    CHECK(report.Frames == STEADY_FRAMES);
    CHECK(report.Baselines == 1);
    CHECK(report.ClockRegressions == 0);
    CHECK(report.SimulationTicks == STEADY_FRAMES);
    CHECK(report.PacingIntervalNanoseconds == INTERVAL_NANOSECONDS);
    CHECK(report.PaceOther == 1); // The anchoring Baseline.
    CHECK(report.PaceWaited == STEADY_FRAMES - 1);
    CHECK(report.PaceLate == 0);
    CHECK(report.PaceRescheduled == 0);
    CHECK(report.LastOutcome == PaceOutcome::Waited);
    // The first paced frame wakes one oversleep after the baseline's next
    // target; every later interval is exactly one pacing interval.
    CHECK(report.AcceptedNanoseconds == (STEADY_FRAMES - 1) * INTERVAL_NANOSECONDS + OVERSLEEP_NANOSECONDS);
    CHECK(report.DiscardedNanoseconds == 0);
    CHECK(report.WindowFrames == TIMING_WINDOW_FRAMES);
    CHECK(report.WindowIntervalMinimumNanoseconds == INTERVAL_NANOSECONDS);
    CHECK(report.WindowIntervalMeanNanoseconds == INTERVAL_NANOSECONDS);
    CHECK(report.WindowIntervalMaximumNanoseconds == INTERVAL_NANOSECONDS);
    CHECK(report.WindowLatenessMeanNanoseconds == OVERSLEEP_NANOSECONDS);
    CHECK(report.WindowLatenessMaximumNanoseconds == OVERSLEEP_NANOSECONDS);
}

TEST_CASE("timing telemetry keeps stalls and regressions visible without a catch-up burst", "[timing]")
{
    SyntheticClock syntheticClock;
    const PacingClock clock = MakeClock(syntheticClock);
    FramePacer pacer = MakeHostPacer();
    FrameClock frames{Duration{FRAME_DELTA_NANOSECONDS}};
    TimingTelemetry timing;
    RunFrame(pacer, frames, timing, clock, syntheticClock);
    RunFrame(pacer, frames, timing, clock, syntheticClock);

    // A one-second stall (a load or debugger stop) between frames.
    constexpr uint64 STALL_NANOSECONDS = 1'000'000'000;
    syntheticClock.Now += STALL_NANOSECONDS;
    RunFrame(pacer, frames, timing, clock, syntheticClock);
    TimingReport report = timing.GetReport();
    CHECK(report.LastOutcome == PaceOutcome::Rescheduled);
    CHECK(report.PaceRescheduled == 1);
    // The stalled frame was due one interval after the previous wake's target.
    constexpr uint64 STALL_LATENESS_NANOSECONDS = STALL_NANOSECONDS + OVERSLEEP_NANOSECONDS - INTERVAL_NANOSECONDS;
    // Rescheduled reports, and abandons rather than runs, the missed targets.
    CHECK(report.SkippedTargets == STALL_LATENESS_NANOSECONDS / INTERVAL_NANOSECONDS);
    CHECK(report.DiscardedNanoseconds == STALL_NANOSECONDS - FRAME_DELTA_NANOSECONDS);
    CHECK(report.WindowIntervalMaximumNanoseconds == STALL_NANOSECONDS);
    CHECK(report.WindowLatenessMaximumNanoseconds == STALL_LATENESS_NANOSECONDS);

    // The next frame waits a full interval from the re-anchored schedule.
    RunFrame(pacer, frames, timing, clock, syntheticClock);
    report = timing.GetReport();
    CHECK(report.LastOutcome == PaceOutcome::Waited);

    // A sample below the frame clock's previous reading adds no interval.
    const uint64 framesBefore = report.WindowFrames;
    timing.Record(frames.Sample(Timestamp{EPOCH_NANOSECONDS}), PaceResult{});
    report = timing.GetReport();
    CHECK(report.ClockRegressions == 1);
    CHECK(report.WindowFrames == framesBefore);
    CHECK(report.LastOutcome == PaceOutcome::Disabled);
    CHECK(report.PaceOther == 2); // The first Baseline and the Disabled result.

    // A lifecycle reset counts a new baseline and still adds no interval.
    frames.Reset();
    timing.Record(frames.Sample(Timestamp{syntheticClock.Now}), PaceResult{});
    CHECK(timing.GetReport().Baselines == 2);
    CHECK(timing.GetReport().WindowFrames == framesBefore);
}

TEST_CASE("timing telemetry saturates totals at the editor's exact integer range", "[timing][overflow]")
{
    TimingTelemetry timing;
    FrameSample huge;
    huge.Elapsed = Duration{MAXIMUM_TIMING_VALUE};
    huge.Discarded = Duration{~uint64{0}};
    PaceResult pace;
    pace.Outcome = PaceOutcome::Late;
    pace.Lateness = Duration{~uint64{0}};
    pace.SkippedTargets = ~uint64{0};
    pace.EarlyWakeCount = ~uint32{0};
    timing.Record(huge, pace);
    timing.Record(huge, pace);
    timing.SetPacingInterval(Duration{~uint64{0}});

    const TimingReport report = timing.GetReport();
    CHECK(report.AcceptedNanoseconds == MAXIMUM_TIMING_VALUE);
    CHECK(report.DiscardedNanoseconds == MAXIMUM_TIMING_VALUE);
    CHECK(report.SkippedTargets == MAXIMUM_TIMING_VALUE);
    CHECK(report.EarlyWakes == uint64{2} * ~uint32{0});
    CHECK(report.PacingIntervalNanoseconds == MAXIMUM_TIMING_VALUE);
    CHECK(report.WindowFrames == 2);
    CHECK(report.WindowIntervalMaximumNanoseconds == MAXIMUM_TIMING_VALUE);
    CHECK(report.WindowIntervalMeanNanoseconds == MAXIMUM_TIMING_VALUE);
    CHECK(report.WindowLatenessMaximumNanoseconds == MAXIMUM_TIMING_VALUE);

    Message record;
    WriteTimingRecord(report, record);
    TimingReport decoded;
    REQUIRE(ReadTimingRecord(RoundTrip(record), decoded));
    CHECK(decoded.AcceptedNanoseconds == MAXIMUM_TIMING_VALUE);
    CHECK(decoded.SkippedTargets == MAXIMUM_TIMING_VALUE);

    // An unsaturated report is clamped by the encoder, never sent above range.
    TimingReport unsaturated = MakeValidReport();
    unsaturated.AcceptedNanoseconds = ~uint64{0};
    Message clamped;
    WriteTimingRecord(unsaturated, clamped);
    REQUIRE(ReadTimingRecord(RoundTrip(clamped), decoded));
    CHECK(decoded.AcceptedNanoseconds == MAXIMUM_TIMING_VALUE);
}

TEST_CASE("timing records round-trip and reject out-of-contract values", "[timing][protocol]")
{
    const TimingReport original = MakeValidReport();
    Message record;
    WriteTimingRecord(original, record);
    CHECK(record.FieldCount() == 21);

    // Carried as a one-record array on a Status result, as the host sends it.
    Message status;
    status.SetString("event", "CommandResult");
    status.SetRecords(TIMING_RECORD_KEY, {&record, 1});
    const Message parsedStatus = RoundTrip(status);
    ludus::foundation::Array<Message> records;
    REQUIRE(parsedStatus.GetRecords(TIMING_RECORD_KEY, records));
    REQUIRE(records.GetSize() == 1);
    TimingReport decoded;
    REQUIRE(ReadTimingRecord(records[0], decoded));
    CHECK(decoded.Frames == original.Frames);
    CHECK(decoded.PaceWaited == original.PaceWaited);
    CHECK(decoded.LastOutcome == PaceOutcome::Waited);
    CHECK(decoded.WindowIntervalMaximumNanoseconds == original.WindowIntervalMaximumNanoseconds);

    TimingReport preserved = MakeValidReport();
    preserved.Frames = 77;
    const auto expectRejected = [&preserved](const Message& candidate) {
        CHECK_FALSE(ReadTimingRecord(RoundTrip(candidate), preserved));
        CHECK(preserved.Frames == 77);
    };
    expectRejected(MakeRecordWithUint("frames", MAXIMUM_TIMING_VALUE + 1));
    expectRejected(MakeRecordWithUint("accepted_ns", ~uint64{0}));
    expectRejected(MakeRecordWithUint("window_frames", TIMING_WINDOW_FRAMES + 1));
    expectRejected(MakeRecordWithUint("window_frames", 11)); // More than the 10 recorded frames.
    expectRejected(MakeRecordWithUint("window_interval_min_ns", INTERVAL_NANOSECONDS + 2));
    expectRejected(MakeRecordWithUint("window_interval_mean_ns", INTERVAL_NANOSECONDS + 2));
    expectRejected(MakeRecordWithUint("window_lateness_mean_ns", OVERSLEEP_NANOSECONDS + 1));
    expectRejected(MakeRecordWithUint("surprise", 1));

    Message unknownOutcome;
    WriteTimingRecord(original, unknownOutcome);
    unknownOutcome.SetString("last_outcome", "Sleeping");
    expectRejected(unknownOutcome);

    Message wrongType;
    WriteTimingRecord(original, wrongType);
    wrongType.SetString("frames", "10");
    expectRejected(wrongType);

    Message negative;
    WriteTimingRecord(original, negative);
    negative.SetInt("pace_late", -1);
    expectRejected(negative);

    Message missing;
    for (const std::string_view key : {"frames", "baselines"})
    {
        missing.SetUint(key, 1);
    }
    expectRejected(missing);
}

TEST_CASE("every pace outcome has a wire name that parses back", "[timing][protocol]")
{
    for (const PaceOutcome outcome : {PaceOutcome::Disabled,
                                      PaceOutcome::InvalidClock,
                                      PaceOutcome::Baseline,
                                      PaceOutcome::Waited,
                                      PaceOutcome::Late,
                                      PaceOutcome::Rescheduled,
                                      PaceOutcome::WaitFailed,
                                      PaceOutcome::WaitLimit,
                                      PaceOutcome::ClockRegression,
                                      PaceOutcome::Overflow})
    {
        PaceOutcome parsed = PaceOutcome::Disabled;
        REQUIRE(TryParsePaceOutcome(PaceOutcomeName(outcome), parsed));
        CHECK(parsed == outcome);
    }
    PaceOutcome unchanged = PaceOutcome::Late;
    CHECK_FALSE(TryParsePaceOutcome("waited", unchanged));
    CHECK(unchanged == PaceOutcome::Late);
}

TEST_CASE("host pacing waits at the hidden rate while the window cannot be seen", "[timing][pacing]")
{
    SyntheticClock syntheticClock;
    const PacingClock clock = MakeClock(syntheticClock);
    HostPacing pacing;
    CHECK(pacing.IsVisible() == true);
    CHECK(pacing.GetInterval().Nanoseconds == INTERVAL_NANOSECONDS);
    CHECK(pacing.GetInterval().Nanoseconds == HOST_FRAME_INTERVAL_NANOSECONDS);
    CHECK(pacing.GetPacer().GetPolicy().MaximumLateness.Nanoseconds == 4 * INTERVAL_NANOSECONDS);
    CHECK(pacing.GetPacer().IsScheduled() == false);

    // Visible: one simulated second of 4 ms frames waits for 60 targets on the anchor's grid.
    CHECK(pacing.Pace(clock).Outcome == PaceOutcome::Baseline);
    const uint64 visibleAnchor = syntheticClock.Now;
    for (uint64 frame = 1; frame <= 60; ++frame)
    {
        syntheticClock.Now += 4 * MILLISECOND_NANOSECONDS;
        const PaceResult result = pacing.Pace(clock);
        CHECK(result.Outcome == PaceOutcome::Waited);
        CHECK(result.Target.Nanoseconds == visibleAnchor + frame * INTERVAL_NANOSECONDS);
    }
    CHECK(syntheticClock.WaitCount == 60);

    // Hiding selects the 10 Hz interval and drops the schedule: the next frame starts at once.
    pacing.SetVisible(false);
    CHECK(pacing.IsVisible() == false);
    CHECK(pacing.GetInterval().Nanoseconds == HIDDEN_INTERVAL_NANOSECONDS);
    CHECK(pacing.GetInterval().Nanoseconds == HOST_HIDDEN_FRAME_INTERVAL_NANOSECONDS);
    CHECK(pacing.GetPacer().GetPolicy().MaximumLateness.Nanoseconds == 4 * HIDDEN_INTERVAL_NANOSECONDS);
    CHECK(pacing.GetPacer().IsScheduled() == false);
    syntheticClock.WaitCount = 0;
    const uint64 hideReading = syntheticClock.Now;
    const PaceResult firstHidden = pacing.Pace(clock);
    CHECK(firstHidden.Outcome == PaceOutcome::Baseline);
    CHECK(firstHidden.Wake.Nanoseconds == hideReading);
    CHECK(syntheticClock.WaitCount == 0);

    // Hidden: one simulated second waits exactly ten times, on anchor + k * 100 ms.
    const uint64 hiddenAnchor = syntheticClock.Now;
    for (uint64 frame = 1; frame <= 10; ++frame)
    {
        syntheticClock.Now += 4 * MILLISECOND_NANOSECONDS;
        const PaceResult result = pacing.Pace(clock);
        CHECK(result.Outcome == PaceOutcome::Waited);
        CHECK(result.Target.Nanoseconds == hiddenAnchor + frame * HIDDEN_INTERVAL_NANOSECONDS);
    }
    CHECK(syntheticClock.WaitCount == 10);
    CHECK(syntheticClock.Now - hiddenAnchor == 10 * HIDDEN_INTERVAL_NANOSECONDS + OVERSLEEP_NANOSECONDS);

    // Repeating the current visibility keeps the schedule.
    pacing.SetVisible(false);
    CHECK(pacing.GetPacer().IsScheduled() == true);
    CHECK(pacing.GetInterval().Nanoseconds == HIDDEN_INTERVAL_NANOSECONDS);

    // Showing the window 30 ms into a hidden interval starts the next frame at once instead of
    // waiting out the old 100 ms target, then follows the 60 Hz grid anchored at the reveal.
    syntheticClock.Now += 30 * MILLISECOND_NANOSECONDS;
    pacing.SetVisible(true);
    CHECK(pacing.GetInterval().Nanoseconds == INTERVAL_NANOSECONDS);
    CHECK(pacing.GetPacer().IsScheduled() == false);
    syntheticClock.WaitCount = 0;
    const uint64 showReading = syntheticClock.Now;
    const PaceResult firstShown = pacing.Pace(clock);
    CHECK(firstShown.Outcome == PaceOutcome::Baseline);
    CHECK(firstShown.Wake.Nanoseconds == showReading);
    CHECK(syntheticClock.WaitCount == 0);
    const PaceResult secondShown = pacing.Pace(clock);
    CHECK(secondShown.Outcome == PaceOutcome::Waited);
    CHECK(secondShown.Target.Nanoseconds == showReading + INTERVAL_NANOSECONDS);

    // Reset drops the schedule and keeps the interval.
    pacing.Reset();
    CHECK(pacing.GetPacer().IsScheduled() == false);
    CHECK(pacing.GetInterval().Nanoseconds == INTERVAL_NANOSECONDS);
    CHECK(pacing.Pace(clock).Outcome == PaceOutcome::Baseline);
}

TEST_CASE("timing telemetry reports the active pacing interval while hidden", "[timing][pacing]")
{
    SyntheticClock syntheticClock;
    const PacingClock clock = MakeClock(syntheticClock);
    HostPacing pacing;
    FrameClock frames{Duration{FRAME_DELTA_NANOSECONDS}};
    TimingTelemetry timing;

    // The host loop's order: pace and sample, then apply the visibility pumped by the frame.
    const auto runFrame = [&](bool isVisible) {
        const PaceResult pace = pacing.Pace(clock);
        timing.Record(frames.Sample(Timestamp{syntheticClock.Now}), pace);
        pacing.SetVisible(isVisible);
        timing.SetPacingInterval(pacing.GetInterval());
    };
    for (uint32 i = 0; i < 5; ++i)
    {
        runFrame(true);
    }
    CHECK(timing.GetReport().PacingIntervalNanoseconds == INTERVAL_NANOSECONDS);

    // The frame that observes the hidden window reports the hidden interval; the next frame is the
    // hidden schedule's baseline and the one after it waits for 100 ms.
    runFrame(false);
    CHECK(timing.GetReport().PacingIntervalNanoseconds == HIDDEN_INTERVAL_NANOSECONDS);
    runFrame(false);
    CHECK(timing.GetReport().LastOutcome == PaceOutcome::Baseline);
    runFrame(false);
    TimingReport report = timing.GetReport();
    CHECK(report.LastOutcome == PaceOutcome::Waited);
    CHECK(report.WindowIntervalMaximumNanoseconds == HIDDEN_INTERVAL_NANOSECONDS + OVERSLEEP_NANOSECONDS);
    CHECK(report.PacingIntervalNanoseconds == HIDDEN_INTERVAL_NANOSECONDS);

    // Showing the window restores the 60 Hz interval in the same frame.
    runFrame(true);
    report = timing.GetReport();
    CHECK(report.PacingIntervalNanoseconds == INTERVAL_NANOSECONDS);
    CHECK(report.Baselines == 1); // Visibility changes drop the pacing schedule, not the frame clock.
}
