#pragma once

// View model for the editor Timing panel. Reads the runtime timing record that
// GameHost attaches to a Status result and turns it into display rows. The
// contract is owned by docs/architecture/high-resolution-time.md, "Runtime
// timing telemetry": every value is an interval or count computed by the
// runtime. The editor never subtracts its own clock readings from runtime
// readings, and nothing here reads an editor clock.
//
// Private editor header (not installed). Pure functions over QJson values, so
// tests run without a game host, widgets or a timer.

#include <ludus/foundation/base/types.h>

#include <QJsonObject>
#include <QList>
#include <QString>

namespace ludus::editor
{
/// Window length GameHost reports (TIMING_WINDOW_FRAMES in
/// modules/runtime/game_host/src/internal/timing_telemetry.h). The editor does
/// not link GameHost, so the protocol value is mirrored here and validated.
inline constexpr foundation::uint64 TIMING_WINDOW_FRAMES = 120;

/// Largest timing value on the wire: 2^53 - 1, exact in Qt's binary64 JSON
/// numbers. GameHost saturates totals here (MAXIMUM_TIMING_VALUE).
inline constexpr foundation::uint64 MAXIMUM_TIMING_VALUE = (foundation::uint64{1} << 53U) - 1U;

/// One decoded runtime timing record. Field meanings match GameHost's TimingReport.
struct TimingSnapshot final
{
    /// Frames recorded since the session started, including baselines and regressions.
    foundation::uint64 Frames = 0;
    /// Frame-clock baselines: startup and lifecycle resets such as a module reload.
    foundation::uint64 Baselines = 0;
    /// Frame samples that read below the previous runtime reading.
    foundation::uint64 ClockRegressions = 0;
    /// Total accepted frame time, in nanoseconds.
    foundation::uint64 AcceptedNanoseconds = 0;
    /// Total frame time beyond the runtime's frame clamp, in nanoseconds.
    foundation::uint64 DiscardedNanoseconds = 0;
    /// Simulation ticks advanced since the session started.
    foundation::uint64 SimulationTicks = 0;
    /// The runtime's pacing interval in nanoseconds; zero when it does not pace.
    foundation::uint64 PacingIntervalNanoseconds = 0;
    /// Frames whose pacer waited for the target.
    foundation::uint64 PaceWaited = 0;
    /// Frames that started late, within the lateness bound.
    foundation::uint64 PaceLate = 0;
    /// Frames later than the lateness bound; the runtime re-anchored its schedule.
    foundation::uint64 PaceRescheduled = 0;
    /// Frames that started early because a wait failed or kept returning early.
    foundation::uint64 PaceWaitFailed = 0;
    /// Baseline and fault outcomes.
    foundation::uint64 PaceOther = 0;
    /// Missed pacing targets skipped or abandoned instead of run back to back.
    foundation::uint64 SkippedTargets = 0;
    /// Waits that returned before their target.
    foundation::uint64 EarlyWakes = 0;
    /// PaceOutcome name of the latest frame.
    QString LastOutcome;
    /// Measured frames in the rolling window, at most TIMING_WINDOW_FRAMES.
    foundation::uint64 WindowFrames = 0;
    /// Shortest frame interval in the window, in nanoseconds.
    foundation::uint64 WindowIntervalMinimumNanoseconds = 0;
    /// Mean frame interval in the window, in nanoseconds.
    foundation::uint64 WindowIntervalMeanNanoseconds = 0;
    /// Longest frame interval in the window, in nanoseconds.
    foundation::uint64 WindowIntervalMaximumNanoseconds = 0;
    /// Mean pacing lateness in the window, in nanoseconds.
    foundation::uint64 WindowLatenessMeanNanoseconds = 0;
    /// Largest pacing lateness in the window, in nanoseconds.
    foundation::uint64 WindowLatenessMaximumNanoseconds = 0;
};

/// One label/value line of the Timing panel.
struct TimingRow final
{
    QString Label;
    QString Value;
};

/// Decode the `timing` record of a host Status result.
///
/// The record must be a one-element array holding an object with exactly the
/// GameHost fields: non-negative integers no larger than MAXIMUM_TIMING_VALUE,
/// a known outcome name, at most TIMING_WINDOW_FRAMES window frames (and no more
/// than Frames), and ordered window statistics.
/// @param status The `host` object of a Status result.
/// @param outSnapshot Replaced on success; unchanged on failure.
/// @param outError Set to a short reason on failure; unchanged on success.
/// @return false when the record is absent or violates the contract.
[[nodiscard]] bool ReadTimingSnapshot(const QJsonObject& status, TimingSnapshot& outSnapshot, QString& outError);

/// Format a snapshot for display. Values are runtime intervals and counts; the
/// only derived values are unit conversions and the pacing rate (one second
/// divided by the runtime's interval).
[[nodiscard]] QList<TimingRow> MakeTimingRows(const TimingSnapshot& snapshot);
} // namespace ludus::editor
