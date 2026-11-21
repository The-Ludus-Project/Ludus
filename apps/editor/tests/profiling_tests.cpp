#include "internal/editor_commands.h"
#include "internal/hosted_profiling_session.h"
#include "internal/main_window.h"
#include "internal/profiling_panel.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/world_graph_view.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/profiling/integration.hpp>
#include <ludus/foundation/profiling/perfetto_export.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

#include <QAction>
#include <QByteArray>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

#include <cstring>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace profile = ludus::foundation::profiling;
#if LUDUS_PROFILING_ENABLED
namespace
{
bool FinishRecording(ProfilingController& inoutController)
{
    QElapsedTimer timer;
    timer.start();
    while (inoutController.GetState() == ProfilingState::Finalizing && timer.elapsed() < 10000)
    {
        QTest::qWait(1);
    }
    return inoutController.GetState() != ProfilingState::Finalizing;
}
#    if defined(LUDUS_EDITOR_GPU_PREVIEW)
RenderScene MakeProfilingScene()
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("profiling-triangle");
    scene.AssetsIdentity = scene.Identity;
    PreviewMesh mesh;
    mesh.Id = QStringLiteral("triangle");
    mesh.Positions = {{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("object");
    object.Mesh = 0;
    object.Tint = {1, 0, 0, 1};
    scene.Objects.append(object);
    return scene;
}
#    endif
/// A hosted game's recording channel, scripted by the test.
class ScriptedRecordingTarget final : public QObject, public HostedRecordingTarget
{
public:
    static constexpr uint64 PROCESS_ID = 4321;
    QList<ludus::platform::HostedRecordingRequest> Requests;
    bool IsConnected = true;
    uint64 ConsumedFrames = 0;

    [[nodiscard]] bool IsRecordingConnected() const noexcept override
    {
        return IsConnected;
    }
    [[nodiscard]] ludus::platform::HostedEndpointStatus
    SendRecordingRequest(const ludus::platform::HostedRecordingRequest& request) noexcept override
    {
        if (IsConnected == false)
        {
            return ludus::platform::HostedEndpointStatus::NotConnected;
        }
        Requests.append(request);
        return ludus::platform::HostedEndpointStatus::Ok;
    }
    [[nodiscard]] uint64 GetProducerProcessId() const noexcept override
    {
        return PROCESS_ID;
    }
    [[nodiscard]] uint64 GetConsumedFrames() const noexcept override
    {
        return ConsumedFrames;
    }
    [[nodiscard]] uint64 GetDroppedRecordingEvents() const noexcept override
    {
        return 0;
    }
    void SetRecordingSink(HostedRecordingSink* sinkOrNull) noexcept override
    {
        mSinkOrNull = sinkOrNull;
    }
    /// Answer `request`; a successful Start reports per-pass counters unless
    /// `isPassTimingSupported` is false, as from a device without them.
    void Acknowledge(const ludus::platform::HostedRecordingRequest& request,
                     ludus::platform::HostedRecordingResult result,
                     uint64 frame,
                     const ludus::platform::HostedRecordingCoverage& coverage = {},
                     bool isPassTimingSupported = true)
    {
        REQUIRE(mSinkOrNull != nullptr);
        ludus::platform::HostedRecordingEvent event;
        event.Kind = ludus::platform::HostedRecordingEventKind::Acknowledgement;
        event.Acknowledgement.Kind = request.Kind;
        event.Acknowledgement.Result = result;
        event.Acknowledgement.Recording = request.Recording;
        event.Acknowledgement.Request = request.Request;
        event.Acknowledgement.Frame = frame;
        event.Acknowledgement.Ticks = frame * 1000;
        event.Acknowledgement.Coverage = coverage;
        event.Acknowledgement.IsPassTimingSupported =
            isPassTimingSupported == true && request.Kind == ludus::platform::HostedRecordingRequestKind::Start &&
            result == ludus::platform::HostedRecordingResult::Ok;
        std::memcpy(event.Acknowledgement.ProducerName.GetData(), "scripted game", 13);
        mSinkOrNull->AcceptRecordingEvent(event);
    }
    // Recording then frame, as in HostedRecordingSample.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void SendSample(uint64 recording, uint64 frame)
    {
        REQUIRE(mSinkOrNull != nullptr);
        ludus::platform::HostedRecordingEvent event;
        event.Kind = ludus::platform::HostedRecordingEventKind::Sample;
        ludus::platform::HostedRecordingSample& sample = event.Sample;
        sample.Recording = recording;
        sample.Frame = frame;
        sample.SubmissionTicks = frame * 1000;
        sample.CollectionTicks = frame * 1000 + 10;
        sample.DurationNanoseconds = 500;
        sample.ScopeIdentity = 9;
        sample.LogicalPassCount = 1;
        std::memcpy(sample.Name.GetData(), "Game/Surface", 12);
        mSinkOrNull->AcceptRecordingEvent(event);
    }
    /// Send one whole-frame sample, as a device without per-pass counters does.
    // Recording then frame, as in HostedRecordingSample.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void SendFrameSample(uint64 recording, uint64 frame)
    {
        REQUIRE(mSinkOrNull != nullptr);
        ludus::platform::HostedRecordingEvent event;
        event.Kind = ludus::platform::HostedRecordingEventKind::Sample;
        ludus::platform::HostedRecordingSample& sample = event.Sample;
        sample.Recording = recording;
        sample.Frame = frame;
        sample.SubmissionTicks = frame * 1000;
        sample.CollectionTicks = frame * 1000 + 10;
        sample.DurationNanoseconds = 700;
        sample.IsFrame = true;
        mSinkOrNull->AcceptRecordingEvent(event);
    }
    void Disconnect()
    {
        IsConnected = false;
        HostedRecordingSink* sinkOrNull = mSinkOrNull;
        mSinkOrNull = nullptr;
        if (sinkOrNull != nullptr)
        {
            sinkOrNull->EndRecordingChannel();
        }
    }

private:
    HostedRecordingSink* mSinkOrNull = nullptr;
};

bool AwaitState(ProfilingController& inoutController, ProfilingState state)
{
    QElapsedTimer timer;
    timer.start();
    while (inoutController.GetState() != state && timer.elapsed() < 10000)
    {
        QTest::qWait(1);
    }
    return inoutController.GetState() == state;
}

QJsonObject ReadTrace(const QString& path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) == false)
    {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}
} // namespace

