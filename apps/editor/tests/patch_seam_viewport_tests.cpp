#include "fixture.h"
#include "internal/render_scene.h"
#include "internal/viewport_raster_capture.h"

#include <ludus/graphics/rhi/rhi.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::qa::patch;
using namespace ludus::editor;
namespace rhi = ludus::graphics::rhi;

RenderScene MakeScene(const geom::SurfaceMeshView& mesh)
{
    RenderScene scene;
    PreviewMesh model;
    model.Id = QStringLiteral("joined");
    for (const geom::SurfaceMeshVertex& vertex : mesh.Vertices)
    {
        model.Positions.append(vertex.Position);
        model.Normals.append(vertex.Normal);
    }
    for (const uint32 index : mesh.Indices)
    {
        model.Indices.append(index);
    }
    scene.Meshes.append(model);
    PreviewSurface surface;
    surface.Emission = {1, 1, 1};
    scene.Surfaces.append(surface);
    PreviewObject object;
    object.Id = model.Id;
    object.Surface = 0;
    scene.Objects.append(object);
    scene.Identity = "joined";
    return scene;
}

void CaptureViewport(rhi::DeviceHandle device,
                     rhi::SurfaceHandle target,
                     RenderScene& inoutScene,
                     const Camera& camera,
                     uint32 size,
                     const QString& path)
{
    inoutScene.Objects[0].Transform.Columns[0].X = camera.Scale.X;
    inoutScene.Objects[0].Transform.Columns[1].Y = camera.Scale.Y;
    inoutScene.Objects[0].Transform.Columns[2].Z = camera.Scale.Z;
    ViewportRasterSource source;
    source.PackagePath = QStringLiteral(LUDUS_PATCH_SEAM_PACKAGE);
    ViewportRasterCapture capture;
    QString error;
    QSize extent;
    const QSize requested(static_cast<int>(size), static_cast<int>(size));
    INFO(path.toStdString());
    const bool isLoaded = capture.Load(device, target, source, requested, extent, error);
    INFO(error.toStdString());
    REQUIRE(isLoaded == true);
    REQUIRE(extent == requested);
    const bool hasStarted = capture.Begin(inoutScene, MakeView(camera), error);
    INFO(error.toStdString());
    REQUIRE(hasStarted == true);
    QElapsedTimer timer;
    timer.start();
    while (capture.Service() == ViewportRasterPhase::Rendering && timer.elapsed() < 30000)
    {
        QThread::usleep(200);
    }
    INFO(capture.GetError().toStdString());
    REQUIRE(capture.GetPhase() == ViewportRasterPhase::Complete);
    const InspectedTexture& texture = capture.GetTexture();
    REQUIRE(texture.Encoding == TexelEncoding::Rgba8Unorm);
    QFile output(path);
    REQUIRE(output.open(QIODevice::WriteOnly) == true);
    REQUIRE(output.write(texture.Bytes) == texture.Bytes.size());
}
} // namespace

TEST_CASE("The World viewport presents the cooked patch seam at both capture scales",
          "[editor][gpu][patch-seam-capture]")
{
    rhi::Shutdown();
    rhi::DeviceHandle device;
    rhi::SurfaceHandle target;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::StartupInfo failure;
    if (rhi::CreateDevice({}, { .Width = 1, .Height = 1 }, description, device, target, &failure) !=
        rhi::DeviceStatus::Ready)
    {
        REQUIRE(failure.Error == rhi::StartupError::AdapterUnavailable);
        SKIP("No native raster adapter");
    }
    QTemporaryDir root;
    REQUIRE(root.isValid() == true);
    for (const Case& fixture : CASES)
    {
        for (usize level = 0; level < std::size(ERRORS); ++level)
        {
            const QString directory =
                root.filePath(QStringLiteral("%1-%2").arg(QString::fromLatin1(fixture.Name)).arg(level));
            REQUIRE(QDir().mkpath(directory) == true);
            geom::SurfaceMesh mesh;
            REQUIRE(BuildFixture(fixture, ERRORS[level], mesh, directory.toStdString()) == true);
            RenderScene scene = MakeScene(mesh.GetView());
            for (const uint32 size : EXTENTS)
            {
                for (usize i = 0; i < GetCameraCount(fixture.Surface); ++i)
                {
                    CaptureViewport(device,
                                    target,
                                    scene,
                                    CAMERAS[i],
                                    size,
                                    directory + QStringLiteral("/%1-%2.rgba").arg(size).arg(i));
                }
            }
        }
    }
    REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
    rhi::Shutdown();
    QProcess verify;
    verify.setProcessChannelMode(QProcess::MergedChannels);
    verify.start(QStringLiteral(LUDUS_PATCH_SEAM_PYTHON),
                 {QStringLiteral(LUDUS_PATCH_SEAM_VERIFY), QStringLiteral("raster"), root.path()});
    REQUIRE(verify.waitForFinished(120000) == true);
    INFO(verify.readAll().toStdString());
    REQUIRE(verify.exitStatus() == QProcess::NormalExit);
    REQUIRE(verify.exitCode() == 0);
}
