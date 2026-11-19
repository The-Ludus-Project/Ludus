// Replay the same World geometry and cameras as the relocated SDK against the World viewport producer.
#include "../../../tests/sdk_consumer/directional_near/doorway.hpp"
#include "internal/preview_viewport.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_device_description.h"
#include "internal/world_graph_view.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QThread>
#include <cstdio>

namespace
{
using namespace ludus::editor;
using namespace ludus::testing::directional;

RenderScene MakeDoorwayWorld()
{
    DoorwayGeometry geometry;
    StaticArray<renderer::RenderGraphMesh, 2> meshes{};
    geometry.Describe(meshes);
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("directional-near-doorway");
    scene.Lit = true;
    scene.Surfaces.append(PreviewSurface{});
    for (const renderer::RenderGraphMesh& record : meshes)
    {
        const renderer::MeshDescription& source = record.Geometry;
        PreviewMesh mesh;
        mesh.Id = QStringLiteral("mesh-%1").arg(scene.Meshes.size());
        mesh.Positions = QVector<math::Vector3>(source.Positions, source.Positions + source.VertexCount);
        mesh.Normals = QVector<math::Vector3>(source.Normals, source.Normals + source.VertexCount);
        mesh.Indices = QVector<uint32>(source.Indices, source.Indices + source.IndexCount);
        PreviewObject object;
        object.Id = mesh.Id;
        object.Mesh = static_cast<int32>(scene.Meshes.size());
        object.Surface = 0;
        scene.Meshes.append(mesh);
        scene.Objects.append(object);
    }
    scene.AuthoredLights.Count = 2;
    scene.AuthoredLights.Lights[0].SourceId = 9;
    scene.AuthoredLights.Lights[0].Direction = {-1, 0, -1};
    scene.AuthoredLights.Lights[0].Intensity = 2;
    scene.AuthoredLights.Lights[1] = scene.AuthoredLights.Lights[0];
    scene.AuthoredLights.Lights[1].SourceId = 1;
    scene.AuthoredLights.Lights[1].Direction = {1, 0, -1};
    return scene;
}
struct RasterSession final
{
    ~RasterSession()
    {
        rhi::Shutdown();
    }
};
} // namespace
TEST_CASE("The Raster World viewport resolves each directional doorway shadow", "[.directional-near-world]")
{
    const QString runtime = qEnvironmentVariable("LUDUS_DIRECTIONAL_GRAPH");
    REQUIRE(runtime.isEmpty() == false);
    QFile capture(qEnvironmentVariable("LUDUS_DIRECTIONAL_CAPTURE"));
    if (capture.fileName().isEmpty() == false)
    {
        REQUIRE(capture.open(QIODevice::WriteOnly) == true);
    }
    RasterSession session;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    REQUIRE(
        rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, MakeViewportDeviceDescription(), device, surface) ==
        rhi::DeviceStatus::Ready);
    const RenderScene scene = MakeDoorwayWorld();
    RendererWorkspace workspace({});
    REQUIRE(workspace.GetPreviewOrNull() != nullptr);
    RenderGraphPreview& preview = *workspace.GetPreviewOrNull();
    WorldGraphView display(workspace, nullptr);
    display.SetCanPresent(true);
    REQUIRE(preview.Attach(device, surface) == true);
    preview.SetDirectPresentation(false);
    preview.SetReferenceSize(QSize(WIDTH, HEIGHT));
    uint64 revision = 0;
    for (const float32 eyeOffset : {0.F, 0.01F, -3.F, 0.F})
    {
        for (const float32 y : {-1.F, 0.F, 1.F})
        {
            for (uint32 light = 0; light < scene.AuthoredLights.Count; ++light)
            {
                renderer::ViewDescription camera;
                AimCamera({eyeOffset, y - 3, 2}, {0, y, 0}, camera);
                camera.LightingView = renderer::DeferredLightingView::ShadowCoverage;
                camera.LightingViewSourceId = scene.AuthoredLights.Lights[light].SourceId;
                QString error;
                RenderGraphExtraction extraction;
                ++revision;
                REQUIRE(MakeRenderGraphExtraction(scene, camera, revision, extraction, error) == true);
                extraction.ContentRevision = 1;
                preview.SetExtraction(extraction);
                if (revision == 1)
                {
                    REQUIRE(preview.Load(runtime) == RenderGraphPreviewLoad::Staged);
                }
                QElapsedTimer timer;
                timer.start();
                while ((preview.GetPresentedRevision() != revision || preview.HasPendingWork() == true) &&
                       timer.elapsed() < 30000)
                {
                    preview.Tick();
                    REQUIRE(preview.GetReport().Candidate != renderer::RenderGraphCandidateState::Failed);
                    QThread::msleep(1);
                }
                INFO(preview.GetStatus().toStdString());
                REQUIRE(preview.GetPresentedRevision() == revision);
                REQUIRE(display.GetPresentedFrames() > 0);
                const InspectedTexture& texture = preview.GetOutputTexture();
                REQUIRE(texture.Encoding == TexelEncoding::Rgba8Unorm);
                REQUIRE(texture.Bytes.size() == WIDTH * HEIGHT * CHANNELS);
                constexpr usize CENTER = (HEIGHT / 2 * WIDTH + WIDTH / 2) * CHANNELS;
                const uint8 visible = static_cast<uint8>(texture.Bytes.at(CENTER));
                if (light == 0 && y != 0)
                {
                    CHECK(visible <= 12);
                }
                else
                {
                    CHECK(visible >= 243);
                }
                if (capture.isOpen() == true)
                {
                    REQUIRE(capture.write(texture.Bytes) == texture.Bytes.size());
                }
            }
        }
    }
    preview.Detach();
}