TEST_CASE("GPU-only Perfetto export preserves CPU data and attributes zero duration to submission",
          "[profiling][editor]")
{
    REQUIRE(profile::BeginCapture(1) == true);
    {
        LUDUS_PROFILE_SCOPE(CpuMustRemain);
    }
    profile::EndCapture();
    const profile::GpuTraceSample sample
    {
        .SubmissionTicks = 1000,
        .CollectionTicks = 9000,
        .DurationNanoseconds = 0,
        .FrameId = 7,
        .DeviceSession = 3,
        .PassName = {},
    };
    Array<uint8> bytes;
    REQUIRE(profile::SerializePerfettoTrace(bytes, {&sample, 1}, { .EligibleFrames = 4 }, false) == true);
    const QByteArray json(reinterpret_cast<const char*>(bytes.GetData()), static_cast<qsizetype>(bytes.GetSize()));
    const QJsonObject trace = QJsonDocument::fromJson(json).object();
    REQUIRE(trace.isEmpty() == false);
    CHECK(json.contains("CpuMustRemain") == false);
    CHECK(trace.value(QStringLiteral("ludusGpuTiming")).toObject().value(QStringLiteral("sampleCount")).toInt() == 1);
    bool hasCounter = false;
    for (const QJsonValue& value : trace.value(QStringLiteral("traceEvents")).toArray())
    {
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("ph")).toString() == QStringLiteral("C"))
        {
            hasCounter = true;
            CHECK(event.value(QStringLiteral("ts")).toDouble() == 1);
            CHECK(event.value(QStringLiteral("args")).toObject().value(QStringLiteral("value")).toDouble() == 0);
        }
        CHECK(event.value(QStringLiteral("ph")).toString() != QStringLiteral("X"));
    }
    CHECK(hasCounter == true);
    REQUIRE(profile::SerializePerfettoTrace(bytes) == true);
    CHECK(QByteArray(reinterpret_cast<const char*>(bytes.GetData()), static_cast<qsizetype>(bytes.GetSize()))
              .contains("CpuMustRemain") == true);
}

TEST_CASE("editor CPU recording retains a failed save for retry and refuses replacement", "[profiling][editor]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    QFile blocker(directory.filePath(QStringLiteral("blocker")));
    REQUIRE(blocker.open(QIODevice::WriteOnly) == true);
    blocker.close();
    RenderPreview preview;
    ProfilingController controller(&preview);
    REQUIRE(controller.Start(ProfilingMode::Cpu, blocker.fileName() + QStringLiteral("/trace.json")) == true);
    {
        LUDUS_PROFILE_SCOPE(EditorRetryEvidence);
        const ViewportTraceToken token =
            BeginViewportTrace("Retained viewport span", ViewportTraceLane::Stage, {1, 1, 1});
        EndViewportTrace(token);
    }
    CHECK(preview.HasGpuProfilingPending() == false);
    CHECK(controller.Start(ProfilingMode::Cpu, directory.filePath(QStringLiteral("other.json"))) == false);
    controller.Stop();
    REQUIRE(FinishRecording(controller) == true);
    REQUIRE(controller.GetState() == ProfilingState::Failed);
    REQUIRE(controller.HasUnsavedSnapshot() == true);
    CHECK(controller.Start(ProfilingMode::Cpu, directory.filePath(QStringLiteral("other.json"))) == false);
    const QString path = directory.filePath(QStringLiteral("saved.json"));
    REQUIRE(controller.RetrySave(path) == true);
    REQUIRE(FinishRecording(controller) == true);
    REQUIRE(controller.GetState() == ProfilingState::Complete);
    CHECK(controller.HasUnsavedSnapshot() == false);
    const QJsonObject trace = ReadTrace(path);
    REQUIRE(trace.isEmpty() == false);
    CHECK(trace.value(QStringLiteral("ludusGpuTiming")).toObject().value(QStringLiteral("sampleCount")).toInt() == 0);
    CHECK(QJsonDocument(trace).toJson().contains("EditorRetryEvidence") == true);
    CHECK(QJsonDocument(trace).toJson().contains("Retained viewport span") == true);
    CHECK(controller.Start(ProfilingMode::Cpu, path) == false);
    CHECK(ReadTrace(path) == trace);
}

