#pragma once

// Runtime timing telemetry for one play session. The contract (what is
// measured, the rolling window, saturation and the rule that only intervals and
// counts cross the process boundary) is owned by
// docs/architecture/high-resolution-time.md, "Runtime timing telemetry".
//
// The host records one frame per loop iteration on its own thread. Recording is
// allocation-free and constant time; a report is computed only when the editor
// asks for Status. No absolute timestamp is retained or reported: the editor
// must never subtract its own clock readings from runtime readings.
#include "internal/protocol_codec.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/platform/frame_pacing.h>

#include <string_view>

namespace ludus::runtime::game_host
{
/// Frames kept in the rolling window: two seconds at the host's 60 Hz pacing.
inline constexpr ludus::foundation::usize TIMING_WINDOW_FRAMES = 120;

/// Largest value the timing record carries: 2^53 - 1, the largest integer a
/// binary64 JSON reader (the editor's Qt reader) represents exactly. Totals
/// saturate here instead of wrapping or losing precision on the editor side.
inline constexpr ludus::foundation::uint64 MAXIMUM_TIMING_VALUE = (ludus::foundation::uint64{1} << 53U) - 1U;

/// Key of the one-record array that carries the report on a Status result.
inline constexpr std::string_view TIMING_RECORD_KEY = "timing";

/// Runtime-computed timing for one session. Every field is a count or an
/// interval in nanoseconds; none is an absolute timestamp. Totals cover the
/// whole session and saturate at MAXIMUM_TIMING_VALUE. Window fields cover the
/// most recent measured frames, at most TIMING_WINDOW_FRAMES.
struct TimingReport final
{
    /// Frames recorded, including baselines and clock regressions.
    ludus::foundation::uint64 Frames = 0;
    /// Frames that established a new FrameClock baseline (startup and lifecycle resets).
    ludus::foundation::uint64 Baselines = 0;
    /// Frame samples that read below the frame clock's previous reading.
    ludus::foundation::uint64 ClockRegressions = 0;
    /// Sum of accepted frame intervals (at most the frame clamp each).
    ludus::foundation::uint64 AcceptedNanoseconds = 0;
    /// Sum of interval time beyond the frame clamp.
    ludus::foundation::uint64 DiscardedNanoseconds = 0;
    /// Simulation ticks advanced while recording.
    ludus::foundation::uint64 SimulationTicks = 0;
    /// The pacer's configured interval between frame starts; zero when pacing is disabled.
    ludus::foundation::uint64 PacingIntervalNanoseconds = 0;
    /// PaceOutcome::Waited frames.
    ludus::foundation::uint64 PaceWaited = 0;
    /// PaceOutcome::Late frames: started late within the lateness bound.
    ludus::foundation::uint64 PaceLate = 0;
    /// PaceOutcome::Rescheduled frames: later than the lateness bound.
    ludus::foundation::uint64 PaceRescheduled = 0;
    /// PaceOutcome::WaitFailed and WaitLimit frames: started early without a completed wait.
    ludus::foundation::uint64 PaceWaitFailed = 0;
    /// Baseline, Disabled, InvalidClock, ClockRegression and Overflow outcomes.
    ludus::foundation::uint64 PaceOther = 0;
    /// Sum of PaceResult::SkippedTargets.
    ludus::foundation::uint64 SkippedTargets = 0;
    /// Sum of PaceResult::EarlyWakeCount.
    ludus::foundation::uint64 EarlyWakes = 0;
    /// Outcome of the latest recorded frame; Disabled before the first frame.
    platform::PaceOutcome LastOutcome = platform::PaceOutcome::Disabled;
    /// Measured frames in the window. Baselines and regressions carry no interval and are excluded.
    ludus::foundation::uint64 WindowFrames = 0;
    /// Shortest raw frame interval (accepted plus discarded) in the window.
    ludus::foundation::uint64 WindowIntervalMinimumNanoseconds = 0;
    /// Mean raw frame interval in the window, rounded down.
    ludus::foundation::uint64 WindowIntervalMeanNanoseconds = 0;
    /// Longest raw frame interval in the window.
    ludus::foundation::uint64 WindowIntervalMaximumNanoseconds = 0;
    /// Mean pacing lateness (wake minus target) of the window's frames, rounded down.
    ludus::foundation::uint64 WindowLatenessMeanNanoseconds = 0;
    /// Largest pacing lateness in the window.
    ludus::foundation::uint64 WindowLatenessMaximumNanoseconds = 0;
};

/// Owner-local, allocation-free timing aggregation for one host session.
///
/// Record() runs once per host frame with the values the frame loop already
/// produced, so tests drive it with synthetic samples and pace results. The
/// value has no synchronization; the host thread owns it.
class TimingTelemetry final
{
public:
    /// Record one frame start: its FrameClock sample and the PaceResult that preceded it.
    void Record(const foundation::time::FrameSample& sample, const platform::PaceResult& pace) noexcept;
    /// Count one advanced simulation tick.
    void RecordTick() noexcept;
    /// Set the pacing interval reported with the totals.
    void SetPacingInterval(foundation::time::Duration interval) noexcept;
    /// Compute the session totals and the current window.
    [[nodiscard]] TimingReport GetReport() const noexcept;

private:
    struct WindowFrame final
    {
        ludus::foundation::uint64 IntervalNanoseconds = 0;
        ludus::foundation::uint64 LatenessNanoseconds = 0;
    };
    ludus::foundation::StaticArray<WindowFrame, TIMING_WINDOW_FRAMES> mWindow{};
    ludus::foundation::usize mWindowNext = 0;
    ludus::foundation::usize mWindowSize = 0;
    TimingReport mTotals;
};

/// Return the wire name of a pace outcome (the PaceOutcome enumerator spelling).
[[nodiscard]] std::string_view PaceOutcomeName(platform::PaceOutcome outcome) noexcept;

/// Parse a wire pace outcome name.
/// @return false for an unknown name; outOutcome is then unchanged.
[[nodiscard]] bool TryParsePaceOutcome(std::string_view name, platform::PaceOutcome& outOutcome) noexcept;

/// Encode a report as one flat protocol record. Values above MAXIMUM_TIMING_VALUE are written saturated.
void WriteTimingRecord(const TimingReport& report, protocol::Message& outRecord);

/// Decode and validate one timing record.
///
/// Rejects missing or unknown fields, values above MAXIMUM_TIMING_VALUE, an
/// unknown outcome, more window frames than TIMING_WINDOW_FRAMES or than frames,
/// and window statistics that are not ordered minimum <= mean <= maximum.
/// @return false on any violation; outReport is then unchanged.
[[nodiscard]] bool ReadTimingRecord(const protocol::Message& record, TimingReport& outReport);
} // namespace ludus::runtime::game_host
