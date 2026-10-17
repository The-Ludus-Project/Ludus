#include "internal/timing_telemetry.h"

#include <ludus/foundation/base/checked_integer.hpp>

#include <string>

namespace ludus::runtime::game_host
{
namespace
{
using ludus::foundation::uint64;
using ludus::foundation::usize;
using platform::PaceOutcome;

/// Enumerators of platform::PaceOutcome, Disabled through Overflow.
constexpr usize PACE_OUTCOME_COUNT = 10;

/// Wire field names, in record order. ReadTimingRecord requires exactly these keys.
constexpr std::string_view FRAMES_KEY = "frames";
constexpr std::string_view BASELINES_KEY = "baselines";
constexpr std::string_view CLOCK_REGRESSIONS_KEY = "clock_regressions";
constexpr std::string_view ACCEPTED_KEY = "accepted_ns";
constexpr std::string_view DISCARDED_KEY = "discarded_ns";
constexpr std::string_view SIMULATION_TICKS_KEY = "simulation_ticks";
constexpr std::string_view PACING_INTERVAL_KEY = "pacing_interval_ns";
constexpr std::string_view PACE_WAITED_KEY = "pace_waited";
constexpr std::string_view PACE_LATE_KEY = "pace_late";
constexpr std::string_view PACE_RESCHEDULED_KEY = "pace_rescheduled";
constexpr std::string_view PACE_WAIT_FAILED_KEY = "pace_wait_failed";
constexpr std::string_view PACE_OTHER_KEY = "pace_other";
constexpr std::string_view SKIPPED_TARGETS_KEY = "skipped_targets";
constexpr std::string_view EARLY_WAKES_KEY = "early_wakes";
constexpr std::string_view LAST_OUTCOME_KEY = "last_outcome";
constexpr std::string_view WINDOW_FRAMES_KEY = "window_frames";
constexpr std::string_view WINDOW_INTERVAL_MINIMUM_KEY = "window_interval_min_ns";
constexpr std::string_view WINDOW_INTERVAL_MEAN_KEY = "window_interval_mean_ns";
constexpr std::string_view WINDOW_INTERVAL_MAXIMUM_KEY = "window_interval_max_ns";
constexpr std::string_view WINDOW_LATENESS_MEAN_KEY = "window_lateness_mean_ns";
constexpr std::string_view WINDOW_LATENESS_MAXIMUM_KEY = "window_lateness_max_ns";

/// Add `amount` to a total, saturating at MAXIMUM_TIMING_VALUE.
void AddSaturated(uint64& inoutTotal, uint64 amount) noexcept
{
    uint64 sum = 0;
    if (ludus::foundation::TryAdd(inoutTotal, amount, sum) == false || sum > MAXIMUM_TIMING_VALUE)
    {
        sum = MAXIMUM_TIMING_VALUE;
    }
    inoutTotal = sum;
}

[[nodiscard]] uint64 Saturate(uint64 value) noexcept
{
    return value > MAXIMUM_TIMING_VALUE ? MAXIMUM_TIMING_VALUE : value;
}

[[nodiscard]] bool ReadBounded(const protocol::Message& record, std::string_view key, uint64& outValue)
{
    uint64 value = 0;
    if (record.GetUint(key, value) == false || value > MAXIMUM_TIMING_VALUE)
    {
        return false;
    }
    outValue = value;
    return true;
}

[[nodiscard]] bool IsOrdered(uint64 minimum, uint64 mean, uint64 maximum) noexcept
{
    return minimum <= mean && mean <= maximum;
}
} // namespace

void TimingTelemetry::Record(const foundation::time::FrameSample& sample, const platform::PaceResult& pace) noexcept
{
    AddSaturated(mTotals.Frames, 1);
    switch (pace.Outcome)
    {
        case PaceOutcome::Waited:
            AddSaturated(mTotals.PaceWaited, 1);
            break;
        case PaceOutcome::Late:
            AddSaturated(mTotals.PaceLate, 1);
            break;
        case PaceOutcome::Rescheduled:
            AddSaturated(mTotals.PaceRescheduled, 1);
            break;
        case PaceOutcome::WaitFailed:
        case PaceOutcome::WaitLimit:
            AddSaturated(mTotals.PaceWaitFailed, 1);
            break;
        case PaceOutcome::Disabled:
        case PaceOutcome::InvalidClock:
        case PaceOutcome::Baseline:
        case PaceOutcome::ClockRegression:
        case PaceOutcome::Overflow:
            AddSaturated(mTotals.PaceOther, 1);
            break;
    }
    AddSaturated(mTotals.SkippedTargets, pace.SkippedTargets);
    AddSaturated(mTotals.EarlyWakes, pace.EarlyWakeCount);
    mTotals.LastOutcome = pace.Outcome;

    if (sample.IsBaseline == true)
    {
        AddSaturated(mTotals.Baselines, 1);
        return;
    }
    if (sample.Status != foundation::time::TimeStatus::Ok)
    {
        AddSaturated(mTotals.ClockRegressions, 1);
        return;
    }
    AddSaturated(mTotals.AcceptedNanoseconds, sample.Elapsed.Nanoseconds);
    AddSaturated(mTotals.DiscardedNanoseconds, sample.Discarded.Nanoseconds);
    uint64 interval = 0;
    if (ludus::foundation::TryAdd(sample.Elapsed.Nanoseconds, sample.Discarded.Nanoseconds, interval) == false)
    {
        interval = MAXIMUM_TIMING_VALUE;
    }
    WindowFrame& slot = mWindow[mWindowNext];
    slot.IntervalNanoseconds = Saturate(interval);
    slot.LatenessNanoseconds = Saturate(pace.Lateness.Nanoseconds);
    mWindowNext = (mWindowNext + 1) % TIMING_WINDOW_FRAMES;
    if (mWindowSize < TIMING_WINDOW_FRAMES)
    {
        ++mWindowSize;
    }
}

void TimingTelemetry::RecordTick() noexcept
{
    AddSaturated(mTotals.SimulationTicks, 1);
}

void TimingTelemetry::SetPacingInterval(foundation::time::Duration interval) noexcept
{
    mTotals.PacingIntervalNanoseconds = Saturate(interval.Nanoseconds);
}

TimingReport TimingTelemetry::GetReport() const noexcept
{
    TimingReport report = mTotals;
    report.WindowFrames = mWindowSize;
    if (mWindowSize == 0)
    {
        return report;
    }
    // Each stored value is at most 2^53 - 1 and the window holds at most 120,
    // so neither sum can overflow uint64.
    uint64 intervalSum = 0;
    uint64 latenessSum = 0;
    uint64 intervalMinimum = MAXIMUM_TIMING_VALUE;
    uint64 intervalMaximum = 0;
    uint64 latenessMaximum = 0;
    for (usize i = 0; i < mWindowSize; ++i)
    {
        const WindowFrame& frame = mWindow[i];
        intervalSum += frame.IntervalNanoseconds;
        latenessSum += frame.LatenessNanoseconds;
        intervalMinimum = frame.IntervalNanoseconds < intervalMinimum ? frame.IntervalNanoseconds : intervalMinimum;
        intervalMaximum = frame.IntervalNanoseconds > intervalMaximum ? frame.IntervalNanoseconds : intervalMaximum;
        latenessMaximum = frame.LatenessNanoseconds > latenessMaximum ? frame.LatenessNanoseconds : latenessMaximum;
    }
    report.WindowIntervalMinimumNanoseconds = intervalMinimum;
    report.WindowIntervalMeanNanoseconds = intervalSum / mWindowSize;
    report.WindowIntervalMaximumNanoseconds = intervalMaximum;
    report.WindowLatenessMeanNanoseconds = latenessSum / mWindowSize;
    report.WindowLatenessMaximumNanoseconds = latenessMaximum;
    return report;
}

std::string_view PaceOutcomeName(PaceOutcome outcome) noexcept
{
    switch (outcome)
    {
        case PaceOutcome::Disabled:
            return "Disabled";
        case PaceOutcome::InvalidClock:
            return "InvalidClock";
        case PaceOutcome::Baseline:
            return "Baseline";
        case PaceOutcome::Waited:
            return "Waited";
        case PaceOutcome::Late:
            return "Late";
        case PaceOutcome::Rescheduled:
            return "Rescheduled";
        case PaceOutcome::WaitFailed:
            return "WaitFailed";
        case PaceOutcome::WaitLimit:
            return "WaitLimit";
        case PaceOutcome::ClockRegression:
            return "ClockRegression";
        case PaceOutcome::Overflow:
            return "Overflow";
    }
    return "Disabled";
}

bool TryParsePaceOutcome(std::string_view name, PaceOutcome& outOutcome) noexcept
{
    constexpr ludus::foundation::StaticArray<PaceOutcome, PACE_OUTCOME_COUNT> OUTCOMES = {
        PaceOutcome::Disabled,
        PaceOutcome::InvalidClock,
        PaceOutcome::Baseline,
        PaceOutcome::Waited,
        PaceOutcome::Late,
        PaceOutcome::Rescheduled,
        PaceOutcome::WaitFailed,
        PaceOutcome::WaitLimit,
        PaceOutcome::ClockRegression,
        PaceOutcome::Overflow,
    };
    for (const PaceOutcome outcome : OUTCOMES)
    {
        if (PaceOutcomeName(outcome) == name)
        {
            outOutcome = outcome;
            return true;
        }
    }
    return false;
}

void WriteTimingRecord(const TimingReport& report, protocol::Message& outRecord)
{
    outRecord.SetUint(FRAMES_KEY, Saturate(report.Frames));
    outRecord.SetUint(BASELINES_KEY, Saturate(report.Baselines));
    outRecord.SetUint(CLOCK_REGRESSIONS_KEY, Saturate(report.ClockRegressions));
    outRecord.SetUint(ACCEPTED_KEY, Saturate(report.AcceptedNanoseconds));
    outRecord.SetUint(DISCARDED_KEY, Saturate(report.DiscardedNanoseconds));
    outRecord.SetUint(SIMULATION_TICKS_KEY, Saturate(report.SimulationTicks));
    outRecord.SetUint(PACING_INTERVAL_KEY, Saturate(report.PacingIntervalNanoseconds));
    outRecord.SetUint(PACE_WAITED_KEY, Saturate(report.PaceWaited));
    outRecord.SetUint(PACE_LATE_KEY, Saturate(report.PaceLate));
    outRecord.SetUint(PACE_RESCHEDULED_KEY, Saturate(report.PaceRescheduled));
    outRecord.SetUint(PACE_WAIT_FAILED_KEY, Saturate(report.PaceWaitFailed));
    outRecord.SetUint(PACE_OTHER_KEY, Saturate(report.PaceOther));
    outRecord.SetUint(SKIPPED_TARGETS_KEY, Saturate(report.SkippedTargets));
    outRecord.SetUint(EARLY_WAKES_KEY, Saturate(report.EarlyWakes));
    outRecord.SetString(LAST_OUTCOME_KEY, PaceOutcomeName(report.LastOutcome));
    outRecord.SetUint(WINDOW_FRAMES_KEY, Saturate(report.WindowFrames));
    outRecord.SetUint(WINDOW_INTERVAL_MINIMUM_KEY, Saturate(report.WindowIntervalMinimumNanoseconds));
    outRecord.SetUint(WINDOW_INTERVAL_MEAN_KEY, Saturate(report.WindowIntervalMeanNanoseconds));
    outRecord.SetUint(WINDOW_INTERVAL_MAXIMUM_KEY, Saturate(report.WindowIntervalMaximumNanoseconds));
    outRecord.SetUint(WINDOW_LATENESS_MEAN_KEY, Saturate(report.WindowLatenessMeanNanoseconds));
    outRecord.SetUint(WINDOW_LATENESS_MAXIMUM_KEY, Saturate(report.WindowLatenessMaximumNanoseconds));
}

bool ReadTimingRecord(const protocol::Message& record, TimingReport& outReport)
{
    if (record.HasOnly({FRAMES_KEY,
                        BASELINES_KEY,
                        CLOCK_REGRESSIONS_KEY,
                        ACCEPTED_KEY,
                        DISCARDED_KEY,
                        SIMULATION_TICKS_KEY,
                        PACING_INTERVAL_KEY,
                        PACE_WAITED_KEY,
                        PACE_LATE_KEY,
                        PACE_RESCHEDULED_KEY,
                        PACE_WAIT_FAILED_KEY,
                        PACE_OTHER_KEY,
                        SKIPPED_TARGETS_KEY,
                        EARLY_WAKES_KEY,
                        LAST_OUTCOME_KEY,
                        WINDOW_FRAMES_KEY,
                        WINDOW_INTERVAL_MINIMUM_KEY,
                        WINDOW_INTERVAL_MEAN_KEY,
                        WINDOW_INTERVAL_MAXIMUM_KEY,
                        WINDOW_LATENESS_MEAN_KEY,
                        WINDOW_LATENESS_MAXIMUM_KEY}) == false)
    {
        return false;
    }
    TimingReport report;
    std::string outcome;
    const bool isRead =
        ReadBounded(record, FRAMES_KEY, report.Frames) == true &&
        ReadBounded(record, BASELINES_KEY, report.Baselines) == true &&
        ReadBounded(record, CLOCK_REGRESSIONS_KEY, report.ClockRegressions) == true &&
        ReadBounded(record, ACCEPTED_KEY, report.AcceptedNanoseconds) == true &&
        ReadBounded(record, DISCARDED_KEY, report.DiscardedNanoseconds) == true &&
        ReadBounded(record, SIMULATION_TICKS_KEY, report.SimulationTicks) == true &&
        ReadBounded(record, PACING_INTERVAL_KEY, report.PacingIntervalNanoseconds) == true &&
        ReadBounded(record, PACE_WAITED_KEY, report.PaceWaited) == true &&
        ReadBounded(record, PACE_LATE_KEY, report.PaceLate) == true &&
        ReadBounded(record, PACE_RESCHEDULED_KEY, report.PaceRescheduled) == true &&
        ReadBounded(record, PACE_WAIT_FAILED_KEY, report.PaceWaitFailed) == true &&
        ReadBounded(record, PACE_OTHER_KEY, report.PaceOther) == true &&
        ReadBounded(record, SKIPPED_TARGETS_KEY, report.SkippedTargets) == true &&
        ReadBounded(record, EARLY_WAKES_KEY, report.EarlyWakes) == true &&
        record.GetString(LAST_OUTCOME_KEY, outcome) == true &&
        TryParsePaceOutcome(outcome, report.LastOutcome) == true &&
        ReadBounded(record, WINDOW_FRAMES_KEY, report.WindowFrames) == true &&
        ReadBounded(record, WINDOW_INTERVAL_MINIMUM_KEY, report.WindowIntervalMinimumNanoseconds) == true &&
        ReadBounded(record, WINDOW_INTERVAL_MEAN_KEY, report.WindowIntervalMeanNanoseconds) == true &&
        ReadBounded(record, WINDOW_INTERVAL_MAXIMUM_KEY, report.WindowIntervalMaximumNanoseconds) == true &&
        ReadBounded(record, WINDOW_LATENESS_MEAN_KEY, report.WindowLatenessMeanNanoseconds) == true &&
        ReadBounded(record, WINDOW_LATENESS_MAXIMUM_KEY, report.WindowLatenessMaximumNanoseconds) == true;
    if (isRead == false || report.WindowFrames > TIMING_WINDOW_FRAMES || report.WindowFrames > report.Frames ||
        IsOrdered(report.WindowIntervalMinimumNanoseconds,
                  report.WindowIntervalMeanNanoseconds,
                  report.WindowIntervalMaximumNanoseconds) == false ||
        report.WindowLatenessMeanNanoseconds > report.WindowLatenessMaximumNanoseconds)
    {
        return false;
    }
    outReport = report;
    return true;
}
} // namespace ludus::runtime::game_host