TEST_CASE("profiling controls share F9 action and refuse GPU mode without a device", "[profiling][editor]")
{
    QTemporaryDir directory;
    RenderPreview preview;
    QAction* action = CreateEditorCommand(PROFILING_RECORD_COMMAND, &preview);
    ProfilingPanel panel(&preview, action);
    panel.SetProjectRoot(directory.path());
    panel.addAction(action);
    QComboBox* modes = panel.findChild<QComboBox*>(QStringLiteral("profilingMode"));
    REQUIRE(modes != nullptr);
    // Record Profiling moved from F9 to Ctrl+Alt+F9 so F9 can toggle a breakpoint
    // in a code editor (shortcut resolution decided 2026-11-03).
    CHECK(action->shortcut() == QKeySequence(QStringLiteral("Ctrl+Alt+F9")));
    CHECK(modes->count() == 3);
    modes->setCurrentIndex(1);
    action->trigger();
    CHECK(panel.GetController().GetState() != ProfilingState::Recording);
    modes->setCurrentIndex(0);
    action->trigger();
    REQUIRE(panel.GetController().GetState() == ProfilingState::Recording);
    CHECK(action->isChecked() == true);
    CHECK(modes->isEnabled() == false);
    {
        LUDUS_PROFILE_SCOPE(PanelRecording);
    }
    action->trigger();
    REQUIRE(FinishRecording(panel.GetController()) == true);
    CHECK(panel.GetController().GetState() == ProfilingState::Complete);
    CHECK(action->isChecked() == false);
    CHECK(modes->isEnabled() == true);
}

TEST_CASE("profiling reports the failed prerequisite without starting a recording", "[profiling][editor]")
{
    QTemporaryDir directory;
    RenderPreview preview;
    ProfilingController controller(&preview);
    CHECK(controller.Start(ProfilingMode::Cpu, {}) == false);
    CHECK(controller.GetStatus().contains(QStringLiteral("output path")) == true);
    const QString existingPath = directory.filePath(QStringLiteral("existing.json"));
    QFile existing(existingPath);
    REQUIRE(existing.open(QIODevice::WriteOnly) == true);
    existing.close();
    CHECK(controller.Start(ProfilingMode::Cpu, existingPath) == false);
    CHECK(controller.GetStatus().contains(QStringLiteral("already exists")) == true);
    CHECK(controller.Start(ProfilingMode::Both, directory.filePath(QStringLiteral("both.json"))) == false);
    CHECK(controller.GetStatus().contains(QStringLiteral("GPU timing")) == true);
    CHECK(controller.GetStatus().contains(QStringLiteral("CPU")) == true);
    CHECK(profile::IsCaptureActive() == false);
}

TEST_CASE("profiling disconnects a previous viewport and preserves the source of an active capture",
          "[profiling][editor]")
{
    QTemporaryDir directory;
    RenderPreview selectedPreview;
    ProfilingController controller(&selectedPreview);
    {
        RenderPreview previousPreview;
        REQUIRE(controller.SetPreviewSource(&previousPreview) == true);
        REQUIRE(controller.SetPreviewSource(&selectedPreview) == true);
        REQUIRE(controller.Start(ProfilingMode::Cpu, directory.filePath(QStringLiteral("selected.json"))) == true);
        CHECK(controller.SetPreviewSource(&previousPreview) == false);
    }
    CHECK(controller.GetState() == ProfilingState::Recording);
    controller.Stop();
    REQUIRE(FinishRecording(controller) == true);
    CHECK(controller.GetState() == ProfilingState::Complete);
    CHECK(ReadTrace(controller.GetLastPath()).isEmpty() == false);
}

