#include "internal/viewport_trace.h"

#include <ludus/foundation/profiling/profiling.hpp>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
#if LUDUS_PROFILING_ENABLED
namespace
{
struct ViewportCaptureCleanup final
{
    ~ViewportCaptureCleanup()
    {
        static_cast<void>(StopViewportTrace());
    }
};
} // namespace
TEST_CASE("viewport trace includes scheduling gaps and matching frame stages through paint", "[profiling][editor]")
{
    const ViewportCaptureCleanup cleanup;
    REQUIRE(StartViewportTrace() == true);
    ViewportFrameTrace frame;
    ViewportPaintTrace paint;
    constexpr uint64 FRAME_ID = 42;
    constexpr uint64 DEVICE_SESSION = 7;
    frame.BeginService(FRAME_ID, DEVICE_SESSION);
    frame.BeginFrame();
    frame.SetStage("Await producer completion (observed)");
    frame.EndService();
    QTest::qWait(2);
    frame.BeginService(FRAME_ID + 1, DEVICE_SESSION);
    frame.SetStage("Readback requests and wait (observed)");
    frame.EndService();
    QTest::qWait(2);
    frame.BeginService(FRAME_ID + 1, DEVICE_SESSION);
    frame.SetStage("CPU display image conversion");
    frame.Publish();
    paint.Queue(frame.GetPublishedIdentity(), frame.GetPublishedBeginTicks());
    frame.EndService();
    QTest::qWait(2);
    paint.BeginPaint();
    paint.EndPaint();
    const ViewportTraceSnapshot snapshot = StopViewportTrace();
    CHECK(snapshot.DroppedSpans == 0);
    QJsonObject trace;
    AppendViewportTrace(trace, snapshot);
    bool hasFrame = false;
    bool hasReadbackWait = false;
    bool hasConversion = false;
    bool hasSchedulingGap = false;
    bool hasPaintWait = false;
    bool hasPaint = false;
    bool hasLatency = false;
    for (const QJsonValue& value : trace.value(QStringLiteral("traceEvents")).toArray())
    {
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("ph")).toString() != QStringLiteral("X"))
        {
            continue;
        }
        const QJsonObject arguments = event.value(QStringLiteral("args")).toObject();
        CHECK(arguments.value(QStringLiteral("frameId")).toInteger() == FRAME_ID);
        CHECK(arguments.value(QStringLiteral("deviceSession")).toInteger() == DEVICE_SESSION);
        CHECK(event.value(QStringLiteral("dur")).toDouble() >= 0);
        const QString name = event.value(QStringLiteral("name")).toString();
        hasFrame = hasFrame == true || name == QStringLiteral("Frame to image ready");
        hasReadbackWait = hasReadbackWait == true || name == QStringLiteral("Readback requests and wait (observed)");
        hasConversion = hasConversion == true || name == QStringLiteral("CPU display image conversion");
        hasPaint = hasPaint == true || name == QStringLiteral("Qt paint image");
        hasLatency = hasLatency == true || name == QStringLiteral("Frame to Qt paint complete");
        if (name == QStringLiteral("Image ready to Qt paint"))
        {
            hasPaintWait = true;
            CHECK(event.value(QStringLiteral("dur")).toDouble() > 0);
        }
    }
    for (const ViewportTraceSpan& span : snapshot.Spans)
    {
        if (span.Lane == ViewportTraceLane::Scheduling && span.EndTicks > span.BeginTicks)
        {
            hasSchedulingGap = true;
        }
    }
    CHECK(hasFrame == true);
    CHECK(hasReadbackWait == true);
    CHECK(hasConversion == true);
    CHECK(hasSchedulingGap == true);
    CHECK(hasPaintWait == true);
    CHECK(hasPaint == true);
    CHECK(hasLatency == true);
}
TEST_CASE("viewport captures close pending spans and reject handles from an earlier recording", "[profiling][editor]")
{
    const ViewportCaptureCleanup cleanup;
    REQUIRE(StartViewportTrace() == true);
    const ViewportTraceIdentity identity{1, 2, 1};
    const ViewportTraceToken old = BeginViewportTrace("Old pending readback", ViewportTraceLane::Stage, identity);
    const ViewportTraceSnapshot first = StopViewportTrace();
    REQUIRE(first.Spans.GetSize() == 1);
    CHECK(first.Spans[0].EndTicks == first.StoppedTicks);
    CHECK(QString::fromUtf8(first.Spans[0].Outcome) == QStringLiteral("capture stopped while pending"));
    REQUIRE(StartViewportTrace() == true);
    const ViewportTraceToken current =
        BeginViewportTrace("New readback", ViewportTraceLane::Stage, identity, first.StartedTicks);
    EndViewportTrace(old, "must not affect new recording");
    EndViewportTrace(current);
    const ViewportTraceSnapshot second = StopViewportTrace();
    REQUIRE(second.Spans.GetSize() == 1);
    CHECK(second.Spans[0].BeginTicks == second.StartedTicks);
    CHECK(QString::fromUtf8(second.Spans[0].Outcome) == QStringLiteral("capture started during interval"));
}
TEST_CASE("viewport capture bounds memory and reports overflow", "[profiling][editor]")
{
    const ViewportCaptureCleanup cleanup;
    REQUIRE(StartViewportTrace() == true);
    for (usize index = 0; index < MAX_VIEWPORT_TRACE_SPANS + 1; ++index)
    {
        const ViewportTraceToken token = BeginViewportTrace("Bounded span", ViewportTraceLane::Stage, {1, 1, 1});
        EndViewportTrace(token);
    }
    const ViewportTraceSnapshot snapshot = StopViewportTrace();
    CHECK(snapshot.Spans.GetSize() == MAX_VIEWPORT_TRACE_SPANS);
    CHECK(snapshot.DroppedSpans == 1);
}
TEST_CASE("viewport traces retain pending frames across capture start and record cancellation", "[profiling][editor]")
{
    const ViewportCaptureCleanup cleanup;
    ViewportFrameTrace frame;
    frame.BeginService(1, 1);
    frame.BeginFrame();
    frame.SetStage("Readback requests and wait (observed)");
    frame.EndService();
    REQUIRE(StartViewportTrace() == true);
    frame.BeginService(2, 1);
    frame.Cancel("readback failed");
    frame.EndService();
    const ViewportTraceSnapshot snapshot = StopViewportTrace();
    bool hasCancelledFrame = false;
    bool hasCancelledReadback = false;
    for (const ViewportTraceSpan& span : snapshot.Spans)
    {
        CHECK(span.BeginTicks >= snapshot.StartedTicks);
        CHECK(span.EndTicks <= snapshot.StoppedTicks);
        if (span.Lane == ViewportTraceLane::Frame)
        {
            hasCancelledFrame = true;
            CHECK(span.Identity.FrameId == 1);
            CHECK(QString::fromUtf8(span.Outcome) == QStringLiteral("readback failed"));
        }
        if (span.Lane == ViewportTraceLane::Stage)
        {
            hasCancelledReadback = true;
            CHECK(QString::fromUtf8(span.Name) == QStringLiteral("Readback requests and wait (observed)"));
            CHECK(QString::fromUtf8(span.Outcome) == QStringLiteral("readback failed"));
        }
    }
    CHECK(hasCancelledFrame == true);
    CHECK(hasCancelledReadback == true);
}
TEST_CASE("viewport paint intervals follow the arriving image and superseded images stay explicit",
          "[profiling][editor]")
{
    const ViewportCaptureCleanup cleanup;
    REQUIRE(StartViewportTrace() == true);
    ViewportPaintTrace paint;
    paint.Queue({1, 1, 1}, 0);
    paint.BeginPaint();
    paint.EndPaint();
    paint.Queue({1, 2, 1}, 0);
    paint.Queue({1, 3, 1}, 0);
    paint.BeginPaint();
    paint.EndPaint();
    const ViewportTraceSnapshot snapshot = StopViewportTrace();
    bool hasInterval = false;
    bool hasSupersededImage = false;
    for (const ViewportTraceSpan& span : snapshot.Spans)
    {
        if (span.Lane == ViewportTraceLane::Interval && QString::fromUtf8(span.Outcome) == QStringLiteral("complete"))
        {
            hasInterval = true;
            CHECK(span.Identity.FrameId == 3);
        }
        if (span.Lane == ViewportTraceLane::Presentation && span.Identity.FrameId == 2)
        {
            hasSupersededImage = true;
            CHECK(QString::fromUtf8(span.Outcome) == QStringLiteral("superseded before paint"));
        }
    }
    CHECK(hasInterval == true);
    CHECK(hasSupersededImage == true);
}

