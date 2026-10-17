#include "internal/timing_view.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>

namespace ludus::editor
{
namespace
{
using foundation::float64;
using foundation::uint64;

constexpr float64 NANOSECONDS_PER_MILLISECOND = 1'000'000.0;
constexpr float64 NANOSECONDS_PER_SECOND = 1'000'000'000.0;

/// Mirror of GameHost's PaceOutcome wire names (PaceOutcomeName).
const QStringList& GetPaceOutcomeNames()
{
    static const QStringList NAMES = {
        QStringLiteral("Disabled"),
        QStringLiteral("InvalidClock"),
        QStringLiteral("Baseline"),
        QStringLiteral("Waited"),
        QStringLiteral("Late"),
        QStringLiteral("Rescheduled"),
        QStringLiteral("WaitFailed"),
        QStringLiteral("WaitLimit"),
        QStringLiteral("ClockRegression"),
        QStringLiteral("Overflow"),
    };
    return NAMES;
}

[[nodiscard]] bool ReadBounded(const QJsonObject& record, const QString& key, uint64& outValue)
{
    const QJsonValue value = record.value(key);
    if (value.isDouble() == false)
    {
        return false;
    }
    const float64 number = value.toDouble();
    // Exact integers only: binary64 represents every integer up to 2^53 - 1.
    if ((number >= 0.0) == false || number > static_cast<float64>(MAXIMUM_TIMING_VALUE) ||
        number != static_cast<float64>(static_cast<uint64>(number)))
    {
        return false;
    }
    outValue = static_cast<uint64>(number);
    return true;
}

/// One numeric wire field and the snapshot member it fills.
struct TimingField final
{
    const char* Key;
    uint64 TimingSnapshot::*Member;
};

/// GameHost's numeric timing fields (WriteTimingRecord); last_outcome is read separately.
const QList<TimingField>& GetTimingFields()
{
    static const QList<TimingField> FIELDS = {
        {"frames", &TimingSnapshot::Frames},
        {"baselines", &TimingSnapshot::Baselines},
        {"clock_regressions", &TimingSnapshot::ClockRegressions},
        {"accepted_ns", &TimingSnapshot::AcceptedNanoseconds},
        {"discarded_ns", &TimingSnapshot::DiscardedNanoseconds},
        {"simulation_ticks", &TimingSnapshot::SimulationTicks},
        {"pacing_interval_ns", &TimingSnapshot::PacingIntervalNanoseconds},
        {"pace_waited", &TimingSnapshot::PaceWaited},
        {"pace_late", &TimingSnapshot::PaceLate},
        {"pace_rescheduled", &TimingSnapshot::PaceRescheduled},
        {"pace_wait_failed", &TimingSnapshot::PaceWaitFailed},
        {"pace_other", &TimingSnapshot::PaceOther},
        {"skipped_targets", &TimingSnapshot::SkippedTargets},
        {"early_wakes", &TimingSnapshot::EarlyWakes},
        {"window_frames", &TimingSnapshot::WindowFrames},
        {"window_interval_min_ns", &TimingSnapshot::WindowIntervalMinimumNanoseconds},
        {"window_interval_mean_ns", &TimingSnapshot::WindowIntervalMeanNanoseconds},
        {"window_interval_max_ns", &TimingSnapshot::WindowIntervalMaximumNanoseconds},
        {"window_lateness_mean_ns", &TimingSnapshot::WindowLatenessMeanNanoseconds},
        {"window_lateness_max_ns", &TimingSnapshot::WindowLatenessMaximumNanoseconds},
    };
    return FIELDS;
}

[[nodiscard]] QString FormatMilliseconds(uint64 nanoseconds)
{
    return QString::number(static_cast<float64>(nanoseconds) / NANOSECONDS_PER_MILLISECOND, 'f', 3);
}

[[nodiscard]] QString FormatSeconds(uint64 nanoseconds)
{
    return QString::number(static_cast<float64>(nanoseconds) / NANOSECONDS_PER_SECOND, 'f', 3);
}
} // namespace

bool ReadTimingSnapshot(const QJsonObject& status, TimingSnapshot& outSnapshot, QString& outError)
{
    const QJsonValue value = status.value(QStringLiteral("timing"));
    if (value.isUndefined() == true)
    {
        outError = QStringLiteral("The runtime did not report timing.");
        return false;
    }
    const QJsonArray records = value.toArray();
    if (value.isArray() == false || records.size() != 1 || records.at(0).isObject() == false)
    {
        outError = QStringLiteral("Malformed runtime timing record.");
        return false;
    }
    const QJsonObject record = records.at(0).toObject();
    TimingSnapshot snapshot;
    // The outcome name is the one non-numeric field.
    if (record.size() != GetTimingFields().size() + 1)
    {
        outError = QStringLiteral("Runtime timing record has unknown or missing fields.");
        return false;
    }
    for (const TimingField& field : GetTimingFields())
    {
        if (ReadBounded(record, QString::fromLatin1(field.Key), snapshot.*field.Member) == false)
        {
            outError = QStringLiteral("Runtime timing field %1 is missing or out of range.")
                           .arg(QString::fromLatin1(field.Key));
            return false;
        }
    }
    const QJsonValue outcome = record.value(QStringLiteral("last_outcome"));
    if (outcome.isString() == false || GetPaceOutcomeNames().contains(outcome.toString()) == false)
    {
        outError = QStringLiteral("Runtime timing reported an unknown pacing outcome.");
        return false;
    }
    snapshot.LastOutcome = outcome.toString();
    if (snapshot.WindowFrames > TIMING_WINDOW_FRAMES || snapshot.WindowFrames > snapshot.Frames ||
        snapshot.WindowIntervalMinimumNanoseconds > snapshot.WindowIntervalMeanNanoseconds ||
        snapshot.WindowIntervalMeanNanoseconds > snapshot.WindowIntervalMaximumNanoseconds ||
        snapshot.WindowLatenessMeanNanoseconds > snapshot.WindowLatenessMaximumNanoseconds)
    {
        outError = QStringLiteral("Runtime timing window is inconsistent.");
        return false;
    }
    outSnapshot = snapshot;
    return true;
}

QList<TimingRow> MakeTimingRows(const TimingSnapshot& snapshot)
{
    QList<TimingRow> rows;
    if (snapshot.PacingIntervalNanoseconds == 0)
    {
        rows.append({QStringLiteral("Pacing target"), QStringLiteral("Not paced by the runtime")});
    }
    else
    {
        const float64 rate = NANOSECONDS_PER_SECOND / static_cast<float64>(snapshot.PacingIntervalNanoseconds);
        rows.append({QStringLiteral("Pacing target"),
                     QStringLiteral("%1 Hz (%2 ms)")
                         .arg(QString::number(rate, 'f', 2), FormatMilliseconds(snapshot.PacingIntervalNanoseconds))});
    }
    if (snapshot.WindowFrames == 0)
    {
        rows.append({QStringLiteral("Frame interval"), QStringLiteral("No measured frames yet")});
        rows.append({QStringLiteral("Wake lateness"), QStringLiteral("No measured frames yet")});
    }
    else
    {
        rows.append({QStringLiteral("Frame interval"),
                     QStringLiteral("min %1 · mean %2 · max %3 ms (last %4 frames)")
                         .arg(FormatMilliseconds(snapshot.WindowIntervalMinimumNanoseconds),
                              FormatMilliseconds(snapshot.WindowIntervalMeanNanoseconds),
                              FormatMilliseconds(snapshot.WindowIntervalMaximumNanoseconds))
                         .arg(snapshot.WindowFrames)});
        rows.append({QStringLiteral("Wake lateness"),
                     QStringLiteral("mean %1 · max %2 ms")
                         .arg(FormatMilliseconds(snapshot.WindowLatenessMeanNanoseconds),
                              FormatMilliseconds(snapshot.WindowLatenessMaximumNanoseconds))});
    }
    rows.append({QStringLiteral("Pacing outcomes"),
                 QStringLiteral("waited %1 · late %2 · rescheduled %3 · wait failed %4 · other %5")
                     .arg(snapshot.PaceWaited)
                     .arg(snapshot.PaceLate)
                     .arg(snapshot.PaceRescheduled)
                     .arg(snapshot.PaceWaitFailed)
                     .arg(snapshot.PaceOther)});
    rows.append({QStringLiteral("Latest outcome"), snapshot.LastOutcome});
    rows.append({QStringLiteral("Skipped targets"),
                 QStringLiteral("%1 skipped · %2 early wakes").arg(snapshot.SkippedTargets).arg(snapshot.EarlyWakes)});
    rows.append({QStringLiteral("Frame time"),
                 QStringLiteral("accepted %1 s · discarded %2 s")
                     .arg(FormatSeconds(snapshot.AcceptedNanoseconds), FormatSeconds(snapshot.DiscardedNanoseconds))});
    rows.append({QStringLiteral("Frames"),
                 QStringLiteral("%1 frames · %2 simulation ticks").arg(snapshot.Frames).arg(snapshot.SimulationTicks)});
    rows.append(
        {QStringLiteral("Clock"),
         QStringLiteral("%1 baselines · %2 regressions").arg(snapshot.Baselines).arg(snapshot.ClockRegressions)});
    return rows;
}
} // namespace ludus::editor