TEST_CASE("repeated profiling toggles save distinct traces and enable revealing the last capture",
          "[profiling][editor]")
{
    QTemporaryDir directory;
    RenderPreview preview;
    QAction* action = CreateEditorCommand(PROFILING_RECORD_COMMAND, &preview);
    ProfilingPanel panel(&preview, action);
    panel.SetProjectRoot(directory.path());
    QLabel* status = panel.findChild<QLabel*>(QStringLiteral("profilingStatus"));
    QPushButton* reveal = panel.findChild<QPushButton*>(QStringLiteral("profilingReveal"));
    REQUIRE(status != nullptr);
    REQUIRE(reveal != nullptr);
    CHECK(reveal->isEnabled() == false);
    QString previousPath;
    constexpr usize CAPTURE_COUNT = 8;
    for (usize index = 0; index < CAPTURE_COUNT; ++index)
    {
        action->trigger();
        REQUIRE(panel.GetController().GetState() == ProfilingState::Recording);
        CHECK(action->text() == QStringLiteral("Stop Profiling"));
        CHECK(status->text().contains(QStringLiteral("Recording")) == true);
        action->trigger();
        CHECK(panel.GetController().GetState() == ProfilingState::Finalizing);
        CHECK(action->isEnabled() == false);
        action->trigger();
        REQUIRE(FinishRecording(panel.GetController()) == true);
        INFO(panel.GetController().GetStatus().toStdString());
        REQUIRE(panel.GetController().GetState() == ProfilingState::Complete);
        const QString path = panel.GetController().GetLastPath();
        CHECK(path != previousPath);
        const QByteArray trace = QJsonDocument(ReadTrace(path)).toJson();
        CHECK(trace.contains("EditorRecordingStarted") == true);
        CHECK(trace.contains("EditorRecordingStopped") == true);
        CHECK(reveal->isEnabled() == true);
        CHECK(status->text().contains(path) == true);
        previousPath = path;
    }
}

