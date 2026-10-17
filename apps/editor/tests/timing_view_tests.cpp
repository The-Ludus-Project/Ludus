// Timing panel view model: decoding the runtime timing record from a host
// Status result, its bounds, display rows and the panel status line.

#include "internal/controller.h"
#include "internal/timing_panel.h"
#include "internal/timing_view.h"

#include <ludus/foundation/base/types.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using ludus::foundation::float64;
using ludus::foundation::uint64;

namespace
{
/// The GameHost 60 Hz pacing interval in whole nanoseconds.
constexpr uint64 INTERVAL_NANOSECONDS = 16'666'667;

[[nodiscard]] QJsonObject MakeRecord()
{
    return QJsonObject{
        {QStringLiteral("frames"), 200},
        {QStringLiteral("baselines"), 1},
        {QStringLiteral("clock_regressions"), 0},
        {QStringLiteral("accepted_ns"), 3'317'666'733.0},
        {QStringLiteral("discarded_ns"), 750'000'000},
        {QStringLiteral("simulation_ticks"), 199},
        {QStringLiteral("pacing_interval_ns"), static_cast<float64>(INTERVAL_NANOSECONDS)},
        {QStringLiteral("pace_waited"), 197},
        {QStringLiteral("pace_late"), 1},
        {QStringLiteral("pace_rescheduled"), 1},
        {QStringLiteral("pace_wait_failed"), 0},
        {QStringLiteral("pace_other"), 1},
        {QStringLiteral("skipped_targets"), 59},
        {QStringLiteral("early_wakes"), 0},
        {QStringLiteral("last_outcome"), QStringLiteral("Waited")},
        {QStringLiteral("window_frames"), 120},
        {QStringLiteral("window_interval_min_ns"), 16'500'000},
        {QStringLiteral("window_interval_mean_ns"), static_cast<float64>(INTERVAL_NANOSECONDS)},
        {QStringLiteral("window_interval_max_ns"), 1'000'000'000},
        {QStringLiteral("window_lateness_mean_ns"), 1'010'000},
        {QStringLiteral("window_lateness_max_ns"), 1'270'000},
    };
}

[[nodiscard]] QJsonObject MakeStatus(const QJsonObject& record)
{
    return QJsonObject{
        {QStringLiteral("presented_frames"), 200},
        {QStringLiteral("timing"), QJsonArray{record}},
    };
}

[[nodiscard]] QJsonObject WithField(const QString& key, const QJsonValue& value)
{
    QJsonObject record = MakeRecord();
    record.insert(key, value);
    return MakeStatus(record);
}

/// Decoding must fail with a reason and leave the previous snapshot unchanged.
void CheckRejected(const QJsonObject& status)
{
    TimingSnapshot snapshot;
    snapshot.Frames = 77;
    QString error;
    CHECK_FALSE(ReadTimingSnapshot(status, snapshot, error));
    CHECK_FALSE(error.isEmpty());
    CHECK(snapshot.Frames == 77);
}
} // namespace

TEST_CASE("Timing view decodes the runtime Status timing record", "[editor][timing]")
{
    TimingSnapshot snapshot;
    QString error = QStringLiteral("unchanged");
    REQUIRE(ReadTimingSnapshot(MakeStatus(MakeRecord()), snapshot, error));
    CHECK(error == QStringLiteral("unchanged"));
    CHECK(snapshot.Frames == 200);
    CHECK(snapshot.AcceptedNanoseconds == 3'317'666'733);
    CHECK(snapshot.PacingIntervalNanoseconds == INTERVAL_NANOSECONDS);
    CHECK(snapshot.SkippedTargets == 59);
    CHECK(snapshot.LastOutcome == QStringLiteral("Waited"));
    CHECK(snapshot.WindowFrames == TIMING_WINDOW_FRAMES);
    CHECK(snapshot.WindowIntervalMaximumNanoseconds == 1'000'000'000);

    // The largest saturated total is exact in a binary64 JSON number.
    TimingSnapshot saturated;
    REQUIRE(ReadTimingSnapshot(WithField(QStringLiteral("accepted_ns"), static_cast<float64>(MAXIMUM_TIMING_VALUE)),
                               saturated,
                               error));
    CHECK(saturated.AcceptedNanoseconds == MAXIMUM_TIMING_VALUE);
}

TEST_CASE("Timing view rejects records outside the runtime contract", "[editor][timing][overflow]")
{
    CheckRejected(QJsonObject{{QStringLiteral("presented_frames"), 1}});
    CheckRejected(QJsonObject{{QStringLiteral("timing"), MakeRecord()}}); // Not wrapped in a one-record array.
    CheckRejected(QJsonObject{{QStringLiteral("timing"), QJsonArray{MakeRecord(), MakeRecord()}}});
    CheckRejected(QJsonObject{{QStringLiteral("timing"), QJsonArray{1}}});
    // 2^53 is beyond the saturated range; the editor cannot tell it from 2^53 + 1.
    CheckRejected(WithField(QStringLiteral("accepted_ns"), static_cast<float64>(MAXIMUM_TIMING_VALUE) + 1.0));
    CheckRejected(WithField(QStringLiteral("frames"), -1));
    CheckRejected(WithField(QStringLiteral("frames"), 1.5));
    CheckRejected(WithField(QStringLiteral("frames"), QStringLiteral("200")));
    CheckRejected(WithField(QStringLiteral("last_outcome"), QStringLiteral("Sleeping")));
    CheckRejected(WithField(QStringLiteral("window_frames"), static_cast<float64>(TIMING_WINDOW_FRAMES + 1)));
    CheckRejected(WithField(QStringLiteral("frames"), 100)); // Fewer frames than the 120-frame window.
    CheckRejected(WithField(QStringLiteral("window_interval_min_ns"), 20'000'000));
    CheckRejected(WithField(QStringLiteral("window_interval_mean_ns"), 2'000'000'000));
    CheckRejected(WithField(QStringLiteral("window_lateness_mean_ns"), 2'000'000));
    CheckRejected(WithField(QStringLiteral("surprise"), 1));
    // Absolute runtime timestamps are not part of the record and are refused.
    CheckRejected(WithField(QStringLiteral("wake_ns"), 4'611'686'018'427'387'904.0));
    QJsonObject missing = MakeRecord();
    missing.remove(QStringLiteral("early_wakes"));
    missing.insert(QStringLiteral("early_wake"), 0);
    CheckRejected(MakeStatus(missing));
}

TEST_CASE("Timing view rows show runtime values and units", "[editor][timing]")
{
    TimingSnapshot snapshot;
    QString error;
    REQUIRE(ReadTimingSnapshot(MakeStatus(MakeRecord()), snapshot, error));
    const QList<TimingRow> rows = MakeTimingRows(snapshot);
    REQUIRE(rows.size() == 9);
    CHECK(rows.at(0).Label == QStringLiteral("Pacing target"));
    CHECK(rows.at(0).Value == QStringLiteral("60.00 Hz (16.667 ms)"));
    CHECK(rows.at(1).Value == QStringLiteral("min 16.500 · mean 16.667 · max 1000.000 ms (last 120 frames)"));
    CHECK(rows.at(2).Value == QStringLiteral("mean 1.010 · max 1.270 ms"));
    CHECK(rows.at(3).Value == QStringLiteral("waited 197 · late 1 · rescheduled 1 · wait failed 0 · other 1"));
    CHECK(rows.at(4).Value == QStringLiteral("Waited"));
    CHECK(rows.at(5).Value == QStringLiteral("59 skipped · 0 early wakes"));
    CHECK(rows.at(6).Value == QStringLiteral("accepted 3.318 s · discarded 0.750 s"));
    CHECK(rows.at(7).Value == QStringLiteral("200 frames · 199 simulation ticks"));
    CHECK(rows.at(8).Value == QStringLiteral("1 baselines · 0 regressions"));

    TimingSnapshot empty;
    const QList<TimingRow> emptyRows = MakeTimingRows(empty);
    CHECK(emptyRows.at(0).Value == QStringLiteral("Not paced by the runtime"));
    CHECK(emptyRows.at(1).Value == QStringLiteral("No measured frames yet"));
}

TEST_CASE("Timing panel status follows the play session", "[editor][timing]")
{
    PlaySnapshot play;
    CHECK(DescribeTimingState(play).startsWith(QStringLiteral("No running session")));
    play.Phase = PlayPhase::Running;
    CHECK(DescribeTimingState(play) == QStringLiteral("Waiting for the runtime's timing report."));
    play.TimingMessage = QStringLiteral("Runtime timing window is inconsistent.");
    CHECK(DescribeTimingState(play) == play.TimingMessage);
    play.HasTiming = true;
    CHECK(DescribeTimingState(play).startsWith(QStringLiteral("Runtime-reported")));
    play.Phase = PlayPhase::Stopped;
    CHECK(DescribeTimingState(play).startsWith(QStringLiteral("Last runtime report")));
}
