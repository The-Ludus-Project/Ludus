#include "../../../modules/graphics/renderer/shaders/probe_volume_layout.h"

#include "editor_default_graph.h"
#include "hybrid_diffuse_scene.h"
#include "internal/graph_display_frames.h"
#include "internal/level.h"
#include "internal/level_workspace.h"
#include "internal/preview_gpu_generation.h"
#include "internal/preview_lighting.h"
#include "internal/preview_shading.h"
#include "internal/preview_viewport.h"
#include "internal/readback.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_object_interaction.h"
#include "internal/viewport_reference_capture.h"
#include "internal/world_surface_bake.h"
#include "internal/world_workspace.h"
#include "probe_qualification.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include <QQuickWindow>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/owned_object.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/static_lighting.hpp>
#include <ludus/graphics/renderer/tone_mapping.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/platform/entropy.h>
#include <ludus/runtime/resources/scene.h>

#import <Cocoa/Cocoa.h>

#include <cmath>

#include <QApplication>
#include <QComboBox>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSize>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;
bool Frames(RenderPreview& preview, uint64 target)
{
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport") && window->isExposed() == false)
        {
            // Qt suspends consumer presentation for occluded native children.
            // Frame-progress assertions require an exposed test window.
            QWindow* topLevel = window;
            while (topLevel->parent() != nullptr)
            {
                topLevel = topLevel->parent();
            }
            topLevel->raise();
            topLevel->requestActivate();
            if (QTest::qWaitForWindowExposed(window, 5000) == false)
            {
                WARN("Native viewport could not become exposed for the frame-progress check");
                return false;
            }
        }
    }
    QElapsedTimer timer;
    timer.start();
    while (preview.Frames() < target && timer.elapsed() < 10000 && preview.Running())
    {
        QTest::qWait(20);
    }
    if (preview.Frames() >= target)
    {
        QTest::qWait(100);
    }
    if (preview.Frames() < target)
    {
        WARN("Viewport frames " << preview.Frames() << " / " << target << "; pending=" << preview.HasPendingFrame());
        for (QWindow* window : QGuiApplication::allWindows())
        {
            if (window->objectName() == QStringLiteral("LudusGpuViewport"))
            {
                WARN("Native viewport exposed=" << window->isExposed() << "; visible=" << window->isVisible());
            }
        }
    }
    return preview.Frames() >= target;
}
QImage ReadViewport()
{
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport"))
        {
            // Independent test-only Qt framebuffer readback; product presentation never reads pixels.
            return static_cast<QQuickWindow*>(window)->grabWindow();
        }
    }
    return {};
}
QSize GetViewportPixelSize()
{
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport"))
        {
            const float64 ratio = window->devicePixelRatio();
            const int32 width = qRound(static_cast<float64>(window->width()) * ratio);
            const int32 height = qRound(static_cast<float64>(window->height()) * ratio);
            return {width, height};
        }
    }
    return {};
}
} // namespace
TEST_CASE("Supplied imported World remains visible in solid and albedo GPU views", "[.render-native]")
{
    const QString root = qEnvironmentVariable("LUDUS_VIEWPORT_TEST_PROJECT");
    if (root.isEmpty() == true)
    {
        SUCCEED("Set LUDUS_VIEWPORT_TEST_PROJECT and LUDUS_VIEWPORT_TEST_WORLD for imported-World acceptance.");
        return;
    }
    WorldWorkspace workspace;
    workspace.resize(1200, 850);
    workspace.show();
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(QDir(root).filePath(qEnvironmentVariable("LUDUS_VIEWPORT_TEST_WORLD"))));
    PreviewViewport* viewport =
        static_cast<PreviewViewport*>(workspace.findChild<QWidget*>(QStringLiteral("worldSceneViewport")));
    REQUIRE(viewport != nullptr);
    QComboBox* shading = viewport->findChild<QComboBox*>(QStringLiteral("viewportShading"));
    REQUIRE(shading != nullptr);
    const QJsonObject draft = workspace.Document().Draft();
    const QString imagePrefix = qEnvironmentVariable("LUDUS_VIEWPORT_TEST_IMAGE");
    QImage previous;
    for (int32 mode = 0; mode < 3; ++mode)
    {
        const uint64 target = viewport->PresentedFrames() + 2;
        shading->setCurrentIndex(mode);
        QElapsedTimer timer;
        timer.start();
        while (viewport->PresentedFrames() < target && timer.elapsed() < 15000)
        {
            QTest::qWait(20);
        }
        REQUIRE(viewport->PresentedFrames() >= target);
        const QImage image = ReadViewport();
        REQUIRE_FALSE(image.isNull());
        if (mode == 0)
        {
            bool hasContrast = false;
            const QColor first = image.pixelColor(0, 0);
            for (int32 y = 0; y < image.height(); y += 16)
            {
                for (int32 x = 0; x < image.width(); x += 16)
                {
                    hasContrast = hasContrast || image.pixelColor(x, y) != first;
                }
            }
            CHECK(hasContrast);
        }
        if (mode > 0)
        {
            CHECK(image != previous);
        }
        if (imagePrefix.isEmpty() == false)
        {
            REQUIRE(image.save(imagePrefix + QString::number(mode) + QStringLiteral(".png")));
        }
        previous = image;
    }
    LevelWorkspace* sceneWorkspace = workspace.findChild<LevelWorkspace*>();
    REQUIRE(sceneWorkspace != nullptr);
    const auto awaitPresentation = [viewport](uint64 target) {
        QElapsedTimer timer;
        timer.start();
        while (viewport->PresentedFrames() < target && timer.elapsed() < 10000)
        {
            QTest::qWait(20);
        }
        return viewport->PresentedFrames() >= target;
    };
    preview_lighting::Inputs inputs;
    REQUIRE(preview_lighting::BuildInputs(sceneWorkspace->PreviewScene(),
                                          preview_lighting::MakeKey(sceneWorkspace->PreviewScene()),
                                          inputs) == ludus::graphics::rhi::RasterStatus::Ready);
    if (inputs.AreaCount != 0)
    {
        REQUIRE(sceneWorkspace->StatusText().contains(
            QStringLiteral("%1 planar area proxies / %2 shadow faces").arg(inputs.AreaCount).arg(inputs.ShadowCount)));
        REQUIRE(awaitPresentation(viewport->PresentedFrames() + 2));
        INFO(sceneWorkspace->StatusText().toStdString());
        REQUIRE(sceneWorkspace->StatusText().contains(QStringLiteral("direct lighting")) == true);
        CHECK(sceneWorkspace->StatusText().contains(QStringLiteral("bake")) == false);
        const QImage litImage = ReadViewport();
        REQUIRE(litImage.isNull() == false);
        if (imagePrefix.isEmpty() == false)
        {
            REQUIRE(litImage.save(imagePrefix + QStringLiteral("direct.png")) == true);
        }
        const QColor background = litImage.pixelColor(0, 0);
        uint32 litPixels = 0;
        for (int32 y = 0; y < litImage.height(); y += 4)
        {
            for (int32 x = 0; x < litImage.width(); x += 4)
            {
                const QColor color = litImage.pixelColor(x, y);
                // Count visible nonsaturated geometry, excluding clear pixels
                // and the bright emitter patches which were already visible.
                const int32 sum = color.red() + color.green() + color.blue();
                if (color != background && sum > 12 && sum < 600)
                {
                    ++litPixels;
                }
            }
        }
        CHECK(litPixels > 100);
    }
    CHECK(workspace.Document().Draft() == draft);
    CHECK_FALSE(workspace.Dirty());
    workspace.StopPreview();
}
TEST_CASE("Native World preview renders authored assets, cameras, culling, updates and resize", "[.render-native]")
{
    REQUIRE(QApplication::platformName() == QStringLiteral("cocoa"));
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto root = QFileInfo(directory.path()).canonicalFilePath();
    REQUIRE(QDir(root).mkpath(QStringLiteral("authoring")));
    for (const auto& name : {QStringLiteral("world.schema.json"),
                             QStringLiteral("rendering.world.json"),
                             QStringLiteral("render.preview.json"),
                             QStringLiteral("cube.mesh.json"),
                             QStringLiteral("ludus.png")})
    {
        QFile source(QStringLiteral(":/samples/live-edit-game/authoring/") + name);
        REQUIRE(source.open(QIODevice::ReadOnly));
        QFile target(root + QStringLiteral("/authoring/") + name);
        REQUIRE(target.open(QIODevice::WriteOnly));
        const auto bytes = source.readAll();
        REQUIRE(target.write(bytes) == bytes.size());
    }
    const auto read = [&root](const QString& name) {
        QFile file(root + QStringLiteral("/authoring/") + name);
        REQUIRE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    };
    WorldDocument world;
    QString error;
    REQUIRE(world.LoadSchema(read(QStringLiteral("world.schema.json")), error));
    REQUIRE(world.Load(read(QStringLiteral("rendering.world.json")), error));
    RenderScene scene;
    REQUIRE(RenderScene::Load(root, world, scene, error));
    const auto original = world.Draft();
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(640, 480);
    // Exercise a child surface smaller than the widget on both 1x and Retina displays.
    viewport.setContentsMargins(7, 11, 13, 17);
    viewport.show();
    REQUIRE(preview.SetScene(scene));
    preview.Navigate(0, {0, 0}, 3, false);
    REQUIRE(preview.Start());
    const bool presented = Frames(preview, 3);
    INFO(preview.Status().toStdString());
    REQUIRE(presented);
    QImage embedded = ReadViewport();
    REQUIRE(viewport.PresentedFrames() >= 1);
    REQUIRE_FALSE(embedded.isNull());
    const QSize initialPixelSize = GetViewportPixelSize();
    CHECK(embedded.size() == initialPixelSize);
    CHECK(initialPixelSize.width() < qRound(static_cast<float64>(viewport.width()) * viewport.devicePixelRatioF()));
    CHECK(embedded.pixelColor(embedded.width() / 2, embedded.height() / 2) != embedded.pixelColor(0, 0));
    if (qEnvironmentVariableIsSet("LUDUS_GPU_VIEWPORT_IMAGE") == true)
    {
        REQUIRE(embedded.save(qEnvironmentVariable("LUDUS_GPU_VIEWPORT_IMAGE")));
    }
    // Switching sources retires the current image item. Its replacement must stay below the gizmos.
    RenderPreview alternateSource;
    viewport.SetFrameSource(&alternateSource);
    viewport.SetFrameSource(&preview);
    REQUIRE(Frames(preview, preview.Frames() + 1));
    ViewportObjectInteraction& interaction = viewport.GetObjectInteraction();
    interaction.SetWorld(world, QStringLiteral("starter.cube"), true);
    interaction.SetTool(ViewportObjectInteraction::Tool::Select);
    const QImage withoutHandles = ReadViewport();
    REQUIRE(withoutHandles.isNull() == false);
    interaction.SetTool(ViewportObjectInteraction::Tool::Move);
    const QImage withHandles = ReadViewport();
    REQUIRE(withHandles.isNull() == false);
    CHECK(withHandles != withoutHandles);
    interaction.SetWorld(world, {}, false);
    CHECK(ReadViewport() == embedded);
    // Rebuilding a mode must retain the actual Qt GPU image throughout setup.
    bool didResetFrames = false;
    QObject::connect(&preview, &RenderPreview::ResettingFrames, &preview, [&didResetFrames]() {
        didResetFrames = true;
    });
    preview.SetShading(PreviewShading::Albedo);
    preview.Tick();
    CHECK(didResetFrames == false);
    CHECK(ReadViewport() == embedded);
    REQUIRE(Frames(preview, 3));
    const QImage albedo = ReadViewport();
    CHECK(albedo != embedded);
    CHECK(didResetFrames == false);
    // Supersede preparation before its first ready frame; the active image stays.
    preview.SetShading(PreviewShading::Lit);
    preview.Tick();
    preview.SetShading(PreviewShading::Solid);
    CHECK(ReadViewport() == albedo);
    REQUIRE(Frames(preview, 3));
    CHECK(ReadViewport() == embedded);
    CHECK(didResetFrames == false);
    // The private admission boundary accepts this snapshot, but GPU texture
    // creation rejects its empty pixels. Failure must preserve the presented view.
    RenderScene invalidCandidate = scene;
    invalidCandidate.Identity = QByteArray("invalid GPU candidate");
    invalidCandidate.AssetsIdentity = invalidCandidate.Identity;
    invalidCandidate.Textures[0].Pixels = {};
    bool hasFailedPreparation = false;
    QObject::connect(&preview, &RenderPreview::Failed, &preview, [&hasFailedPreparation](const QString&) {
        hasFailedPreparation = true;
    });
    REQUIRE(preview.SetScene(invalidCandidate));
    QElapsedTimer preparationTimer;
    preparationTimer.start();
    while (hasFailedPreparation == false && preparationTimer.elapsed() < 10000)
    {
        QTest::qWait(10);
    }
    REQUIRE(hasFailedPreparation == true);
    CHECK(ReadViewport() == embedded);
    CHECK(didResetFrames == false);
    REQUIRE(preview.SetScene(scene));
    REQUIRE(Frames(preview, 3));
    CHECK(ReadViewport() == embedded);
    CHECK(world.Draft() == original);
    // Native child input changes the live extracted World image without editing the document.
    const QJsonObject authoredNavigation = preview.Navigation();
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport"))
        {
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
            QTest::mouseMove(window, QPoint(145, 120));
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, QPoint(145, 120));
            CHECK(preview.Navigation() == authoredNavigation);
            preview.Navigate(-1, {80, 40}, 20, false);
            REQUIRE(Frames(preview, preview.Frames() + 3));
            CHECK(ReadViewport() == embedded);
            QTest::mousePress(window, Qt::LeftButton, Qt::AltModifier, QPoint(100, 100));
            const QPoint orbitPosition(145, 120);
            QMouseEvent orbitMove(QEvent::MouseMove,
                                  orbitPosition,
                                  window->mapToGlobal(orbitPosition),
                                  Qt::NoButton,
                                  Qt::LeftButton,
                                  Qt::AltModifier);
            QApplication::sendEvent(window, &orbitMove);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::AltModifier, QPoint(145, 120));
            REQUIRE(Frames(preview, preview.Frames() + 3));
            const QImage orbited = ReadViewport();
            CHECK(orbited != embedded);
            QTest::mousePress(window, Qt::MiddleButton, Qt::NoModifier, QPoint(145, 120));
            QTest::mouseMove(window, QPoint(170, 130));
            QTest::mouseRelease(window, Qt::MiddleButton, Qt::NoModifier, QPoint(170, 130));
            REQUIRE(Frames(preview, preview.Frames() + 3));
            CHECK(ReadViewport() != orbited);
            const QJsonObject beforeFly = preview.Navigation();
            QTest::mousePress(window, Qt::RightButton, Qt::NoModifier, QPoint(170, 130));
            QTest::qWait(20);
            CHECK(preview.Navigation() == beforeFly);
            QTest::keyPress(window, Qt::Key_W);
            QTest::qWait(100);
            QTest::mouseRelease(window, Qt::RightButton, Qt::NoModifier, QPoint(170, 130));
            CHECK(preview.Navigation() != beforeFly);
            const QJsonObject stopped = preview.Navigation();
            QTest::qWait(60);
            CHECK(preview.Navigation() == stopped);
            preview.SetSelection(QStringLiteral("starter.cube"));
            QTest::keyClick(window, Qt::Key_F);
            REQUIRE(Frames(preview, preview.Frames() + 3));
            CHECK(preview.Report().Visible == 1);
            CHECK(ReadViewport() != embedded);
            if (qEnvironmentVariableIsSet("LUDUS_GPU_NAVIGATION_IMAGE") == true)
            {
                REQUIRE(ReadViewport().save(qEnvironmentVariable("LUDUS_GPU_NAVIGATION_IMAGE")));
            }
        }
    }
    CHECK(world.Draft() == original);
    const QJsonObject navigation = preview.Navigation();
    preview.Navigate(-1, {70, 30}, 4, false);
    preview.RestoreNavigation(navigation);
    CHECK(preview.Navigation() == navigation);
    preview.Navigate(0, {0, 0}, 3, false);
    REQUIRE(Frames(preview, preview.Frames() + 3));
    CHECK(preview.Report().Submitted == 1);
    CHECK(preview.Report().Visible == 1);
    // One instanced scene packet plus the analytic grid's two indexed draws.
    constexpr uint32 INSTANCED_SCENE_AND_GRID_DRAWS = 3;
    CHECK(preview.Report().Draws == INSTANCED_SCENE_AND_GRID_DRAWS);
    CHECK(world.Draft() == original);
    CHECK_FALSE(world.Dirty());
    // More than eight identical textured objects consume one shared material.
    for (int32 i = 0; i < 11; ++i)
    {
        QString id;
        REQUIRE(world.DuplicateEntity(ludus::platform::GetSystemEntropySource(),
                                      error,
                                      QStringLiteral("starter.cube"),
                                      id));
    }
    REQUIRE(RenderScene::Load(root, world, scene, error));
    CHECK(scene.Surfaces.size() == 1);
    REQUIRE(preview.SetScene(scene));
    preview.Navigate(0, {0, 0}, 3, false);
    REQUIRE(Frames(preview, 3));
    CHECK(preview.Report().Visible == 12);
    CHECK(preview.Report().Draws == INSTANCED_SCENE_AND_GRID_DRAWS);
    for (int32 i = 0; i < 11; ++i)
    {
        REQUIRE(world.Undo());
    }
    REQUIRE(RenderScene::Load(root, world, scene, error));
    REQUIRE(preview.SetScene(scene));
    preview.Navigate(0, {0, 0}, 3, false);
    REQUIRE(Frames(preview, 3));
    CHECK_FALSE(world.Dirty());
    CHECK(preview.WindowInfo().System == ludus::platform::WindowSystem::Headless);
    CHECK(preview.WindowInfo().CocoaWindow == nullptr);
    viewport.resize(720, 480);
    REQUIRE(Frames(preview, preview.Frames() + 3));
    CHECK(preview.Report().Visible == 1);
    embedded = ReadViewport();
    CHECK(embedded.size() == GetViewportPixelSize());
    CHECK(embedded.width() > initialPixelSize.width());
    const QImage beforeCulling = embedded;
    auto entity = world.Entity(QStringLiteral("starter.cube"));
    auto components = entity.value(QStringLiteral("components")).toObject();
    auto transform = components.value(QStringLiteral("transform")).toObject();
    transform.insert(QStringLiteral("x"), 50);
    components.insert(QStringLiteral("transform"), transform);
    REQUIRE(
        world.Edit(QStringLiteral("starter.cube"), components, entity.value(QStringLiteral("name")).toString(), error));
    REQUIRE(RenderScene::Load(root, world, scene, error));
    REQUIRE(preview.SetScene(scene));
    preview.Navigate(0, {0, 0}, 3, false);
    REQUIRE(Frames(preview, 3));
    CHECK(preview.Report().Culled == 1);
    CHECK(preview.Report().Visible == 0);
    embedded = ReadViewport();
    // The coordinate grid stays visible when the scene mesh is culled; the old mesh image must change.
    CHECK(embedded != beforeCulling);
    preview.Navigate(0, {0, 0}, 3, true);
    REQUIRE(Frames(preview, preview.Frames() + 3));
    CHECK(preview.Report().Visible == 1);
    CHECK(preview.Report().Culled == 0);
    REQUIRE(world.Undo());
    REQUIRE(RenderScene::Load(root, world, scene, error));
    REQUIRE(preview.SetScene(scene));
    preview.Navigate(0, {0, 0}, 3, false);
    REQUIRE(Frames(preview, 3));
    CHECK(preview.Report().Visible == 1);
    CHECK_FALSE(world.Dirty());
    // Object edits preserve the independent orbit anchor until explicit Frame All.
    preview.Navigate(-1, {25, 20}, 3, false);
    REQUIRE(Frames(preview, preview.Frames() + 3));
    const ludus::foundation::math::Vector3 eye = preview.ViewDescription().CameraPosition;
    const QJsonObject orbitNavigation = preview.Navigation();
    entity = world.Entity(QStringLiteral("starter.cube"));
    components = entity.value(QStringLiteral("components")).toObject();
    transform = components.value(QStringLiteral("transform")).toObject();
    transform.insert(QStringLiteral("x"), 50);
    components.insert(QStringLiteral("transform"), transform);
    REQUIRE(
        world.Edit(QStringLiteral("starter.cube"), components, entity.value(QStringLiteral("name")).toString(), error));
    REQUIRE(RenderScene::Load(root, world, scene, error));
    REQUIRE(preview.SetScene(scene));
    CHECK(preview.Navigation() == orbitNavigation);
    REQUIRE(Frames(preview, 3));
    CHECK(preview.ViewDescription().CameraPosition == eye);
    CHECK(preview.Report().Culled == 1);
    preview.FrameAll();
    REQUIRE(Frames(preview, preview.Frames() + 3));
    CHECK(preview.Report().Visible == 1);
    REQUIRE(world.Undo());
    REQUIRE(RenderScene::Load(root, world, scene, error));
    REQUIRE(preview.SetScene(scene));
    preview.FrameAll();
    preview.Navigate(0, {0, 0}, 3, false);
    REQUIRE(Frames(preview, 3));
    preview.Stop();
    CHECK_FALSE(preview.Running());
    REQUIRE(preview.Start());
    REQUIRE(Frames(preview, 3));
    preview.Stop();
    // A receiver can stop before Qt samples the candidate without losing later signal delivery.
    RenderPreview closing;
    PreviewViewport closingViewport(&closing);
    closingViewport.show();
    bool hasNotification = false;
    REQUIRE(closing.SetScene(scene));
    QObject::connect(&closing, &RenderPreview::FrameReady, &closing, [&closing]() { closing.Stop(); });
    QObject::connect(&closing, &RenderPreview::FrameReady, &closing, [&hasNotification]() { hasNotification = true; });
    REQUIRE(closing.Start());
    QElapsedTimer closingTimer;
    closingTimer.start();
    while (closing.Running() == true && closingTimer.elapsed() < 10000)
    {
        QTest::qWait(10);
    }
    REQUIRE_FALSE(closing.Running());
    REQUIRE(hasNotification);
    // A headless Metal pixel oracle complements the embedded viewport lifecycle:
    // render an extracted red cube and independently check foreground/background.
    entity = world.Entity(QStringLiteral("starter.cube"));
    components = entity.value(QStringLiteral("components")).toObject();
    auto renderable = components.value(QStringLiteral("renderable")).toObject();
    renderable.insert(QStringLiteral("texture"), QString());
    renderable.insert(QStringLiteral("g"), 0);
    renderable.insert(QStringLiteral("b"), 0);
    components.insert(QStringLiteral("renderable"), renderable);
    REQUIRE(
        world.Edit(QStringLiteral("starter.cube"), components, entity.value(QStringLiteral("name")).toString(), error));
    REQUIRE(RenderScene::Load(root, world, scene, error));
    namespace rhi = ludus::graphics::rhi;
    namespace rr = ludus::graphics::renderer;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    REQUIRE(rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, description, device, surface) ==
            rhi::DeviceStatus::Ready);
    struct Guard final
    {
        rhi::DeviceHandle& Device;
        ~Guard()
        {
            (void)rhi::DestroyDevice(Device);
        }
    } guard{device};
    rr::Renderer renderer;
    REQUIRE(renderer.Initialize(device,
                                ludus::shaders::renderer_flat::Vertex(),
                                ludus::shaders::renderer_flat::Fragment()) == rhi::RasterStatus::Ready);
    REQUIRE(renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                     ludus::shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    const auto& meshData = scene.Meshes[0];
    rr::Mesh mesh;
    REQUIRE(renderer.CreateMesh(
                {
                    .Positions = meshData.Positions.constData(),
                    .VertexCount = static_cast<usize>(meshData.Positions.size()),
                    .Indices = meshData.Indices.constData(),
                    .IndexCount = static_cast<usize>(meshData.Indices.size()),
                    .Texcoords = meshData.Uvs.constData(),
                },
                mesh) == rhi::RasterStatus::Ready);
    rr::SceneItem item;
    item.Geometry = mesh;
    item.Transform = scene.Objects[0].Transform;
    item.Material.Color = scene.Objects[0].Tint;
    item.SourceId = 1;
    rr::Snapshot snapshot;
    REQUIRE(renderer.CreateSnapshot(&item, 1, snapshot) == rhi::RasterStatus::Ready);
    rr::ViewDescription lens;
    REQUIRE(scene.View(
        {
            .Camera = 0,
            .OrbitDegrees = {0, 0},
            .Distance = 3,
            .Width = 96,
            .Height = 64,
            .Reference = false,
        },
        lens,
        error));
    rr::PreparedView view;
    rr::ViewReport report;
    REQUIRE(renderer.PrepareView(snapshot, lens, view, report) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    REQUIRE(renderer.DrawView(view, { .Clear = {0.035F, 0.045F, 0.06F, 1}, .ClearDepth = 0 }) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    uint8 pixels[96 * 64 * 4]{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
    const usize center = (usize{32} * 96 + 48) * 4;
    CHECK(pixels[center] > 240);
    CHECK(pixels[center + 1] < 10);
    CHECK(pixels[center + 2] < 10);
    CHECK(pixels[0] < 100);
    CHECK(pixels[1] < 100);
    CHECK(pixels[2] < 100);
    if (qEnvironmentVariable("LUDUS_RENDER_NATIVE_IMAGE").isEmpty() == false)
    {
        const QImage image(pixels, 96, 64, QImage::Format_RGBA8888);
        REQUIRE(image.save(qEnvironmentVariable("LUDUS_RENDER_NATIVE_IMAGE")));
    }
    renderer.Reset();
}

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;
void AddLightingQuad(ludus::resources::SceneMesh& inoutMesh,
                     float32 x,
                     float32 z,
                     float32 width,
                     float32 y,
                     bool isFacingDown)
{
    const uint32 start = static_cast<uint32>(inoutMesh.Positions.GetSize());
    REQUIRE(inoutMesh.Positions.TryAdd({x, y, z}));
    REQUIRE(inoutMesh.Positions.TryAdd({x + width, y, z}));
    REQUIRE(inoutMesh.Positions.TryAdd({x + width, y, z + width}));
    REQUIRE(inoutMesh.Positions.TryAdd({x, y, z + width}));
    for (usize i = 0; i < 4; ++i)
    {
        REQUIRE(inoutMesh.Normals.TryAdd({0, isFacingDown == true ? -1.0F : 1.0F, 0}));
        REQUIRE(inoutMesh.Texcoords.TryAdd({0, 0}));
    }
    const uint32 order[6]{0, 1, 2, 0, 2, 3};
    for (usize i = 0; i < 6; i += 3)
    {
        REQUIRE(inoutMesh.Indices.TryAdd(start + order[i]));
        REQUIRE(inoutMesh.Indices.TryAdd(start + order[i + (isFacingDown == true ? 1 : 2)]));
        REQUIRE(inoutMesh.Indices.TryAdd(start + order[i + (isFacingDown == true ? 2 : 1)]));
    }
}
// Receiver-mesh inputs for native preview validation.
enum class LightingReceiverDefect : uint8
{
    None,
    // GPU raster accepts a zero-area receiver primitive; static transport skips it.
    ZeroAreaTriangle,
    // Direct lighting ignores authored lightmap coordinates.
    MismatchedBakeTexcoords,
    OutOfRangeIndex,
};
// Builds the definition privately and publishes it as an immutable snapshot.
RenderScene MakeLightingScene(LightingReceiverDefect defect = LightingReceiverDefect::None,
                              bool shouldAddAnalyticLight = false)
{
    RenderScene scene;
    SharedPtr<ludus::resources::SceneDefinition> definition;
    REQUIRE(TryMakeShared<ludus::resources::SceneDefinition>(GetSystemAllocationDomain(), definition) ==
            OwnershipStatus::Ok);
    scene.Identity = QByteArrayLiteral("lighting-fixture");
    scene.AssetsIdentity = QByteArrayLiteral("lighting-assets");
    scene.LogicalId = QStringLiteral("level/lighting-fixture");
    scene.Lit = true;
    scene.Center = {0, .5F, 0};
    scene.Radius = 4;
    ludus::resources::SceneDefinition& data = *definition;
    REQUIRE(data.Meshes.TryResize(2));
    REQUIRE(data.Materials.TryResize(2));
    REQUIRE(data.Objects.TryResize(2));
    AddLightingQuad(data.Meshes[0], -2, -2, 4, 0, false);
    for (usize i = 0; i < 13; ++i)
    {
        const usize row = i / 5;
        AddLightingQuad(data.Meshes[1],
                        static_cast<float32>(i % 5) * .6F - 1.3F,
                        static_cast<float32>(row) * .6F - .7F,
                        .2F,
                        1,
                        true);
    }
    data.Materials[1].Emission = {20, 20, 20};
    data.Materials[1].AreaSourceId = 51;
    data.Materials[1].IsEmissionTwoSided = false;
    data.Objects[0].SourceId = 101;
    data.Objects[1].SourceId = 102;
    data.Objects[1].Mesh = 1;
    data.Objects[1].Material = 1;
    data.Eye = {0, 2, 4};
    data.Target = {0, 0, 0};
    data.Up = {0, 1, 0};
    ludus::resources::SceneMesh& receiver = data.Meshes[0];
    if (defect == LightingReceiverDefect::ZeroAreaTriangle)
    {
        const uint32 degenerate[] = {0, 0, 0};
        REQUIRE(receiver.Indices.TryAddRange(degenerate));
    }
    else if (defect == LightingReceiverDefect::MismatchedBakeTexcoords)
    {
        REQUIRE(receiver.BakeTexcoords.TryResize(receiver.Positions.GetSize()));
        for (math::Vector2& textureCoordinateBakeAtlas : receiver.BakeTexcoords)
        {
            textureCoordinateBakeAtlas = {0, 0};
        }
    }
    else if (defect == LightingReceiverDefect::OutOfRangeIndex)
    {
        receiver.Indices[0] = ~uint32{0};
    }
    if (shouldAddAnalyticLight == true)
    {
        REQUIRE(data.Lights.TryResize(1) == true);
        ludus::resources::SceneLight& light = data.Lights[0];
        light.SourceId = 103;
        light.Kind = ludus::resources::SceneLightKind::Directional;
        light.DirectionWorld = {0, -1, 0};
        light.IntensityRgb = {2, 1, 0};
        QString error;
        REQUIRE(RenderScene::LoadCookedLights(data, scene.AuthoredLights, error) == true);
    }
    scene.Definition = Move(definition);
    scene.Meshes.resize(2);
    scene.Surfaces.resize(2);
    for (int32 i = 0; i < 2; ++i)
    {
        scene.Surfaces[i].Texture = -1;
        scene.Surfaces[i].Tint = {.7F, .7F, .7F, 1};
        scene.Surfaces[i].Emission = data.Materials[static_cast<usize>(i)].Emission;
        PreviewObject object;
        object.Id = QString::number(101 + i);
        object.Mesh = i;
        object.Surface = i;
        object.Texture = -1;
        scene.Objects.append(object);
    }
    return scene;
}
RenderScene MakeGiProbeScene(float32 receiverX)
{
    using ludus::resources::SceneDefinition;
    using ludus::resources::SceneLight;
    SharedPtr<SceneDefinition> definition;
    REQUIRE(TryMakeShared<SceneDefinition>(GetSystemAllocationDomain(), definition) == OwnershipStatus::Ok);
    SceneDefinition& data = *definition;
    constexpr uint32 FLOOR = 0;
    constexpr uint32 WALL = 1;
    constexpr uint32 RECEIVER = 2;
    constexpr uint32 OBJECT_COUNT = RECEIVER + 1;
    constexpr uint64 FIRST_SOURCE = 110;
    REQUIRE(data.Meshes.TryResize(OBJECT_COUNT) == true);
    REQUIRE(data.Materials.TryResize(OBJECT_COUNT) == true);
    REQUIRE(data.Objects.TryResize(OBJECT_COUNT) == true);
    AddLightingQuad(data.Meshes[FLOOR], -2, -2, 4, 0, false);
    AddLightingQuad(data.Meshes[WALL], -2, 0, 4, 0, false);
    for (math::Vector3& positionMesh : data.Meshes[WALL].Positions)
    {
        positionMesh = {0, positionMesh.Z, positionMesh.X};
    }
    for (math::Vector3& normalMesh : data.Meshes[WALL].Normals)
    {
        normalMesh = {1, 0, 0};
    }
    AddLightingQuad(data.Meshes[RECEIVER], -.25F, -.25F, .5F, 0, false);
    for (math::Vector3& positionMesh : data.Meshes[RECEIVER].Positions)
    {
        positionMesh = {positionMesh.X, -positionMesh.Z, 0};
    }
    for (math::Vector3& normalMesh : data.Meshes[RECEIVER].Normals)
    {
        normalMesh = {0, 0, 1};
    }
    data.Objects[RECEIVER].Transform.Translation = {receiverX, .8F, .45F};
    data.Eye = {0, 2, 5};
    data.Target = {0, .8F, 0};
    data.Up = {0, 1, 0};
    REQUIRE(data.Lights.TryResize(1) == true);
    SceneLight& light = data.Lights[0];
    light.SourceId = FIRST_SOURCE + OBJECT_COUNT;
    light.Kind = ludus::resources::SceneLightKind::Point;
    light.PositionWorld = {-1, 1.4F, 0};
    light.IntensityRgb = {15, 0, 0};
    light.Range = 6;

    RenderScene scene;
    scene.LogicalId = QStringLiteral("level/gi-probe-fixture");
    scene.Identity = QByteArrayLiteral("gi-probe-fixture/") + QByteArray::number(static_cast<float64>(receiverX));
    scene.AssetsIdentity = QByteArrayLiteral("gi-probe-assets");
    scene.Lit = true;
    // Look along the divider so both receiver positions are visible to the camera.
    scene.StartView =
    {
        .EyeWorld = {static_cast<float64>(data.Eye.X),
                     static_cast<float64>(data.Eye.Y),
                     static_cast<float64>(data.Eye.Z)},
        .TargetWorld = {static_cast<float64>(data.Target.X),
                        static_cast<float64>(data.Target.Y),
                        static_cast<float64>(data.Target.Z)},
        .UpWorld = data.Up,
        .VerticalFov = data.VerticalFov,
    };
    scene.HasStartView = true;
    scene.Center = data.Target;
    scene.Radius = 4;
    scene.Meshes.resize(OBJECT_COUNT);
    scene.Surfaces.resize(OBJECT_COUNT);
    for (uint32 index = 0; index < OBJECT_COUNT; ++index)
    {
        ludus::resources::SceneObject& source = data.Objects[index];
        source.Mesh = index;
        source.Material = index;
        source.SourceId = FIRST_SOURCE + index;
        PreviewObject object;
        object.Id = QString::number(source.SourceId);
        object.Transform = source.Transform;
        object.Mesh = static_cast<int32>(index);
        object.Surface = static_cast<int32>(index);
        scene.Objects.append(object);
        scene.Surfaces[index].Tint = index == WALL ? math::Vector4{0, 0, 0, 1} : math::Vector4{.7F, .7F, .7F, 1};
        scene.Surfaces[index].Roughness = 1;
    }
    QString error;
    REQUIRE(RenderScene::LoadCookedLights(data, scene.AuthoredLights, error) == true);
    scene.AlwaysOnLightIds.append(light.SourceId);
    scene.GiProbeReceiverIds.append(data.Objects[RECEIVER].SourceId);
    scene.ProbeGrid.OriginWorld = {-1.5F, .3F, -.75F};
    scene.ProbeGrid.Spacing = 1.5F;
    scene.ProbeGrid.Counts = {3, 2, 2};
    scene.Definition = Move(definition);
    return scene;
}
} // namespace

TEST_CASE("Native Lit viewport renders thirteen shadowed area patches without baking", "[.render-native][lighting]")
{
    RenderScene scene = MakeLightingScene();
    const QByteArray key = preview_lighting::MakeKey(scene);
    preview_lighting::Inputs inputs;
    REQUIRE(preview_lighting::BuildInputs(scene, key, inputs) == ludus::graphics::rhi::RasterStatus::Ready);
    REQUIRE(inputs.AreaCount == 13);
    REQUIRE(inputs.ShadowCount == 78);
    RenderScene cameraOnly = scene;
    cameraOnly.Identity = QByteArrayLiteral("changed-camera");
    PreviewCamera camera;
    camera.Id = QStringLiteral("authored-camera-update");
    cameraOnly.Cameras.append(camera);
    REQUIRE(preview_lighting::MakeKey(cameraOnly) == key);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(320, 240);
    viewport.show();
    REQUIRE(preview.SetScene(scene));
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start());
    REQUIRE(Frames(preview, 2));
    REQUIRE(viewport.PresentedFrames() > 0);
    const uint64 beforeCamera = preview.Frames();
    preview.Orbit(2, 0);
    REQUIRE(preview.SetScene(cameraOnly));
    REQUIRE(Frames(preview, beforeCamera + 2));
    INFO(preview.Status().toStdString());
    REQUIRE(preview.Status().contains(QStringLiteral("13 planar area proxies / 78 shadow faces")) == true);
    CHECK(preview.Status().contains(QStringLiteral("direct lighting")) == true);
    CHECK(preview.Status().contains(QStringLiteral("bake")) == false);
    REQUIRE(ReadViewport().isNull() == false);
    preview.Stop();
}
TEST_CASE("Native viewport screenshot reads the presented GPU frame back without overlays",
          "[.render-native][screenshot]")
{
    RenderScene scene = MakeLightingScene();
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(320, 240);
    viewport.show();
    REQUIRE(preview.SetScene(scene));
    preview.SetShading(PreviewShading::Lit);
    preview.SetStatisticsVisible(true);
    REQUIRE(preview.Start());
    REQUIRE(Frames(preview, 3));
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = QDir(directory.path()).filePath(QStringLiteral("screenshot.png"));
    QString error;
    const bool hasStarted = viewport.SaveScreenshot(path, error);
    INFO(error.toStdString());
    REQUIRE(hasStarted == true);
    REQUIRE(QTest::qWaitFor([&viewport]() { return viewport.IsTakingScreenshot() == false; }, 10000) == true);
    INFO(viewport.GetScreenshotError().toStdString());
    REQUIRE(viewport.GetScreenshotPath() == path);
    const QImage window = ReadViewport().convertToFormat(QImage::Format_RGB32);
    QImage screenshot;
    REQUIRE(screenshot.load(path, "PNG") == true);
    CHECK(screenshot.hasAlphaChannel() == false);
    // The built-in preview renders the window's physical size, so the frame fills it texel for texel.
    REQUIRE(screenshot.size() == window.size());
    screenshot = screenshot.convertToFormat(QImage::Format_RGB32);
    const auto isClose = [](const QImage& first, const QImage& second) {
        constexpr int32 TOLERANCE = 2;
        for (int32 y = 0; y < first.height(); ++y)
        {
            for (int32 x = 0; x < first.width(); ++x)
            {
                const QRgb a = first.pixel(x, y);
                const QRgb b = second.pixel(x, y);
                if (qAbs(qRed(a) - qRed(b)) > TOLERANCE || qAbs(qGreen(a) - qGreen(b)) > TOLERANCE ||
                    qAbs(qBlue(a) - qBlue(b)) > TOLERANCE)
                {
                    return false;
                }
            }
        }
        return true;
    };
    // The bottom half, clear of overlays, is the same frame; the statistics panel in the top-left corner and the
    // axis widget in the top-right corner are only in the window.
    const QRect bottom(0, window.height() / 2, window.width(), window.height() - window.height() / 2);
    CHECK(isClose(screenshot.copy(bottom), window.copy(bottom)) == true);
    const QRect corner(0, 0, window.width() / 4, window.height() / 4);
    CHECK(isClose(screenshot.copy(corner), window.copy(corner)) == false);
    preview.Stop();
}
TEST_CASE("Native Lit viewport ignores lightmap UVs and keeps live camera rendering", "[.render-native][lighting]")
{
    RenderScene scene = MakeLightingScene(LightingReceiverDefect::MismatchedBakeTexcoords);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(320, 240);
    viewport.show();
    REQUIRE(preview.SetScene(scene));
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start());
    REQUIRE(Frames(preview, 2));
    INFO(preview.Status().toStdString());
    CHECK(preview.Status().contains(QStringLiteral("direct lighting")) == true);
    CHECK(preview.Status().contains(QStringLiteral("bake")) == false);
    const uint64 previous = preview.Frames();
    preview.Orbit(4, 2);
    REQUIRE(Frames(preview, previous + 2));
    CHECK(viewport.PresentedFrames() > 0);
    preview.Stop();
}
TEST_CASE("Native Lit viewport publishes an opted-in always-on bake and falls back on failure",
          "[.render-native][lighting]")
{
    RenderScene scene = MakeLightingScene(LightingReceiverDefect::None, true);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(320, 240);
    viewport.show();
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start() == true);
    REQUIRE(Frames(preview, 2) == true);
    CHECK(preview.Status().contains(QStringLiteral("direct lighting")) == true);

    const auto waitForBakeStatus = [&preview](const QString& expected) {
        constexpr int64 BAKE_DEADLINE_MILLISECONDS = 15000;
        constexpr int32 POLL_MILLISECONDS = 10;
        QElapsedTimer timer;
        timer.start();
        while (preview.Status().contains(expected) == false && timer.elapsed() < BAKE_DEADLINE_MILLISECONDS)
        {
            QTest::qWait(POLL_MILLISECONDS);
        }
        return preview.Status().contains(expected);
    };
    const QImage previousFrame = ReadViewport();
    REQUIRE(previousFrame.isNull() == false);
    scene.Identity = QByteArrayLiteral("selected-always-on-light");
    scene.AlwaysOnLightIds.append(scene.AuthoredLights.Lights[0].SourceId);
    REQUIRE(preview.SetScene(scene) == true);
    // The previous image stays visible while the replacement prepares. The bake can finish before
    // the replacement presents its first frame.
    CHECK(ReadViewport() == previousFrame);
    const bool hasBaked = waitForBakeStatus(QStringLiteral("always-on lighting baked"));
    INFO(preview.Status().toStdString());
    REQUIRE(hasBaked == true);
    REQUIRE(Frames(preview, preview.Frames() + 2) == true);
    CHECK(ReadViewport().isNull() == false);

    scene.Identity = QByteArrayLiteral("missing-always-on-light");
    scene.AlwaysOnLightIds[0] = 1000;
    REQUIRE(preview.SetScene(scene) == true);
    REQUIRE(waitForBakeStatus(QStringLiteral("Using realtime lighting")) == true);
    REQUIRE(Frames(preview, preview.Frames() + 2) == true);
    CHECK(ReadViewport().isNull() == false);
    preview.Stop();
}