TEST_CASE("editor shell Ctrl+Alt+F9 records through the same menu toolbar and Profiling window action",
          "[profiling][editor]")
{
    QTemporaryDir directory;
    EditorController controller({}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    QAction* action = window.findChild<QAction*>(QStringLiteral("profiling.record"));
    ProfilingPanel* panel = window.findChild<ProfilingPanel*>(QStringLiteral("profilingPanel"));
    REQUIRE(action != nullptr);
    REQUIRE(panel != nullptr);
    QToolBar* toolbar = panel->findChild<QToolBar*>();
    QMenu* tools = window.findChild<QMenu*>(QStringLiteral("toolsMenu"));
    REQUIRE(toolbar != nullptr);
    REQUIRE(tools != nullptr);
    CHECK(toolbar->actions().contains(action) == true);
    CHECK(tools->actions().contains(action) == true);
    QLineEdit* destination = panel->findChild<QLineEdit*>(QStringLiteral("profilingDestination"));
    REQUIRE(destination != nullptr);
    destination->setText(directory.filePath(QStringLiteral("shell.json")));
    window.show();
    QDockWidget* dock = window.findChild<QDockWidget*>(QStringLiteral("profilingDock"));
    REQUIRE(dock != nullptr);
    dock->hide();
    window.activateWindow();
    QTest::qWait(20);
    // Record Profiling is Ctrl+Alt+F9 in every keymap profile (shortcut resolution decided 2026-11-03).
    QTest::keyClick(&window, Qt::Key_F9, Qt::ControlModifier | Qt::AltModifier);
    REQUIRE(panel->GetController().GetState() == ProfilingState::Recording);
    CHECK(dock->isVisible() == true);
    {
        LUDUS_PROFILE_SCOPE(EditorF9Evidence);
    }
    QTest::keyClick(&window, Qt::Key_F9, Qt::ControlModifier | Qt::AltModifier);
    REQUIRE(FinishRecording(panel->GetController()) == true);
    CHECK(panel->GetController().GetState() == ProfilingState::Complete);
    CHECK(ReadTrace(destination->text()).isEmpty() == false);
    const QString screenshot = qEnvironmentVariable("LUDUS_EDITOR_PROFILING_SCREENSHOT");
    if (screenshot.isEmpty() == false)
    {
        REQUIRE(window.grab().save(screenshot) == true);
    }
    destination->setText(directory.filePath(QStringLiteral("closing.json")));
    action->trigger();
    REQUIRE(panel->GetController().GetState() == ProfilingState::Recording);
    {
        LUDUS_PROFILE_SCOPE(EditorClosingEvidence);
    }
    CHECK(window.close() == false);
    REQUIRE(FinishRecording(panel->GetController()) == true);
    CHECK(panel->GetController().GetState() == ProfilingState::Complete);
    CHECK(QJsonDocument(ReadTrace(destination->text())).toJson().contains("EditorClosingEvidence") == true);
}
#endif

#if LUDUS_PROFILING_ENABLED && defined(LUDUS_EDITOR_GPU_PREVIEW)
TEST_CASE("real viewport GPU timing exports GPU-only and combined traces with Stats hidden", "[.profiling-gpu]")
{
    QTemporaryDir directory;
    RenderPreview preview;
    preview.SetPresentationVisible(true);
    preview.Resize(64, 64);
    const RenderScene scene = MakeProfilingScene();
    REQUIRE(preview.SetScene(scene) == true);
    REQUIRE(preview.Start() == true);
    REQUIRE(preview.AdmitOffscreenDevice() == true);
    const bool hasFrame = QTest::qWaitFor([&preview]() { return preview.FrameAvailable(); }, 10000);
    INFO(preview.Status().toStdString());
    REQUIRE(hasFrame == true);
    ProfilingController controller(&preview);
    for (const ProfilingMode mode : {ProfilingMode::Gpu, ProfilingMode::Both})
    {
        const QString path =
            directory.filePath(mode == ProfilingMode::Gpu ? QStringLiteral("gpu.json") : QStringLiteral("both.json"));
        REQUIRE(controller.Start(mode, path) == true);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 500)
        {
            {
                LUDUS_PROFILE_SCOPE(GpuModeCpuEvidence);
                preview.Tick();
            }
            ludus::graphics::renderer::ViewportFrame frame;
            ludus::graphics::renderer::ViewportImage image;
            if (preview.TakeFrame(frame, image) == true)
            {
                preview.ReleaseFrame(frame);
            }
            QTest::qWait(1);
        }
        controller.Stop();
        REQUIRE(FinishRecording(controller) == true);
        INFO(controller.GetStatus().toStdString());
        REQUIRE(controller.GetState() == ProfilingState::Complete);
        const QString evidence = qEnvironmentVariable("LUDUS_EDITOR_PROFILING_CAPTURE");
        if (evidence.isEmpty() == false)
        {
            REQUIRE(QFile::copy(path,
                                evidence + (mode == ProfilingMode::Gpu ? QStringLiteral("-gpu.json")
                                                                       : QStringLiteral("-both.json"))) == true);
        }
        const QJsonObject trace = ReadTrace(path);
        const QJsonObject coverage = trace.value(QStringLiteral("ludusGpuTiming")).toObject();
        CHECK(coverage.value(QStringLiteral("sampleCount")).toInt() > 0);
        if (preview.CanProfileGpuPasses() == true)
        {
            CHECK(coverage.value(QStringLiteral("passTimingSupported")).toBool() == true);
            CHECK(coverage.value(QStringLiteral("passSampleCount")).toInt() > 0);
            CHECK(QJsonDocument(trace).toJson().contains("GPU/Pass/Viewport/Scene") == true);
            CHECK(QJsonDocument(trace).toJson().contains("GPU/Pass/Viewport/Composite") == true);
#    if defined(LUDUS_PLATFORM_MACOS)
            // Metal calibrates pass timestamps onto the CPU clock, so passes become named slices.
            CHECK(coverage.value(QStringLiteral("executionSliceCount")).toInt() > 0);
            CHECK(
                QJsonDocument(trace).toJson(QJsonDocument::Compact).contains("\"name\":\"Viewport/Scene (Raster)\"") ==
                true);
#    endif
        }
        CHECK(coverage.value(QStringLiteral("eligibleFrames")).toInt() >=
              coverage.value(QStringLiteral("frameSampleCount")).toInt());
        CHECK(QJsonDocument(trace).toJson().contains("GpuModeCpuEvidence") == (mode == ProfilingMode::Both));
        CHECK(trace.contains(QStringLiteral("ludusViewportTiming")) == (mode == ProfilingMode::Both));
    }
    preview.Stop();
}
TEST_CASE("delegated project graph Stats and GPU recording share the viewport sampler", "[.profiling-gpu]")
{
    QTemporaryDir directory;
    RenderPreview initialPreview;
    ProfilingController controller(&initialPreview);
    CHECK(controller.CanRecordGpu() == false);
    RendererWorkspace workspace({});
    RenderPreview preview;
    preview.UseProjectGraph();
    workspace.SetPreviewSource(&preview);
    workspace.EnableWorldRendering();
    workspace.SetProject(directory.path(), 1);
    REQUIRE(preview.SetScene(MakeProfilingScene()) == true);
    preview.SetStatisticsVisible(true);
    REQUIRE(preview.Start() == true);
    REQUIRE(preview.AdmitOffscreenDevice() == true);
    REQUIRE(controller.SetPreviewSource(&preview) == true);
    CHECK(controller.CanRecordGpu() == true);
    WorldGraphView view(workspace, nullptr);
    view.resize(320, 180);
    view.show();
    REQUIRE(workspace.GetPreviewOrNull() != nullptr);
    QElapsedTimer timer;
    timer.start();
    while (workspace.GetPreviewOrNull()->GetImage().isNull() == true && timer.elapsed() < 10000)
    {
        workspace.ServicePreview();
        QTest::qWait(1);
    }
    REQUIRE(workspace.GetPreviewOrNull()->GetImage().isNull() == false);
    const QString path = directory.filePath(QStringLiteral("graph.json"));
    REQUIRE(controller.Start(ProfilingMode::Both, path) == true);
    CHECK(controller.SetPreviewSource(&initialPreview) == false);
    timer.restart();
    while (timer.elapsed() < 1000)
    {
        preview.Look(1, 0);
        workspace.ServicePreview();
        QTest::qWait(1);
    }
    controller.Stop();
    REQUIRE(FinishRecording(controller) == true);
    INFO(controller.GetStatus().toStdString());
    REQUIRE(controller.GetState() == ProfilingState::Complete);
    const QJsonObject trace = ReadTrace(path);
    CHECK(trace.value(QStringLiteral("ludusGpuTiming")).toObject().value(QStringLiteral("sampleCount")).toInt() > 0);
    CHECK(QJsonDocument(trace).toJson().contains("EditorRenderGraphTick") == true);
    CHECK(QJsonDocument(trace).toJson().contains("Await producer completion (observed)") == true);
    CHECK(QJsonDocument(trace).toJson().contains("Readback requests and wait (observed)") == true);
    CHECK(QJsonDocument(trace).toJson().contains("CPU display image conversion") == true);
    CHECK(QJsonDocument(trace).toJson().contains("Frame to Qt paint complete") == true);
    CHECK(trace.value(QStringLiteral("ludusViewportTiming")).toObject().value(QStringLiteral("droppedSpans")).toInt() ==
          0);
    const QJsonArray graphFrames = trace.value(QStringLiteral("ludusGpuGraphFrames")).toArray();
    REQUIRE(graphFrames.isEmpty() == false);
    QFile cookedPackage(workspace.GetPreviewOrNull()->GetActivePackagePath());
    REQUIRE(cookedPackage.open(QIODevice::ReadOnly) == true);
    const QByteArray cookedIdentity =
        QCryptographicHash::hash(cookedPackage.readAll(), QCryptographicHash::Sha256).toHex();
    for (const QJsonValue& frameValue : graphFrames)
    {
        CHECK(frameValue.toObject().value(QStringLiteral("runtime_sha256")).toString().toLatin1() == cookedIdentity);
    }
    bool hasMatchedGpuFrame = false;
    for (const QJsonValue& wallValue : trace.value(QStringLiteral("traceEvents")).toArray())
    {
        const QJsonObject wall = wallValue.toObject();
        if (wall.value(QStringLiteral("name")).toString() != QStringLiteral("Frame to Qt paint complete"))
        {
            continue;
        }
        const QJsonObject identity = wall.value(QStringLiteral("args")).toObject();
        for (const QJsonValue& gpuValue : trace.value(QStringLiteral("traceEvents")).toArray())
        {
            const QJsonObject gpu = gpuValue.toObject();
            const QJsonObject arguments = gpu.value(QStringLiteral("args")).toObject();
            if (gpu.value(QStringLiteral("name")).toString() == QStringLiteral("GPU sample identity") &&
                identity.value(QStringLiteral("frameId")) == arguments.value(QStringLiteral("frameId")) &&
                identity.value(QStringLiteral("deviceSession")) == arguments.value(QStringLiteral("deviceSession")))
            {
                hasMatchedGpuFrame = true;
                const float64 submitted = gpu.value(QStringLiteral("ts")).toDouble();
                const float64 frameBegin = wall.value(QStringLiteral("ts")).toDouble();
                CHECK(submitted >= frameBegin);
                CHECK(submitted <= frameBegin + wall.value(QStringLiteral("dur")).toDouble());
            }
        }
    }
    CHECK(hasMatchedGpuFrame == true);
    const QString evidence = qEnvironmentVariable("LUDUS_EDITOR_PROFILING_CAPTURE");
    if (evidence.isEmpty() == false)
    {
        REQUIRE(QFile::copy(path, evidence + QStringLiteral("-graph.json")) == true);
    }
    if (preview.CanProfileGpuPasses() == true)
    {
        CHECK(
            trace.value(QStringLiteral("ludusGpuTiming")).toObject().value(QStringLiteral("passSampleCount")).toInt() >
            0);
        CHECK(QJsonDocument(trace).toJson().contains("GPU/Pass/Main/raster/draw") == true);
    }
    CHECK(workspace.GetPreviewOrNull()->GetImage().isNull() == false);
    CHECK(preview.GetStatistics().GpuState == ViewportGpuTimeState::Available);
    CHECK(preview.GetStatistics().GpuMilliseconds.GetCount() > 0);
    CHECK(workspace.GetPreviewOrNull()->GetStatistics().GetCpuMilliseconds().GetCount() > 0);
    CHECK(workspace.GetPreviewOrNull()->GetStatistics().GetReadbackMilliseconds().GetCount() > 0);
    preview.Stop();
    workspace.SetPreviewSource(nullptr);
}

