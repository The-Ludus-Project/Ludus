// Ray path inspection in the editor (docs/architecture/render-graph-authoring.md#ray-path-inspection-proposed,
// "Acceptance evidence"): navigation keeps the frozen paths, a World edit marks the capture stale,
// leaving inspection restores the live graph, and a device without native rays reports capture as
// unavailable while raster navigation keeps working. The capture fixture is the diagnostic
// `LUDUS_RAY_PATH_CAPTURE` package of the same sources as LUDUS_EDITOR_RAY_GRAPH_FIXTURE.
// Hosts without a GPU adapter, or without native ray queries, skip the GPU cases.
#include "editor_default_graph.h"
#include "internal/ray_path_capture.h"
#include "internal/ray_path_inspection.h"
#include "internal/ray_path_inspector.h"
#include "internal/ray_path_overlay.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/workspace_style.h"
#include "renderer_composite.h"

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/rhi.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QMenu>
#include <QObject>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QtGlobal>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace default_graph = ludus::render_graph_packages::editor_default_graph;
namespace shaders = ludus::shaders;
namespace
{
constexpr usize ATTEMPTS = 20000;
constexpr uint32 POLL_MICROSECONDS = 200;
/// Ticks a frozen preview gets to prove it submits no live frame.
constexpr usize IDLE_TICKS = 32;
/// Lines of the frozen frustum (near and image-plane rectangles plus four edges) and footprint.
constexpr usize CAMERA_LINES = 16;
/// Index of the first event line, after the camera lines.
constexpr qsizetype FIRST_EVENT_LINE = static_cast<qsizetype>(CAMERA_LINES);
/// Graph and node of the RayTracingPipeline in tests/sdk_consumer/rg2/raytracing.
RayPathTarget GetTraceTarget()
{
    return {QStringLiteral("Main"), QStringLiteral("trace")};
}

struct DeviceFixture final
{
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    bool IsAvailable = false;
    explicit DeviceFixture(bool shouldRequestRay)
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Preferred.RayQueries = shouldRequestRay;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus started =
            rhi::CreateDevice({}, { .Width = 1, .Height = 1 }, description, Device, Surface, &failure);
        IsAvailable = started == rhi::DeviceStatus::Ready;
        if (started != rhi::DeviceStatus::Ready)
        {
            REQUIRE(failure.Error == rhi::StartupError::AdapterUnavailable);
        }
    }
    ~DeviceFixture()
    {
        if (IsAvailable == true)
        {
            static_cast<void>(rhi::DestroyDevice(Device));
        }
        rhi::Shutdown();
    }
    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;
};

/// One opaque triangle at depth 0.5 in front of the orthographic identity camera.
RenderGraphExtraction MakeExtraction(math::Vector4 tint, uint64 revision)
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("ray-path-fixture");
    PreviewMesh mesh;
    mesh.Id = QStringLiteral("triangle");
    mesh.Positions = {{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("object");
    object.Mesh = 0;
    object.Tint = tint;
    scene.Objects.append(object);
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(scene, {}, revision, extraction, error) == true);
    // Identity WorldToClip describes an orthographic camera for native camera-ray generation.
    extraction.View.OrthographicCamera = true;
    extraction.ContentRevision = revision;
    return extraction;
}

RenderGraphPreview MakePreview()
{
    return RenderGraphPreview(
        {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
        shaders::renderer_composite::Vertex(),
        shaders::renderer_composite::Fragment());
}

template <typename Predicate>
bool TickUntil(RenderGraphPreview& inoutPreview, Predicate predicate)
{
    for (usize attempt = 0; attempt < ATTEMPTS; ++attempt)
    {
        if (predicate() == true)
        {
            return true;
        }
        inoutPreview.Tick();
        QThread::usleep(POLL_MICROSECONDS);
    }
    return predicate();
}

bool IsSettled(RayPathCaptureState state) noexcept
{
    return state == RayPathCaptureState::Ready || state == RayPathCaptureState::Failed ||
           state == RayPathCaptureState::Unavailable;
}

renderer::RayPathEvent
MakeEvent(uint32 sample, renderer::RayPathEventKind kind, renderer::RayPathOutcome outcome, uint32 sequence)
{
    renderer::RayPathEvent event;
    event.Sample = sample;
    event.Sequence = sequence;
    event.Kind = kind;
    event.Outcome = outcome;
    event.OriginWorld = {0, 0, 0};
    event.DirectionWorld = {0, 0, 1};
    event.MaxDistanceWorld = 100;
    event.HitDistanceWorld = outcome == renderer::RayPathOutcome::Hit ? .5F : event.MaxDistanceWorld;
    event.SourceId = outcome == renderer::RayPathOutcome::Hit ? 1 : 0;
    return event;
}
} // namespace

