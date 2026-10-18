#include "internal/level_workspace.h"
#include "internal/preview_lighting.h"
#include "internal/preview_shading.h"
#include "internal/preview_viewport.h"
#include "internal/readback.h"
#include "internal/render_preview.h"
#include "internal/world_workspace.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include <QQuickWindow>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/platform/entropy.h>

#import <Cocoa/Cocoa.h>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QSize>
#include <QTemporaryDir>
#include <QTest>

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
        QElapsedTimer bakeTimer;
        bakeTimer.start();
        while (bakeTimer.elapsed() < 300000 &&
               sceneWorkspace->StatusText().contains(QStringLiteral("static diffuse baked")) == false &&
               sceneWorkspace->StatusText().contains(QStringLiteral("bake failed")) == false)
        {
            QTest::qWait(20);
        }
        INFO(sceneWorkspace->StatusText().toStdString());
        REQUIRE(sceneWorkspace->StatusText().contains(QStringLiteral("static diffuse baked")));
        REQUIRE(awaitPresentation(viewport->PresentedFrames() + 2));
        const QImage baked = ReadViewport();
        REQUIRE(baked.isNull() == false);
        if (imagePrefix.isEmpty() == false)
        {
            REQUIRE(baked.save(imagePrefix + QStringLiteral("baked.png")));
        }
        const QColor background = baked.pixelColor(0, 0);
        uint32 litPixels = 0;
        for (int32 y = 0; y < baked.height(); y += 4)
        {
            for (int32 x = 0; x < baked.width(); x += 4)
            {
                const QColor color = baked.pixelColor(x, y);
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
    CHECK(preview.Report().Draws == 1);
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
    CHECK(preview.Report().Draws == 1);
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
    CHECK(embedded.pixelColor(embedded.width() / 2, embedded.height() / 2) == embedded.pixelColor(0, 0));
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
RenderScene MakeLightingScene()
{
    RenderScene scene;
    scene.Definition = QSharedPointer<ludus::resources::SceneDefinition>::create();
    scene.Identity = QByteArrayLiteral("lighting-fixture");
    scene.AssetsIdentity = QByteArrayLiteral("lighting-assets");
    scene.LogicalId = QStringLiteral("level/lighting-fixture");
    scene.Lit = true;
    scene.Center = {0, .5F, 0};
    scene.Radius = 4;
    ludus::resources::SceneDefinition& data = *scene.Definition;
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
} // namespace

TEST_CASE("Native Lit viewport publishes thirteen shadowed area patches and per-instance baked charts",
          "[.render-native][lighting]")
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
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 40000 && preview.Status().contains(QStringLiteral("static diffuse baked")) == false)
    {
        QTest::qWait(20);
    }
    INFO(preview.Status().toStdString());
    REQUIRE(preview.Status().contains(QStringLiteral("13 planar area proxies / 78 shadow faces")));
    REQUIRE(preview.Status().contains(QStringLiteral("static diffuse baked")));
    REQUIRE(Frames(preview, beforeCamera + 2));
    REQUIRE(ReadViewport().isNull() == false);
    preview.Stop();
}
TEST_CASE("Native Lit bake failure retains direct lighting and live camera rendering", "[.render-native][lighting]")
{
    RenderScene scene = MakeLightingScene();
    // GPU raster rebuilds the receiver's bake atlas UVs from its chart layout, so it
    // ignores these authored ones; static transport rejects UVs that disagree with
    // that layout (Unsupported) and must not retire the direct snapshot. (0, 0) is
    // never an interior texel centre, so every corner disagrees.
    ludus::resources::SceneMesh& receiver = scene.Definition->Meshes[0];
    REQUIRE(receiver.BakeTexcoords.TryResize(receiver.Positions.GetSize()));
    for (math::Vector2& textureCoordinateBakeAtlas : receiver.BakeTexcoords)
    {
        textureCoordinateBakeAtlas = {0, 0};
    }
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(320, 240);
    viewport.show();
    REQUIRE(preview.SetScene(scene));
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start());
    REQUIRE(Frames(preview, 2));
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000 &&
           preview.Status().contains(QStringLiteral("bake failed; direct lighting retained")) == false)
    {
        QTest::qWait(20);
    }
    INFO(preview.Status().toStdString());
    REQUIRE(preview.Status().contains(QStringLiteral("bake failed; direct lighting retained")));
    CHECK(preview.GetBakeState() == PreviewBakeState::Failed);
    const uint64 previous = preview.Frames();
    preview.Orbit(4, 2);
    REQUIRE(Frames(preview, previous + 2));
    CHECK(viewport.PresentedFrames() > 0);
    preview.Stop();
}
TEST_CASE("Native Lit bake skips a zero-area triangle and reports it", "[.render-native][lighting]")
{
    RenderScene scene = MakeLightingScene();
    // GPU raster accepts a zero-area receiver primitive; static transport skips it,
    // still publishes the bake and reports the skipped triangle.
    const uint32 degenerate[] = {0, 0, 0};
    REQUIRE(scene.Definition->Meshes[0].Indices.TryAddRange(degenerate));
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(320, 240);
    viewport.show();
    REQUIRE(preview.SetScene(scene));
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start());
    REQUIRE(Frames(preview, 2));
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 40000 && preview.Status().contains(QStringLiteral("static diffuse baked")) == false)
    {
        QTest::qWait(20);
    }
    INFO(preview.Status().toStdString());
    REQUIRE(preview.Status().contains(QStringLiteral("static diffuse baked")));
    CHECK(preview.Status().contains(QStringLiteral("skipped 1 zero-area triangles in 1 object")));
    CHECK(preview.GetBakeState() == PreviewBakeState::Baked);
    PreviewBakeNotice notice;
    REQUIRE(preview.GetBakeNotice(notice));
    CHECK(notice.SkippedTriangleCount == 1);
    CHECK(notice.AffectedObjectCount == 1);
    const uint64 previous = preview.Frames();
    preview.Orbit(4, 2);
    REQUIRE(Frames(preview, previous + 2));
    CHECK(viewport.PresentedFrames() > 0);
    preview.Stop();
}