TEST_CASE("a hosted recording keeps the game's identity across a disconnect and refuses earlier samples",
          "[profiling][editor][hosted]")
{
    namespace platform = ludus::platform;
    QTemporaryDir directory;
    RenderPreview preview;
    ProfilingController controller(&preview);
    ScriptedRecordingTarget game;
    REQUIRE(controller.SetHostedTarget(&game, &game) == true);
    CHECK(controller.GetTargetDescription().contains(QStringLiteral("process 4321")) == true);
    CHECK(controller.Start(ProfilingMode::Both, directory.filePath(QStringLiteral("both.json"))) == false);
    CHECK(controller.GetStatus().contains(QStringLiteral("Both is unavailable")) == true);
    CHECK(game.Requests.isEmpty() == true);

    const QString firstPath = directory.filePath(QStringLiteral("first.json"));
    REQUIRE(controller.Start(ProfilingMode::Gpu, firstPath) == true);
    REQUIRE(game.Requests.size() == 1);
    const platform::HostedRecordingRequest start = game.Requests[0];
    CHECK(start.Kind == platform::HostedRecordingRequestKind::Start);
    // The game's acknowledged start frame bounds what is admitted.
    game.Acknowledge(start, platform::HostedRecordingResult::Ok, 5);
    game.SendSample(start.Recording + 1, 5);
    game.SendSample(start.Recording, 4);
    game.SendSample(start.Recording, 5);
    game.SendSample(start.Recording, 6);
    CHECK(controller.SetHostedTarget(nullptr, nullptr) == false);
    // The game goes away after samples and before acknowledging Stop.
    game.Disconnect();
    REQUIRE(AwaitState(controller, ProfilingState::Complete) == true);
    CHECK(controller.GetStatus().contains(QStringLiteral("disconnected before acknowledging Stop")) == true);
    const QJsonObject first = ReadTrace(firstPath);
    REQUIRE(first.isEmpty() == false);
    const QJsonObject producer = first.value(QStringLiteral("ludusHostedProducer")).toObject();
    CHECK(producer.value(QStringLiteral("processId")).toString() == QStringLiteral("4321"));
    CHECK(producer.value(QStringLiteral("producerName")).toString() == QStringLiteral("scripted game"));
    CHECK(producer.value(QStringLiteral("recording")).toString() == QString::number(start.Recording));
    CHECK(producer.value(QStringLiteral("startFrame")).toString() == QStringLiteral("5"));
    CHECK(producer.value(QStringLiteral("lastSampleFrame")).toString() == QStringLiteral("6"));
    CHECK(producer.value(QStringLiteral("complete")).toBool() == false);
    CHECK(producer.value(QStringLiteral("disconnected")).toBool() == true);
    CHECK(producer.value(QStringLiteral("stopAcknowledged")).toBool() == false);
    CHECK(producer.value(QStringLiteral("rejectedSamples")).toString() == QStringLiteral("2"));
    CHECK(producer.value(QStringLiteral("clockRelationship")).toString().contains(QStringLiteral("game's")) == true);
    CHECK(first.value(QStringLiteral("ludusGpuTiming")).toObject().value(QStringLiteral("sampleCount")).toInt() == 2);
    CHECK(QJsonDocument(first).toJson().contains("hosted game scripted game (pid 4321)") == true);
    CHECK(QJsonDocument(first).toJson().contains("Game/Surface") == true);
    // The game's device reported per-pass counters, and the trace says so.
    CHECK(producer.value(QStringLiteral("passTimingSupported")).toBool() == true);
    CHECK(first.value(QStringLiteral("ludusGpuTiming"))
              .toObject()
              .value(QStringLiteral("passTimingSupported"))
              .toBool() == true);
    CHECK(controller.GetStatus().contains(QStringLiteral("Per-pass counters unavailable")) == false);

    // Reconnected: the next recording refuses the earlier recording's delayed samples even
    // though the new game process numbers its frames from one again.
    game.IsConnected = true;
    const QString secondPath = directory.filePath(QStringLiteral("second.json"));
    REQUIRE(controller.Start(ProfilingMode::Gpu, secondPath) == true);
    REQUIRE(game.Requests.size() == 2);
    const platform::HostedRecordingRequest restart = game.Requests[1];
    CHECK(restart.Recording > start.Recording);
    CHECK(restart.Request > start.Request);
    game.SendSample(start.Recording, 1);
    game.Acknowledge(restart, platform::HostedRecordingResult::Ok, 1);
    game.SendSample(start.Recording, 1);
    game.SendSample(restart.Recording, 1);
    controller.Stop();
    REQUIRE(game.Requests.size() == 3);
    const platform::HostedRecordingRequest stop = game.Requests[2];
    CHECK(stop.Kind == platform::HostedRecordingRequestKind::Stop);
    CHECK(stop.Recording == restart.Recording);
    game.Acknowledge(stop,
                     platform::HostedRecordingResult::Ok,
                     3,
                     { .SubmittedFrames = 3, .MeasuredFrames = 2, .SkippedFrames = 1, .DeliveredSamples = 1 });
    REQUIRE(AwaitState(controller, ProfilingState::Complete) == true);
    const QJsonObject second = ReadTrace(secondPath);
    const QJsonObject restarted = second.value(QStringLiteral("ludusHostedProducer")).toObject();
    CHECK(restarted.value(QStringLiteral("complete")).toBool() == true);
    CHECK(restarted.value(QStringLiteral("stopFrame")).toString() == QStringLiteral("3"));
    CHECK(restarted.value(QStringLiteral("skippedFrames")).toString() == QStringLiteral("1"));
    CHECK(restarted.value(QStringLiteral("rejectedSamples")).toString() == QStringLiteral("2"));
    CHECK(second.value(QStringLiteral("ludusGpuTiming")).toObject().value(QStringLiteral("sampleCount")).toInt() == 1);
    CHECK(second.value(QStringLiteral("ludusGpuTiming")).toObject().value(QStringLiteral("eligibleFrames")).toInt() ==
          3);
}