TEST_CASE("Ray path inspector keeps controls readable in a narrow dock and synchronizes overlay settings",
          "[editor][ray-paths][layout]")
{
    RenderGraphPreview preview = MakePreview();
    RayPathInspection inspection;
    inspection.SetPreview(&preview);
    RayPathInspector inspector(inspection);
    ApplyWorkspaceStyle(&inspector);
    inspector.resize(280, 720);
    inspector.show();
    QCoreApplication::processEvents();
    QComboBox* colorOrNull = inspector.findChild<QComboBox*>(QStringLiteral("rayPathColorMode"));
    QComboBox* sampleOrNull = inspector.findChild<QComboBox*>(QStringLiteral("rayPathSample"));
    QCheckBox* xRayOrNull = inspector.findChild<QCheckBox*>(QStringLiteral("rayPathXRay"));
    QPushButton* captureOrNull = inspector.findChild<QPushButton*>(QStringLiteral("rayPathCaptureAgain"));
    QPushButton* leaveOrNull = inspector.findChild<QPushButton*>(QStringLiteral("rayPathLeave"));
    REQUIRE(colorOrNull != nullptr);
    REQUIRE(sampleOrNull != nullptr);
    REQUIRE(xRayOrNull != nullptr);
    REQUIRE(captureOrNull != nullptr);
    REQUIRE(leaveOrNull != nullptr);
    CHECK(inspector.width() == 280);
    CHECK(inspector.GetStatusText().contains(QStringLiteral("Start the World viewport")) == true);
    CHECK(colorOrNull->isEnabled() == false);
    CHECK(sampleOrNull->isEnabled() == false);
    CHECK(sampleOrNull->currentText() == QStringLiteral("All samples"));
    CHECK(xRayOrNull->isEnabled() == false);
    CHECK(captureOrNull->isEnabled() == false);
    CHECK(leaveOrNull->isEnabled() == false);
    CHECK(colorOrNull->width() >= colorOrNull->fontMetrics().horizontalAdvance(QStringLiteral("Bounce depth")));
    CHECK(captureOrNull->width() >= captureOrNull->fontMetrics().horizontalAdvance(captureOrNull->text()));
    CHECK(leaveOrNull->width() >= leaveOrNull->fontMetrics().horizontalAdvance(leaveOrNull->text()));
    CHECK(colorOrNull->mapTo(&inspector, colorOrNull->rect().bottomRight()).x() < inspector.width());
    CHECK(sampleOrNull->mapTo(&inspector, sampleOrNull->rect().bottomRight()).x() < inspector.width());
    CHECK(colorOrNull->mapTo(&inspector, QPoint{}).y() < sampleOrNull->mapTo(&inspector, QPoint{}).y());
    inspection.SetXRay(true);
    inspection.SetColorMode(RayPathColorMode::BounceDepth);
    CHECK(xRayOrNull->isChecked() == true);
    CHECK(colorOrNull->currentData().toInt() == static_cast<int32>(RayPathColorMode::BounceDepth));
    CHECK(inspector.SelectEventRow(-1) == false);
    CHECK(inspector.GetDetailsText().isEmpty() == true);
}

