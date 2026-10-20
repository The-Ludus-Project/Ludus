#include "internal/editor_commands.h"
#include "internal/main_window.h"
#include "internal/profiling_panel.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/profiling/perfetto_export.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

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
    CHECK(action->shortcut() == QKeySequence(QStringLiteral("F9")));
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

TEST_CASE("editor shell F9 records through the same menu toolbar and Profiling window action", "[profiling][editor]")
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
    dock->show();
    dock->raise();
    window.activateWindow();
    QTest::qWait(20);
    QTest::keyClick(&window, Qt::Key_F9);
    REQUIRE(panel->GetController().GetState() == ProfilingState::Recording);
    {
        LUDUS_PROFILE_SCOPE(EditorF9Evidence);
    }
    QTest::keyClick(&window, Qt::Key_F9);
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
        }
        CHECK(coverage.value(QStringLiteral("eligibleFrames")).toInt() >=
              coverage.value(QStringLiteral("frameSampleCount")).toInt());
        CHECK(QJsonDocument(trace).toJson().contains("GpuModeCpuEvidence") == (mode == ProfilingMode::Both));
    }
    preview.Stop();
}
TEST_CASE("delegated project graph GPU recording uses the shared viewport sampler", "[.profiling-gpu]")
{
    QTemporaryDir directory;
    RendererWorkspace workspace({});
    RenderPreview preview;
    preview.UseProjectGraph();
    workspace.SetPreviewSource(&preview);
    workspace.EnableWorldRendering();
    workspace.SetProject(directory.path(), 1);
    REQUIRE(preview.SetScene(MakeProfilingScene()) == true);
    REQUIRE(preview.Start() == true);
    REQUIRE(preview.AdmitOffscreenDevice() == true);
    ProfilingController controller(&preview);
    const QString path = directory.filePath(QStringLiteral("graph.json"));
    REQUIRE(controller.Start(ProfilingMode::Both, path) == true);
    QElapsedTimer timer;
    timer.start();
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
    REQUIRE(workspace.GetPreviewOrNull() != nullptr);
    CHECK(workspace.GetPreviewOrNull()->GetImage().isNull() == false);
    preview.Stop();
    workspace.SetPreviewSource(nullptr);
}
#endif