TEST_CASE("a hosted recording on a game device without per-pass counters records frame timing only",
          "[profiling][editor][hosted]")
{
    namespace platform = ludus::platform;
    QTemporaryDir directory;
    RenderPreview preview;
    ProfilingController controller(&preview);
    ScriptedRecordingTarget game;
    REQUIRE(controller.SetHostedTarget(&game, &game) == true);
    const QString path = directory.filePath(QStringLiteral("frames.json"));
    REQUIRE(controller.Start(ProfilingMode::Gpu, path) == true);
    REQUIRE(game.Requests.size() == 1);
    const platform::HostedRecordingRequest start = game.Requests[0];
    const QString frameTimingOnly =
        QStringLiteral("Per-pass counters unavailable on the game's device; frame timing only.");
    // Before the game answers, nothing is claimed either way.
    CHECK(controller.GetStatus().contains(frameTimingOnly) == false);
    game.Acknowledge(start, platform::HostedRecordingResult::Ok, 2, {}, false);
    CHECK(controller.GetHostedProducer().IsPassTimingSupported == false);
    CHECK(controller.GetStatus().contains(frameTimingOnly) == true);
    // Only the announced kind is admitted: whole frames, not a pass the device cannot time.
    game.SendFrameSample(start.Recording, 2);
    game.SendSample(start.Recording, 2);
    game.SendFrameSample(start.Recording, 3);
    CHECK(controller.GetRecordedSampleCount() == 2);
    controller.Stop();
    REQUIRE(game.Requests.size() == 2);
    game.Acknowledge(game.Requests[1],
                     platform::HostedRecordingResult::Ok,
                     3,
                     { .SubmittedFrames = 2, .MeasuredFrames = 2, .DeliveredSamples = 2 });
    REQUIRE(AwaitState(controller, ProfilingState::Complete) == true);
    CHECK(controller.GetStatus().contains(frameTimingOnly) == true);
    const QJsonObject trace = ReadTrace(path);
    REQUIRE(trace.isEmpty() == false);
    const QJsonObject timing = trace.value(QStringLiteral("ludusGpuTiming")).toObject();
    CHECK(timing.value(QStringLiteral("passTimingSupported")).toBool() == false);
    CHECK(timing.value(QStringLiteral("frameSampleCount")).toInt() == 2);
    CHECK(timing.value(QStringLiteral("passSampleCount")).toInt() == 0);
    const QJsonObject producer = trace.value(QStringLiteral("ludusHostedProducer")).toObject();
    CHECK(producer.value(QStringLiteral("passTimingSupported")).toBool() == false);
    CHECK(producer.value(QStringLiteral("sampleKind")).toString().contains(QStringLiteral("no per-pass counters")) ==
          true);
    CHECK(producer.value(QStringLiteral("rejectedSamples")).toString() == QStringLiteral("1"));
    CHECK(producer.value(QStringLiteral("complete")).toBool() == true);
    CHECK(QJsonDocument(trace).toJson().contains("Game/Surface") == false);
}