TEST_CASE("Ray path overlays draw the frozen camera, color events and stay within the overlay limit",
          "[editor][ray-paths]")
{
    RayPathCamera camera;
    camera.LaunchExtent = {4, 4};
    camera.PixelX = 1;
    camera.PixelY = 2;
    QVector<renderer::RayPathEvent> events{
        MakeEvent(0, renderer::RayPathEventKind::Closest, renderer::RayPathOutcome::Hit, 0),
        MakeEvent(0, renderer::RayPathEventKind::Visibility, renderer::RayPathOutcome::Hit, 1),
        MakeEvent(0, renderer::RayPathEventKind::Closest, renderer::RayPathOutcome::Miss, 2),
        MakeEvent(1, renderer::RayPathEventKind::Closest, renderer::RayPathOutcome::Failure, 0),
    };
    QVector<renderer::RayPathSummary> summaries(2);
    summaries[1].Sample = 1;
    summaries[1].IsTruncated = true;
    const QVector<uint32> depths = ComputeRayPathBounceDepths({events.constData(), static_cast<usize>(events.size())});
    CHECK(depths == QVector<uint32>{0, 0, 1, 0});
    RayPathOverlayOptions options;
    options.SceneBoundsWorld = {{-1, -1, -1}, {1, 1, 1}};
    options.HasSceneBounds = true;
    const RayPathOverlay overlay = BuildRayPathOverlay(camera,
                                                       {events.constData(), static_cast<usize>(events.size())},
                                                       {summaries.constData(), static_cast<usize>(summaries.size())},
                                                       options);
    // Camera, four event segments, one faded miss tail and one three-line hit marker.
    REQUIRE(static_cast<usize>(overlay.Lines.size()) == CAMERA_LINES + 4 + 1 + 3);
    CHECK(overlay.DroppedLines == 0);
    const RayPathLine& hit = overlay.Lines[FIRST_EVENT_LINE];
    const RayPathLine& occluded = overlay.Lines[FIRST_EVENT_LINE + 1];
    const RayPathLine& miss = overlay.Lines[FIRST_EVENT_LINE + 2];
    CHECK(hit.EndWorld.Z == .5F);
    CHECK(hit.Color.X != occluded.Color.X);
    // A miss ends where it leaves the scene bounds, never at its unbounded interval.
    CHECK(miss.EndWorld.Z == 1);
    const RayPathLine& failure = overlay.Lines[FIRST_EVENT_LINE + 4];
    CHECK(failure.Color.W < 1);
    CHECK(failure.WidthPixels < hit.WidthPixels);
    options.PassFilter = 1;
    CHECK(static_cast<usize>(BuildRayPathOverlay(camera,
                                                 {events.constData(), static_cast<usize>(events.size())},
                                                 {summaries.constData(), static_cast<usize>(summaries.size())},
                                                 options)
                                 .Lines.size()) == CAMERA_LINES);
    options.PassFilter = -1;
    options.SelectedEvent = 0;
    const RayPathOverlay selected = BuildRayPathOverlay(camera,
                                                        {events.constData(), static_cast<usize>(events.size())},
                                                        {summaries.constData(), static_cast<usize>(summaries.size())},
                                                        options);
    CHECK(selected.Lines[FIRST_EVENT_LINE].WidthPixels > hit.WidthPixels);
    CHECK(selected.Lines[FIRST_EVENT_LINE + 1].Color.W < occluded.Color.W);
    // Beyond OverlayList's 512 lines, later lines are dropped and counted, never drawn.
    QVector<renderer::RayPathEvent> many;
    for (uint32 i = 0; i < MAX_RAY_PATH_LINES; ++i)
    {
        many.append(MakeEvent(0, renderer::RayPathEventKind::Visibility, renderer::RayPathOutcome::Miss, i));
    }
    const RayPathOverlay capped =
        BuildRayPathOverlay(camera, {many.constData(), static_cast<usize>(many.size())}, {}, {});
    CHECK(static_cast<usize>(capped.Lines.size()) == MAX_RAY_PATH_LINES);
    CHECK(capped.DroppedLines == CAMERA_LINES);
}

