// Replay the same World geometry and cameras as the relocated SDK against the World viewport producer.
#include "../../../tests/sdk_consumer/directional_near/doorway.hpp"
#include "internal/render_graph_preview.h"
#include "internal/render_scene.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_device_description.h"
#include "internal/world_graph_view.h"

#include <QElapsedTimer>
#include <QFile>
#include <QThread>

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
