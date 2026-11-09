#include "internal/level.h"
#include "internal/level_workspace.h"
#include "internal/preview_gpu_generation.h"
#include "internal/preview_lighting.h"
#include "internal/preview_shading.h"
#include "internal/preview_viewport.h"
#include "internal/readback.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/viewport_object_interaction.h"
#include "internal/viewport_reference_capture.h"
#include "internal/world_surface_bake.h"
#include "internal/world_workspace.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include <QQuickWindow>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/owned_object.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/platform/entropy.h>

#import <Cocoa/Cocoa.h>

#include <cmath>

#include <QApplication>
#include <QComboBox>
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
    CHECK(saved->ProbeAtlas.Pixels.isEmpty() == true);
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(litRoom, {}, 1, extraction, error, saved) == true);
    REQUIRE(extraction.SurfaceCharts.size() == litRoom.Objects.size());
    CHECK(extraction.SurfaceCharts.first().SourceId == extraction.Items.first().SourceId);
    CHECK(extraction.SurfaceCharts.last().SourceId == 0);
    REQUIRE(QTest::qWaitFor(
                [&]() {
                    reopened.Poll(litRoom);
                    return reopened.GetReady()->ProbeAtlas.Pixels.isEmpty() == false;
                },
                BAKE_DEADLINE_MILLISECONDS) == true);
    const RenderScene darkRoom = MakeGiProbeScene(.75F);
    REQUIRE(preview.SetScene(darkRoom) == true);
    const QColor dark = sampleReceiver();
    CAPTURE(lit.red(), lit.green(), dark.red(), dark.green());
    CHECK(lit.red() > lit.green() + 15);
    CHECK(dark.red() <= dark.green() + 3);
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
    // the viewport's tone curve (PBR Neutral), then sRGB (renderer_output.slang). The player presents the same codes.
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