TEST_CASE("Canvas nodes and background name capture targets and report unavailable capture",
          "[editor][ray-paths][node-menu]")
{
    CHECK(FindRayPathPass({QStringLiteral("Main/raster"), QStringLiteral("Main/trace")}, GetTraceTarget()) == 1);
    CHECK(FindRayPathPass({QStringLiteral("Main/instance/trace")}, GetTraceTarget()) == 0);
    CHECK(FindRayPathPass({QStringLiteral("Main/trace")}, {QStringLiteral("Main"), QString{}}) == -1);
    QTemporaryDir project;
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/raytracing"));
    REQUIRE(QDir().mkpath(package) == true);
    REQUIRE(QFile::copy(source.filePath(QStringLiteral("main.lurender.json")),
                        QDir(package).filePath(QStringLiteral("main.lurender.json"))) == true);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(QStringLiteral("main.lurender.json"))) == true);
    workspace.SetLivePreview(false);
    RayPathInspection* rayPathsOrNull = workspace.GetRayPathInspectionOrNull();
    REQUIRE(rayPathsOrNull != nullptr);
    RayPathInspector inspector(*rayPathsOrNull);
    workspace.show();
    QCoreApplication::processEvents();
    const auto findAction = [](const QString& name, bool shouldTrigger, bool& outIsFound) {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QAction* actionOrNull = menuOrNull == nullptr ? nullptr : menuOrNull->findChild<QAction*>(name);
        outIsFound = actionOrNull != nullptr;
        if (menuOrNull != nullptr)
        {
            menuOrNull->close();
        }
        if (actionOrNull != nullptr && shouldTrigger == true)
        {
            actionOrNull->trigger();
        }
    };
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    bool hasGraphAction = false;
    QTimer::singleShot(0, &workspace, [&]() {
        findAction(QStringLiteral("renderGraphCaptureAllRayPathsAction"), false, hasGraphAction);
    });
    Q_EMIT canvas.ContextMenuRequested({}, canvas.mapToGlobal(canvas.rect().center()));
    CHECK(hasGraphAction == true);
    REQUIRE(workspace.SelectNode(QStringLiteral("main.lurender.json"),
                                 QStringLiteral("Default"),
                                 GetTraceTarget().NodeId) == true);
    bool hasNodeAction = false;
    QTimer::singleShot(0, &workspace, [&]() {
        findAction(QStringLiteral("renderGraphCaptureRayPathsAction"), true, hasNodeAction);
    });
    Q_EMIT canvas.ContextMenuRequested(GetTraceTarget().NodeId, canvas.mapToGlobal(canvas.rect().center()));
    REQUIRE(hasNodeAction == true);
    // No World viewport device here: nothing freezes and the panel says why.
    REQUIRE(rayPathsOrNull->GetPreviewOrNull() != nullptr);
    CHECK(rayPathsOrNull->GetStage() == RayPathInspectionStage::Inactive);
    CHECK(rayPathsOrNull->GetPreviewOrNull()->GetRayPathCapture().GetState() == RayPathCaptureState::Unavailable);
    CHECK(rayPathsOrNull->GetPreviewOrNull()->IsRayPathFrozen() == false);
    CHECK(inspector.GetStatusText().contains(QStringLiteral("World viewport")) == true);
}