namespace
{
constexpr int32 CHILD_TEST_MILLISECONDS = 60'000;
constexpr const char* CHILD_SCOPE_NAME = "TraceOutputChildScope";
constexpr const char* CHILD_SPAN_NAME = "Trace output child span";
// CPU scopes export as begin/end pairs; viewport spans as complete events.
bool HasEventNamed(const QJsonObject& trace, const char* name)
{
    for (const QJsonValue& value : trace.value(QStringLiteral("traceEvents")).toArray())
    {
        const QJsonObject event = value.toObject();
        const QString phase = event.value(QStringLiteral("ph")).toString();
        if ((phase == QStringLiteral("B") || phase == QStringLiteral("X")) &&
            event.value(QStringLiteral("name")).toString() == QString::fromUtf8(name))
        {
            return true;
        }
    }
    return false;
}
} // namespace
// Runs inside the executable started below with --trace-output, so the capture
// is the entry point's: recording into it must succeed and starting another must not.
TEST_CASE("trace output child records into the entry point capture", "[.trace-output-child]")
{
    {
        LUDUS_PROFILE_SCOPE(TraceOutputChildScope);
        const ViewportTraceToken token = BeginViewportTrace(CHILD_SPAN_NAME, ViewportTraceLane::Stage, {7, 1, 1});
        CHECK(token.Capture != 0);
        EndViewportTrace(token);
    }
    CHECK(StartViewportTrace() == false);
}
// Regression: the shared Catch2 entry point object library lost the preview
// definitions, so `--trace-output` was unknown and the editor viewport runtime
// profile scenario exited with 255 before running a test.
TEST_CASE("editor test executables write the requested combined trace", "[profiling][editor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString tracePath = directory.filePath(QStringLiteral("trace.json"));
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(),
                QStringList{QStringLiteral("[.trace-output-child]"), QStringLiteral("--trace-output"), tracePath});
    REQUIRE(child.waitForStarted(CHILD_TEST_MILLISECONDS) == true);
    REQUIRE(child.waitForFinished(CHILD_TEST_MILLISECONDS) == true);
    INFO(child.readAll().toStdString());
    REQUIRE(child.exitStatus() == QProcess::NormalExit);
    REQUIRE(child.exitCode() == 0);
    QFile file(tracePath);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    const QJsonObject trace = QJsonDocument::fromJson(file.readAll()).object();
    REQUIRE(trace.value(QStringLiteral("traceEvents")).toArray().isEmpty() == false);
    CHECK(HasEventNamed(trace, CHILD_SCOPE_NAME) == true);
    CHECK(HasEventNamed(trace, CHILD_SPAN_NAME) == true);
    // The option is parsed before any test runs: an unknown option is the exact failure seen in CI.
    QProcess rejected;
    rejected.setProcessEnvironment(environment);
    rejected.setProcessChannelMode(QProcess::MergedChannels);
    rejected.start(QCoreApplication::applicationFilePath(),
                   QStringList{QStringLiteral("[.trace-output-child]"), QStringLiteral("--no-such-option")});
    REQUIRE(rejected.waitForFinished(CHILD_TEST_MILLISECONDS) == true);
    CHECK(rejected.exitCode() != 0);
}
#endif
