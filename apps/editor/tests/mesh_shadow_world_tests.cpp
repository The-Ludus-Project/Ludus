// The same saved World's assembled scene and viewport camera feed the installed SDK consumer.
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_device_description.h"
#include "internal/world_graph_view.h"
#include "scene_fixture.h"
#include "viewport_fixture.h"

#include <QElapsedTimer>
#include <QTextStream>
#include <QThread>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace cook = ludus::content::scene_cook;

void WriteFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    REQUIRE(file.flush() == true);
}

RenderScene PrepareWorld(const QDir& directory)
{
    const QString levelName = QStringLiteral("input.lulevel");
    QString error;
    const bool isImported = tests::PrepareImportedLevel(directory.path(),
                                                        levelName,
                                                        directory.filePath(QStringLiteral("source")),
                                                        error,
                                                        "shadow.luscene.json");
    INFO(error.toStdString());
    REQUIRE(isImported == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(
                QByteArrayLiteral(
                    R"({"version":1,"id":"ludus.world.default.v1","label":"World","components":[],"assets":[]})"),
                error) == true);
    REQUIRE(world.Load(QByteArrayLiteral(R"({"version":1,"schema":"ludus.world.default.v1","entities":[]})"), error) ==
            true);
    REQUIRE(world.ImportScene(QJsonDocument::fromJson(tests::ReadTestFile(directory.filePath(levelName))).object(),
                              error) == true);
    REQUIRE(directory.mkpath(QStringLiteral("assets")) == true);
    const QString path = directory.filePath(QStringLiteral("assets/shadow.world.json"));
    WriteFile(path, world.Encode());
    REQUIRE(world.Load(tests::ReadTestFile(path), error) == true);
    const QJsonObject level = world.SceneLevel();
    const QByteArray encoded = QJsonDocument(level).toJson();
    const QByteArray root = directory.absolutePath().toUtf8();
    Array<uint8> bytes;
    cook::AssetError failure;
    const bool isCooked =
        cook::AssembleLevel({root.constData(), static_cast<usize>(root.size())},
                            {reinterpret_cast<const uint8*>(encoded.constData()), static_cast<usize>(encoded.size())},
                            {},
                            bytes,
                            failure);
    INFO(failure.Message.View());
    REQUIRE(isCooked == true);
    const QByteArray cooked(reinterpret_cast<const char*>(bytes.GetData()), static_cast<qsizetype>(bytes.GetSize()));
    WriteFile(directory.filePath(QStringLiteral("scene.luscn")), cooked);
    RenderScene scene;
    REQUIRE(RenderScene::LoadCooked(cooked, level, scene, error, {}) == true);
    scene.HasStartView = TryParseStartView(level.value(QStringLiteral("camera")).toObject(), scene.StartView);
    REQUIRE(scene.HasStartView == true);
    REQUIRE(scene.Lit == true);
    return scene;
}

void CaptureWorld(const QDir& directory,
                  const QString& runtime,
                  rhi::DeviceHandle device,
                  rhi::SurfaceHandle surface,
                  uint32 extent)
{
    const RenderScene scene = PrepareWorld(directory);
    RenderPreview viewport;
    REQUIRE(viewport.SetScene(scene) == true);
    REQUIRE(viewport.GoToStartView() == true);
    viewport.SetShading(PreviewShading::Lit);
    renderer::ViewDescription view;
    QString error;
    REQUIRE(viewport.DescribeGraphView(extent, extent, view, error) == true);
    view.WorldGrid.IsVisible = false;
    QByteArray projection;
    QTextStream stream(&projection);
    stream.setRealNumberPrecision(9);
    for (const math::Vector4 column : view.WorldToClip.Columns)
    {
        stream << column.X << ' ' << column.Y << ' ' << column.Z << ' ' << column.W << '\n';
    }
    stream << view.CameraPosition.X << ' ' << view.CameraPosition.Y << ' ' << view.CameraPosition.Z << '\n';
    stream.flush();
    WriteFile(directory.filePath(QStringLiteral("camera.txt")), projection);

    RendererWorkspace workspace({});
    REQUIRE(workspace.GetPreviewOrNull() != nullptr);
    RenderGraphPreview& preview = *workspace.GetPreviewOrNull();
    WorldGraphView display(workspace, nullptr);
    display.SetCanPresent(true);
    REQUIRE(preview.Attach(device, surface) == true);
    preview.SetDirectPresentation(false);
    preview.SetReferenceSize(QSize(static_cast<int>(extent), static_cast<int>(extent)));
    RenderGraphExtraction extraction;
    REQUIRE(MakeRenderGraphExtraction(scene, view, 1, extraction, error) == true);
    extraction.ContentRevision = 1;
    preview.SetExtraction(extraction);
    REQUIRE(preview.Load(runtime) == RenderGraphPreviewLoad::Staged);
    QElapsedTimer timer;
    timer.start();
    while ((preview.GetPresentedRevision() != 1 || preview.HasPendingWork() == true) && timer.elapsed() < 120000)
    {
        preview.Tick();
        REQUIRE(preview.GetReport().Candidate != renderer::RenderGraphCandidateState::Failed);
        QThread::msleep(1);
    }
    INFO(preview.GetStatus().toStdString());
    REQUIRE(preview.GetPresentedRevision() == 1);
    REQUIRE(preview.HasPendingWork() == false);
    REQUIRE(display.GetPresentedFrames() > 0);
    REQUIRE(display.GetImage() == preview.GetImage());
    const InspectedTexture& texture = preview.GetOutputTexture();
    REQUIRE(texture.IsValid() == true);
    REQUIRE(texture.Width == extent);
    REQUIRE(texture.Height == extent);
    REQUIRE(texture.Encoding == TexelEncoding::Rgba8Unorm);
    WriteFile(directory.filePath(QStringLiteral("viewport.rgba")), texture.Bytes);
    preview.Detach();
}
} // namespace

TEST_CASE("Saved Worlds exercise the Lit mesh-light cube seam", "[.mesh-shadow-world]")
{
    const QDir root(qEnvironmentVariable("LUDUS_SHADOW_CASES"));
    const QString runtime = qEnvironmentVariable("LUDUS_SHADOW_GRAPH");
    const uint32 extent = qEnvironmentVariableIntValue("LUDUS_SHADOW_EXTENT");
    REQUIRE(extent > 0);
    REQUIRE(QFileInfo::exists(runtime) == true);
    const QStringList directories = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    REQUIRE(directories.isEmpty() == false);
    rhi::Shutdown();
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    REQUIRE(rhi::CreateDevice({},
                              {
                                  .Width = extent,
                                  .Height = extent,
                              },
                              MakeViewportDeviceDescription(),
                              device,
                              surface) == rhi::DeviceStatus::Ready);
    rhi::DeviceInfo info;
    REQUIRE(rhi::GetDeviceInfo(device, info) == rhi::DeviceStatus::Ready);
    REQUIRE(info.Startup.SelectedBackend == rhi::Backend::Metal);
    for (const QString& name : directories)
    {
        INFO(name.toStdString());
        CaptureWorld(QDir(root.filePath(name)), runtime, device, surface, extent);
    }
    REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
    rhi::Shutdown();
}