TEST_CASE("Frozen ray paths survive navigation, go stale on World edits and leaving restores the live graph",
          "[editor][ray-paths][gpu]")
{
    DeviceFixture device(true);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    renderer::RenderGraphRenderer automatic;
    REQUIRE(renderer::SelectRenderGraphRenderer(device.Device, renderer::RenderGraphPreference::Automatic, automatic) ==
            rhi::RasterStatus::Ready);
    if (automatic != renderer::RenderGraphRenderer::HardwareRayTracing)
    {
        SKIP("Native ray queries are unavailable on this host; the unavailable case covers it");
    }
    RenderGraphPreview preview = MakePreview();
    RayPathInspection inspection;
    inspection.SetPreview(&preview);
    RayPathInspector inspector(inspection);
    QPushButton* captureButtonOrNull = inspector.findChild<QPushButton*>(QStringLiteral("rayPathCaptureAgain"));
    REQUIRE(captureButtonOrNull != nullptr);
    CHECK(captureButtonOrNull->isEnabled() == false);
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({1, 0, 0, 1}, 1));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RAY_GRAPH_FIXTURE), GetDefaultRenderGraphRuntime()) ==
            RenderGraphPreviewLoad::Staged);
    const bool hasFrame = TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; });
    INFO(preview.GetStatus().toStdString());
    REQUIRE(hasFrame == true);
    REQUIRE(preview.GetReport().ActiveRenderer == renderer::RenderGraphRenderer::HardwareRayTracing);
    REQUIRE(preview.DescribeRayPathCaptureProblem().isEmpty() == true);

    CHECK(captureButtonOrNull->isEnabled() == true);
    QSignalSpy builds(&inspection, &RayPathInspection::BuildRequested);
    REQUIRE(inspection.Begin(GetTraceTarget()) == true);
    CHECK(inspection.GetStage() == RayPathInspectionStage::PickingPixel);
    CHECK(preview.IsRayPathFrozen() == false);
    REQUIRE(inspection.PickPixel({.5F, .5F}) == true);
    CHECK(inspection.GetStage() == RayPathInspectionStage::Inspecting);
    CHECK(builds.count() == 1);
    REQUIRE(preview.IsRayPathFrozen() == true);
    CHECK(preview.GetRayPathCapture().GetState() == RayPathCaptureState::Building);
    const RayPathFreeze freeze = preview.GetRayPathFreeze();
    CHECK(freeze.SceneRevision == 1);
    CHECK(freeze.Generation == 1);
    CHECK(freeze.GenerationIdentity.size() ==
          static_cast<qsizetype>(ludus::content::render_graph_format::IDENTITY_BYTES));
    inspection.AcceptBuild(QStringLiteral(LUDUS_EDITOR_RAY_CAPTURE_FIXTURE));
    const bool isSettled = TickUntil(preview, [&]() { return IsSettled(preview.GetRayPathCapture().GetState()); });
    INFO(preview.GetRayPathCapture().GetMessage().toStdString());
    REQUIRE(isSettled == true);
    REQUIRE(preview.GetRayPathCapture().GetState() == RayPathCaptureState::Ready);
    const RayPathCapture& capture = preview.GetRayPathCapture();
    REQUIRE(capture.GetEvents().isEmpty() == false);
    CHECK(capture.GetSummaries().isEmpty() == false);
    CHECK(capture.GetRenderedSamples() <= RayPathInspection::SAMPLE_COUNT);
    CHECK(capture.GetTargetPass() >= 0);
    CHECK(capture.GetPassNames().value(capture.GetTargetPass()) == QStringLiteral("Main/trace"));
    CHECK(static_cast<int32>(capture.GetPixelX()) == capture.GetLaunchExtent().width() / 2);
    CHECK(static_cast<int32>(capture.GetPixelY()) == capture.GetLaunchExtent().height() / 2);
    // The center camera ray hits the triangle, whose SourceId names the World object.
    const renderer::RayPathEvent first = capture.GetEvents().first();
    CHECK(first.Kind == renderer::RayPathEventKind::Closest);
    REQUIRE(first.Outcome == renderer::RayPathOutcome::Hit);
    CHECK(capture.GetObjectId(first.SourceId) == QStringLiteral("object"));
    CHECK(inspector.GetEventRowCount() == static_cast<int32>(capture.GetEvents().size()));
    QSignalSpy selections(&inspection, &RayPathInspection::ObjectSelectionRequested);
    REQUIRE(inspector.SelectEventRow(0) == true);
    CHECK(inspection.GetSelectedEvent() == 0);
    REQUIRE(selections.count() == 1);
    CHECK(selections.first().first().toString() == QStringLiteral("object"));
    CHECK(inspector.GetDetailsText().contains(QStringLiteral("object")) == true);
    const RayPathOverlay overlay = inspection.BuildOverlay(nullptr);
    CHECK(static_cast<usize>(overlay.Lines.size()) > CAMERA_LINES);

    SECTION("Sample filtering keeps the event list, selection and overlay in agreement")
    {
        QComboBox* sampleOrNull = inspector.findChild<QComboBox*>(QStringLiteral("rayPathSample"));
        QTreeWidget* treeOrNull = inspector.findChild<QTreeWidget*>(QStringLiteral("rayPathEvents"));
        REQUIRE(sampleOrNull != nullptr);
        REQUIRE(treeOrNull != nullptr);
        REQUIRE(treeOrNull->topLevelItemCount() > 1);
        CHECK(treeOrNull->columnCount() == 1);
        CHECK(treeOrNull->topLevelItem(0)->isExpanded() == true);
        CHECK(treeOrNull->topLevelItem(1)->isExpanded() == false);
        sampleOrNull->setCurrentIndex(2);
        CHECK(inspection.GetSampleFilter() == 1);
        CHECK(inspection.GetSelectedEvent() == -1);
        CHECK(inspector.GetDetailsText().isEmpty() == true);
        CHECK(treeOrNull->topLevelItem(0)->isHidden() == true);
        CHECK(treeOrNull->topLevelItem(1)->isHidden() == false);
        CHECK(treeOrNull->topLevelItem(1)->isExpanded() == true);
        CHECK(inspector.SelectEventRow(0) == false);
        treeOrNull->setCurrentItem(treeOrNull->topLevelItem(1)->child(0));
        CHECK(inspector.GetDetailsText().contains(QStringLiteral("Radiance (linear RGB)")) == true);
        inspection.SetSampleFilter(-1);
        CHECK(sampleOrNull->currentData().toInt() == -1);
        CHECK(treeOrNull->topLevelItem(0)->isHidden() == false);
        REQUIRE(inspector.SelectEventRow(0) == true);
        CHECK(inspector.GetDetailsText().contains(QStringLiteral("Hit distance:")) == true);
        CHECK(inspector.GetDetailsText().contains(QStringLiteral("Object: object")) == true);
        inspection.SelectEvent(-1);
        CHECK(treeOrNull->currentItem() == nullptr);
        CHECK(inspector.GetDetailsText().isEmpty() == true);
        REQUIRE(inspector.SelectEventRow(0) == true);
        QPushButton* collapseOrNull = inspector.findChild<QPushButton*>(QStringLiteral("rayPathCollapseAll"));
        REQUIRE(collapseOrNull != nullptr);
        collapseOrNull->click();
        CHECK(treeOrNull->topLevelItem(0)->isExpanded() == false);
        REQUIRE(inspector.SelectEventRow(0) == true);
        CHECK(treeOrNull->topLevelItem(0)->isExpanded() == true);
        CHECK(treeOrNull->topLevelItem(0)->child(0)->isExpanded() == true);
    }
    SECTION("Event details wrap and scroll independently at dock width")
    {
        ApplyWorkspaceStyle(&inspector);
        inspector.resize(320, 720);
        inspector.show();
        QCoreApplication::processEvents();
        QPlainTextEdit* detailsOrNull = inspector.findChild<QPlainTextEdit*>(QStringLiteral("rayPathDetails"));
        REQUIRE(detailsOrNull != nullptr);
        CHECK(detailsOrNull->horizontalScrollBar()->maximum() == 0);
        CHECK(detailsOrNull->verticalScrollBar()->maximum() > 0);
        CHECK(detailsOrNull->isReadOnly() == true);
        // Optional render evidence from the real GPU capture; normal test runs write no image.
        const QString screenshotPath = qEnvironmentVariable("LUDUS_RAY_INSPECTOR_SCREENSHOT");
        if (screenshotPath.isEmpty() == false)
        {
            const QPalette originalPalette = QApplication::palette();
            QPalette palette = originalPalette;
            palette.setColor(QPalette::Window, QColor(QStringLiteral("#18242c")));
            QApplication::setPalette(palette);
            ApplyWorkspaceStyle(&inspector);
            QApplication::setPalette(inspector.palette());
            QCoreApplication::processEvents();
            const bool isSaved = inspector.grab().save(screenshotPath);
            QApplication::setPalette(originalPalette);
            REQUIRE(isSaved == true);
        }
    }
    SECTION("A hit-selection listener may leave inspection without retaining deleted tree rows")
    {
        inspection.SelectEvent(-1);
        QObject::connect(&inspection,
                         &RayPathInspection::ObjectSelectionRequested,
                         &inspector,
                         [&inspection](const QString&) { inspection.Leave(); });
        REQUIRE(inspector.SelectEventRow(0) == true);
        CHECK(inspection.GetStage() == RayPathInspectionStage::Inactive);
        CHECK(inspector.GetEventRowCount() == 0);
        CHECK(inspector.GetDetailsText().isEmpty() == true);
        CHECK(preview.IsRayPathFrozen() == false);
    }
    SECTION("A new capture resets the displayed sample filter")
    {
        inspection.SetSampleFilter(1);
        REQUIRE(inspection.Begin(GetTraceTarget()) == true);
        REQUIRE(inspection.PickPixel({.5F, .5F}) == true);
        QComboBox* sampleOrNull = inspector.findChild<QComboBox*>(QStringLiteral("rayPathSample"));
        REQUIRE(sampleOrNull != nullptr);
        CHECK(sampleOrNull->currentData().toInt() == -1);
        CHECK(inspection.GetSampleFilter() == -1);
        CHECK(inspector.GetEventRowCount() == 0);
        CHECK(inspector.GetDetailsText().isEmpty() == true);
        inspection.RejectBuild(QStringLiteral("test build failure"));
        CHECK(inspector.GetStatusText().contains(QStringLiteral("test build failure")) == true);
        CHECK(sampleOrNull->isEnabled() == false);
        inspection.Leave();
    }
    SECTION("Navigation keeps the frozen paths")
    {
        const uint64 presentedRevision = preview.GetPresentedRevision();
        renderer::ViewDescription moved = freeze.View;
        moved.WorldToClip.Columns[3].X = .25F;
        preview.SetCamera(moved, 2);
        for (usize tick = 0; tick < IDLE_TICKS; ++tick)
        {
            preview.Tick();
            QThread::usleep(POLL_MICROSECONDS);
        }
        CHECK(preview.GetRayPathFreeze().View.WorldToClip == freeze.View.WorldToClip);
        CHECK(preview.GetRayPathStaleReason().isEmpty() == true);
        CHECK(preview.GetRayPathCapture().GetState() == RayPathCaptureState::Ready);
        CHECK(preview.GetRayPathCapture().GetEvents().first().OriginWorld.X == first.OriginWorld.X);
        // The frozen live graph submits no frame while the author explores the paths.
        CHECK(preview.GetPresentedRevision() == presentedRevision);
        CHECK(inspection.BuildOverlay(nullptr).Lines.size() == overlay.Lines.size());
        inspection.Leave();
        CHECK(preview.IsRayPathFrozen() == false);
        REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 2; }) == true);
    }
    SECTION("A World edit marks the capture stale and leaving restores the live graph")
    {
        QSignalSpy changes(&preview, &RenderGraphPreview::RayPathCaptureChanged);
        preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 3));
        CHECK(changes.count() == 1);
        CHECK(preview.GetRayPathStaleReason().isEmpty() == false);
        CHECK(inspection.GetStatus().startsWith(QStringLiteral("Stale:")) == true);
        CHECK(inspector.GetStatusText().startsWith(QStringLiteral("Stale:")) == true);
        CHECK(inspection.GetSelectedEvent() == 0);
        CHECK(inspector.GetDetailsText().contains(QStringLiteral("Object: object")) == true);
        CHECK(inspector.GetEventRowCount() == static_cast<int32>(capture.GetEvents().size()));
        // The stale capture keeps its paths; it is never mixed with the newer World.
        CHECK(preview.GetRayPathCapture().GetEvents().size() == capture.GetEvents().size());
        CHECK(preview.GetPresentedRevision() == 1);
        inspection.Leave();
        CHECK(inspection.GetStage() == RayPathInspectionStage::Inactive);
        CHECK(preview.GetRayPathCapture().GetState() == RayPathCaptureState::Idle);
        CHECK(inspection.BuildOverlay(nullptr).Lines.isEmpty() == true);
        // The edit made while frozen reaches the live graph once inspection ends.
        REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 3; }) == true);
        CHECK(preview.GetPresentedGeneration() == 1);
    }
    SECTION("The panel can start a new all-pass capture and cancel pixel picking")
    {
        inspection.Leave();
        QPushButton* leaveOrNull = inspector.findChild<QPushButton*>(QStringLiteral("rayPathLeave"));
        REQUIRE(captureButtonOrNull != nullptr);
        REQUIRE(leaveOrNull != nullptr);
        REQUIRE(captureButtonOrNull->isEnabled() == true);
        captureButtonOrNull->click();
        CHECK(inspection.GetStage() == RayPathInspectionStage::PickingPixel);
        CHECK(inspection.GetTarget().NodeId.isEmpty() == true);
        CHECK(leaveOrNull->text() == QStringLiteral("Cancel picking"));
        leaveOrNull->click();
        CHECK(inspection.GetStage() == RayPathInspectionStage::Inactive);
        CHECK(preview.IsRayPathFrozen() == false);
    }
    SECTION("A resize marks the capture stale")
    {
        preview.SetReferenceSize(freeze.ReferenceSize + QSize(1, 1));
        CHECK(preview.GetRayPathStaleReason().contains(QStringLiteral("resized")) == true);
        inspection.Leave();
    }
    preview.Detach();
}