TEST_CASE("Native Lit bake chart preparation rejects out-of-range mesh indices", "[.render-native][lighting]")
{
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(320, 240);
    viewport.show();
    REQUIRE(preview.SetScene(MakeLightingScene()) == true);
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start() == true);
    REQUIRE(Frames(preview, 2) == true);

    RenderScene invalid = MakeLightingScene(LightingReceiverDefect::OutOfRangeIndex, true);
    invalid.AlwaysOnLightIds.append(invalid.AuthoredLights.Lights[0].SourceId);
    PreviewGpuGeneration replacement(preview.GetDevice(), invalid, PreviewShading::Lit, preview.GetShaders());
    QString error;
    PreviewPreparationStatus status = PreviewPreparationStatus::Pending;
    REQUIRE(QTest::qWaitFor([&]() {
                status = replacement.Prepare(preview.GetSurface(), error);
                return status != PreviewPreparationStatus::Pending;
            }) == true);
    INFO(error.toStdString());
    REQUIRE(status == PreviewPreparationStatus::Failed);
    CHECK(replacement.DescribeBake().contains(QStringLiteral("Could not prepare bake chart geometry")) == true);
    CHECK(Frames(preview, preview.Frames() + 1) == true);
    CHECK(ReadViewport().isNull() == false);
}

