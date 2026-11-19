// Save and reload the shared shutter fixture through the editor World and display its default Raster output.
#include "../../../tests/fixtures/large_world_sky.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_device_description.h"
#include "internal/world_graph_view.h"
#include "scene_fixture.h"
#include "viewport_fixture.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <limits>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace cook = ludus::content::scene_cook;
constexpr int64 VIEWPORT_TIMEOUT_MILLISECONDS = 120000;

void WriteFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    REQUIRE(file.flush() == true);
}

void WriteRoom(const QDir& directory, renderer::testing::Shutter shutter)
{
    using namespace renderer::testing;
    REQUIRE(directory.mkpath(QStringLiteral("source")) == true);
    const Panels panels = MakePanels();
    QJsonArray objects;
    for (uint32 i = 0; i < PANEL_COUNT; ++i)
    {
        if (i == ROOF_INDEX && shutter == Shutter::RoofRemoved)
        {
            continue;
        }
        const Panel& panel = panels[i];
        math::Vector3 centerWorld = panel.CenterWorld;
        if (i == SHUTTER_INDEX)
        {
            centerWorld.X += GetShutterTranslation(shutter);
        }
        QJsonArray positions;
        const math::Vector3 u = panel.HalfUWorld;
        const math::Vector3 v = panel.HalfVWorld;
        for (const math::Vector3 point :
             {centerWorld - u - v, centerWorld + u - v, centerWorld + u + v, centerWorld - u + v})
        {
            positions.append(
                QJsonArray{static_cast<double>(point.X), static_cast<double>(point.Y), static_cast<double>(point.Z)});
        }
        objects.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Panel %1").arg(i)},
                                   {QStringLiteral("material"), QStringLiteral("Matte")},
                                   {QStringLiteral("positions"), positions},
                                   {QStringLiteral("polygons"), QJsonArray{QJsonArray{0, 1, 2, 3}}}});
    }
    const QJsonObject camera{{QStringLiteral("eye"), QJsonArray{-0.0625, 2, -3.5}},
                             {QStringLiteral("target"), QJsonArray{-0.0625, 0, 0.5}},
                             {QStringLiteral("up"), QJsonArray{0, 1, 0}},
                             {QStringLiteral("vertical_fov_degrees"), 90}};
    const QJsonObject material{{QStringLiteral("name"), QStringLiteral("Matte")},
                               {QStringLiteral("model"), QStringLiteral("diffuse")},
                               {QStringLiteral("base_color"), QJsonArray{1, 1, 1}}};
    const QJsonObject source{{QStringLiteral("version"), 1},
                             {QStringLiteral("name"), QStringLiteral("Shutter room")},
                             {QStringLiteral("camera"), camera},
                             {QStringLiteral("materials"), QJsonArray{material}},
                             {QStringLiteral("objects"), objects}};
    // The scene importer sniffs the format token before reading the rest of the object.
    const QByteArray members = QJsonDocument(source).toJson(QJsonDocument::Compact);
    WriteFile(directory.filePath(QStringLiteral("source/room.luscene.json")),
              QByteArrayLiteral(R"({"format":"ludus.scene",)") + members.mid(1));
}

RenderScene PrepareWorld(const QDir& directory)
{
    const QString levelName = QStringLiteral("input.lulevel");
    QString error;
    const bool isImported = tests::PrepareImportedLevel(directory.path(),
                                                        levelName,
                                                        directory.filePath(QStringLiteral("source")),
                                                        error,
                                                        "room.luscene.json");
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
    const QString path = directory.filePath(QStringLiteral("assets/room.world.json"));
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

    return scene;
}