TEST_CASE("The native World viewport swaps new doorway frames", "[.directional-near-pacing]")
{
    REQUIRE(PreviewViewport::GetPresentation() == ViewportPresentation::SharedDevice);
    const QString runtime = qEnvironmentVariable("LUDUS_DIRECTIONAL_GRAPH");
    REQUIRE(runtime.isEmpty() == false);
    RendererWorkspace workspace({});
    RenderPreview source;
    PreviewViewport viewport(&source);
    viewport.UseProjectGraph(workspace);
    viewport.resize(480, 320);
    const RenderScene scene = MakeDoorwayWorld();
    // The viewport owns the display device. Its navigation proxy uses procedural geometry;
    // the graph below receives the full lit World and source identities on every update.
    RenderScene navigation = scene;
    navigation.Lit = false;
    navigation.Surfaces.clear();
    for (PreviewObject& object : navigation.Objects)
    {
        object.Surface = -1;
    }
    REQUIRE(source.SetScene(navigation) == true);
    viewport.show();
    viewport.raise();
    viewport.activateWindow();
    REQUIRE(source.Start() == true);
    const auto waitFor = [](auto predicate) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 15000)
        {
            if (predicate() == true)
            {
                return true;
            }
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        return predicate();
    };
    REQUIRE(waitFor([&]() { return source.HasDevice(); }) == true);
    QQuickWindow* windowOrNull = nullptr;
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport") && window->isExposed() == true)
        {
            windowOrNull = static_cast<QQuickWindow*>(window);
        }
    }
    REQUIRE(windowOrNull != nullptr);
    REQUIRE(workspace.GetPreviewOrNull() != nullptr);
    RenderGraphPreview& preview = *workspace.GetPreviewOrNull();
    REQUIRE(preview.Attach(source.GetDevice(), source.GetSurface()) == true);
    const QScopeGuard detachPreview([&]() { preview.Detach(); });
    preview.SetDirectPresentation(true);
    preview.SetReferenceSize(QSize(WIDTH, HEIGHT));
    QElapsedTimer clock;
    clock.start();
    uint64 lastSwapSerial = 0;
    qint64 lastSwapTicks = 0;
    const QMetaObject::Connection swapConnection = QObject::connect(
        windowOrNull,
        &QQuickWindow::frameSwapped,
        &viewport,
        [&]() {
            const uint64 serial = viewport.GetPresentedGraphFrameSerial();
            if (serial > lastSwapSerial)
            {
                lastSwapSerial = serial;
                lastSwapTicks = clock.nsecsElapsed();
            }
        },
        Qt::DirectConnection);
    const QScopeGuard disconnectSwap([&]() { QObject::disconnect(swapConnection); });
    qint64 previous = 0;
    renderer::ViewDescription camera;
    AimCamera({0, -3, 2}, {0, 0, 0}, camera);
    camera.LightingView = renderer::DeferredLightingView::ShadowCoverage;
    camera.LightingViewSourceId = scene.AuthoredLights.Lights[0].SourceId;
    constexpr uint64 WARMUP_FRAMES = 2;
    constexpr uint64 MEASURED_FRAMES = 64;
    for (uint64 revision = 1; revision <= WARMUP_FRAMES + MEASURED_FRAMES; ++revision)
    {
        QString error;
        RenderGraphExtraction extraction;
        REQUIRE(MakeRenderGraphExtraction(scene, camera, revision, extraction, error) == true);
        extraction.ContentRevision = 1;
        preview.SetExtraction(extraction);
        if (revision == 1)
        {
            REQUIRE(preview.Load(runtime) == RenderGraphPreviewLoad::Staged);
        }
        const uint64 previousSerial = lastSwapSerial;
        const bool hasSwapped = waitFor([&]() {
            if (preview.GetPresentedRevision() != revision)
            {
                preview.Tick();
            }
            if (preview.GetPresentedRevision() == revision &&
                lastSwapSerial != preview.GetDisplayFrames().GetLatestSerial())
            {
                viewport.ShowFrame();
                windowOrNull->update();
            }
            return preview.GetPresentedRevision() == revision && lastSwapSerial > previousSerial &&
                   lastSwapSerial == preview.GetDisplayFrames().GetLatestSerial();
        });
        INFO(preview.GetStatus().toStdString());
        CAPTURE(revision,
                lastSwapSerial,
                preview.GetDisplayFrames().GetLatestSerial(),
                viewport.GetPresentedGraphFrameSerial(),
                viewport.PresentedFrames(),
                source.Status().toStdString());
        REQUIRE(hasSwapped == true);
        REQUIRE(preview.HasGpuFrame() == true);
        REQUIRE(preview.GetTargetSize() == QSize(WIDTH, HEIGHT));
        if (revision > WARMUP_FRAMES)
        {
            std::printf("swap,%llu,%lld\n",
                        static_cast<unsigned long long>(revision),
                        static_cast<long long>(lastSwapTicks - previous));
        }
        previous = lastSwapTicks;
    }
}