TEST_CASE("a hosted game without a recorder fails the recording with its reason", "[profiling][editor][hosted]")
{
    namespace platform = ludus::platform;
    QTemporaryDir directory;
    RenderPreview preview;
    ProfilingController controller(&preview);
    ScriptedRecordingTarget game;
    REQUIRE(controller.SetHostedTarget(&game, &game) == true);
    REQUIRE(controller.Start(ProfilingMode::Gpu, directory.filePath(QStringLiteral("refused.json"))) == true);
    REQUIRE(game.Requests.size() == 1);
    game.Acknowledge(game.Requests[0], platform::HostedRecordingResult::Unsupported, 0);
    REQUIRE(AwaitState(controller, ProfilingState::Failed) == true);
    CHECK(controller.GetStatus().contains(QStringLiteral("cannot record GPU timing")) == true);
    CHECK(controller.HasUnsavedSnapshot() == false);
    CHECK(QFileInfo::exists(directory.filePath(QStringLiteral("refused.json"))) == false);
    // A game that goes away takes the target with it; the editor viewport is targeted again.
    {
        ScriptedRecordingTarget transient;
        REQUIRE(controller.SetHostedTarget(&transient, &transient) == true);
    }
    CHECK(controller.GetTargetDescription().contains(QStringLiteral("World viewport's scene device")) == true);
}
#endif