StaticArray<float64, 2> CaptureWorld(const RenderScene& scene,
                                     const QDir& directory,
                                     const QString& runtime,
                                     rhi::DeviceHandle device,
                                     rhi::SurfaceHandle surface,
                                     uint32 extent,
                                     renderer::DeferredLightingView lightingView,
                                     bool hasAtmosphere)
{
    RenderPreview viewport;
    REQUIRE(viewport.SetScene(scene) == true);
    REQUIRE(viewport.GoToStartView() == true);
    viewport.SetShading(PreviewShading::Lit);
    renderer::ViewDescription view;
    QString error;
    REQUIRE(viewport.DescribeGraphView(extent, extent, view, error) == true);
    view.WorldGrid.IsVisible = false;
    view.LightingView = lightingView;
    QByteArray projection;
    QTextStream stream(&projection);
    stream.setRealNumberPrecision(std::numeric_limits<float32>::max_digits10);
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
    extraction.ShaderInputs.Environment = {2, 2, 2};
    extraction.View.Atmosphere.IsEnabled = hasAtmosphere;
    extraction.View.Atmosphere.ShouldLightScene = true;
    extraction.View.Atmosphere.SolarIlluminance = {16, 16, 16};
    extraction.View.Atmosphere.DirectionToSunWorld = {0, 0.25F, 0.96824584F};
    preview.SetExtraction(extraction);
    REQUIRE(preview.Load(runtime) == RenderGraphPreviewLoad::Staged);
    QElapsedTimer timer;
    timer.start();
    while ((preview.GetPresentedRevision() != 1 || preview.HasPendingWork() == true) &&
           timer.elapsed() < VIEWPORT_TIMEOUT_MILLISECONDS)
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
    WriteFile(directory.filePath(
                  QStringLiteral("viewport-%1-%2.rgba").arg(static_cast<int>(lightingView)).arg(hasAtmosphere)),
              texture.Bytes);
    StaticArray<float64, 2> luminance{};
    constexpr StaticArray<float32, 2> FLOOR_Z{{-2, 3.75F}};
    for (usize i = 0; i < FLOOR_Z.GetSize(); ++i)
    {
        const math::Vector4 clip = view.WorldToClip * math::Vector4{0, 0, FLOOR_Z[i], 1};
        REQUIRE(clip.W > 0);
        const float32 column = (0.5F + 0.5F * clip.X / clip.W) * static_cast<float32>(extent);
        const float32 row = (0.5F - 0.5F * clip.Y / clip.W) * static_cast<float32>(extent);
        REQUIRE((column >= 0 && column < static_cast<float32>(extent)));
        REQUIRE((row >= 0 && row < static_cast<float32>(extent)));
        const qsizetype offset =
            static_cast<qsizetype>((static_cast<uint32>(row) * extent + static_cast<uint32>(column)) * 4);
        luminance[i] = (0.2126 * static_cast<uint8>(texture.Bytes[offset]) +
                        0.7152 * static_cast<uint8>(texture.Bytes[offset + 1]) +
                        0.0722 * static_cast<uint8>(texture.Bytes[offset + 2])) /
                       255;
    }
    preview.Detach();
    return luminance;
}
} // namespace

TEST_CASE("Saved shutter World presents default Raster skylight", "[.large-world-sky-viewport]")
{
    using namespace renderer::testing;
    const QString runtime = qEnvironmentVariable("LUDUS_SKY_GRAPH");
    REQUIRE(QFileInfo::exists(runtime) == true);
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid() == true);
    const QDir root(qEnvironmentVariable("LUDUS_SKY_CASES", temporary.path()));
    constexpr uint32 EXTENT = 128;
    rhi::Shutdown();
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    REQUIRE(rhi::CreateDevice({},
                              {
                                  .Width = EXTENT,
                                  .Height = EXTENT,
                              },
                              MakeViewportDeviceDescription(),
                              device,
                              surface) == rhi::DeviceStatus::Ready);
    StaticArray<StaticArray<float64, 2>, 4> exposed{};
    for (Shutter shutter : {Shutter::RoofRemoved, Shutter::Open})
    {
        const QDir directory(
            root.filePath(shutter == Shutter::Open ? QStringLiteral("open") : QStringLiteral("exposed")));
        WriteRoom(directory, shutter);
        const RenderScene scene = PrepareWorld(directory);
        usize diagnostic = 0;
        for (bool hasAtmosphere : {false, true})
        {
            for (renderer::DeferredLightingView view :
                 {renderer::DeferredLightingView::SkyVisibility, renderer::DeferredLightingView::SkyDiffuse})
            {
                const StaticArray<float64, 2> values =
                    CaptureWorld(scene, directory, runtime, device, surface, EXTENT, view, hasAtmosphere);
                if (shutter == Shutter::RoofRemoved)
                {
                    exposed[diagnostic] = values;
                }
                else
                {
                    CAPTURE(hasAtmosphere, view, values[0], values[1], exposed[diagnostic][0], exposed[diagnostic][1]);
                    CHECK(exposed[diagnostic][0] > 0);
                    CHECK(values[0] <= 0.30 * exposed[diagnostic][0]);
                    CHECK(values[1] >= 0.40 * exposed[diagnostic][1]);
                }
                ++diagnostic;
            }
        }
    }
    REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
    rhi::Shutdown();
}