TEST_CASE("Ray path capture is unavailable without native rays while raster navigation keeps drawing",
          "[editor][ray-paths][gpu]")
{
    // Without preferred ray features the device enables no native traversal, so the authored
    // raster graph draws, as on a GPU or API without hardware raytracing.
    DeviceFixture device(false);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    renderer::RenderGraphRenderer automatic;
    REQUIRE(renderer::SelectRenderGraphRenderer(device.Device, renderer::RenderGraphPreference::Automatic, automatic) ==
            rhi::RasterStatus::Ready);
    RenderGraphPreview preview = MakePreview();
    // Metal enables native traversal on every device; there the same raster selection comes from Force raster.
    preview.SetForceRaster(automatic != renderer::RenderGraphRenderer::Raster);
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({1, 0, 0, 1}, 1));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RAY_GRAPH_FIXTURE), GetDefaultRenderGraphRuntime()) ==
            RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; }) == true);
    REQUIRE(preview.GetReport().ActiveRenderer == renderer::RenderGraphRenderer::Raster);
    RayPathInspection inspection;
    inspection.SetPreview(&preview);
    RayPathInspector inspector(inspection);
    CHECK(inspection.Begin({}) == false);
    CHECK(inspection.GetStage() == RayPathInspectionStage::Inactive);
    CHECK(preview.IsRayPathFrozen() == false);
    CHECK(preview.GetRayPathCapture().GetState() == RayPathCaptureState::Unavailable);
    CHECK(preview.GetRayPathCapture().GetMessage().contains(QStringLiteral("raster")) == true);
    CHECK(inspector.GetStatusText() == preview.GetRayPathCapture().GetMessage());
    CHECK(inspection.PickPixel({.5F, .5F}) == false);
    // Raster navigation is unaffected: a new camera still reaches the live graph.
    renderer::ViewDescription moved;
    moved.WorldToClip.Columns[3].X = .25F;
    preview.SetCamera(moved, 2);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 2; }) == true);
    preview.Detach();
}