TEST_CASE("Native Lit probe GI follows a receiver and rejects bounce through a wall",
          "[.render-native][lighting][probe-gi]")
{
    constexpr int32 BAKE_DEADLINE_MILLISECONDS = 15000;
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString worldPath = QDir(directory.path()).filePath(QStringLiteral("probes.world.json"));
    RenderPreview preview;
    preview.SetSurfaceBakeWorldPath(worldPath);
    PreviewViewport viewport(&preview);
    viewport.resize(320, 240);
    viewport.show();
    const RenderScene litRoom = MakeGiProbeScene(-.75F);
    REQUIRE(preview.SetScene(litRoom) == true);
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start() == true);

    const auto sampleReceiver = [&preview]() {
        constexpr int32 POLL_MILLISECONDS = 10;
        QElapsedTimer timer;
        timer.start();
        while (preview.Status().contains(QStringLiteral("probe GI baked")) == false &&
               timer.elapsed() < BAKE_DEADLINE_MILLISECONDS)
        {
            QTest::qWait(POLL_MILLISECONDS);
        }
        INFO(preview.Status().toStdString());
        REQUIRE(preview.Status().contains(QStringLiteral("probe GI baked")) == true);
        REQUIRE(Frames(preview, preview.Frames() + 2) == true);
        const QImage image = ReadViewport();
        REQUIRE(image.isNull() == false);
        ludus::graphics::renderer::ViewDescription view;
        QString error;
        REQUIRE(preview.DescribeView(static_cast<uint32>(image.width()),
                                     static_cast<uint32>(image.height()),
                                     view,
                                     error) == true);
        const math::Vector3 centreWorld = preview.Scene().Objects.last().Transform.Translation;
        const math::Vector4 clip = view.WorldToClip * math::Vector4{centreWorld.X, centreWorld.Y, centreWorld.Z, 1};
        const QPoint positionPixel(
            static_cast<int32>((clip.X / clip.W * .5F + .5F) * static_cast<float32>(image.width())),
            static_cast<int32>((.5F - clip.Y / clip.W * .5F) * static_cast<float32>(image.height())));
        REQUIRE(image.rect().contains(positionPixel) == true);
        return image.pixelColor(positionPixel);
    };
    const QColor lit = sampleReceiver();
    REQUIRE(preview.SaveSurfaceBake() == true);
    WorldSurfaceBakeState reopened;
    reopened.SetWorldPath(worldPath);
    reopened.SetScene(litRoom);
    const QSharedPointer<const WorldSurfaceBake> saved = reopened.GetReady();
    REQUIRE(saved.isNull() == false);
    // Save World retains the probe atlas beside the charts, so reopening reuses the whole bake.
    REQUIRE(preview.GetSurfaceBake().isNull() == false);
    CHECK(saved->ProbeAtlas.Pixels.isEmpty() == false);
    CHECK(saved->ProbeAtlas.Pixels == preview.GetSurfaceBake()->ProbeAtlas.Pixels);
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(litRoom, {}, 1, extraction, error, saved) == true);
    REQUIRE(extraction.SurfaceCharts.size() == litRoom.Objects.size());
    CHECK(extraction.SurfaceCharts.first().SourceId == extraction.Items.first().SourceId);
    CHECK(extraction.SurfaceCharts.last().SourceId == 0);
    // The receiver has no chart; deferred Baked Lighting passes sample the saved probe atlas for it instead.
    CHECK(extraction.ProbeReceiverIds == QVector<uint64>{extraction.Items.last().SourceId});
    CHECK(extraction.ShaderInputs.ProbeVolume.Atlas.Width == saved->ProbeAtlas.Width);
    CHECK(extraction.ShaderInputs.ProbeVolume.Atlas.Height == saved->ProbeAtlas.Height);
    CHECK(extraction.ShaderInputs.ProbeVolume.IrradianceScale == saved->ProbeAtlas.IrradianceScale);
    CHECK(extraction.ShaderInputs.ProbeVolume.Grid.Spacing == litRoom.ProbeGrid.Spacing);
    reopened.Poll(litRoom);
    CHECK(reopened.GetReady() == saved);
    // Use the real cache prefix and the legacy atlas extent to exercise the saved-cache admission path.
    QFile cache(worldPath + QStringLiteral(".ludus-lighting"));
    REQUIRE(cache.open(QIODevice::ReadWrite) == true);
    constexpr qint64 ATLAS_HEADER_BYTES = sizeof(uint32) + sizeof(uint32) + sizeof(float32);
    const qint64 atlasStart = cache.size() - saved->ProbeAtlas.Pixels.size() - ATLAS_HEADER_BYTES;
    REQUIRE(cache.resize(atlasStart) == true);
    REQUIRE(cache.seek(atlasStart) == true);
    QDataStream stream(&cache);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    constexpr uint32 LEGACY_ATLAS_WIDTH = 256;
    constexpr uint32 LEGACY_FACE_WIDTH = 8;
    constexpr uint32 LEGACY_PROBE_TEXELS = LUDUS_GI_PROBE_FACES * (1 + LEGACY_FACE_WIDTH * LEGACY_FACE_WIDTH);
    const ludus::graphics::renderer::GiProbeGrid& grid = litRoom.ProbeGrid;
    const uint32 probeCount = grid.Counts[0] * grid.Counts[1] * grid.Counts[2];
    const uint32 legacyHeight = (probeCount * LEGACY_PROBE_TEXELS + LEGACY_ATLAS_WIDTH - 1) / LEGACY_ATLAS_WIDTH;
    stream << LEGACY_ATLAS_WIDTH << legacyHeight << saved->ProbeAtlas.IrradianceScale;
    const QByteArray legacyPixels(static_cast<qsizetype>(legacyHeight) * LEGACY_ATLAS_WIDTH *
                                      ludus::content::scene_format::TEXEL_BYTES,
                                  '\0');
    REQUIRE(stream.writeRawData(legacyPixels.constData(), legacyPixels.size()) == legacyPixels.size());
    cache.close();
    WorldSurfaceBakeState legacy;
    legacy.SetWorldPath(worldPath);
    legacy.SetScene(litRoom);
    CHECK(legacy.GetReady().isNull() == true);
    REQUIRE(QTest::qWaitFor(
                [&legacy, &litRoom]() {
                    legacy.Poll(litRoom);
                    return legacy.GetReady().isNull() == false;
                },
                BAKE_DEADLINE_MILLISECONDS) == true);
    CHECK(legacy.GetReady()->ProbeAtlas.Pixels == saved->ProbeAtlas.Pixels);
    // Keep the receiver and geometry in place; move only the volume away from them.
    ludus::graphics::renderer::GiProbeGrid movedGrid = litRoom.ProbeGrid;
    constexpr float32 MOVED_GRID_ORIGIN_X_METRES = 10;
    movedGrid.OriginWorld.X = MOVED_GRID_ORIGIN_X_METRES;
    preview.SetProbeGrid(movedGrid);
    CHECK(preview.GetSurfaceBake().isNull() == true);
    CHECK(preview.Scene().Identity == litRoom.Identity);
    REQUIRE(QTest::qWaitFor(
                [&preview, &movedGrid]() {
                    const QSharedPointer<const WorldSurfaceBake> bake = preview.GetSurfaceBake();
                    return bake.isNull() == false && bake->ProbeAtlas.Pixels.isEmpty() == false &&
                           preview.Scene().ProbeGrid.OriginWorld.X == movedGrid.OriginWorld.X;
                },
                BAKE_DEADLINE_MILLISECONDS) == true);
    CHECK(preview.GetSurfaceBake()->LightingKey == preview_lighting::MakeKey(preview.Scene()));
    const QColor outside = sampleReceiver();
    CHECK(outside.red() <= outside.green() + 3);

    preview.SetProbeGrid(litRoom.ProbeGrid);
    const QColor restored = sampleReceiver();
    CHECK(restored.red() > restored.green() + 15);

    const RenderScene darkRoom = MakeGiProbeScene(.75F);
    REQUIRE(preview.SetScene(darkRoom) == true);
    const QColor dark = sampleReceiver();
    CAPTURE(lit.red(), lit.green(), dark.red(), dark.green());
    CHECK(lit.red() > lit.green() + 15);
    CHECK(dark.red() <= dark.green() + 3);
    preview.Stop();
}

