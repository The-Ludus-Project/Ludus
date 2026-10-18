#include "internal/preview_gpu_generation.h"
#include "internal/preview_shading.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"

#include <ludus/graphics/renderer/viewport.hpp>

#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/rhi.h>

#include <QByteArray>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;

namespace
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
constexpr int32 PREVIEW_TIMEOUT_MILLISECONDS = 30000;
constexpr int32 PREVIEW_PIXELS = 64;

RenderScene MakeScene(const QByteArray& identity)
{
    RenderScene scene;
    scene.Identity = identity;
    scene.AssetsIdentity = identity;
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Uvs = {{0, 0}, {1, 0}, {.5F, 1}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("triangle");
    scene.Objects.append(object);
    return scene;
}

bool AwaitFrame(RenderPreview& inoutPreview)
{
    return QTest::qWaitFor([&inoutPreview]() { return inoutPreview.HasPendingFrame(); }, PREVIEW_TIMEOUT_MILLISECONDS);
}
#endif
} // namespace

TEST_CASE("Preview preparation failure is terminal for its resource generation", "[editor][preview-generation]")
{
    const RenderScene scene;
    PreviewGpuGeneration generation({}, scene, PreviewShading::Solid);
    QString error;
    CHECK(generation.IsPrepared() == false);
    CHECK(generation.Prepare({}, error) == PreviewPreparationStatus::Failed);
    CHECK(error.contains(QStringLiteral("GPU renderer initialization failed")) == true);
    CHECK(generation.Prepare({}, error) == PreviewPreparationStatus::Failed);
    CHECK(generation.IsPrepared() == false);
}

TEST_CASE("Preview replacement retains leased output through pending failure and publication",
          "[editor][preview-generation][gpu]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    rhi::Shutdown();
    RenderPreview preview;
    QSignalSpy failures(&preview, &RenderPreview::Failed);
    bool isAdmitted = false;
    QObject::connect(&preview, &RenderPreview::DeviceRequested, &preview, [&preview, &isAdmitted]() {
        isAdmitted = preview.AdmitOffscreenDevice();
    });
    preview.Resize(PREVIEW_PIXELS, PREVIEW_PIXELS);
    const RenderScene firstScene = MakeScene(QByteArrayLiteral("first"));
    REQUIRE(preview.SetScene(firstScene) == true);
    const bool hasStarted = preview.Start();
    if (isAdmitted == false)
    {
        SKIP("No headless Vulkan device; install lavapipe to exercise GPU generation ownership");
    }
    REQUIRE(hasStarted == true);
    // A metadata-only replacement may arrive before the first generation exists.
    RenderScene latestScene = firstScene;
    latestScene.Identity = QByteArrayLiteral("latest-before-preparation");
    REQUIRE(preview.SetScene(latestScene) == true);
    INFO(preview.Status().toStdString());
    REQUIRE(AwaitFrame(preview) == true);
    renderer::ViewportFrame previousFrame;
    renderer::ViewportImage previousImage;
    REQUIRE(preview.TakeFrame(previousFrame, previousImage) == true);
    REQUIRE(rhi::GetStatus(preview.GetDevice(), previousImage.Texture) == rhi::RasterStatus::Ready);

    // This passes snapshot admission but fails GPU mesh creation after staging.
    RenderScene rejected = MakeScene(QByteArrayLiteral("rejected"));
    rejected.Meshes[0].Indices.clear();
    REQUIRE(preview.SetScene(rejected) == true);
    preview.Tick();
    CHECK(preview.FrameAvailable() == true);
    CHECK(rhi::GetStatus(preview.GetDevice(), previousImage.Texture) == rhi::RasterStatus::Ready);
    REQUIRE(QTest::qWaitFor([&failures]() { return failures.count() != 0; }, PREVIEW_TIMEOUT_MILLISECONDS) == true);
    CHECK(preview.Running() == true);
    CHECK(preview.FrameAvailable() == true);
    CHECK(preview.HasPendingFrame() == false);
    CHECK(rhi::GetStatus(preview.GetDevice(), previousImage.Texture) == rhi::RasterStatus::Ready);

    REQUIRE(preview.SetScene(MakeScene(QByteArrayLiteral("replacement"))) == true);
    REQUIRE(AwaitFrame(preview) == true);
    renderer::ViewportFrame replacementFrame;
    renderer::ViewportImage replacementImage;
    REQUIRE(preview.TakeFrame(replacementFrame, replacementImage) == true);
    CHECK(failures.count() == 1);
    CHECK(rhi::GetStatus(preview.GetDevice(), previousImage.Texture) == rhi::RasterStatus::Ready);
    CHECK(rhi::GetStatus(preview.GetDevice(), replacementImage.Texture) == rhi::RasterStatus::Ready);
    preview.ReleaseFrame(previousFrame);
    CHECK(rhi::GetStatus(preview.GetDevice(), previousImage.Texture) == rhi::RasterStatus::InvalidHandle);
    CHECK(rhi::GetStatus(preview.GetDevice(), replacementImage.Texture) == rhi::RasterStatus::Ready);
    preview.ReleaseFrame(replacementFrame);
    preview.Stop();
    CHECK(preview.FrameAvailable() == false);
    CHECK(preview.Running() == false);
#else
    SKIP("Headless preview generation regression requires the native Linux preview");
#endif
}

TEST_CASE("Preview publication permits a receiver to stop the resource owner", "[editor][preview-generation][gpu]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    rhi::Shutdown();
    RenderPreview preview;
    bool isAdmitted = false;
    bool hasPublished = false;
    QObject::connect(&preview, &RenderPreview::DeviceRequested, &preview, [&preview, &isAdmitted]() {
        isAdmitted = preview.AdmitOffscreenDevice();
    });
    QObject::connect(&preview, &RenderPreview::FrameReady, &preview, [&preview, &hasPublished]() {
        hasPublished = true;
        preview.Stop();
    });
    preview.Resize(PREVIEW_PIXELS, PREVIEW_PIXELS);
    REQUIRE(preview.SetScene(MakeScene(QByteArrayLiteral("stop-on-publication"))) == true);
    const bool hasStarted = preview.Start();
    if (isAdmitted == false)
    {
        SKIP("No headless Vulkan device; install lavapipe to exercise publication shutdown");
    }
    REQUIRE(hasStarted == true);
    REQUIRE(QTest::qWaitFor([&hasPublished]() { return hasPublished; }, PREVIEW_TIMEOUT_MILLISECONDS) == true);
    CHECK(preview.Running() == false);
    CHECK(preview.HasPendingFrame() == false);
    preview.Tick();
    CHECK(preview.FrameAvailable() == false);
#else
    SKIP("Headless preview generation regression requires the native Linux preview");
#endif
}