TEST_CASE("Ray path inspection draws the built-in raster scene and overlay, then returns frames to the graph",
          "[editor][ray-paths][gpu]")
{
    constexpr int32 TIMEOUT_MILLISECONDS = 30000;
    constexpr int32 PREVIEW_PIXELS = 64;
    rhi::Shutdown();
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("ray-path-inspection");
    scene.AssetsIdentity = scene.Identity;
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Uvs = {{0, 0}, {1, 0}, {.5F, 1}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("triangle");
    scene.Objects.append(object);
    RenderPreview preview;
    bool isAdmitted = false;
    QObject::connect(&preview, &RenderPreview::DeviceRequested, &preview, [&preview, &isAdmitted]() {
        isAdmitted = preview.AdmitOffscreenDevice();
    });
    preview.UseProjectGraph();
    preview.Resize(PREVIEW_PIXELS, PREVIEW_PIXELS);
    REQUIRE(preview.SetScene(scene) == true);
    const bool hasStarted = preview.Start();
    if (isAdmitted == false)
    {
        SKIP("No headless device for the built-in raster preview on this host");
    }
    REQUIRE(hasStarted == true);
    // Delegated to the project graph, this preview draws nothing of its own.
    for (usize tick = 0; tick < IDLE_TICKS; ++tick)
    {
        preview.Tick();
    }
    CHECK(preview.HasPendingFrame() == false);
    preview.SetRayPathInspection(true);
    RayPathOverlay overlay;
    overlay.Lines.append({{-1, 0, 0}, {1, 0, 0}, {0, 1, 0, 1}, 2});
    overlay.Lines.append({{0, -1, 0}, {0, 1, 0}, {1, 0, 0, 1}, 2});
    preview.SetRayPathOverlay(overlay);
    // Consume frames so the preview keeps producing until the overlay version is attached.
    const auto consume = [&preview]() {
        renderer::ViewportFrame frame;
        renderer::ViewportImage image;
        if (preview.TakeFrame(frame, image) == true)
        {
            preview.ReleaseFrame(frame);
        }
        return preview.GetPresentedRayPathLines() == 2;
    };
    const bool hasOverlay = QTest::qWaitFor(consume, TIMEOUT_MILLISECONDS);
    INFO(preview.Status().toStdString());
    REQUIRE(hasOverlay == true);
    CHECK(preview.IsRayPathInspection() == true);
    preview.SetRayPathInspection(false);
    CHECK(preview.GetPresentedRayPathLines() == 0);
    CHECK(preview.HasPendingFrame() == false);
    for (usize tick = 0; tick < IDLE_TICKS; ++tick)
    {
        preview.Tick();
    }
    CHECK(preview.HasPendingFrame() == false);
    preview.Stop();
}