namespace
{
namespace rr = ludus::graphics::renderer;
// Probe GI visibility qualification. The grid mirrors the synthetic unit cell of
// docs/architecture/renderer-systems.md#probe-visibility-near-walls-and-openings: the studied cell
// spans x = 0 (lit probes) to x = 1 (dark probes), y = 0.5 to 1.5 and z = -0.5 to 0.5, in metres.
enum class ProbeDivider : uint8
{
    AxisWall,
    DiagonalWall,
    Doorway,
    EmbeddedLayers,
};
enum class ProbeVisibility : uint8
{
    DepthMap,
    DistanceMoments,
    Exact,
};
struct ProbeQuad final
{
    math::Vector3 CornerWorld;
    math::Vector3 EdgeU;
    math::Vector3 EdgeV;
};
struct ProbeOccluders final
{
    QVector<ProbeQuad> Grey;
    QVector<ProbeQuad> Black;
};
struct ProbeLookup final
{
    float32 Irradiance = 0;
    uint32 AdmittedProbes = 0;
    float32 FalseAdmission = 0;
    float32 FalseRejection = 0;
};
constexpr float32 PROBE_ROOM_HALF_WIDTH_METRES = 3;
constexpr float32 PROBE_WALL_HEIGHT_METRES = 3;
constexpr float32 PROBE_WALL_X_METRES = .5F;
constexpr float32 PROBE_DIAGONAL_SLOPE = .4F;
constexpr float32 PROBE_CELL_CENTRE_Y_METRES = 1;
constexpr float32 PROBE_DOOR_BOTTOM_METRES = .9F;
constexpr float32 PROBE_DOOR_TOP_METRES = 1.1F;
constexpr float32 PROBE_LOWER_LAYER_Y_METRES = .5F;
constexpr float32 PROBE_UPPER_LAYER_Y_METRES = 1.5F;
// Inside the bake's minimum clearance (2/255 of the two-cell depth range), so every probe touches geometry.
constexpr float32 PROBE_EMBEDDED_CLEARANCE_METRES = .003F;
constexpr float32 PROBE_RECEIVER_SIZE_METRES = .06F;
constexpr float32 PROBE_RECEIVER_ALBEDO = .7F;
constexpr uint32 PROBE_PATH_RECEIVERS = 10;
constexpr float32 PROBE_PATH_STEP_METRES = .1F;
constexpr uint32 PROBE_REFERENCE_SEEDS = 16;
constexpr uint32 PROBE_REFERENCE_WORKERS = 4;
constexpr int32 PROBE_BAKE_DEADLINE_MILLISECONDS = 30000;
constexpr uint32 PROBE_FACES = LUDUS_GI_PROBE_FACES;
constexpr uint32 PROBE_FACE_WIDTH = LUDUS_GI_PROBE_FACE_WIDTH;
constexpr uint32 PROBE_FACE_TEXELS = LUDUS_GI_PROBE_FACE_TEXELS;
constexpr uint32 PROBE_TEXELS = LUDUS_GI_PROBE_TEXELS;
constexpr uint32 PROBE_ATLAS_WIDTH = LUDUS_GI_PROBE_ATLAS_WIDTH;
constexpr uint32 PROBE_DEPTH_CELLS = LUDUS_GI_PROBE_DEPTH_CELLS;
constexpr float32 PROBE_DEPTH_BIAS_CELLS = LUDUS_GI_PROBE_DEPTH_BIAS_CELLS;
constexpr usize PROBE_FOOTPRINT_TAPS = 4;
using ProbeFootprint = StaticArray<float32, PROBE_FOOTPRINT_TAPS>;
constexpr uint32 PROBE_POSITIVE_Z_FACE = 4;
constexpr usize PROBE_ALPHA_CHANNEL = 3;
constexpr float32 UNORM_BYTE_MAX = 255;

rr::GiProbeGrid MakeProbeQualificationGrid()
{
    rr::GiProbeGrid grid;
    grid.OriginWorld = {-1, PROBE_LOWER_LAYER_Y_METRES, -.5F};
    grid.Spacing = 1;
    grid.Counts = {3, 2, 2};
    return grid;
}
float32 GetComponent(math::Vector3 value, uint32 axis)
{
    return axis == 0 ? value.X : (axis == 1 ? value.Y : value.Z);
}
math::Vector3 MakeAxis(uint32 axis, float32 length)
{
    return {axis == 0 ? length : 0, axis == 1 ? length : 0, axis == 2 ? length : 0};
}
void AddProbeQuad(ludus::resources::SceneMesh& inoutMesh, const ProbeQuad& quad)
{
    const uint32 start = static_cast<uint32>(inoutMesh.Positions.GetSize());
    const math::Vector3 normalWorld = math::Cross(quad.EdgeU, quad.EdgeV);
    const math::Vector3 unitNormalWorld = normalWorld / math::Length(normalWorld);
    const StaticArray<math::Vector3, 4> cornersWorld{quad.CornerWorld,
                                                     quad.CornerWorld + quad.EdgeU,
                                                     quad.CornerWorld + quad.EdgeU + quad.EdgeV,
                                                     quad.CornerWorld + quad.EdgeV};
    const StaticArray<math::Vector2, 4> texcoords{math::Vector2{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (usize i = 0; i < cornersWorld.GetSize(); ++i)
    {
        REQUIRE(inoutMesh.Positions.TryAdd(cornersWorld[i]) == true);
        REQUIRE(inoutMesh.Normals.TryAdd(unitNormalWorld) == true);
        REQUIRE(inoutMesh.Texcoords.TryAdd(texcoords[i]) == true);
    }
    // Counter-clockwise about Cross(EdgeU, EdgeV), the front face.
    const StaticArray<uint32, 6> order{0, 1, 2, 0, 2, 3};
    for (const uint32 corner : order)
    {
        REQUIRE(inoutMesh.Indices.TryAdd(start + corner) == true);
    }
}
ProbeOccluders MakeProbeOccluders(ProbeDivider divider)
{
    constexpr float32 HALF = PROBE_ROOM_HALF_WIDTH_METRES;
    constexpr float32 SPAN = 2 * PROBE_ROOM_HALF_WIDTH_METRES;
    constexpr float32 HEIGHT = PROBE_WALL_HEIGHT_METRES;
    ProbeOccluders occluders;
    const bool isEmbedded = divider == ProbeDivider::EmbeddedLayers;
    const float32 floorY = isEmbedded == true ? PROBE_LOWER_LAYER_Y_METRES - PROBE_EMBEDDED_CLEARANCE_METRES : 0;
    occluders.Grey.append(ProbeQuad{{-HALF, floorY, -HALF}, {0, 0, SPAN}, {SPAN, 0, 0}});
    if (isEmbedded == true)
    {
        // A ceiling just above the upper probe layer, facing down.
        occluders.Grey.append(ProbeQuad{{-HALF, PROBE_UPPER_LAYER_Y_METRES + PROBE_EMBEDDED_CLEARANCE_METRES, -HALF},
                                        {SPAN, 0, 0},
                                        {0, 0, SPAN}});
    }
    if (divider == ProbeDivider::DiagonalWall)
    {
        // The plane x + 0.4 (y - 1) = 0.5 leans over the lit room and contains the camera's view axis.
        const float32 baseX = PROBE_WALL_X_METRES + PROBE_DIAGONAL_SLOPE * PROBE_CELL_CENTRE_Y_METRES;
        occluders.Black.append(ProbeQuad{{baseX, 0, -HALF}, {-PROBE_DIAGONAL_SLOPE * HEIGHT, HEIGHT, 0}, {0, 0, SPAN}});
    }
    else if (divider == ProbeDivider::Doorway)
    {
        // An opening for 0.9 < y < 1.1 across the whole wall.
        occluders.Black.append(
            ProbeQuad{{PROBE_WALL_X_METRES, 0, -HALF}, {0, PROBE_DOOR_BOTTOM_METRES, 0}, {0, 0, SPAN}});
        occluders.Black.append(ProbeQuad{{PROBE_WALL_X_METRES, PROBE_DOOR_TOP_METRES, -HALF},
                                         {0, HEIGHT - PROBE_DOOR_TOP_METRES, 0},
                                         {0, 0, SPAN}});
    }
    else
    {
        occluders.Black.append(ProbeQuad{{PROBE_WALL_X_METRES, 0, -HALF}, {0, HEIGHT, 0}, {0, 0, SPAN}});
    }
    return occluders;
}
// Receivers face +Z, away from the point light at z = -0.4, so their only illumination is probe GI.
RenderScene MakeProbeQualificationScene(ProbeDivider divider,
                                        const QVector<math::Vector3>& receiversWorld,
                                        const QByteArray& identity,
                                        float32 receiverScale = 1)
{
    using ludus::resources::SceneDefinition;
    using ludus::resources::SceneLight;
    SharedPtr<SceneDefinition> definition;
    REQUIRE(TryMakeShared<SceneDefinition>(GetSystemAllocationDomain(), definition) == OwnershipStatus::Ok);
    SceneDefinition& data = *definition;
    constexpr uint32 GREY = 0;
    constexpr uint32 BLACK = 1;
    constexpr uint32 RECEIVER_MESH = 2;
    constexpr uint32 MESH_COUNT = RECEIVER_MESH + 1;
    constexpr uint32 MATERIAL_COUNT = BLACK + 1;
    constexpr uint64 FIRST_SOURCE = 300;
    const ProbeOccluders occluders = MakeProbeOccluders(divider);
    REQUIRE(data.Meshes.TryResize(MESH_COUNT) == true);
    for (const ProbeQuad& quad : occluders.Grey)
    {
        AddProbeQuad(data.Meshes[GREY], quad);
    }
    for (const ProbeQuad& quad : occluders.Black)
    {
        AddProbeQuad(data.Meshes[BLACK], quad);
    }
    constexpr float32 HALF_RECEIVER = PROBE_RECEIVER_SIZE_METRES / 2;
    AddProbeQuad(
        data.Meshes[RECEIVER_MESH],
        {{-HALF_RECEIVER, -HALF_RECEIVER, 0}, {PROBE_RECEIVER_SIZE_METRES, 0, 0}, {0, PROBE_RECEIVER_SIZE_METRES, 0}});
    REQUIRE(data.Materials.TryResize(MATERIAL_COUNT) == true);
    const usize objectCount = RECEIVER_MESH + static_cast<usize>(receiversWorld.size());
    REQUIRE(data.Objects.TryResize(objectCount) == true);
    for (usize index = 0; index < objectCount; ++index)
    {
        ludus::resources::SceneObject& object = data.Objects[index];
        object.SourceId = FIRST_SOURCE + index;
        object.Mesh = static_cast<uint32>(index < RECEIVER_MESH ? index : RECEIVER_MESH);
        object.Material = index == BLACK ? BLACK : GREY;
        if (index >= RECEIVER_MESH)
        {
            object.Transform.Translation = receiversWorld[static_cast<qsizetype>(index - RECEIVER_MESH)];
            object.Transform.Columns[0].X = receiverScale;
            object.Transform.Columns[1].Y = receiverScale;
            object.Transform.Columns[2].Z = receiverScale;
        }
    }
    data.Eye = {PROBE_WALL_X_METRES, PROBE_CELL_CENTRE_Y_METRES, 3.5F};
    data.Target = {PROBE_WALL_X_METRES, PROBE_CELL_CENTRE_Y_METRES, 0};
    data.Up = {0, 1, 0};
    REQUIRE(data.Lights.TryResize(1) == true);
    SceneLight& light = data.Lights[0];
    light.SourceId = FIRST_SOURCE - 1;
    light.Kind = ludus::resources::SceneLightKind::Point;
    light.PositionWorld = {-.5F, 1.2F, -.4F};
    light.IntensityRgb = {15, 0, 0};
    light.Range = 6;

    RenderScene scene;
    scene.LogicalId = QStringLiteral("level/gi-probe-qualification");
    scene.Identity = identity;
    // Geometry differs per divider, so each divider needs its own asset identity in the lighting key.
    scene.AssetsIdentity =
        QByteArrayLiteral("gi-probe-qualification-assets/") + QByteArray::number(static_cast<uint32>(divider));
    scene.Lit = true;
    scene.StartView =
    {
        .EyeWorld = {static_cast<float64>(data.Eye.X),
                     static_cast<float64>(data.Eye.Y),
                     static_cast<float64>(data.Eye.Z)},
        .TargetWorld = {static_cast<float64>(data.Target.X),
                        static_cast<float64>(data.Target.Y),
                        static_cast<float64>(data.Target.Z)},
        .UpWorld = data.Up,
        .VerticalFov = data.VerticalFov,
    };
    scene.HasStartView = true;
    scene.Center = data.Target;
    scene.Radius = 4;
    scene.Meshes.resize(MESH_COUNT);
    scene.Surfaces.resize(MATERIAL_COUNT);
    scene.Surfaces[GREY].Tint = {PROBE_RECEIVER_ALBEDO, PROBE_RECEIVER_ALBEDO, PROBE_RECEIVER_ALBEDO, 1};
    scene.Surfaces[BLACK].Tint = {0, 0, 0, 1};
    for (PreviewSurface& surface : scene.Surfaces)
    {
        surface.Roughness = 1;
    }
    for (usize index = 0; index < objectCount; ++index)
    {
        const ludus::resources::SceneObject& source = data.Objects[index];
        PreviewObject object;
        object.Id = QString::number(source.SourceId);
        object.Transform = source.Transform;
        object.Mesh = static_cast<int32>(source.Mesh);
        object.Surface = static_cast<int32>(source.Material);
        scene.Objects.append(object);
        if (index >= RECEIVER_MESH)
        {
            scene.GiProbeReceiverIds.append(source.SourceId);
        }
    }
    QString error;
    REQUIRE(RenderScene::LoadCookedLights(data, scene.AuthoredLights, error) == true);
    scene.AlwaysOnLightIds.append(light.SourceId);
    scene.ProbeGrid = MakeProbeQualificationGrid();
    scene.Definition = Move(definition);
    return scene;
}
float32 DecodeSrgb(float64 encoded)
{
    constexpr float64 LINEAR_LIMIT = .04045;
    constexpr float64 LINEAR_SLOPE = 12.92;
    constexpr float64 OFFSET = .055;
    constexpr float64 GAMMA = 2.4;
    return static_cast<float32>(encoded <= LINEAR_LIMIT ? encoded / LINEAR_SLOPE
                                                        : math::Pow((encoded + OFFSET) / (1 + OFFSET), GAMMA));
}
struct ProbeReceiverMeasurement final
{
    float32 Irradiance = 0;
    /// World positions on the receiver plane (z = receiver z) of the averaged pixel centres.
    QVector<math::Vector3> PixelsWorld;
};
math::Vector2 ProjectToPixel(const rr::ViewDescription& view, const QImage& image, math::Vector3 positionWorld)
{
    const math::Vector4 clip = view.WorldToClip * math::Vector4{positionWorld.X, positionWorld.Y, positionWorld.Z, 1};
    return {(clip.X / clip.W * .5F + .5F) * static_cast<float32>(image.width()),
            (.5F - clip.Y / clip.W * .5F) * static_cast<float32>(image.height())};
}
// Probe irradiance recovered from the presented frame: decode sRGB, invert the viewport's PBR Neutral curve at
// exposure one, then divide the scene-linear red radiance by the receiver's Lambertian reflectance and the
// shader's Schlick transmission (1 - F) for its view angle. The pixel centres are mapped back to the receiver
// plane through the local projection so a CPU lookup can be evaluated at the same points.
ProbeReceiverMeasurement MeasureProbeReceiver(const QImage& image,
                                              const rr::ViewDescription& view,
                                              math::Vector3 receiverWorld,
                                              math::Vector3 eyeWorld)
{
    const math::Vector2 centre = ProjectToPixel(view, image, receiverWorld);
    constexpr float32 JACOBIAN_STEP_METRES = .01F;
    const math::Vector2 alongX =
        (ProjectToPixel(view, image, receiverWorld + math::Vector3{JACOBIAN_STEP_METRES, 0, 0}) - centre) /
        JACOBIAN_STEP_METRES;
    const math::Vector2 alongY =
        (ProjectToPixel(view, image, receiverWorld + math::Vector3{0, JACOBIAN_STEP_METRES, 0}) - centre) /
        JACOBIAN_STEP_METRES;
    const float32 determinant = alongX.X * alongY.Y - alongY.X * alongX.Y;
    REQUIRE(determinant != 0);
    const QPoint centrePixel(static_cast<int32>(centre.X), static_cast<int32>(centre.Y));
    constexpr int32 RADIUS_PIXELS = 1;
    ProbeReceiverMeasurement measurement;
    float64 sum = 0;
    for (int32 y = -RADIUS_PIXELS; y <= RADIUS_PIXELS; ++y)
    {
        for (int32 x = -RADIUS_PIXELS; x <= RADIUS_PIXELS; ++x)
        {
            const QPoint positionPixel = centrePixel + QPoint(x, y);
            REQUIRE(image.rect().contains(positionPixel) == true);
            const QColor color = image.pixelColor(positionPixel);
            const math::Vector3 display = {DecodeSrgb(static_cast<float64>(color.redF())),
                                           DecodeSrgb(static_cast<float64>(color.greenF())),
                                           DecodeSrgb(static_cast<float64>(color.blueF()))};
            sum += static_cast<float64>(rr::InvertPbrNeutral(display).X);
            const math::Vector2 offsetPixel = math::Vector2{static_cast<float32>(positionPixel.x()) + .5F,
                                                            static_cast<float32>(positionPixel.y()) + .5F} -
                                              centre;
            const float32 metresX = (offsetPixel.X * alongY.Y - alongY.X * offsetPixel.Y) / determinant;
            const float32 metresY = (alongX.X * offsetPixel.Y - offsetPixel.X * alongX.Y) / determinant;
            measurement.PixelsWorld.append(receiverWorld + math::Vector3{metresX, metresY, 0});
        }
    }
    const math::Vector3 viewWorld = eyeWorld - receiverWorld;
    const float32 cosineView = viewWorld.Z / math::Length(viewWorld);
    constexpr float32 DIELECTRIC_FRESNEL = .04F;
    const float32 grazing = 1 - cosineView;
    const float32 fresnel =
        DIELECTRIC_FRESNEL + (1 - DIELECTRIC_FRESNEL) * grazing * grazing * grazing * grazing * grazing;
    measurement.Irradiance = static_cast<float32>(sum / static_cast<float64>(measurement.PixelsWorld.size())) *
                             math::kPiF / (PROBE_RECEIVER_ALBEDO * (1 - fresnel));
    return measurement;
}
float32 ReadProbeUnorm(const WorldProbeAtlas& atlas, uint32 probe, uint32 texel, usize channel)
{
    // The atlas rows are PROBE_ATLAS_WIDTH texels, so texel indices run contiguously across rows.
    const usize offset = (usize{probe} * PROBE_TEXELS + texel) * ludus::content::scene_format::TEXEL_BYTES + channel;
    REQUIRE(offset < static_cast<usize>(atlas.Pixels.size()));
    return static_cast<float32>(static_cast<uint8>(atlas.Pixels[static_cast<qsizetype>(offset)])) / UNORM_BYTE_MAX;
}
bool IsSegmentBlocked(math::Vector3 fromWorld, math::Vector3 toWorld, const ProbeOccluders& occluders)
{
    constexpr float32 END_EPSILON = 1e-4F;
    const math::Vector3 segmentWorld = toWorld - fromWorld;
    const auto isBlockedBy = [&](const ProbeQuad& quad) {
        const math::Vector3 normalWorld = math::Cross(quad.EdgeU, quad.EdgeV);
        const float32 denominator = math::Dot(normalWorld, segmentWorld);
        if (denominator == 0)
        {
            return false;
        }
        const float32 t = math::Dot(normalWorld, quad.CornerWorld - fromWorld) / denominator;
        if (t <= END_EPSILON || t >= 1 - END_EPSILON)
        {
            return false;
        }
        const math::Vector3 localWorld = fromWorld + segmentWorld * t - quad.CornerWorld;
        const float32 u = math::Dot(localWorld, quad.EdgeU) / math::Dot(quad.EdgeU, quad.EdgeU);
        const float32 v = math::Dot(localWorld, quad.EdgeV) / math::Dot(quad.EdgeV, quad.EdgeV);
        return u >= 0 && u <= 1 && v >= 0 && v <= 1;
    };
    for (const QVector<ProbeQuad>* group : {&occluders.Grey, &occluders.Black})
    {
        for (const ProbeQuad& quad : *group)
        {
            if (isBlockedBy(quad) == true)
            {
                return true;
            }
        }
    }
    return false;
}
float32 ComputeMomentVisibility(const ProbeFootprint& depths, float32 biasedDistance, const ProbeFootprint& weights)
{
    float32 mean = 0;
    float32 secondMoment = 0;
    for (usize tap = 0; tap < PROBE_FOOTPRINT_TAPS; ++tap)
    {
        mean += weights[tap] * depths[tap];
        secondMoment += weights[tap] * depths[tap] * depths[tap];
    }
    const float32 delta = biasedDistance - mean;
    const float32 variance = math::Max(0.F, secondMoment - mean * mean);
    return delta <= 0 ? 1.F : variance / (variance + delta * delta);
}

float32 ComputeVisibleCoverage(const ProbeFootprint& depths, float32 biasedDistance, const ProbeFootprint& weights)
{
    float32 coverage = 0;
    for (usize tap = 0; tap < PROBE_FOOTPRINT_TAPS; ++tap)
    {
        if (biasedDistance <= depths[tap])
        {
            coverage += weights[tap];
        }
    }
    return coverage;
}

// CPU twin of renderer_lit.slang. The moment comparator uses identical raw depths and footprint weights.
float32 ComputeDepthMapProbeVisibility(const WorldProbeAtlas& atlas,
                                       uint32 probe,
                                       math::Vector3 offsetWorldInCells,
                                       ProbeVisibility visibility)
{
    const math::Vector3 magnitude = {math::Abs(offsetWorldInCells.X),
                                     math::Abs(offsetWorldInCells.Y),
                                     math::Abs(offsetWorldInCells.Z)};
    const uint32 axis =
        magnitude.X >= magnitude.Y && magnitude.X >= magnitude.Z ? 0 : (magnitude.Y >= magnitude.Z ? 1 : 2);
    const float32 distanceInCells = GetComponent(magnitude, axis);
    if (distanceInCells == 0)
    {
        return 1;
    }
    const auto computeFootprintCoordinate = [&](uint32 directionAxis) {
        const float32 direction = GetComponent(offsetWorldInCells, directionAxis) / distanceInCells;
        return math::Clamp((direction * .5F + .5F) * PROBE_FACE_WIDTH - .5F,
                           0.F,
                           static_cast<float32>(PROBE_FACE_WIDTH - 1));
    };
    const float32 u = computeFootprintCoordinate((axis + 1) % rr::GI_PROBE_DIMENSIONS);
    const float32 v = computeFootprintCoordinate((axis + 2) % rr::GI_PROBE_DIMENSIONS);
    const uint32 column = static_cast<uint32>(u);
    const uint32 row = static_cast<uint32>(v);
    const float32 columnFraction = u - static_cast<float32>(column);
    const float32 rowFraction = v - static_cast<float32>(row);
    const uint32 face = 2 * axis + (GetComponent(offsetWorldInCells, axis) < 0 ? 1 : 0);
    const uint32 texel = PROBE_FACES + face * PROBE_FACE_TEXELS + row * PROBE_FACE_WIDTH + column;
    ProbeFootprint depths;
    for (usize tap = 0; tap < PROBE_FOOTPRINT_TAPS; ++tap)
    {
        depths[tap] = ReadProbeUnorm(atlas, probe, texel, tap) * PROBE_DEPTH_CELLS;
    }
    const ProbeFootprint weights{(1 - columnFraction) * (1 - rowFraction),
                                 columnFraction * (1 - rowFraction),
                                 (1 - columnFraction) * rowFraction,
                                 columnFraction * rowFraction};
    const float32 biasedDistance = distanceInCells - PROBE_DEPTH_BIAS_CELLS;
    if (visibility == ProbeVisibility::DistanceMoments)
    {
        return ComputeMomentVisibility(depths, biasedDistance, weights);
    }
    const float32 coverage = ComputeVisibleCoverage(depths, biasedDistance, weights);
    constexpr float32 MIN_COVERAGE = LUDUS_GI_PROBE_MIN_VISIBLE_COVERAGE;
    return math::Clamp((coverage - MIN_COVERAGE) / (1 - MIN_COVERAGE), 0.F, 1.F);
}
// CPU twin of renderer_lit.slang EvaluateGiProbeIrradiance for a +Z normal, with either the baked depth test or an
// exact probe-to-receiver segment test. Both keep the same trilinear weights, validity flags and renormalization.
ProbeLookup EvaluateProbeLookup(const WorldProbeAtlas& atlas,
                                const rr::GiProbeGrid& grid,
                                math::Vector3 positionWorld,
                                ProbeVisibility visibility,
                                const ProbeOccluders& occluders)
{
    ProbeLookup lookup;
    const math::Vector3 positionInGrid = (positionWorld - grid.OriginWorld) / grid.Spacing;
    StaticArray<uint32, rr::GI_PROBE_DIMENSIONS> first{};
    StaticArray<float32, rr::GI_PROBE_DIMENSIONS> fraction{};
    for (uint32 axis = 0; axis < rr::GI_PROBE_DIMENSIONS; ++axis)
    {
        const float32 coordinate = GetComponent(positionInGrid, axis);
        if (coordinate < 0 || coordinate > static_cast<float32>(grid.Counts[axis] - 1))
        {
            return lookup;
        }
        first[axis] = math::Min(static_cast<uint32>(coordinate), grid.Counts[axis] - 2);
        fraction[axis] = coordinate - static_cast<float32>(first[axis]);
    }
    float32 sum = 0;
    float32 totalWeight = 0;
    for (uint32 corner = 0; corner < 8; ++corner)
    {
        StaticArray<uint32, rr::GI_PROBE_DIMENSIONS> cell{};
        float32 weight = 1;
        for (uint32 axis = 0; axis < rr::GI_PROBE_DIMENSIONS; ++axis)
        {
            const uint32 offset = (corner >> axis) & 1U;
            cell[axis] = first[axis] + offset;
            weight *= offset == 1 ? fraction[axis] : 1 - fraction[axis];
        }
        const uint32 probe = cell[0] + grid.Counts[0] * (cell[1] + grid.Counts[1] * cell[2]);
        const math::Vector3 cellInGrid = {static_cast<float32>(cell[0]),
                                          static_cast<float32>(cell[1]),
                                          static_cast<float32>(cell[2])};
        if (weight <= 0 || ReadProbeUnorm(atlas, probe, 0, PROBE_ALPHA_CHANNEL) <= 0)
        {
            continue;
        }
        const bool isBlocked = IsSegmentBlocked(grid.OriginWorld + cellInGrid * grid.Spacing, positionWorld, occluders);
        const float32 visibilityWeight =
            visibility == ProbeVisibility::Exact
                ? (isBlocked == true ? 0.F : 1.F)
                : ComputeDepthMapProbeVisibility(atlas, probe, positionInGrid - cellInGrid, visibility);
        const float32 visibleWeight = weight * visibilityWeight;
        if (isBlocked == true)
        {
            lookup.FalseAdmission += visibleWeight;
        }
        else
        {
            lookup.FalseRejection += weight - visibleWeight;
        }
        if (visibleWeight > 0)
        {
            sum += visibleWeight * ReadProbeUnorm(atlas, probe, PROBE_POSITIVE_Z_FACE, 0);
            totalWeight += visibleWeight;
            ++lookup.AdmittedProbes;
        }
    }
    lookup.Irradiance = totalWeight > 0 ? sum * atlas.IrradianceScale / totalWeight : 0;
    return lookup;
}
// Reference irradiance about +Z at each receiver: the bake's own probe transport (selected lights, at least one
// bounce, receivers excluded) evaluated at the receiver positions, averaged over independent seeds.
QVector<float64> BakeReferenceIrradiance(const RenderScene& scene,
                                         math::Vector3 firstWorld,
                                         uint32 pathAxis,
                                         float64& outMillisecondsPerBake)
{
    rr::GiProbeGrid grid;
    grid.OriginWorld = firstWorld;
    grid.Spacing = PROBE_PATH_STEP_METRES;
    grid.Counts = {2, 2, 2};
    grid.Counts[pathAxis] = PROBE_PATH_RECEIVERS;
    const QByteArray key = preview_lighting::MakeKey(scene);
    preview_lighting::Inputs inputs;
    REQUIRE(preview_lighting::BuildInputs(scene, key, inputs, preview_lighting::InputsTarget::Surfaces) ==
            ludus::graphics::rhi::RasterStatus::Ready);
    threading::JobSystem jobs;
    REQUIRE(jobs.Initialize({ .WorkerCount = PROBE_REFERENCE_WORKERS }) == threading::JobStatus::Ok);
    const uint32 stride = pathAxis == 0 ? 1 : (pathAxis == 1 ? grid.Counts[0] : grid.Counts[0] * grid.Counts[1]);
    const uint32 probeCount = grid.Counts[0] * grid.Counts[1] * grid.Counts[2];
    QVector<float64> sums(static_cast<qsizetype>(PROBE_PATH_RECEIVERS), 0);
    QElapsedTimer timer;
    timer.start();
    for (uint32 seed = 0; seed < PROBE_REFERENCE_SEEDS; ++seed)
    {
        rr::StaticLightingBakeDescription description;
        description.SceneRevision = inputs.SceneRevision;
        description.DependencyRevision = inputs.DependencyRevision;
        // Charts are unused; probes always take their fixed sample count.
        description.SamplesPerTriangle = 1;
        description.Seed = seed + 1;
        description.Surfaces = {inputs.Surfaces.GetData(), inputs.Surfaces.GetSize()};
        description.AnalyticLightIds = {scene.AlwaysOnLightIds.constData(),
                                        static_cast<usize>(scene.AlwaysOnLightIds.size())};
        description.ProbeGrid = grid;
        rr::StaticLightingBake bake;
        REQUIRE(bake.Submit(jobs, *scene.Definition, description) == ludus::graphics::rhi::RasterStatus::Pending);
        REQUIRE(bake.Finish() == ludus::graphics::rhi::RasterStatus::Ready);
        rr::StaticGiProbeAtlas atlas;
        REQUIRE(bake.GetProbeAtlas(atlas) == ludus::graphics::rhi::RasterStatus::Ready);
        REQUIRE(atlas.Height == (probeCount * PROBE_TEXELS + PROBE_ATLAS_WIDTH - 1) / PROBE_ATLAS_WIDTH);
        for (uint32 i = 0; i < PROBE_PATH_RECEIVERS; ++i)
        {
            const usize offset =
                (usize{i} * stride * PROBE_TEXELS + PROBE_POSITIVE_Z_FACE) * ludus::content::scene_format::TEXEL_BYTES;
            REQUIRE(atlas.Pixels[offset + PROBE_ALPHA_CHANNEL] == static_cast<uint8>(UNORM_BYTE_MAX));
            sums[static_cast<qsizetype>(i)] += static_cast<float64>(atlas.Pixels[offset]) /
                                               static_cast<float64>(UNORM_BYTE_MAX) *
                                               static_cast<float64>(atlas.IrradianceScale);
        }
    }
    outMillisecondsPerBake = static_cast<float64>(timer.nsecsElapsed()) / 1e6 / PROBE_REFERENCE_SEEDS;
    for (float64& sum : sums)
    {
        sum /= PROBE_REFERENCE_SEEDS;
    }
    return sums;
}
bool WaitForProbeBake(RenderPreview& inoutPreview, const RenderScene& scene)
{
    const QByteArray key = preview_lighting::MakeKey(scene);
    constexpr int32 POLL_MILLISECONDS = 10;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < PROBE_BAKE_DEADLINE_MILLISECONDS)
    {
        const QSharedPointer<const WorldSurfaceBake> bake = inoutPreview.GetSurfaceBake();
        if (bake.isNull() == false && bake->LightingKey == key && bake->ProbeAtlas.Pixels.isEmpty() == false &&
            inoutPreview.Scene().Identity == scene.Identity &&
            inoutPreview.Status().contains(QStringLiteral("probe GI baked")) == true)
        {
            return Frames(inoutPreview, inoutPreview.Frames() + 3);
        }
        QTest::qWait(POLL_MILLISECONDS);
    }
    return false;
}
} // namespace

namespace
{
struct ProbeReceiverResult final
{
    math::Vector3 PositionWorld;
    float64 Reference = 0;
    float32 Native = 0;
    float32 DepthMap = 0;
    float32 DistanceMoments = 0;
    float32 Exact = 0;
    uint32 MinimumAdmitted = 0;
};
// Average one CPU lookup over the receiver points behind the sampled pixels, as the native sample averages them.
ProbeLookup AverageProbeLookup(const WorldProbeAtlas& atlas,
                               const rr::GiProbeGrid& grid,
                               const QVector<math::Vector3>& pointsWorld,
                               ProbeVisibility visibility,
                               const ProbeOccluders& occluders)
{
    ProbeLookup average;
    average.AdmittedProbes = ~uint32{0};
    for (const math::Vector3& pointWorld : pointsWorld)
    {
        const ProbeLookup lookup = EvaluateProbeLookup(atlas, grid, pointWorld, visibility, occluders);
        average.Irradiance += lookup.Irradiance;
        average.FalseAdmission += lookup.FalseAdmission;
        average.FalseRejection += lookup.FalseRejection;
        average.AdmittedProbes = math::Min(average.AdmittedProbes, lookup.AdmittedProbes);
    }
    const float32 pixelCount = static_cast<float32>(pointsWorld.size());
    average.Irradiance /= pixelCount;
    average.FalseAdmission /= pixelCount;
    average.FalseRejection /= pixelCount;
    return average;
}
QImage ReadProbeFrame(RenderPreview& inoutPreview, rr::ViewDescription& outView)
{
    const QImage image = ReadViewport();
    REQUIRE(image.isNull() == false);
    QString error;
    REQUIRE(inoutPreview.DescribeView(static_cast<uint32>(image.width()),
                                      static_cast<uint32>(image.height()),
                                      outView,
                                      error) == true);
    return image;
}
} // namespace

namespace ludus::editor::testing
{
foundation::float32 ReadPositiveZProbeIrradiance(const WorldProbeAtlas& atlas,
                                                 const graphics::renderer::GiProbeGrid& grid,
                                                 foundation::math::Vector3 positionWorld)
{
    return EvaluateProbeLookup(atlas, grid, positionWorld, ProbeVisibility::DepthMap, {}).Irradiance;
}
} // namespace ludus::editor::testing

TEST_CASE("Equal probe depth moments lose the visible fraction of the footprint", "[probe-gi]")
{
    constexpr float32 TAP_WEIGHT = 1.F / PROBE_FOOTPRINT_TAPS;
    constexpr float32 RECEIVER_DISTANCE = 2.5F;
    constexpr float32 RECEIVER_BIAS = 0;
    const ProbeFootprint weights{TAP_WEIGHT, TAP_WEIGHT, TAP_WEIGHT, TAP_WEIGHT};
    constexpr float32 PAIRED_FAR_DEPTH = 3;
    const ProbeFootprint pairedDepths{1, 1, PAIRED_FAR_DEPTH, PAIRED_FAR_DEPTH};
    const float32 rootTwo = math::Sqrt(2.F);
    const ProbeFootprint spreadDepths{2 - rootTwo, 2, 2, 2 + rootTwo};
    const float32 biasedDistance = RECEIVER_DISTANCE - RECEIVER_BIAS;
    constexpr float32 MOMENT_VISIBILITY = .8F;
    constexpr float32 MOMENT_TOLERANCE = 1e-5F;
    CHECK(math::Abs(ComputeMomentVisibility(pairedDepths, biasedDistance, weights) - MOMENT_VISIBILITY) <
          MOMENT_TOLERANCE);
    CHECK(math::Abs(ComputeMomentVisibility(spreadDepths, biasedDistance, weights) - MOMENT_VISIBILITY) <
          MOMENT_TOLERANCE);
    CHECK(ComputeVisibleCoverage(pairedDepths, biasedDistance, weights) == .5F);
    CHECK(ComputeVisibleCoverage(spreadDepths, biasedDistance, weights) == TAP_WEIGHT);
}

TEST_CASE("Native Lit probe GI visibility qualification around thin and diagonal walls and a doorway",
          "[.render-native][lighting][probe-gi]")
{
    struct QualificationCase final
    {
        const char* Name;
        ProbeDivider Divider;
        math::Vector3 FirstWorld;
        uint32 PathAxis;
    };
    // Paths cross the studied cell at its centre height unless they test a wall's near field.
    enum : uint8
    {
        AXIS_CONTROL,
        AXIS_NEAR_FIELD,
        DIAGONAL,
        DOORWAY,
        EMBEDDED,
        CASE_COUNT,
    };
    const StaticArray<QualificationCase, CASE_COUNT> cases{
        QualificationCase{"axis wall control", ProbeDivider::AxisWall, {.05F, 1, 0}, 0},
        QualificationCase{"axis wall near field", ProbeDivider::AxisWall, {.55F, .55F, 0}, 1},
        QualificationCase{"diagonal wall", ProbeDivider::DiagonalWall, {.05F, .75F, 0}, 0},
        QualificationCase{"doorway", ProbeDivider::Doorway, {.05F, 1, 0}, 0},
        QualificationCase{"embedded probe layers", ProbeDivider::EmbeddedLayers, {.05F, 1, 0}, 0},
    };
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(640, 480);
    viewport.show();
    bool hasStarted = false;
    StaticArray<QVector<ProbeReceiverResult>, CASE_COUNT> results;
    StaticArray<WorldProbeAtlas, CASE_COUNT> atlases;
    QString bakeTimes;
    QString table = QStringLiteral("case\tx\ty\tz\treference\tnative\tdepth map\tdistance moments\texact\tadmitted\n");
    for (uint32 caseIndex = 0; caseIndex < CASE_COUNT; ++caseIndex)
    {
        const QualificationCase& qualification = cases[caseIndex];
        QVector<math::Vector3> receiversWorld;
        for (uint32 i = 0; i < PROBE_PATH_RECEIVERS; ++i)
        {
            receiversWorld.append(qualification.FirstWorld +
                                  MakeAxis(qualification.PathAxis, PROBE_PATH_STEP_METRES * static_cast<float32>(i)));
        }
        const RenderScene scene =
            MakeProbeQualificationScene(qualification.Divider,
                                        receiversWorld,
                                        QByteArray("gi-probe-qualification/") + qualification.Name);
        REQUIRE(preview.SetScene(scene) == true);
        if (hasStarted == false)
        {
            preview.SetShading(PreviewShading::Lit);
            REQUIRE(preview.Start() == true);
            hasStarted = true;
        }
        INFO(qualification.Name);
        const bool hasBaked = WaitForProbeBake(preview, scene);
        INFO(preview.Status().toStdString());
        REQUIRE(hasBaked == true);
        atlases[caseIndex] = preview.GetSurfaceBake()->ProbeAtlas;
        float64 millisecondsPerBake = 0;
        const QVector<float64> reference =
            BakeReferenceIrradiance(scene, qualification.FirstWorld, qualification.PathAxis, millisecondsPerBake);
        bakeTimes += QStringLiteral("%1: %2 ms\n")
                         .arg(QString::fromUtf8(qualification.Name))
                         .arg(millisecondsPerBake, 0, 'f', 1);
        const ProbeOccluders occluders = MakeProbeOccluders(qualification.Divider);
        rr::ViewDescription view;
        const QImage image = ReadProbeFrame(preview, view);
        for (uint32 i = 0; i < PROBE_PATH_RECEIVERS; ++i)
        {
            ProbeReceiverResult result;
            result.PositionWorld = receiversWorld[static_cast<qsizetype>(i)];
            result.Reference = reference[static_cast<qsizetype>(i)];
            const ProbeReceiverMeasurement measurement =
                MeasureProbeReceiver(image, view, result.PositionWorld, scene.Definition->Eye);
            result.Native = measurement.Irradiance;
            const ProbeLookup depthMap = AverageProbeLookup(atlases[caseIndex],
                                                            scene.ProbeGrid,
                                                            measurement.PixelsWorld,
                                                            ProbeVisibility::DepthMap,
                                                            occluders);
            const ProbeLookup exact = AverageProbeLookup(atlases[caseIndex],
                                                         scene.ProbeGrid,
                                                         measurement.PixelsWorld,
                                                         ProbeVisibility::Exact,
                                                         occluders);
            result.DepthMap = depthMap.Irradiance;
            result.DistanceMoments = AverageProbeLookup(atlases[caseIndex],
                                                        scene.ProbeGrid,
                                                        measurement.PixelsWorld,
                                                        ProbeVisibility::DistanceMoments,
                                                        occluders)
                                         .Irradiance;
            result.Exact = exact.Irradiance;
            result.MinimumAdmitted = depthMap.AdmittedProbes;
            results[caseIndex].append(result);
            table += QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10/%11\n")
                         .arg(QString::fromUtf8(qualification.Name))
                         .arg(static_cast<float64>(result.PositionWorld.X), 0, 'f', 2)
                         .arg(static_cast<float64>(result.PositionWorld.Y), 0, 'f', 2)
                         .arg(static_cast<float64>(result.PositionWorld.Z), 0, 'f', 2)
                         .arg(result.Reference, 0, 'f', 4)
                         .arg(static_cast<float64>(result.Native), 0, 'f', 4)
                         .arg(static_cast<float64>(result.DepthMap), 0, 'f', 4)
                         .arg(static_cast<float64>(result.DistanceMoments), 0, 'f', 4)
                         .arg(static_cast<float64>(result.Exact), 0, 'f', 4)
                         .arg(depthMap.AdmittedProbes)
                         .arg(exact.AdmittedProbes);
        }
    }
    WARN(table.toStdString());

    // Irradiance tolerances are absolute, in the fixture's units: the lit half of the studied cell receives about
    // 0.4 in the reference. Native frames carry 8-bit sRGB quantization after tone mapping.
    constexpr float32 NATIVE_ABSOLUTE_TOLERANCE = .02F;
    constexpr float32 NATIVE_RELATIVE_TOLERANCE = .03F;
    constexpr float64 DARK_REFERENCE = .001;
    constexpr float64 LEAK_TOLERANCE = .01;
    const auto isNativeLookup = [](const ProbeReceiverResult& result) {
        return math::Abs(result.Native - result.DepthMap) <=
               NATIVE_ABSOLUTE_TOLERANCE + NATIVE_RELATIVE_TOLERANCE * result.DepthMap;
    };
    for (uint32 caseIndex = 0; caseIndex < CASE_COUNT; ++caseIndex)
    {
        for (const ProbeReceiverResult& result : results[caseIndex])
        {
            CAPTURE(cases[caseIndex].Name,
                    result.PositionWorld.X,
                    result.PositionWorld.Y,
                    result.Native,
                    result.DepthMap);
            // The native Lit shader evaluates the baked lookup that the CPU twin models.
            CHECK(isNativeLookup(result) == true);
            if (cases[caseIndex].Divider != ProbeDivider::EmbeddedLayers && result.Reference <= DARK_REFERENCE)
            {
                // With exact visibility the same probes and weights keep every closed-off receiver dark, so any
                // remaining leak belongs to the depth-map representation.
                CHECK(static_cast<float64>(result.Exact) <= result.Reference + LEAK_TOLERANCE);
            }
        }
    }
    // Closed walls stay dark away from the near field, including behind the diagonal wall that the 4 x 4 faces
    // leaked through (0.51 at x = 0.65).
    for (const uint32 caseIndex : {static_cast<uint32>(AXIS_CONTROL), static_cast<uint32>(DIAGONAL)})
    {
        for (const ProbeReceiverResult& result : results[caseIndex])
        {
            CAPTURE(cases[caseIndex].Name, result.PositionWorld.X, result.Native);
            if (result.Reference <= DARK_REFERENCE)
            {
                CHECK(static_cast<float64>(result.Native) <= result.Reference + LEAK_TOLERANCE);
            }
        }
    }
    // Trilinear interpolation over 1 m cells overestimates the lit side by about half; record a drift.
    constexpr float64 LIT_INTERPOLATION_RANGE = 2;
    for (const ProbeReceiverResult& result : results[AXIS_CONTROL])
    {
        if (result.Reference > DARK_REFERENCE)
        {
            CAPTURE(result.PositionWorld.X, result.Native, result.Reference);
            CHECK(static_cast<float64>(result.Native) >= result.Reference / LIT_INTERPOLATION_RANGE);
            CHECK(static_cast<float64>(result.Native) <= result.Reference * LIT_INTERPOLATION_RANGE);
        }
    }
    // Keep the measured 5 cm near-field leak well below the 8 x 8 baseline (0.157).
    constexpr float32 NEAR_FIELD_LEAK_BOUND = .04F;
    for (const ProbeReceiverResult& result : results[AXIS_NEAR_FIELD])
    {
        CAPTURE(result.PositionWorld.Y, result.Native);
        CHECK(result.Native <= NEAR_FIELD_LEAK_BOUND);
    }
    // Bounce through the opening survives just past the wall; the 4 x 4 faces kept 5 % of it at x = 0.55.
    constexpr qsizetype PAST_OPENING = 5;
    constexpr float64 RETAINED_FRACTION = .5;
    const ProbeReceiverResult& pastOpening = results[DOORWAY][PAST_OPENING];
    CAPTURE(pastOpening.Native, pastOpening.Reference);
    CHECK(static_cast<float64>(pastOpening.Native) >= RETAINED_FRACTION * pastOpening.Reference);
    constexpr qsizetype DARK_SIDE = PAST_OPENING + 1;
    const ProbeReceiverResult& darkSide = results[DOORWAY][DARK_SIDE];
    CAPTURE(darkSide.Native, darkSide.Reference);
    CHECK(static_cast<float64>(darkSide.Native) <= darkSide.Reference + LEAK_TOLERANCE);
    // Probe layers on the floor and ceiling invalidate every probe. Receivers then fall back to live lighting
    // with no probe contribution rather than a stale or undefined one.
    const WorldProbeAtlas& embedded = atlases[EMBEDDED];
    const rr::GiProbeGrid qualificationGrid = MakeProbeQualificationGrid();
    for (uint32 probe = 0;
         probe < qualificationGrid.Counts[0] * qualificationGrid.Counts[1] * qualificationGrid.Counts[2];
         ++probe)
    {
        CHECK(ReadProbeUnorm(embedded, probe, 0, PROBE_ALPHA_CHANNEL) == 0);
    }
    constexpr float32 FALLBACK_TOLERANCE = .001F;
    for (const ProbeReceiverResult& result : results[EMBEDDED])
    {
        CHECK(result.MinimumAdmitted == 0);
        CHECK(result.Native <= FALLBACK_TOLERANCE);
    }
    WARN("Reference bake (40 probes, one chart sample per triangle, 4 workers) per seed:\n" << bakeTimes.toStdString());

    // Receiver movement: one marked receiver steps through the doorway. Each step is a new World extraction.
    QString movement = QStringLiteral("x\tbake ms\tnative\tdepth map\n");
    const StaticArray<float32, 4> movementX{.25F, .45F, .55F, .75F};
    for (const float32 x : movementX)
    {
        const math::Vector3 receiverWorld = {x, PROBE_CELL_CENTRE_Y_METRES, 0};
        const RenderScene scene = MakeProbeQualificationScene(ProbeDivider::Doorway,
                                                              {receiverWorld},
                                                              QByteArrayLiteral("gi-probe-movement/") +
                                                                  QByteArray::number(static_cast<float64>(x)));
        QElapsedTimer timer;
        timer.start();
        REQUIRE(preview.SetScene(scene) == true);
        REQUIRE(WaitForProbeBake(preview, scene) == true);
        const int64 bakeMilliseconds = timer.elapsed();
        rr::ViewDescription view;
        const QImage image = ReadProbeFrame(preview, view);
        const ProbeReceiverMeasurement measurement =
            MeasureProbeReceiver(image, view, receiverWorld, scene.Definition->Eye);
        const ProbeLookup model = AverageProbeLookup(preview.GetSurfaceBake()->ProbeAtlas,
                                                     scene.ProbeGrid,
                                                     measurement.PixelsWorld,
                                                     ProbeVisibility::DepthMap,
                                                     MakeProbeOccluders(ProbeDivider::Doorway));
        // The moved receiver shows the same bounce as the static receiver at that position, without a stale frame.
        const qsizetype staticIndex = static_cast<qsizetype>(
            math::Floor(static_cast<float64>((x - cases[DOORWAY].FirstWorld.X) / PROBE_PATH_STEP_METRES) + .5));
        const ProbeReceiverResult& staticResult = results[DOORWAY][staticIndex];
        CAPTURE(x, measurement.Irradiance, staticResult.Native, model.Irradiance);
        CHECK(math::Abs(measurement.Irradiance - staticResult.Native) <=
              NATIVE_ABSOLUTE_TOLERANCE + NATIVE_RELATIVE_TOLERANCE * staticResult.Native);
        CHECK(math::Abs(measurement.Irradiance - model.Irradiance) <=
              NATIVE_ABSOLUTE_TOLERANCE + NATIVE_RELATIVE_TOLERANCE * model.Irradiance);
        movement += QStringLiteral("%1\t%2\t%3\t%4\n")
                        .arg(static_cast<float64>(x), 0, 'f', 2)
                        .arg(bakeMilliseconds)
                        .arg(static_cast<float64>(measurement.Irradiance), 0, 'f', 4)
                        .arg(static_cast<float64>(model.Irradiance), 0, 'f', 4);
    }
    WARN(movement.toStdString());

    // Cross the projected doorway edge laterally. Report errors against independent segments
    // before renormalization, so bright and dark probes cannot cancel each other's mistakes.
    constexpr float32 DOORWAY_SWEEP_X_METRES = .65F;
    constexpr float32 EDGE_SWEEP_START_Y_METRES = .91F;
    constexpr float32 EDGE_SWEEP_STEP_METRES = .02F;
    const ProbeOccluders doorway = MakeProbeOccluders(ProbeDivider::Doorway);
    QString sweep = QStringLiteral("y\tnative\texact\tfalse admission\tfalse rejection\t"
                                   "moment false admission\tmoment false rejection\n");
    for (uint32 i = 0; i < PROBE_PATH_RECEIVERS; ++i)
    {
        const float32 y = EDGE_SWEEP_START_Y_METRES + static_cast<float32>(i) * EDGE_SWEEP_STEP_METRES;
        const math::Vector3 receiverWorld = {DOORWAY_SWEEP_X_METRES, y, 0};
        const RenderScene scene =
            MakeProbeQualificationScene(ProbeDivider::Doorway,
                                        {receiverWorld},
                                        QByteArrayLiteral("gi-probe-edge/") + QByteArray::number(i));
        REQUIRE(preview.SetScene(scene) == true);
        REQUIRE(WaitForProbeBake(preview, scene) == true);
        rr::ViewDescription view;
        const QImage image = ReadProbeFrame(preview, view);
        const ProbeReceiverMeasurement measurement =
            MeasureProbeReceiver(image, view, receiverWorld, scene.Definition->Eye);
        const WorldProbeAtlas& atlas = preview.GetSurfaceBake()->ProbeAtlas;
        const ProbeLookup filtered =
            AverageProbeLookup(atlas, scene.ProbeGrid, measurement.PixelsWorld, ProbeVisibility::DepthMap, doorway);
        const ProbeLookup exact =
            AverageProbeLookup(atlas, scene.ProbeGrid, measurement.PixelsWorld, ProbeVisibility::Exact, doorway);
        const ProbeLookup moments = AverageProbeLookup(atlas,
                                                       scene.ProbeGrid,
                                                       measurement.PixelsWorld,
                                                       ProbeVisibility::DistanceMoments,
                                                       doorway);
        CAPTURE(y, measurement.Irradiance, filtered.Irradiance);
        CHECK(math::Abs(measurement.Irradiance - filtered.Irradiance) <=
              NATIVE_ABSOLUTE_TOLERANCE + NATIVE_RELATIVE_TOLERANCE * filtered.Irradiance);
        sweep += QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\n")
                     .arg(static_cast<float64>(y), 0, 'f', 2)
                     .arg(static_cast<float64>(measurement.Irradiance), 0, 'f', 4)
                     .arg(static_cast<float64>(exact.Irradiance), 0, 'f', 4)
                     .arg(static_cast<float64>(filtered.FalseAdmission), 0, 'f', 4)
                     .arg(static_cast<float64>(filtered.FalseRejection), 0, 'f', 4)
                     .arg(static_cast<float64>(moments.FalseAdmission), 0, 'f', 4)
                     .arg(static_cast<float64>(moments.FalseRejection), 0, 'f', 4);
    }
    WARN(sweep.toStdString());

    // Keep the 1 m transport reference fixed and replace only the probe grid used by the lookup.
    // Use exact segments for both grids to isolate the irradiance interpolation.
    QVector<math::Vector3> litReceiversWorld;
    for (const ProbeReceiverResult& result : results[AXIS_CONTROL])
    {
        if (result.Reference > DARK_REFERENCE)
        {
            litReceiversWorld.append(result.PositionWorld);
        }
    }
    RenderScene finerGrid = MakeProbeQualificationScene(ProbeDivider::AxisWall,
                                                        litReceiversWorld,
                                                        QByteArrayLiteral("gi-probe-finer-grid"));
    finerGrid.ProbeGrid.Spacing = .5F;
    constexpr float32 FINER_GRID_OFFSET_X_METRES = -.25F;
    finerGrid.ProbeGrid.OriginWorld.X += FINER_GRID_OFFSET_X_METRES;
    constexpr uint32 FINER_X_COUNT = 5;
    constexpr uint32 FINER_LAYER_COUNT = 3;
    finerGrid.ProbeGrid.Counts = {FINER_X_COUNT, FINER_LAYER_COUNT, FINER_LAYER_COUNT};
    REQUIRE(preview.SetScene(finerGrid) == true);
    REQUIRE(WaitForProbeBake(preview, finerGrid) == true);
    const ProbeOccluders wall = MakeProbeOccluders(ProbeDivider::AxisWall);
    QString spacing = QStringLiteral("x\ttransport reference\t1 m exact\t0.5 m offset exact\n");
    for (qsizetype i = 0; i < litReceiversWorld.size(); ++i)
    {
        const math::Vector3 receiverWorld = litReceiversWorld[i];
        const ProbeLookup coarse =
            EvaluateProbeLookup(atlases[AXIS_CONTROL], qualificationGrid, receiverWorld, ProbeVisibility::Exact, wall);
        const ProbeLookup finer = EvaluateProbeLookup(preview.GetSurfaceBake()->ProbeAtlas,
                                                      finerGrid.ProbeGrid,
                                                      receiverWorld,
                                                      ProbeVisibility::Exact,
                                                      wall);
        spacing += QStringLiteral("%1\t%2\t%3\t%4\n")
                       .arg(static_cast<float64>(receiverWorld.X), 0, 'f', 2)
                       .arg(results[AXIS_CONTROL][i].Reference, 0, 'f', 4)
                       .arg(static_cast<float64>(coarse.Irradiance), 0, 'f', 4)
                       .arg(static_cast<float64>(finer.Irradiance), 0, 'f', 4);
    }
    WARN(spacing.toStdString());

    // GPU frame cost with one receiver covering most of the view, against the same frame with the volume moved
    // away so every lookup returns before reading probes. The preview draws on demand, so small orbits drive frames.
    preview.SetStatisticsVisible(true);
    constexpr float32 COST_RECEIVER_SCALE = 30;
    const RenderScene costScene = MakeProbeQualificationScene(ProbeDivider::Doorway,
                                                              {{PROBE_WALL_X_METRES, PROBE_CELL_CENTRE_Y_METRES, 0}},
                                                              QByteArrayLiteral("gi-probe-cost"),
                                                              COST_RECEIVER_SCALE);
    const auto measureGpu = [&preview]() {
        constexpr uint32 TIMED_FRAMES = 120;
        constexpr float32 ORBIT_PIXELS = .5F;
        for (uint32 frame = 0; frame < TIMED_FRAMES; ++frame)
        {
            preview.Orbit(frame % 2 == 0 ? ORBIT_PIXELS : -ORBIT_PIXELS, 0);
            // Timing is evidence, not an acceptance gate: an occluded test window stops presentation.
            if (Frames(preview, preview.Frames() + 1) == false)
            {
                break;
            }
        }
        const ViewportStatistics& statistics = preview.GetStatistics();
        return QStringLiteral("%1 x %2: mean %3 ms, p95 %4 ms over %5 frames (state %6)")
            .arg(statistics.Width)
            .arg(statistics.Height)
            .arg(statistics.GpuMilliseconds.GetAverage(), 0, 'f', 3)
            .arg(statistics.GpuMilliseconds.GetPercentile95(), 0, 'f', 3)
            .arg(statistics.GpuMilliseconds.GetCount())
            .arg(static_cast<uint32>(statistics.GpuState));
    };
    REQUIRE(preview.SetScene(costScene) == true);
    REQUIRE(WaitForProbeBake(preview, costScene) == true);
    const QString withProbes = measureGpu();
    const qsizetype atlasBytes = preview.GetSurfaceBake()->ProbeAtlas.Pixels.size();
    rr::GiProbeGrid outsideGrid = costScene.ProbeGrid;
    constexpr float32 OUTSIDE_GRID_ORIGIN_X_METRES = 10;
    outsideGrid.OriginWorld.X = OUTSIDE_GRID_ORIGIN_X_METRES;
    preview.SetProbeGrid(outsideGrid);
    RenderScene outsideScene = costScene;
    outsideScene.ProbeGrid = outsideGrid;
    REQUIRE(WaitForProbeBake(preview, outsideScene) == true);
    const QString withoutProbes = measureGpu();
    WARN("GPU with probe lookups " << withProbes.toStdString() << "; outside the volume " << withoutProbes.toStdString()
                                   << "; atlas " << atlasBytes << " bytes");
    preview.Stop();
}

TEST_CASE("Force raster's default graph adds GI Probe Receivers' baked bounce like the native Lit viewport",
          "[.render-native][lighting][probe-gi]")
{
    namespace default_graph = ludus::render_graph_packages::editor_default_graph;
    const QString runtime = GetDefaultRenderGraphRuntime();
    REQUIRE(runtime.isEmpty() == false);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(640, 480);
    viewport.show();
    bool hasStarted = false;
    // The doorway admits bounce from both rooms; the closed wall keeps the dark half dark.
    for (const ProbeDivider divider : {ProbeDivider::AxisWall, ProbeDivider::Doorway})
    {
        QVector<math::Vector3> receiversWorld;
        for (uint32 i = 0; i < PROBE_PATH_RECEIVERS; ++i)
        {
            receiversWorld.append(
                {.05F + PROBE_PATH_STEP_METRES * static_cast<float32>(i), PROBE_CELL_CENTRE_Y_METRES, 0});
        }
        const RenderScene scene = MakeProbeQualificationScene(divider,
                                                              receiversWorld,
                                                              QByteArrayLiteral("gi-probe-graph/") +
                                                                  QByteArray::number(static_cast<uint32>(divider)));
        REQUIRE(preview.SetScene(scene) == true);
        if (hasStarted == false)
        {
            preview.SetShading(PreviewShading::Lit);
            REQUIRE(preview.Start() == true);
            hasStarted = true;
        }
        const bool hasBaked = WaitForProbeBake(preview, scene);
        INFO(preview.Status().toStdString());
        REQUIRE(hasBaked == true);
        rr::ViewDescription litView;
        const QImage lit = ReadProbeFrame(preview, litView);

        RenderGraphPreview graph(
            {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
            ludus::shaders::renderer_composite::Vertex(),
            ludus::shaders::renderer_composite::Fragment());
        REQUIRE(graph.Attach(preview.GetDevice(), preview.GetSurface()) == true);
        graph.SetReferenceSize(lit.size());
        rr::ViewDescription graphView;
        QString error;
        REQUIRE(preview.DescribeConventionalView(static_cast<uint32>(lit.width()),
                                                 static_cast<uint32>(lit.height()),
                                                 graphView,
                                                 error) == true);
        RenderGraphExtraction extraction;
        REQUIRE(MakeRenderGraphExtraction(preview.Scene(), graphView, 1, extraction, error, preview.GetSurfaceBake()) ==
                true);
        REQUIRE(extraction.ProbeReceiverIds.size() == receiversWorld.size());
        graph.SetExtraction(extraction);
        REQUIRE(graph.Load(runtime) == RenderGraphPreviewLoad::Staged);
        constexpr int32 GRAPH_DEADLINE_MILLISECONDS = 30000;
        constexpr int32 POLL_MILLISECONDS = 2;
        QElapsedTimer timer;
        timer.start();
        while (graph.GetPresentedGeneration() == 0 && timer.elapsed() < GRAPH_DEADLINE_MILLISECONDS)
        {
            graph.Tick();
            QApplication::processEvents();
            QTest::qWait(POLL_MILLISECONDS);
        }
        INFO(graph.GetDisplayStatus().toStdString());
        REQUIRE(graph.GetPresentedGeneration() != 0);
        CHECK(graph.GetProbeReceiverDrawCount() == static_cast<usize>(receiversWorld.size()));
        const QImage raster = graph.GetImage();
        REQUIRE(raster.size() == lit.size());
        rr::ViewDescription presented;
        REQUIRE(graph.TryGetPresentedView(presented) == true);
        for (const math::Vector3& receiverWorld : receiversWorld)
        {
            const float32 native = MeasureProbeReceiver(lit, litView, receiverWorld, scene.Definition->Eye).Irradiance;
            const float32 forced =
                MeasureProbeReceiver(raster, presented, receiverWorld, scene.Definition->Eye).Irradiance;
            CAPTURE(static_cast<uint32>(divider), receiverWorld.X, native, forced);
            // Both paths evaluate the same atlas lookup and diffuse term; only display quantization differs.
            constexpr float32 ABSOLUTE_TOLERANCE = .02F;
            constexpr float32 RELATIVE_TOLERANCE = .03F;
            CHECK(math::Abs(forced - native) <= ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * native);
        }
        graph.Detach();
    }
    preview.Stop();
}

namespace
{
constexpr float32 CHART_ROOM_HALF_WIDTH_METRES = 1.5F;
// The room stands above the editor's World grid at y = 0. A floor in that plane depth-fights the grid in both the
// native viewport and the graph, so a grid line can fall in a measured window at one resolution and not another:
// at device pixel ratio 1 (the hosted macOS runner) a floor point read 0.47 natively and 0.65 in the graph.
constexpr float32 CHART_ROOM_FLOOR_HEIGHT_METRES = .25F;
constexpr float32 CHART_LIGHT_INTENSITY = 3;
// The always-on bake stores one irradiance per triangle, evaluated at its centroid, so a two-triangle quad bakes as
// two flat halves split along its diagonal. Six cells per side give each surface 72 chart tiles over several chart
// rows, a stepped falloff and distinct values at the measured points.
constexpr uint32 CHART_ROOM_CELLS_PER_SIDE = 6;
// Adds a quad as cellsPerSide x cellsPerSide sub-quads, each split like AddProbeQuad.
void AddSubdividedQuad(ludus::resources::SceneMesh& inoutMesh, const ProbeQuad& quad, uint32 cellsPerSide)
{
    const float32 cellScale = 1.F / static_cast<float32>(cellsPerSide);
    const math::Vector3 cellEdgeU = quad.EdgeU * cellScale;
    const math::Vector3 cellEdgeV = quad.EdgeV * cellScale;
    for (uint32 v = 0; v < cellsPerSide; ++v)
    {
        for (uint32 u = 0; u < cellsPerSide; ++u)
        {
            const math::Vector3 cornerWorld =
                quad.CornerWorld + cellEdgeU * static_cast<float32>(u) + cellEdgeV * static_cast<float32>(v);
            AddProbeQuad(inoutMesh, {cornerWorld, cellEdgeU, cellEdgeV});
        }
    }
}
// A grey floor and back wall facing each other across their corner, lit by one always-on point light. Their charts
// carry each other's bounce, which realtime lighting omits, so a draw that ignores its chart reads differently.
RenderScene MakeSurfaceChartScene()
{
    using ludus::resources::SceneDefinition;
    using ludus::resources::SceneLight;
    SharedPtr<SceneDefinition> definition;
    REQUIRE(TryMakeShared<SceneDefinition>(GetSystemAllocationDomain(), definition) == OwnershipStatus::Ok);
    SceneDefinition& data = *definition;
    constexpr uint32 FLOOR = 0;
    constexpr uint32 WALL = 1;
    constexpr uint32 OBJECT_COUNT = WALL + 1;
    constexpr uint64 FIRST_SOURCE = 500;
    constexpr float32 HALF = CHART_ROOM_HALF_WIDTH_METRES;
    constexpr float32 SPAN = 2 * CHART_ROOM_HALF_WIDTH_METRES;
    constexpr float32 FLOOR_HEIGHT = CHART_ROOM_FLOOR_HEIGHT_METRES;
    REQUIRE(data.Meshes.TryResize(OBJECT_COUNT) == true);
    AddSubdividedQuad(data.Meshes[FLOOR],
                      {{-HALF, FLOOR_HEIGHT, -HALF}, {0, 0, SPAN}, {SPAN, 0, 0}},
                      CHART_ROOM_CELLS_PER_SIDE);
    AddSubdividedQuad(data.Meshes[WALL],
                      {{-HALF, FLOOR_HEIGHT, -HALF}, {SPAN, 0, 0}, {0, SPAN, 0}},
                      CHART_ROOM_CELLS_PER_SIDE);
    REQUIRE(data.Materials.TryResize(1) == true);
    REQUIRE(data.Objects.TryResize(OBJECT_COUNT) == true);
    for (uint32 index = 0; index < OBJECT_COUNT; ++index)
    {
        data.Objects[index].SourceId = FIRST_SOURCE + index;
        data.Objects[index].Mesh = index;
        data.Objects[index].Material = 0;
    }
    data.Eye = {0, 1.2F + FLOOR_HEIGHT, 3.5F};
    data.Target = {0, .6F + FLOOR_HEIGHT, -HALF};
    data.Up = {0, 1, 0};
    REQUIRE(data.Lights.TryResize(1) == true);
    SceneLight& light = data.Lights[0];
    light.SourceId = FIRST_SOURCE - 1;
    light.Kind = ludus::resources::SceneLightKind::Point;
    light.PositionWorld = {.4F, .9F + FLOOR_HEIGHT, -.6F};
    light.IntensityRgb = {CHART_LIGHT_INTENSITY, 0, 0};
    light.Range = 6;

    RenderScene scene;
    scene.LogicalId = QStringLiteral("level/surface-chart-comparison");
    scene.Identity = QByteArrayLiteral("surface-chart-comparison");
    scene.AssetsIdentity = QByteArrayLiteral("surface-chart-comparison-assets");
    scene.Lit = true;
    scene.StartView =
    {
        .EyeWorld = {static_cast<float64>(data.Eye.X),
                     static_cast<float64>(data.Eye.Y),
                     static_cast<float64>(data.Eye.Z)},
        .TargetWorld = {static_cast<float64>(data.Target.X),
                        static_cast<float64>(data.Target.Y),
                        static_cast<float64>(data.Target.Z)},
        .UpWorld = data.Up,
        .VerticalFov = data.VerticalFov,
    };
    scene.HasStartView = true;
    scene.Center = data.Target;
    scene.Radius = 3;
    scene.Meshes.resize(OBJECT_COUNT);
    scene.Surfaces.resize(1);
    scene.Surfaces[0].Tint = {PROBE_RECEIVER_ALBEDO, PROBE_RECEIVER_ALBEDO, PROBE_RECEIVER_ALBEDO, 1};
    scene.Surfaces[0].Roughness = 1;
    for (uint32 index = 0; index < OBJECT_COUNT; ++index)
    {
        PreviewObject object;
        object.Id = QString::number(data.Objects[index].SourceId);
        object.Transform = data.Objects[index].Transform;
        object.Mesh = static_cast<int32>(index);
        object.Surface = 0;
        scene.Objects.append(object);
    }
    QString error;
    REQUIRE(RenderScene::LoadCookedLights(data, scene.AuthoredLights, error) == true);
    scene.AlwaysOnLightIds.append(light.SourceId);
    scene.Definition = Move(definition);
    return scene;
}
// Scene-linear red radiance at a world point of a presented frame: the 3 x 3 pixels around its projection, decoded
// from sRGB and the viewport's PBR Neutral curve at exposure one.
float32 MeasureSceneRed(const QImage& image, const rr::ViewDescription& view, math::Vector3 pointWorld)
{
    const math::Vector2 centre = ProjectToPixel(view, image, pointWorld);
    const QPoint centrePixel(static_cast<int32>(centre.X), static_cast<int32>(centre.Y));
    constexpr int32 RADIUS_PIXELS = 1;
    float64 sum = 0;
    uint32 count = 0;
    for (int32 y = -RADIUS_PIXELS; y <= RADIUS_PIXELS; ++y)
    {
        for (int32 x = -RADIUS_PIXELS; x <= RADIUS_PIXELS; ++x)
        {
            const QPoint positionPixel = centrePixel + QPoint(x, y);
            REQUIRE(image.rect().contains(positionPixel) == true);
            const QColor color = image.pixelColor(positionPixel);
            const math::Vector3 display = {DecodeSrgb(static_cast<float64>(color.redF())),
                                           DecodeSrgb(static_cast<float64>(color.greenF())),
                                           DecodeSrgb(static_cast<float64>(color.blueF()))};
            sum += static_cast<float64>(rr::InvertPbrNeutral(display).X);
            ++count;
        }
    }
    return static_cast<float32>(sum / count);
}
} // namespace

TEST_CASE("Force raster's default graph draws static surfaces' baked charts like the native Lit viewport",
          "[.render-native][lighting][surface-chart]")
{
    namespace default_graph = ludus::render_graph_packages::editor_default_graph;
    const QString runtime = GetDefaultRenderGraphRuntime();
    REQUIRE(runtime.isEmpty() == false);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(640, 480);
    viewport.show();
    const RenderScene scene = MakeSurfaceChartScene();
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start() == true);
    const QByteArray key = preview_lighting::MakeKey(scene);
    const bool hasBaked = QTest::qWaitFor(
        [&]() {
            const QSharedPointer<const WorldSurfaceBake> bake = preview.GetSurfaceBake();
            return bake.isNull() == false && bake->LightingKey == key &&
                   preview.Status().contains(QStringLiteral("always-on lighting baked")) == true;
        },
        PROBE_BAKE_DEADLINE_MILLISECONDS);
    INFO(preview.Status().toStdString());
    REQUIRE(hasBaked == true);
    REQUIRE(Frames(preview, preview.Frames() + 3) == true);
    rr::ViewDescription litView;
    const QImage lit = ReadProbeFrame(preview, litView);

    // Renders the default graph for one extraction; a null bake draws every surface in realtime.
    const auto renderGraph =
        [&](const QSharedPointer<const WorldSurfaceBake>& bake, QImage& outImage, rr::ViewDescription& outView) {
            RenderGraphPreview graph(
                {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
                ludus::shaders::renderer_composite::Vertex(),
                ludus::shaders::renderer_composite::Fragment());
            REQUIRE(graph.Attach(preview.GetDevice(), preview.GetSurface()) == true);
            graph.SetReferenceSize(lit.size());
            rr::ViewDescription graphView;
            QString error;
            REQUIRE(preview.DescribeConventionalView(static_cast<uint32>(lit.width()),
                                                     static_cast<uint32>(lit.height()),
                                                     graphView,
                                                     error) == true);
            RenderGraphExtraction extraction;
            REQUIRE(MakeRenderGraphExtraction(preview.Scene(), graphView, 1, extraction, error, bake) == true);
            graph.SetExtraction(extraction);
            REQUIRE(graph.Load(runtime) == RenderGraphPreviewLoad::Staged);
            constexpr int32 GRAPH_DEADLINE_MILLISECONDS = 30000;
            constexpr int32 POLL_MILLISECONDS = 2;
            QElapsedTimer timer;
            timer.start();
            while (graph.GetPresentedGeneration() == 0 && timer.elapsed() < GRAPH_DEADLINE_MILLISECONDS)
            {
                graph.Tick();
                QApplication::processEvents();
                QTest::qWait(POLL_MILLISECONDS);
            }
            INFO(graph.GetDisplayStatus().toStdString());
            REQUIRE(graph.GetPresentedGeneration() != 0);
            const usize chartDraws = graph.GetSurfaceChartDrawCount();
            outImage = graph.GetImage();
            REQUIRE(outImage.size() == lit.size());
            REQUIRE(graph.TryGetPresentedView(outView) == true);
            graph.Detach();
            return chartDraws;
        };
    QImage baked;
    rr::ViewDescription bakedView;
    // Both static surfaces draw through the deferred chart G-buffer.
    CHECK(renderGraph(preview.GetSurfaceBake(), baked, bakedView) == static_cast<usize>(scene.Objects.size()));
    QImage realtime;
    rr::ViewDescription realtimeView;
    CHECK(renderGraph({}, realtime, realtimeView) == 0);

    // Points on the floor and the wall, nearer and farther from their shared corner. On each surface the first point
    // lies far from the light and a later one near it.
    constexpr float32 HALF = CHART_ROOM_HALF_WIDTH_METRES;
    constexpr float32 FLOOR_HEIGHT = CHART_ROOM_FLOOR_HEIGHT_METRES;
    constexpr usize POINT_COUNT = 6;
    const StaticArray<math::Vector3, POINT_COUNT> pointsWorld{
        math::Vector3{-.6F, FLOOR_HEIGHT, -.9F},
        {.2F, FLOOR_HEIGHT, -1.2F},
        {.8F, FLOOR_HEIGHT, -.3F},
        {-.6F, .3F + FLOOR_HEIGHT, -HALF},
        {.2F, .8F + FLOOR_HEIGHT, -HALF},
        {.8F, 1.4F + FLOOR_HEIGHT, -HALF},
    };
    // Both paths sample the same chart texels and evaluate the same diffuse and live specular terms; only display
    // quantization and half-float G-buffer storage differ.
    constexpr float32 ABSOLUTE_TOLERANCE = .02F;
    constexpr float32 RELATIVE_TOLERANCE = .03F;
    float32 largestRealtimeError = 0;
    QString table = QStringLiteral("x\ty\tz\tnative\tforced\trealtime\n");
    StaticArray<float32, POINT_COUNT> nativeRed{};
    StaticArray<float32, POINT_COUNT> realtimeRed{};
    for (usize i = 0; i < POINT_COUNT; ++i)
    {
        const math::Vector3& pointWorld = pointsWorld[i];
        const float32 native = MeasureSceneRed(lit, litView, pointWorld);
        const float32 forced = MeasureSceneRed(baked, bakedView, pointWorld);
        const float32 live = MeasureSceneRed(realtime, realtimeView, pointWorld);
        nativeRed[i] = native;
        realtimeRed[i] = live;
        CAPTURE(pointWorld.X, pointWorld.Y, pointWorld.Z, native, forced, live);
        CHECK(math::Abs(forced - native) <= ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * native);
        largestRealtimeError = math::Max(largestRealtimeError,
                                         math::Abs(live - native) / (ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * native));
        table += QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\n")
                     .arg(static_cast<float64>(pointWorld.X), 0, 'f', 2)
                     .arg(static_cast<float64>(pointWorld.Y), 0, 'f', 2)
                     .arg(static_cast<float64>(pointWorld.Z), 0, 'f', 2)
                     .arg(static_cast<float64>(native), 0, 'f', 4)
                     .arg(static_cast<float64>(forced), 0, 'f', 4)
                     .arg(static_cast<float64>(live), 0, 'f', 4);
    }
    WARN(table.toStdString());
    // Realtime lighting misses the bounce and the chart reconstruction, so this comparison discriminates.
    CHECK(largestRealtimeError > 1);
    // The bake follows the light's falloff: a chart that drew a two-triangle quad's flat halves, or read another
    // triangle's tile, would not separate these points as the realtime render does.
    constexpr usize FLOOR_FAR = 0;
    constexpr usize FLOOR_NEAR = 2;
    constexpr usize WALL_FAR = 3;
    constexpr usize WALL_NEAR = 4;
    constexpr float32 FALLOFF_RATIO = 2;
    CHECK(realtimeRed[FLOOR_NEAR] > FALLOFF_RATIO * realtimeRed[FLOOR_FAR]);
    CHECK(realtimeRed[WALL_NEAR] > FALLOFF_RATIO * realtimeRed[WALL_FAR]);
    CHECK(nativeRed[FLOOR_NEAR] > FALLOFF_RATIO * nativeRed[FLOOR_FAR]);
    CHECK(nativeRed[WALL_NEAR] > FALLOFF_RATIO * nativeRed[WALL_FAR]);
    preview.Stop();
}

TEST_CASE("Native Lit viewport accepts zero-area triangles without baking", "[.render-native][lighting]")
{
    RenderScene scene = MakeLightingScene(LightingReceiverDefect::ZeroAreaTriangle);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(320, 240);
    viewport.show();
    REQUIRE(preview.SetScene(scene));
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start());
    REQUIRE(Frames(preview, 2));
    INFO(preview.Status().toStdString());
    CHECK(preview.Status().contains(QStringLiteral("direct lighting")) == true);
    CHECK(preview.Status().contains(QStringLiteral("bake")) == false);
    const uint64 previous = preview.Frames();
    preview.Orbit(4, 2);
    REQUIRE(Frames(preview, previous + 2));
    CHECK(viewport.PresentedFrames() > 0);
    preview.Stop();
}
TEST_CASE("Native World-demo World renders its game extraction in the shared viewport and follows edits",
          "[.render-native]")
{
    REQUIRE(QApplication::platformName() == QStringLiteral("cocoa"));
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path =
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("room.world.json"));
    const std::string_view sample = ludus::world_demo::ExampleLevel();
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(sample.data(), static_cast<int64>(sample.size())) == static_cast<int64>(sample.size()));
    }
    WorldWorkspace workspace;
    workspace.resize(1200, 850);
    workspace.show();
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(path));
    RenderPreview& preview = workspace.GetRenderPreview();
    // The World's extraction loads on a worker; the panel publishes and starts it afterwards.
    QElapsedTimer published;
    published.start();
    while ((preview.Scene().Identity.isEmpty() == true || preview.Running() == false) && published.elapsed() < 10000)
    {
        QTest::qWait(20);
    }
    const bool hasPresented = Frames(preview, 3);
    INFO(preview.Status().toStdString());
    REQUIRE(hasPresented);
    const QString player = workspace.Document().FindEntityKey(QStringLiteral("player-start"));
    // Project an object's centre into the presented image through the current (game) camera.
    const auto locate = [&preview, &player](const QImage& image, QPoint& outPixel) {
        for (const PreviewObject& object : preview.Scene().Objects)
        {
            if (object.Id != player)
            {
                continue;
            }
            ludus::graphics::renderer::ViewDescription view;
            QString error;
            if (preview.DescribeView(static_cast<uint32>(image.width()),
                                     static_cast<uint32>(image.height()),
                                     view,
                                     error) == false)
            {
                return false;
            }
            const math::Vector3 centre = object.Transform.Translation;
            const math::Vector4 clip = view.WorldToClip * math::Vector4{centre.X, centre.Y, centre.Z, 1};
            const float32 x = (clip.X / clip.W * 0.5F + 0.5F) * static_cast<float32>(image.width());
            const float32 y = (0.5F - clip.Y / clip.W * 0.5F) * static_cast<float32>(image.height());
            outPixel = QPoint(static_cast<int32>(x), static_cast<int32>(y));
            return image.rect().contains(outPixel);
        }
        return false;
    };
    const QImage before = ReadViewport();
    REQUIRE_FALSE(before.isNull());
    if (qEnvironmentVariableIsSet("LUDUS_WORLD_DEMO_VIEWPORT_IMAGE") == true)
    {
        REQUIRE(before.save(qEnvironmentVariable("LUDUS_WORLD_DEMO_VIEWPORT_IMAGE")));
    }
    QPoint playerPixel;
    REQUIRE(locate(before, playerPixel));
    // The player's linear colour through the Renderer's display transform: an 8-bit linear target,
    // the viewport's tone curve (PBR Neutral), then sRGB (renderer_output.lusl). The player presents the same codes.
    const QColor drawn = before.pixelColor(playerPixel);
    math::Vector4 playerColor{};
    for (const PreviewObject& object : preview.Scene().Objects)
    {
        if (object.Id == player)
        {
            playerColor = object.Tint;
        }
    }
    const auto quantize = [](float32 linear) { return std::round(linear * 255.0F) / 255.0F; };
    const math::Vector3 mapped = ludus::graphics::renderer::ApplyToneCurve(
        VIEWPORT_TONE_CURVE,
        {quantize(playerColor.X), quantize(playerColor.Y), quantize(playerColor.Z)});
    const auto getDisplayCode = [](float32 linear) {
        const float32 encoded = linear <= 0.0031308F ? 12.92F * linear : 1.055F * std::pow(linear, 1 / 2.4F) - 0.055F;
        return static_cast<int32>(std::lround(encoded * 255.0F));
    };
    constexpr int32 DISPLAY_TOLERANCE = 3;
    CHECK(std::abs(drawn.red() - getDisplayCode(mapped.X)) <= DISPLAY_TOLERANCE);
    CHECK(std::abs(drawn.green() - getDisplayCode(mapped.Y)) <= DISPLAY_TOLERANCE);
    CHECK(std::abs(drawn.blue() - getDisplayCode(mapped.Z)) <= DISPLAY_TOLERANCE);
    CHECK(drawn != before.pixelColor(0, 0));

    // An applied World edit re-extracts, and the player leaves its old pixel.
    QTreeWidget* entitiesOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entitiesOrNull != nullptr);
    for (int32 row = 0; row < entitiesOrNull->topLevelItemCount(); ++row)
    {
        if (entitiesOrNull->topLevelItem(row)->data(0, Qt::UserRole).toString() == player)
        {
            entitiesOrNull->setCurrentItem(entitiesOrNull->topLevelItem(row), 0, QItemSelectionModel::ClearAndSelect);
        }
    }
    QLineEdit* x = workspace.findChild<QLineEdit*>(QStringLiteral("worldField:transform/x"));
    REQUIRE(x != nullptr);
    x->setFocus();
    x->selectAll();
    const QByteArray identityBeforeEdit = preview.Scene().Identity;
    QTest::keyClicks(x, QString::number(x->text().toDouble() + 2));
    REQUIRE(workspace.Apply());
    QElapsedTimer timer;
    timer.start();
    while (preview.Scene().Identity == identityBeforeEdit && timer.elapsed() < 10000)
    {
        QTest::qWait(20);
    }
    REQUIRE(Frames(preview, preview.Frames() + 3));
    const QImage after = ReadViewport();
    CHECK(after.pixelColor(playerPixel) != drawn);
    QPoint movedPixel;
    REQUIRE(locate(after, movedPixel));
    CHECK(movedPixel.x() > playerPixel.x());
    // Grid lines can cross the projected centre after a move. Inspect nearby interior pixels
    // against the same authored display codes instead of requiring the grid-covered texel.
    constexpr int32 PLAYER_SAMPLE_RADIUS_PIXEL = 2;
    constexpr int32 MINIMUM_PLAYER_PIXELS = 4;
    int32 playerPixels = 0;
    for (int32 verticalOffsetPixel = -PLAYER_SAMPLE_RADIUS_PIXEL; verticalOffsetPixel <= PLAYER_SAMPLE_RADIUS_PIXEL;
         ++verticalOffsetPixel)
    {
        for (int32 horizontalOffsetPixel = -PLAYER_SAMPLE_RADIUS_PIXEL;
             horizontalOffsetPixel <= PLAYER_SAMPLE_RADIUS_PIXEL;
             ++horizontalOffsetPixel)
        {
            const QPoint positionPixel = movedPixel + QPoint(horizontalOffsetPixel, verticalOffsetPixel);
            if (after.rect().contains(positionPixel) == false)
            {
                continue;
            }
            const QColor color = after.pixelColor(positionPixel);
            const bool isPlayerColor = std::abs(color.red() - getDisplayCode(mapped.X)) <= DISPLAY_TOLERANCE &&
                                       std::abs(color.green() - getDisplayCode(mapped.Y)) <= DISPLAY_TOLERANCE &&
                                       std::abs(color.blue() - getDisplayCode(mapped.Z)) <= DISPLAY_TOLERANCE;
            playerPixels += isPlayerColor == true ? 1 : 0;
        }
    }
    CHECK(playerPixels >= MINIMUM_PLAYER_PIXELS);
    workspace.StopPreview();
}

TEST_CASE("The Metal World viewport restarts diffuse bounce history after an emitter edit",
          "[.render-native][world-graph][hybrid-indirect-diffuse]")
{
    using namespace ludus::tests::hybrid_diffuse;
    namespace rhi = ludus::graphics::rhi;
    constexpr int32 DEADLINE_MILLISECONDS = 45000;
    constexpr int32 WIDTH = 640;
    constexpr int32 HEIGHT = 480;
    constexpr float64 EYE_DEPTH_WORLD = .05;
    constexpr float32 START_FOV = 1.35F;
    constexpr int32 NOISE_FLOOR = 3;
    constexpr uint64 MINIMUM_LIT_SIGNAL = 256;

    Room room;
    room.Apply(STATES[1]);
    RenderScene lit;
    lit.Identity = QByteArrayLiteral("diffuse-lit");
    lit.AssetsIdentity = QByteArrayLiteral("diffuse-room");
    lit.LogicalId = QStringLiteral("level/diffuse-room");
    lit.Lit = true;
    lit.HasStartView = true;
    lit.StartView.EyeWorld = {0, 0, EYE_DEPTH_WORLD};
    lit.StartView.TargetWorld = {0, 0, static_cast<float64>(room.Positions[RECEIVER][0].Z)};
    lit.StartView.VerticalFov = START_FOV;
    namespace resources = ludus::resources;
    SharedPtr<resources::SceneDefinition> definition;
    REQUIRE(TryMakeShared<resources::SceneDefinition>(GetSystemAllocationDomain(), definition) == OwnershipStatus::Ok);
    resources::SceneDefinition& data = *definition;
    REQUIRE(data.Meshes.TryResize(QUAD_COUNT) == true);
    REQUIRE(data.Materials.TryResize(QUAD_COUNT) == true);
    REQUIRE(data.Objects.TryResize(QUAD_COUNT) == true);
    data.Eye = {0, 0, static_cast<float32>(EYE_DEPTH_WORLD)};
    data.Target = room.Positions[RECEIVER][0];
    data.Target.X = 0;
    data.Target.Y = 0;
    data.Up = lit.StartView.UpWorld;
    data.VerticalFov = START_FOV;
    lit.Meshes.resize(QUAD_COUNT);
    for (usize quad = 0; quad < QUAD_COUNT; ++quad)
    {
        const renderer::MeshDescription& mesh = room.Meshes[quad].Geometry;
        const renderer::SceneShaderMaterial& material = room.Materials[quad];
        resources::SceneMesh& cookedMesh = data.Meshes[quad];
        REQUIRE(cookedMesh.Positions.TryAddRange({mesh.Positions, mesh.VertexCount}) == true);
        REQUIRE(cookedMesh.Normals.TryAddRange({mesh.Normals, mesh.VertexCount}) == true);
        REQUIRE(cookedMesh.Indices.TryAddRange({mesh.Indices, mesh.IndexCount}) == true);
        resources::SceneMaterial& cookedMaterial = data.Materials[quad];
        cookedMaterial.Reflectance = material.BaseColor;
        cookedMaterial.Scattering = material.Scattering;
        cookedMaterial.Emission = material.Emission;
        cookedMaterial.IsEmissionTwoSided = false;
        cookedMaterial.HasSourceMaterial = true;
        cookedMaterial.AreaSourceId = quad == EMITTER ? room.Items[quad].SourceId : 0;
        PreviewSurface surface;
        surface.Tint = {material.BaseColor.X, material.BaseColor.Y, material.BaseColor.Z, 1};
        surface.FlatTint = surface.Tint;
        surface.Scattering = material.Scattering;
        surface.Emission = material.Emission;
        lit.Surfaces.append(surface);
        resources::SceneObject& cookedObject = data.Objects[quad];
        cookedObject.Mesh = static_cast<uint32>(quad);
        cookedObject.Material = cookedObject.Mesh;
        cookedObject.SourceId = room.Items[quad].SourceId;
        cookedObject.Transform = room.Items[quad].MeshToWorld;
        PreviewObject object;
        object.Id = QString::number(cookedObject.SourceId);
        object.Mesh = static_cast<int32>(quad);
        object.Surface = object.Mesh;
        object.Transform = cookedObject.Transform;
        lit.Objects.append(object);
    }
    lit.Definition = Move(definition);

    rhi::Shutdown();
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({});
    RenderPreview source;
    PreviewViewport viewport(&source);
    viewport.resize(WIDTH, HEIGHT);
    viewport.show();
    viewport.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&source);
    workspace.EnableWorldRendering();
    workspace.SetProject(project.path(), 1);
    REQUIRE(source.SetScene(lit) == true);
    REQUIRE(source.Start() == true);
    RenderGraphPreview* graphOrNull = workspace.GetPreviewOrNull();
    REQUIRE(graphOrNull != nullptr);
    graphOrNull->SetSlowFrameMilliseconds(1000);
    graphOrNull->SetWorkLightEnabled(false);

    const auto waitFor = [&](const auto& isReady) {
        QElapsedTimer timer;
        timer.start();
        while (isReady() == false && timer.elapsed() < DEADLINE_MILLISECONDS)
        {
            QTest::qWait(1);
            workspace.ServicePreview();
        }
        return isReady();
    };
    REQUIRE(waitFor([&]() { return source.HasDevice(); }) == true);
    rhi::RayCapabilities capabilities;
    if (rhi::GetRayCapabilities(source.GetDevice(), capabilities) != rhi::RasterStatus::Ready)
    {
        SKIP("The native World viewport device has no ray queries");
    }
    const QString package = QStringLiteral(LUDUS_EDITOR_INDIRECT_DIFFUSE_PACKAGE);
    REQUIRE(graphOrNull->Load(package, GetDefaultRenderGraphRuntime()) == RenderGraphPreviewLoad::Staged);
    const bool hasPresentedInitial = waitFor([&]() {
        return graphOrNull->GetActivePackagePath() == package && graphOrNull->HasGpuFrame() == true &&
               graphOrNull->GetPresentedGeneration() == graphOrNull->GetReport().ActiveGeneration;
    });
    INFO(graphOrNull->GetDisplayStatus().toStdString());
    CAPTURE(graphOrNull->GetReport().ActiveGeneration,
            graphOrNull->GetReport().LatestSceneRevision,
            source.ShouldRenderPresentation());
    REQUIRE(hasPresentedInitial == true);
    REQUIRE(graphOrNull->GetReport().ActiveRenderer == renderer::RenderGraphRenderer::HardwareRayTracing);
    REQUIRE(&viewport.GetFrameSource() == &graphOrNull->GetDisplayFrames());
    const uint64 firstFrame = graphOrNull->GetDisplayFrames().GetLatestSerial();
    REQUIRE(firstFrame != 0);
    REQUIRE(waitFor([&]() { return viewport.GetPresentedGraphFrameSerial() >= firstFrame; }) == true);

    const auto measureRedSignal = []() {
        const QImage image = ReadViewport();
        REQUIRE(image.isNull() == false);
        constexpr int32 SAMPLE_STRIDE = 4;
        constexpr int32 CROP_DIVISOR = 4;
        const int32 marginX = image.width() / CROP_DIVISOR;
        const int32 marginY = image.height() / CROP_DIVISOR;
        uint64 signal = 0;
        for (int32 y = marginY; y < image.height() - marginY; y += SAMPLE_STRIDE)
        {
            for (int32 x = marginX; x < image.width() - marginX; x += SAMPLE_STRIDE)
            {
                const QRgb value = image.pixel(x, y);
                const int32 difference = qRed(value) - qBlue(value);
                if (difference > NOISE_FLOOR)
                {
                    signal += static_cast<uint64>(difference);
                }
            }
        }
        return signal;
    };
    const uint64 litRevision = graphOrNull->GetPresentedRevision();
    const uint64 litFrame = viewport.GetPresentedGraphFrameSerial();
    renderer::ViewDescription litView;
    REQUIRE(graphOrNull->TryGetPresentedView(litView) == true);
    const uint64 litSignal = measureRedSignal();
    REQUIRE(litSignal >= MINIMUM_LIT_SIGNAL);

    RenderScene dark = lit;
    dark.Identity = QByteArrayLiteral("diffuse-dark");
    constexpr float32 DIM_RADIANCE = .125F;
    dark.Surfaces[EMITTER].Emission = {DIM_RADIANCE, DIM_RADIANCE, DIM_RADIANCE};
    SharedPtr<resources::SceneDefinition> darkDefinition;
    REQUIRE(TryMakeShared<resources::SceneDefinition>(GetSystemAllocationDomain(), darkDefinition) ==
            OwnershipStatus::Ok);
    REQUIRE(lit.Definition->TryCopyTo(*darkDefinition) == resources::Status::Ok);
    darkDefinition->Materials[EMITTER].Emission = dark.Surfaces[EMITTER].Emission;
    dark.Definition = Move(darkDefinition);
    REQUIRE(source.SetScene(dark) == true);
    CHECK(graphOrNull->GetPresentedRevision() == litRevision);
    const bool hasPresentedEdit = waitFor([&]() { return graphOrNull->GetPresentedRevision() > litRevision; });
    INFO(graphOrNull->GetDisplayStatus().toStdString());
    INFO(workspace.GetScenePreparationProblem().toStdString());
    CAPTURE(source.ShouldRenderPresentation(),
            graphOrNull->HasPendingWork(),
            graphOrNull->GetReport().LatestSceneRevision,
            litRevision,
            graphOrNull->GetDisplayFrames().GetShownSize().width(),
            graphOrNull->GetDisplayFrames().GetShownSize().height());
    REQUIRE(hasPresentedEdit == true);
    const uint64 darkFrame = graphOrNull->GetDisplayFrames().GetLatestSerial();
    REQUIRE(darkFrame > litFrame);
    REQUIRE(waitFor([&]() { return viewport.GetPresentedGraphFrameSerial() >= darkFrame; }) == true);
    constexpr uint64 MAXIMUM_SIGNAL_FRACTION_DIVISOR = 4;
    CHECK(measureRedSignal() <= litSignal / MAXIMUM_SIGNAL_FRACTION_DIVISOR);
    renderer::ViewDescription darkView;
    REQUIRE(graphOrNull->TryGetPresentedView(darkView) == true);
    CHECK(darkView.WorldToClip == litView.WorldToClip);

    source.Stop();
    workspace.SetPreviewSource(nullptr);
    rhi::Shutdown();
}
