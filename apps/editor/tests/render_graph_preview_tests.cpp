// RG3b trusted render-graph preview on a real headless RHI device. The frozen fixture is the
// checked-in RendererStatic offscreen graph, cooked by the editor build both as the host's
// compiled header (the live runtime path) and as a .lurgrt runtime generation (the preview
// path). Tests prove pending, failed and successful publication keep or replace the last
// drawable view as documented, and that preview pixels equal the live path's pixels. Hosts
// without a GPU adapter skip; this is not native Qt presentation evidence.
#include "editor_default_graph.h"
#include "editor_renderer_static.h"
#include "internal/controller.h"
#include "internal/error_list.h"
#include "internal/graph_display_frames.h"
#include "internal/main_window.h"
#include "internal/offscreen_frame_reader.h"
#include "internal/preview_viewport.h"
#include "internal/reference_capture_window.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_library_capture.h"
#include "internal/render_graph_navigation.h"
#include "internal/render_graph_preview.h"
#include "internal/render_graph_resource_inspection.h"
#include "internal/render_graph_targets.h"
#include "internal/render_preview.h"
#include "internal/render_target_reader.h"
#include "internal/renderer_workspace.h"
#include "internal/texture_inspection.h"
#include "internal/texture_viewer.h"
#include "internal/viewport_device_description.h"
#include "internal/viewport_graph_capture.h"
#include "internal/world_demo_adapter.h"
#include "internal/world_graph_view.h"
#include "internal/world_workspace.h"
#include "renderer_composite.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/content/scene_cook/scene_assets.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/tone_mapping.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/profile.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/platform/entropy.h>
#include <ludus/runtime/resources/scene.h>

#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <new>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QToolButton>
#include <QWidget>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::render_graph_format;
namespace host = ludus::render_graph_packages::editor_renderer_static;
namespace default_graph = ludus::render_graph_packages::editor_default_graph;
namespace shaders = ludus::shaders;
namespace
{
/// Fixture target extent (tests/sdk_consumer/rg2/offscreen/main.lurender.json).
constexpr int32 WIDTH = 96;
constexpr int32 HEIGHT = 64;
constexpr usize ATTEMPTS = 5000;
/// The format owner defines the serialized header words preceding the interface identity.
constexpr qsizetype INTERFACE_OFFSET = format::HEADER_FIELD_COUNT * sizeof(uint32);
constexpr qsizetype TRUNCATED_BYTES = 40;
constexpr int32 COLOR_TOLERANCE = 1;
constexpr usize DISPLAY_READ_COUNT = 3;
constexpr usize MATRIX_COLUMNS = 4;
/// Bytes per RGBA8 test texel.
constexpr usize TEXEL_BYTES = 4;
/// One padded RGBA8 row at the WIDTH fixture extent.
constexpr usize SINGLE_ROW_TRANSFER_BYTES = 512;
/// Largest 8-bit channel code, which encodes 1.
constexpr float32 MAX_CHANNEL_CODE = 255.0F;
/// Absolute linear tolerance of a flat tint against its independently decoded expectation.
constexpr float32 LINEAR_TOLERANCE = 1e-6F;

QByteArray ReadFixture()
{
    QFile file(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE));
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

QString WriteBytes(const QTemporaryDir& directory, const QString& name, const QByteArray& bytes)
{
    const QString path = QDir(directory.path()).filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    return path;
}

/// One headless device per test; RHI is a process-wide session.
struct DeviceFixture final
{
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    bool IsAvailable = false;
    explicit DeviceFixture(const rhi::StorageLimits& storage = {}, bool shouldRequestRay = false)
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Preferred.RayQueries = shouldRequestRay;
        description.Storage = storage;
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

/// One opaque triangle in front of an identity camera, as the World extraction delivers it.
RenderScene MakeScene(math::Vector4 tint)
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("rg3b-frozen-fixture");
    PreviewMesh mesh;
    mesh.Id = QStringLiteral("triangle");
    mesh.Positions = {{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}};
    mesh.Indices = {0, 1, 2};
    // The default's forward and deferred scene path needs a shading normal per vertex.
    mesh.Normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("object");
    object.Mesh = 0;
    object.Tint = tint;
    scene.Objects.append(object);
    return scene;
}

/// An RGBA8 sRGB test texture of one row, filled from straight RGBA texel bytes.
QImage MakeTexture(std::initializer_list<StaticArray<uint8, TEXEL_BYTES>> texels)
{
    QImage image(static_cast<int32>(texels.size()), 1, QImage::Format_RGBA8888);
    REQUIRE(image.isNull() == false);
    uint8* bytes = image.scanLine(0);
    usize offset = 0;
    for (const StaticArray<uint8, TEXEL_BYTES>& texel : texels)
    {
        for (usize channel = 0; channel < TEXEL_BYTES; ++channel)
        {
            bytes[offset++] = texel[channel];
        }
    }
    return image;
}

/// Linear value of one sRGB channel byte, decoded independently of the editor's flat tint.
float32 DecodeChannel(uint8 code)
{
    return math::DecodeSrgb(static_cast<float32>(code) / MAX_CHANNEL_CODE);
}

/// The same triangle composed the way an imported or merged World delivers it: cooked into a
/// decoded definition that owns the geometry, with empty PreviewMesh arrays. A non-null texture
/// becomes the object's base-color texture.
RenderScene MakeCookedScene(const QImage& texture = QImage())
{
    const QJsonObject level{
        {QStringLiteral("id"), QStringLiteral("0f4c2a8e-3b1d-4e6a-9c7f-2d5b8a1e4c90")},
        {QStringLiteral("objects"), QJsonArray{}},
        {QStringLiteral("camera"),
         QJsonObject{{QStringLiteral("eye"), QJsonArray{0, 0, 3}},
                     {QStringLiteral("target"), QJsonArray{0, 0, 0}},
                     {QStringLiteral("up"), QJsonArray{0, 1, 0}},
                     {QStringLiteral("fov"), 1.0}}},
    };
    RenderScene authored = MakeScene({1, 1, 1, 1});
    // Cooking requires a complete static vertex: normals and texture coordinates per position.
    authored.Meshes[0].Normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    authored.Meshes[0].Uvs = {{0, 0}, {1, 0}, {.5F, 1}};
    if (texture.isNull() == false)
    {
        authored.Textures.append({QStringLiteral("albedo"), texture});
        authored.Objects[0].Texture = 0;
    }
    RenderScene cooked;
    QString error;
    const bool isMerged = RenderScene::Merge(authored, {}, level, cooked, error, {});
    INFO(error.toStdString());
    REQUIRE(isMerged == true);
    REQUIRE(cooked.Definition != nullptr);
    REQUIRE(cooked.Meshes.size() == 1);
    REQUIRE(cooked.Meshes.at(0).Positions.isEmpty() == true);
    REQUIRE(cooked.Definition->Meshes[0].Positions.GetSize() == 3);
    return cooked;
}

/// Normalized depth of a world point under a view.
float32 GetDepth(const renderer::ViewDescription& view, math::Vector3 positionWorld)
{
    const math::Vector4 clip = view.WorldToClip * math::Vector4{positionWorld.X, positionWorld.Y, positionWorld.Z, 1};
    REQUIRE(clip.W > 0);
    return clip.Z / clip.W;
}

RenderGraphExtraction MakeExtraction(math::Vector4 tint, uint64 revision)
{
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(MakeScene(tint), {}, revision, extraction, error) == true);
    return extraction;
}

RenderGraphPreview MakePreview()
{
    return RenderGraphPreview({host::INTERFACE_IDENTITY.GetData(), host::INTERFACE_IDENTITY.GetSize()},
                              shaders::renderer_composite::Vertex(),
                              shaders::renderer_composite::Fragment());
}

/// Tick until the predicate holds; the GPU completes asynchronously, so yield between ticks.
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
        QThread::usleep(200);
    }
    return predicate();
}

/// Queued conversion completions are delivered by the GUI event loop, independently of preview ticks.
template <typename Predicate>
bool TickUntilDisplay(RenderGraphPreview& inoutPreview, Predicate predicate)
{
    return TickUntil(inoutPreview, [&]() {
        QApplication::processEvents();
        return predicate();
    });
}

// Channels are named in RGB order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void CheckCenter(const QImage& image, int32 red, int32 green, int32 blue)
{
    REQUIRE(image.size() == QSize(WIDTH, HEIGHT));
    const QColor center = image.pixelColor(WIDTH / 2, HEIGHT / 2);
    CHECK(qAbs(center.red() - red) <= COLOR_TOLERANCE);
    CHECK(qAbs(center.green() - green) <= COLOR_TOLERANCE);
    CHECK(qAbs(center.blue() - blue) <= COLOR_TOLERANCE);
}

/// RGBA8 display codes of a scene-linear colour after the engine default graph's ToneMap node:
/// PBR Neutral at exposure 1, then sRGB encoding (render_graphs/default/main.lurender.json).
QColor MapDefaultGraphColor(math::Vector3 sceneColor)
{
    const math::Vector3 display = renderer::ApplyPbrNeutral(sceneColor);
    return QColor(qRound(math::EncodeSrgb(display.X) * MAX_CHANNEL_CODE),
                  qRound(math::EncodeSrgb(display.Y) * MAX_CHANNEL_CODE),
                  qRound(math::EncodeSrgb(display.Z) * MAX_CHANNEL_CODE));
}

void CheckColor(const QColor& actual, const QColor& expected)
{
    CHECK(qAbs(actual.red() - expected.red()) <= COLOR_TOLERANCE);
    CHECK(qAbs(actual.green() - expected.green()) <= COLOR_TOLERANCE);
    CHECK(qAbs(actual.blue() - expected.blue()) <= COLOR_TOLERANCE);
}

/// The live runtime path: the host's compiled header, a directly owned RenderGraph and
/// independently written inputs, rendered and copied exactly once.
QImage RenderLive(const DeviceFixture& device, math::Vector4 tint)
{
    renderer::RenderGraph graph;
    rhi::RasterStatus status = graph.Initialize(
        device.Device,
        host::MakePlan(),
        host::MakePrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()));
    for (usize attempt = 0; status == rhi::RasterStatus::Pending && attempt < ATTEMPTS; ++attempt)
    {
        QThread::usleep(200);
        status = graph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    StaticArray<math::Vector3, 3> positions{{{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}}};
    StaticArray<uint32, 3> indices{{0, 1, 2}};
    StaticArray<renderer::RenderGraphMesh, 1> meshes{};
    meshes[0].Geometry = {positions.GetData(), positions.GetSize(), indices.GetData(), indices.GetSize()};
    StaticArray<renderer::RenderGraphItem, 1> items{};
    items[0].SourceId = 1;
    items[0].Material.Color = tint;
    StaticArray<renderer::ViewDescription, 1> cameras{};
    renderer::RenderGraphReport report;
    status = graph.Update({meshes.GetData(), meshes.GetSize()},
                          {items.GetData(), items.GetSize()},
                          {cameras.GetData(), cameras.GetSize()},
                          1,
                          renderer::RenderGraphCoveragePolicy::DiagnosticColor,
                          report);
    for (usize attempt = 0; graph.GetDrawableSceneRevision() == 0 && attempt < ATTEMPTS; ++attempt)
    {
        QThread::usleep(200);
        status = graph.Poll();
    }
    REQUIRE(graph.GetDrawableSceneRevision() == 1);
    rhi::SubmissionToken completion;
    REQUIRE(graph.Render(device.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect = rhi::TextureAspect::Color;
    status = graph.GetExport(0, texture, aspect);
    for (usize attempt = 0; status == rhi::RasterStatus::NotReady && attempt < ATTEMPTS; ++attempt)
    {
        QThread::usleep(200);
        status = graph.GetExport(0, texture, aspect);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    OffscreenFrameReader reader;
    REQUIRE(reader.Begin(device.Device, texture, WIDTH, HEIGHT) == true);
    OffscreenReadStatus read = reader.Poll();
    for (usize attempt = 0; read == OffscreenReadStatus::Pending && attempt < ATTEMPTS; ++attempt)
    {
        QThread::usleep(200);
        read = reader.Poll();
    }
    REQUIRE(read == OffscreenReadStatus::Complete);
    const QImage image = reader.TakeImage();
    graph.Reset();
    return image;
}
} // namespace

TEST_CASE("RG3b World extraction maps objects to static opaque graph items", "[editor][render-graph][preview]")
{
    RenderScene scene = MakeScene({1, .5F, .25F, 1});
    scene.Lit = true;
    PreviewSurface surface;
    // The lit tint is not the static profile's color; the flat approximation is.
    surface.Tint = {.25F, 1, 1, 1};
    surface.FlatTint = {.5F, 1, 1, 1};
    surface.Emission = {2, 1, 0};
    surface.Roughness = .3F;
    surface.IsAlphaMasked = true;
    surface.AlphaCutoff = .25F;
    scene.AuthoredLights.Count = 1;
    scene.AuthoredLights.Lights[0].Kind = renderer::LightKind::Point;
    scene.AuthoredLights.Lights[0].SourceId = 17;
    scene.Surfaces.append(surface);
    scene.Objects[0].Surface = 0;
    PreviewObject translucent = scene.Objects[0];
    translucent.Tint = {1, 1, 1, .5F};
    translucent.Surface = -1;
    scene.Objects.append(translucent);
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(scene, {}, 9, extraction, error) == true);
    REQUIRE(extraction.Meshes.size() == 1);
    CHECK(extraction.Meshes[0].Geometry.VertexCount == 3);
    CHECK(extraction.Meshes[0].Geometry.Positions == extraction.Scene.Meshes[0].Positions.constData());
    REQUIRE(extraction.Items.size() == 2);
    CHECK(extraction.Items[0].SourceId == 1);
    CHECK(extraction.Items[0].Material.Color.X == .5F);
    CHECK(extraction.Items[0].Coverage == renderer::RenderGraphMaterial::Opaque);
    // Translucency is declared, never silently drawn as opaque.
    CHECK(extraction.Items[1].Coverage == renderer::RenderGraphMaterial::Transparent);
    CHECK(extraction.Revision == 9);
    REQUIRE(extraction.ShaderInputs.Materials.GetSize() == 1);
    CHECK(extraction.ShaderInputs.Materials[0].BaseColor.X == .25F);
    CHECK(extraction.ShaderInputs.Materials[0].Emission.X == 2);
    CHECK(extraction.ShaderInputs.Materials[0].Roughness == .3F);
    CHECK(extraction.ShaderInputs.Materials[0].IsAlphaMasked == true);
    CHECK(extraction.ShaderInputs.Materials[0].AlphaCutoff == .25F);
    CHECK(extraction.ShaderInputs.Materials[0].Sampler.Filter == rhi::RasterFilter::Linear);
    CHECK(extraction.ShaderInputs.Materials[0].HasError == false);
    REQUIRE(extraction.Items[0].MaterialOverrides.GetSize() == 1);
    CHECK(extraction.Items[0].MaterialOverrides[0] == 0);
    CHECK(extraction.Items[1].MaterialOverrides[0] == renderer::DEFAULT_SCENE_MATERIAL);
    CHECK(extraction.Items[0].SurfaceTint.X == 1);
    REQUIRE(extraction.ShaderInputs.Lights.GetSize() == 1);
    CHECK(extraction.ShaderInputs.Lights[0].SourceId == 17);
    CHECK(extraction.ShaderInputs.Lights[0].Kind == renderer::LightKind::Point);

    RenderGraphExtraction unchanged = extraction;
    CHECK(MakeRenderGraphExtraction(RenderScene{}, {}, 10, unchanged, error) == false);
    CHECK(unchanged.Revision == 9);
    CHECK(MakeRenderGraphExtraction(scene, {}, 0, unchanged, error) == false);
}

TEST_CASE("Unlit World objects reach the lit default as emission over a black base", "[editor][render-graph][preview]")
{
    RenderScene scene = MakeScene({.25F, .5F, .75F, 1});
    REQUIRE(scene.Lit == false);
    QImage texture(1, 1, QImage::Format_RGBA8888);
    texture.fill(Qt::white);
    scene.Textures.append({QStringLiteral("sprite"), texture});
    PreviewSurface surface;
    surface.Texture = 0;
    surface.Tint = {1, .5F, 1, 1};
    surface.IsAlphaMasked = true;
    surface.AlphaCutoff = .5F;
    scene.Surfaces.append(surface);
    PreviewObject textured = scene.Objects[0];
    textured.Texture = 0;
    textured.Surface = 0;
    scene.Objects.append(textured);
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(scene, {}, 3, extraction, error) == true);
    // One material per unlit object follows the surface table; no light is added.
    REQUIRE(extraction.ShaderInputs.Materials.GetSize() == 3);
    CHECK(extraction.ShaderInputs.Lights.GetSize() == 0);
    REQUIRE(extraction.Items.size() == 2);
    const renderer::SceneShaderMaterial& plain = extraction.ShaderInputs.Materials[1];
    CHECK(extraction.Items[0].MaterialOverrides[0] == 1);
    CHECK(plain.BaseColor == math::Vector3{0, 0, 0});
    CHECK(plain.Emission == math::Vector3{.25F, .5F, .75F});
    CHECK(plain.EmissionTexture.Bytes == nullptr);
    // A textured object's surface tint and colour texture modulate its emission.
    const renderer::SceneShaderMaterial& sprite = extraction.ShaderInputs.Materials[2];
    CHECK(extraction.Items[1].MaterialOverrides[0] == 2);
    CHECK(sprite.BaseColor == math::Vector3{0, 0, 0});
    CHECK(sprite.Emission == math::Vector3{.25F, .25F, .75F});
    CHECK(sprite.EmissionTexture.Bytes == extraction.ShaderInputs.Materials[0].ColorTexture.Bytes);
    CHECK(sprite.EmissionTexture.Bytes != nullptr);
    CHECK(sprite.IsAlphaMasked == true);
    CHECK(sprite.AlphaCutoff == .5F);
    CHECK(sprite.ColorTexture.Bytes == sprite.EmissionTexture.Bytes);
}

TEST_CASE("RG3b World-demo Worlds feed the render-graph preview like any other World",
          "[editor][render-graph][preview]")
{
    QFile schemaFile(QStringLiteral(":/world-demo/world.schema.json"));
    REQUIRE(schemaFile.open(QIODevice::ReadOnly));
    WorldDocument world;
    QString error;
    REQUIRE(world.LoadSchema(schemaFile.readAll(), error));
    const std::string_view level = ludus::world_demo::ExampleLevel();
    REQUIRE(world.Load(QByteArray(level.data(), static_cast<qsizetype>(level.size())), error));
    REQUIRE(IsWorldDemo(world));
    RenderScene scene;
    REQUIRE(RenderScene::Load({}, world, scene, error));
    // The viewport's own camera, converted for the conventional-depth runtime profile.
    PreviewViewOptions options;
    options.Camera = scene.DefaultCamera;
    options.Width = 96;
    options.Height = 64;
    options.ShouldUseConventionalDepth = true;
    renderer::ViewDescription view;
    REQUIRE(scene.View(options, view, error));
    RenderGraphExtraction extraction;
    REQUIRE(MakeRenderGraphExtraction(scene, view, 1, extraction, error) == true);
    REQUIRE(extraction.Meshes.size() == 1);
    REQUIRE(extraction.Items.size() == scene.Objects.size());
    for (qsizetype i = 0; i < extraction.Items.size(); ++i)
    {
        // Flat, opaque game colours with the game's transforms; no surface substitutes a tint.
        CHECK(extraction.Items[i].Coverage == renderer::RenderGraphMaterial::Opaque);
        CHECK(extraction.Items[i].Material.Color.X == scene.Objects[i].Tint.X);
        CHECK(extraction.Items[i].MeshToWorld == scene.Objects[i].Transform);
    }
}

TEST_CASE("RG3b cooked World extraction borrows the decoded definition's geometry", "[editor][render-graph][preview]")
{
    const RenderScene cooked = MakeCookedScene();
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(cooked, {}, 3, extraction, error) == true);
    REQUIRE(extraction.Meshes.size() == 1);
    const ludus::resources::SceneMesh& source = cooked.Definition->Meshes[0];
    // Borrowed from the shared definition the extraction retains, not copied.
    CHECK(extraction.Scene.Definition.Get() == cooked.Definition.Get());
    CHECK(extraction.Meshes[0].Geometry.Positions == source.Positions.GetData());
    CHECK(extraction.Meshes[0].Geometry.VertexCount == 3);
    CHECK(extraction.Meshes[0].Geometry.Indices == source.Indices.GetData());
    CHECK(extraction.Meshes[0].Geometry.IndexCount == 3);
    REQUIRE(extraction.Items.size() == 1);
    CHECK(extraction.Items[0].MeshIndex == 0);
}

TEST_CASE("RG3b cooked textured materials preview as their linear mean base color", "[editor][render-graph][preview]")
{
    // Straight alpha 128: the cooked viewport transport draws LUSCN materials Opaque, so the
    // flat tint stays opaque instead of turning the item into a magenta Transparent diagnostic.
    const RenderScene cooked = MakeCookedScene(MakeTexture({{188, 96, 40, 128}}));
    REQUIRE(cooked.Definition->Textures.GetSize() == 1);
    const ludus::resources::SceneTexture& texture = cooked.Definition->Textures[0];
    REQUIRE(texture.Pixels.GetSize() == TEXEL_BYTES);
    REQUIRE(cooked.Surfaces.size() == 1);
    // The lit and textured paths keep the white multiplier the texture is sampled under.
    CHECK(cooked.Surfaces[0].Tint == math::Vector4{1, 1, 1, 1});
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(cooked, {}, 4, extraction, error) == true);
    REQUIRE(extraction.Items.size() == 1);
    const math::Vector4 color = extraction.Items[0].Material.Color;
    CHECK(color.X == Catch::Approx(DecodeChannel(texture.Pixels[0])).margin(LINEAR_TOLERANCE));
    CHECK(color.Y == Catch::Approx(DecodeChannel(texture.Pixels[1])).margin(LINEAR_TOLERANCE));
    CHECK(color.Z == Catch::Approx(DecodeChannel(texture.Pixels[2])).margin(LINEAR_TOLERANCE));
    CHECK(color.X < 1);
    CHECK(color.Z < color.Y);
    CHECK(color.Y < color.X);
    CHECK(color.W == 1);
    CHECK(extraction.Items[0].Coverage == renderer::RenderGraphMaterial::Opaque);

    // Texels average in linear space: white and black make linear one half, not the much
    // darker decode of the averaged sRGB code.
    const RenderScene halves = MakeCookedScene(MakeTexture({{255, 255, 255, 255}, {0, 0, 0, 255}}));
    REQUIRE(MakeRenderGraphExtraction(halves, {}, 5, extraction, error) == true);
    REQUIRE(extraction.Items.size() == 1);
    const math::Vector4 half = extraction.Items[0].Material.Color;
    CHECK(half.X == Catch::Approx(.5F).margin(LINEAR_TOLERANCE));
    CHECK(half.Y == Catch::Approx(.5F).margin(LINEAR_TOLERANCE));
    CHECK(half.Z == Catch::Approx(.5F).margin(LINEAR_TOLERANCE));
    CHECK(half.X > math::DecodeSrgb(.5F));
}

TEST_CASE("RG3b editor reverse-Z camera yields a finite conventional view of the same camera",
          "[editor][render-graph][preview]")
{
    RenderPreview viewport;
    viewport.Resize(WIDTH, HEIGHT);
    REQUIRE(viewport.SetScene(MakeCookedScene()) == true);
    QString error;
    renderer::ViewDescription reverse;
    REQUIRE(viewport.DescribeView(WIDTH, HEIGHT, reverse, error) == true);
    // The editor camera is an infinite reverse-Z perspective, which the runtime rejects.
    CHECK(reverse.Depth == renderer::DepthConvention::ReverseZ);
    CHECK(reverse.InfiniteFar == true);
    renderer::ViewDescription conventional;
    REQUIRE(viewport.DescribeConventionalView(WIDTH, HEIGHT, conventional, error) == true);
    CHECK(conventional.Depth == renderer::DepthConvention::Conventional);
    CHECK(conventional.InfiniteFar == false);
    CHECK(conventional.CameraPosition == reverse.CameraPosition);
    CHECK(conventional.CameraForward == reverse.CameraForward);

    // The same camera: the far plane changes only clip z, so x, y and w agree.
    for (usize column = 0; column < MATRIX_COLUMNS; ++column)
    {
        CHECK(conventional.WorldToClip.At(0, column) == reverse.WorldToClip.At(0, column));
        CHECK(conventional.WorldToClip.At(1, column) == reverse.WorldToClip.At(1, column));
        CHECK(conventional.WorldToClip.At(3, column) == reverse.WorldToClip.At(3, column));
    }
    // Every scene point lies inside the finite conventional depth range, nearer points smaller.
    const math::Vector3 nearWorld{0, .5F, .5F};
    const math::Vector3 farWorld = nearWorld + reverse.CameraForward;
    const float32 nearDepth = GetDepth(conventional, nearWorld);
    for (const math::Vector3 vertexWorld : {math::Vector3{-.5F, -.5F, .5F}, math::Vector3{.5F, -.5F, .5F}, nearWorld})
    {
        const float32 depth = GetDepth(conventional, vertexWorld);
        CHECK(depth > 0);
        CHECK(depth < 1);
    }
    CHECK(GetDepth(conventional, farWorld) > nearDepth);
    CHECK(GetDepth(reverse, farWorld) < GetDepth(reverse, nearWorld));

    // A scene without geometry has no finite far plane; the output is preserved.
    RenderPreview empty;
    renderer::ViewDescription unchanged = conventional;
    CHECK(empty.DescribeConventionalView(WIDTH, HEIGHT, unchanged, error) == false);
    CHECK(unchanged.WorldToClip == conventional.WorldToClip);
}

TEST_CASE("RG3b preview renders a cooked World through the editor camera", "[editor][render-graph][preview]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    // The World viewport's cooked scene and its reverse-Z editor camera, as ServicePreview
    // delivers them: before the fix the empty PreviewMesh geometry and the reverse-Z view were
    // each rejected by RenderGraph::Update and no generation ever presented.
    RenderPreview viewport;
    viewport.Resize(WIDTH, HEIGHT);
    REQUIRE(viewport.SetScene(MakeCookedScene(MakeTexture({{255, 0, 0, 255}}))) == true);
    renderer::ViewDescription view;
    QString error;
    REQUIRE(viewport.DescribeConventionalView(WIDTH, HEIGHT, view, error) == true);
    RenderGraphExtraction extraction;
    REQUIRE(MakeRenderGraphExtraction(viewport.Scene(), view, 1, extraction, error) == true);
    // The base color lives in the cooked texture; the flat profile draws its linear mean.
    REQUIRE(extraction.Items.size() == 1);
    CHECK(extraction.Items[0].Material.Color == math::Vector4{1, 0, 0, 1});

    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(extraction);
    renderer::ViewDescription presented;
    CHECK(preview.TryGetPresentedView(presented) == false);
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; }) == true);
    CHECK(preview.GetPresentedRevision() == 1);
    // The framed triangle covers the center; the corner stays the graph's clear color.
    CheckCenter(preview.GetImage(), 255, 0, 0);
    CHECK(preview.GetImage().pixelColor(0, 0) != preview.GetImage().pixelColor(WIDTH / 2, HEIGHT / 2));
    REQUIRE(preview.TryGetPresentedView(presented) == true);
    CHECK(presented.WorldToClip == view.WorldToClip);

    // Selection overlays project through the shown frame's camera: a newer camera is reported
    // only once a frame rendered with it replaces the image.
    viewport.Orbit(30, 0);
    renderer::ViewDescription moved;
    REQUIRE(viewport.DescribeConventionalView(WIDTH, HEIGHT, moved, error) == true);
    REQUIRE((moved.WorldToClip == view.WorldToClip) == false);
    preview.SetCamera(moved, 2);
    REQUIRE(preview.TryGetPresentedView(presented) == true);
    CHECK(presented.WorldToClip == view.WorldToClip);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 2; }) == true);
    REQUIRE(preview.TryGetPresentedView(presented) == true);
    CHECK(presented.WorldToClip == moved.WorldToClip);
    preview.Detach();
}

TEST_CASE("RG3b preview publishes only complete frames and matches the live path", "[editor][render-graph][preview]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    const math::Vector4 tint{.25F, .5F, .75F, 1};
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction(tint, 1));

    // Pending: the candidate prepares and nothing is presented yet.
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    CHECK(preview.GetImage().isNull() == true);
    CHECK(preview.GetStatistics().GetCpuMilliseconds().GetCount() == 0);
    CHECK(preview.GetStatistics().GetReadbackMilliseconds().GetCount() == 0);
    CHECK(preview.GetPresentedGeneration() == 0);
    CHECK(preview.GetTargetSize() == QSize(WIDTH, HEIGHT));

    // Successful: the first frame of generation 1 at extraction revision 1.
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; }) == true);
    CHECK(preview.GetPresentedRevision() == 1);
    CHECK(preview.GetReport().ActiveGeneration == 1);
    CheckCenter(preview.GetImage(), 64, 128, 191);
    const QImage presented = preview.GetImage();

    // Preview/live parity on the frozen fixture: identical bytes from the compiled host path.
    const QImage live = RenderLive(device, tint);
    CHECK(live == presented);
    preview.Detach();
}

TEST_CASE("Ray preview advances stochastic samples without a scene edit and raster preview becomes idle",
          "[editor][render-graph][preview][progressive]")
{
    DeviceFixture device({}, true);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    renderer::RenderGraphRenderer automatic;
    REQUIRE(renderer::SelectRenderGraphRenderer(device.Device, renderer::RenderGraphPreference::Automatic, automatic) ==
            rhi::RasterStatus::Ready);
    if (automatic != renderer::RenderGraphRenderer::HardwareRayTracing)
    {
        SKIP("Native ray queries are unavailable on this host");
    }
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    // A lit World: its default material scatters stochastic samples, which pure emission would not.
    RenderScene scene = MakeScene({1, 0, 0, 1});
    scene.Lit = true;
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(scene, {}, 1, extraction, error) == true);
    extraction.View.OrthographicCamera = true;
    preview.SetExtraction(extraction);
    QSignalSpy presented(&preview, &RenderGraphPreview::FramePresented);
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_PROGRESSIVE_RAY_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    // A cold native ray pipeline can outlast the short polling window used by raster fixtures.
    constexpr int64 PIPELINE_PREPARATION_TIMEOUT_MILLISECONDS = 10000;
    constexpr uint32 PREPARATION_POLL_MICROSECONDS = 200;
    QElapsedTimer preparation;
    preparation.start();
    while (presented.count() == 0 && preparation.elapsed() < PIPELINE_PREPARATION_TIMEOUT_MILLISECONDS)
    {
        preview.Tick();
        QThread::usleep(PREPARATION_POLL_MICROSECONDS);
    }
    INFO(preview.GetStatus().toStdString());
    REQUIRE(presented.count() >= 1);
    const QImage first = preview.GetImage();
    const uint64 rayGeneration = preview.GetPresentedGeneration();
    REQUIRE(TickUntil(preview, [&]() { return presented.count() >= 2; }) == true);
    const QImage second = preview.GetImage();
    CHECK(second != first);
    CHECK(preview.GetPresentedGeneration() == rayGeneration);
    CHECK(preview.GetPresentedRevision() == 1);
    // Repeating the same extraction revision must not restart history at sample zero.
    preview.SetExtraction(extraction);
    REQUIRE(TickUntil(preview, [&]() { return presented.count() >= 3; }) == true);
    CHECK(preview.GetImage() != second);
    CHECK(preview.GetPresentedRevision() == 1);
    // Identical inputs isolate a history restart from geometric changes caused by camera movement.
    extraction.Revision = 2;
    preview.SetExtraction(extraction);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 2; }) == true);
    CHECK(preview.GetImage() != first);
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() != rayGeneration; }) == true);
    const qsizetype rasterFrames = presented.count();
    const QImage raster = preview.GetImage();
    constexpr usize IDLE_TICKS = 16;
    for (usize tick = 0; tick < IDLE_TICKS; ++tick)
    {
        preview.Tick();
    }
    CHECK(presented.count() == rasterFrames);
    CHECK(preview.GetImage() == raster);
    preview.Detach();
}

TEST_CASE("RG3b failed and pending candidates keep the last drawable view",
          "[editor][render-graph][preview][resource-inspection-preview]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QTemporaryDir directory;
    const QByteArray bytes = ReadFixture();
    QFile names(GetTargetNamesPath(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)));
    REQUIRE(names.open(QIODevice::ReadOnly) == true);
    const RenderGraphPreviewSource provenance{
        QByteArrayLiteral("verified-source"),
        QStringLiteral("Default"),
        QJsonDocument::fromJson(names.readAll()).object().value(QStringLiteral("generation")).toString().toLatin1(),
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
        {},
        42};
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE), {}, provenance) ==
            RenderGraphPreviewLoad::Staged);
    CHECK(preview.GetActiveSource().InputIdentity.isEmpty() == true);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; }) == true);
    CHECK(preview.GetActiveSource().InputIdentity == provenance.InputIdentity);
    const QImage green = preview.GetImage();
    const usize sampleCount = preview.GetStatistics().GetCpuMilliseconds().GetCount();
    REQUIRE(sampleCount != 0);
    CheckCenter(green, 0, 255, 0);
    const auto expectKept = [&]() {
        for (int32 tick = 0; tick < 50; ++tick)
        {
            preview.Tick();
        }
        CHECK(preview.GetImage() == green);
        CHECK(preview.GetStatistics().GetCpuMilliseconds().GetCount() == sampleCount);
        CHECK(preview.GetStatistics().GetReadbackMilliseconds().GetCount() == sampleCount);
        CHECK(preview.GetPresentedGeneration() == 1);
        CHECK(preview.GetReport().ActiveGeneration == 1);
        CHECK(preview.GetActiveSource().InputIdentity == provenance.InputIdentity);
        CHECK(preview.GetActiveSource().SourceRevision == 42);
    };

    // A truncated generation is rejected at decode; nothing is staged.
    CHECK(preview.Load(WriteBytes(directory, QStringLiteral("truncated.lurgrt"), bytes.left(TRUNCATED_BYTES))) ==
          RenderGraphPreviewLoad::Malformed);
    CHECK(preview.GetStatus().contains(QStringLiteral("rejected")) == true);
    expectKept();

    // A foreign CPU interface needs a restart and never replaces the drawing generation.
    QByteArray foreign = bytes;
    foreign[INTERFACE_OFFSET] = static_cast<char>(foreign.at(INTERFACE_OFFSET) ^ 0x5A);
    CHECK(preview.Load(WriteBytes(directory, QStringLiteral("foreign.lurgrt"), foreign)) ==
          RenderGraphPreviewLoad::RestartRequired);
    CHECK(preview.GetReport().Candidate == renderer::RenderGraphCandidateState::RestartRequired);
    CHECK(preview.GetStatus().contains(QStringLiteral("restart")) == true);
    expectKept();

    // A missing file is reported without touching the active generation.
    CHECK(preview.Load(QDir(directory.path()).filePath(QStringLiteral("missing.lurgrt"))) ==
          RenderGraphPreviewLoad::Unreadable);
    expectKept();

    // Pending replacement: until the new candidate activates and renders, generation 1 stays
    // shown. Every Stage takes a serial, including the restart-requiring attempt above.
    // Configurations may share a build generation; the exact runtime digest must match too.
    const RenderGraphPreviewSource wrongProvenance{QByteArrayLiteral("another-source"),
                                                   QStringLiteral("Default"),
                                                   provenance.Generation,
                                                   QByteArrayLiteral("wrong-runtime-package")};
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE), {}, wrongProvenance) ==
            RenderGraphPreviewLoad::Staged);
    CHECK(preview.GetActiveSource().InputIdentity == provenance.InputIdentity);
    const uint64 replacement = preview.GetReport().CandidateGeneration;
    CHECK(replacement > 1);
    CHECK(preview.GetImage() == green);
    CHECK(preview.GetPresentedGeneration() == 1);
    preview.SetExtraction(MakeExtraction({1, 0, 0, 1}, 2));
    bool hasRegressed = false;
    REQUIRE(TickUntil(preview, [&]() {
                hasRegressed =
                    hasRegressed || preview.GetImage().isNull() == true ||
                    (preview.GetPresentedGeneration() != 1 && preview.GetPresentedGeneration() != replacement);
                return preview.GetPresentedGeneration() == replacement && preview.GetPresentedRevision() == 2;
            }) == true);
    CHECK(hasRegressed == false);
    CheckCenter(preview.GetImage(), 255, 0, 0);
    const QImage red = preview.GetImage();
    CHECK(preview.GetActiveSource().InputIdentity.isEmpty() == true);

    // Stopping the viewport device keeps the last presented frame and reports it as such.
    preview.Detach();
    CHECK(preview.IsAttached() == false);
    CHECK(preview.GetStatistics().GetCpuMilliseconds().GetCount() == 0);
    CHECK(preview.GetStatistics().GetReadbackMilliseconds().GetCount() == 0);
    CHECK(preview.GetImage() == red);
    CHECK(preview.GetStatus().contains(QStringLiteral("last presented frame")) == true);
}

#if defined(Q_OS_MACOS)
TEST_CASE("asynchronous activation failure reaches the shell Error List and recovers independently of compilation",
          "[editor][render-graph][preview][diagnostics]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QTemporaryDir project;
    QTemporaryDir settings;
    REQUIRE(project.isValid() == true);
    REQUIRE(settings.isValid() == true);
    EditorController controller({}, nullptr, QDir(settings.path()).filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, QDir(settings.path()).filePath(QStringLiteral("workspace.json")));
    RendererWorkspace* workspaceOrNull = window.findChild<RendererWorkspace*>();
    ErrorList* errorsOrNull = window.findChild<ErrorList*>();
    REQUIRE(workspaceOrNull != nullptr);
    REQUIRE(errorsOrNull != nullptr);
    workspaceOrNull->SetProject(project.path(), 1);
    workspaceOrNull->SetLivePreview(false);
    RenderGraphPreview& preview = *workspaceOrNull->GetPreviewOrNull();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() != 0; }) == true);
    const QImage retained = preview.GetImage();
    const uint64 retainedGeneration = preview.GetPresentedGeneration();

    RenderGraphJobResult compiled;
    compiled.Request.ProjectEpoch = 1;
    compiled.Request.Action = QStringLiteral("build");
    compiled.Request.Revision = workspaceOrNull->GetDocument().GetRevision();
    compiled.IsSuccessful = true;
    workspaceOrNull->AcceptResult(compiled);
    CHECK(errorsOrNull->GetCount(ErrorListSeverity::Error) == 0);
    CHECK(errorsOrNull->GetCount(ErrorListSeverity::Message) == 1);
    QSignalSpy reported(errorsOrNull, &ErrorList::ProblemsReported);

    // Keep the runtime package valid but make native shader preparation fail on its worker lane.
    QFile runtime(GetDefaultRenderGraphRuntime());
    REQUIRE(runtime.open(QIODevice::ReadOnly) == true);
    QByteArray broken = runtime.readAll();
    const QByteArray validInclude = QByteArrayLiteral("#include <metal_stdlib>");
    const QByteArray brokenInclude = QByteArrayLiteral("#include <xxxxx_stdlib>");
    REQUIRE(broken.contains(validInclude) == true);
    REQUIRE(validInclude.size() == brokenInclude.size());
    broken.replace(validInclude, brokenInclude);
    REQUIRE(preview.Load(WriteBytes(project, QStringLiteral("broken.lurgrt"), broken)) ==
            RenderGraphPreviewLoad::Staged);
    CHECK(preview.GetImage() == retained);
    CHECK(errorsOrNull->GetCount(ErrorListSeverity::Error) == 0);
    REQUIRE(TickUntil(preview, [&]() {
                return preview.GetReport().Candidate == renderer::RenderGraphCandidateState::Failed;
            }) == true);
    CHECK(preview.GetReport().LastFailure == rhi::RasterStatus::Failed);
    CHECK(preview.GetPresentedGeneration() == retainedGeneration);
    CHECK(preview.GetImage() == retained);
    CHECK(workspaceOrNull->GetLastResult().IsSuccessful == true);
    CHECK(workspaceOrNull->GetActivationProblem().contains(QStringLiteral("Failed")) == true);
    CHECK(workspaceOrNull->GetActivationProblem().contains(QString::number(preview.GetReport().CandidateGeneration)) ==
          true);
    CHECK(workspaceOrNull->GetStatusText() == workspaceOrNull->GetActivationProblem());
    REQUIRE(errorsOrNull->GetCount(ErrorListSeverity::Error) == 1);
    CHECK(errorsOrNull->GetCount(ErrorListSeverity::Message) == 0);
    CHECK(errorsOrNull->GetEntries().first().Description == preview.GetDisplayStatus());
    CHECK(reported.count() == 1);
    REQUIRE(errorsOrNull->ActivateVisibleRow(0) == true);
    for (usize tick = 0; tick < 2; ++tick)
    {
        preview.Tick();
    }
    CHECK(reported.count() == 1);
    // Even a later successful compiler result cannot erase the current activation error.
    workspaceOrNull->AcceptResult(compiled);
    CHECK(errorsOrNull->GetCount(ErrorListSeverity::Error) == 1);
    CHECK(errorsOrNull->GetCount(ErrorListSeverity::Message) == 0);
    CHECK(workspaceOrNull->GetStatusText() == workspaceOrNull->GetActivationProblem());

    REQUIRE(preview.Load(GetDefaultRenderGraphRuntime()) == RenderGraphPreviewLoad::Staged);
    CHECK(preview.GetImage() == retained);
    CHECK(errorsOrNull->GetCount(ErrorListSeverity::Error) == 0);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() > retainedGeneration; }) == true);
    CHECK(workspaceOrNull->GetActivationProblem().isEmpty() == true);
    CHECK(workspaceOrNull->GetRenderProblem().isEmpty() == true);
    CHECK(preview.GetDisplayStatus().isEmpty() == true);
    CHECK(errorsOrNull->GetCount(ErrorListSeverity::Message) == 1);
    workspaceOrNull->SetProject({}, 2);
    CHECK(errorsOrNull->GetEntries().isEmpty() == true);
    preview.Detach();
}
#endif

TEST_CASE("Editors without OIDN preview a denoising graph unfiltered and say so",
          "[editor][render-graph][preview][denoising]")
{
    if (renderer::IsDenoisingAvailable() == true)
    {
        SKIP("This editor build has the OIDN denoiser");
    }
    QFile file(QStringLiteral(LUDUS_EDITOR_PROGRESSIVE_RAY_FIXTURE));
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    QByteArray bytes = file.readAll();
    const auto readWord = [&bytes](usize offset) {
        uint32 word = 0;
        std::memcpy(&word, bytes.constData() + offset, sizeof(word));
        return word;
    };
    constexpr usize RESOURCE_COUNT_FIELD = 2;
    constexpr usize PASS_COUNT_FIELD = 3;
    constexpr usize CAMERA_COUNT_FIELD = 4;
    const usize passesAt = format::HEADER_BYTES +
                           readWord(RESOURCE_COUNT_FIELD * sizeof(uint32)) * format::RESOURCE_RECORD_BYTES +
                           readWord(CAMERA_COUNT_FIELD * sizeof(uint32)) * format::CAMERA_RECORD_BYTES;
    REQUIRE(readWord(PASS_COUNT_FIELD * sizeof(uint32)) != 0);
    REQUIRE(readWord(passesAt) == static_cast<uint32>(format::Technique::RayTracing));
    // DenoisingQuality is what a Denoise node after this ray pass cooks to; later fields follow it.
    static_assert(offsetof(format::Pass, DenoisingQuality) == format::PASS_DENOISING_QUALITY_FIELD * sizeof(uint32));
    const usize qualityAt = passesAt + format::PASS_DENOISING_QUALITY_FIELD * sizeof(uint32);
    const uint32 quality = static_cast<uint32>(format::ProcessingQuality::Balanced);
    std::memcpy(bytes.data() + qualityAt, &quality, sizeof(quality));
    QTemporaryDir directory;
    const QString denoised = WriteBytes(directory, QStringLiteral("denoised.lurgrt"), bytes);
    DeviceFixture device({}, true);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    renderer::RenderGraphRenderer automatic;
    REQUIRE(renderer::SelectRenderGraphRenderer(device.Device, renderer::RenderGraphPreference::Automatic, automatic) ==
            rhi::RasterStatus::Ready);
    if (automatic != renderer::RenderGraphRenderer::HardwareRayTracing)
    {
        SKIP("Native ray queries are unavailable on this host");
    }
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    RenderGraphExtraction extraction = MakeExtraction({1, 0, 0, 1}, 1);
    extraction.View.OrthographicCamera = true;
    preview.SetExtraction(extraction);
    QSignalSpy presented(&preview, &RenderGraphPreview::FramePresented);
    // The graph previews as if its Denoise node were disabled, instead of failing at World extraction.
    REQUIRE(preview.Load(denoised) == RenderGraphPreviewLoad::Staged);
    constexpr int64 PIPELINE_PREPARATION_TIMEOUT_MILLISECONDS = 10000;
    constexpr uint32 PREPARATION_POLL_MICROSECONDS = 200;
    QElapsedTimer preparation;
    preparation.start();
    while (presented.count() == 0 && preparation.elapsed() < PIPELINE_PREPARATION_TIMEOUT_MILLISECONDS)
    {
        preview.Tick();
        QThread::usleep(PREPARATION_POLL_MICROSECONDS);
    }
    INFO(preview.GetStatus().toStdString());
    REQUIRE(presented.count() >= 1);
    CHECK(preview.GetImage().isNull() == false);
    CHECK(preview.GetStatus().contains(QStringLiteral("Denoise skipped")) == true);
    CHECK(preview.GetStatus().contains(QStringLiteral("rejected")) == false);
    preview.Detach();
}

TEST_CASE("Rejected ray extractions name their measured memory ceiling", "[editor][render-graph][preview]")
{
    renderer::SceneShaderDiagnostics diagnostics;
    diagnostics.RequiredBytes = 4096;
    diagnostics.AvailableBytes = 2048;
    for (const renderer::SceneMemoryBudget budget : {renderer::SceneMemoryBudget::CpuScene,
                                                     renderer::SceneMemoryBudget::CpuScratch,
                                                     renderer::SceneMemoryBudget::GpuPreview,
                                                     renderer::SceneMemoryBudget::RhiResources,
                                                     renderer::SceneMemoryBudget::DeviceStorageRange})
    {
        diagnostics.ExceededBudget = budget;
        const QString message = DescribeRenderGraphExtractionFailure(rhi::RasterStatus::CapacityExceeded, diagnostics);
        CHECK(message.contains(QStringLiteral("requires 4096 bytes; 2048 bytes available")) == true);
        CHECK(message.contains(QStringLiteral("shown frame is kept")) == true);
        switch (budget)
        {
            case renderer::SceneMemoryBudget::CpuScene:
                CHECK(message.contains(QStringLiteral("CPU scene preparation")) == true);
                break;
            case renderer::SceneMemoryBudget::CpuScratch:
                CHECK(message.contains(QStringLiteral("CPU scratch/upload")) == true);
                break;
            case renderer::SceneMemoryBudget::GpuPreview:
                CHECK(message.contains(QStringLiteral("GPU preview (including retained frames)")) == true);
                break;
            case renderer::SceneMemoryBudget::DeviceStorageRange:
                CHECK(message.contains(QStringLiteral("device storage-buffer binding")) == true);
                break;
            case renderer::SceneMemoryBudget::RhiResources:
                CHECK(message.contains(QStringLiteral("device-wide RHI resource")) == true);
                break;
            case renderer::SceneMemoryBudget::None:
                FAIL("Expected a measured memory ceiling");
        }
    }
    CHECK(DescribeRenderGraphExtractionFailure(rhi::RasterStatus::InvalidDescription, {})
              .contains(QStringLiteral("memory budget")) == false);
}

TEST_CASE("RG3b Renderer workspace previews only explicitly requested generations", "[editor][render-graph][preview]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    REQUIRE(workspace.GetPreviewOrNull() != nullptr);
    // Nothing is open: nothing to preview.
    CHECK(workspace.PreviewRuntime() == false);
    // Binding a project shows the engine default but never starts a preview by itself.
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.IsBuiltIn() == true);
    CHECK(workspace.GetPreviewOrNull()->GetPresentedGeneration() == 0);
    CHECK(workspace.GetPreviewOrNull()->GetTargetSize().isEmpty() == true);
    // The explicit request decodes the prebuilt default; it stages once a viewport device exists.
    CHECK(workspace.PreviewRuntime() == true);
    CHECK(workspace.GetPreviewOrNull()->GetTargetSize() ==
          QSize(static_cast<int32>(default_graph::RESOURCES[0].Width),
                static_cast<int32>(default_graph::RESOURCES[0].Height)));
    CHECK(workspace.GetPreviewOrNull()->GetPresentedGeneration() == 0);
    // The default's targets follow the drawable at 1/1, so the World image fills the viewport.
    CHECK(default_graph::RESOURCES[0].ViewNumerator == 1);
    CHECK(default_graph::RESOURCES[0].ViewDenominator == 1);
    const QSize drawable(1000, 562);
    workspace.GetPreviewOrNull()->SetReferenceSize(drawable);
    CHECK(workspace.GetPreviewOrNull()->GetTargetSize() == drawable);
}

TEST_CASE("The editor's prebuilt engine default previews with no project files", "[editor][render-graph][preview]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    const QString runtime = GetDefaultRenderGraphRuntime();
    REQUIRE(runtime.isEmpty() == false);
    const QSize extent(static_cast<int32>(default_graph::RESOURCES[0].Width),
                       static_cast<int32>(default_graph::RESOURCES[0].Height));
    RenderGraphPreview preview(
        {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
        shaders::renderer_composite::Vertex(),
        shaders::renderer_composite::Fragment());
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({.25F, .5F, .75F, 1}, 1));
    REQUIRE(preview.Load(runtime) == RenderGraphPreviewLoad::Staged);
    CHECK(preview.GetTargetSize() == extent);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; }) == true);
    const QImage image = preview.GetImage();
    REQUIRE(image.size() == extent);
    const QColor center = image.pixelColor(extent.width() / 2, extent.height() / 2);
    // The default graph's ToneMap applies PBR Neutral and sRGB encoding to the unlit tint's emission.
    CheckColor(center, MapDefaultGraphColor({.25F, .5F, .75F}));
    // The fixture's generation shares the host interface, so either previews in one editor.
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    preview.Detach();
}

namespace
{
/// The fixture's names sidecar, cooked beside its runtime package.
QByteArray ReadFixtureNames()
{
    QFile file(GetTargetNamesPath(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)));
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

/// Inspected depth at a texel, decoded independently of the viewer's helpers.
float32 GetInspectedDepth(const InspectedTexture& texture, uint32 x, uint32 y)
{
    REQUIRE(texture.Encoding == TexelEncoding::Depth32Float);
    REQUIRE(texture.IsValid() == true);
    float32 value = 0;
    std::memcpy(&value,
                texture.Bytes.constData() + (static_cast<usize>(y) * texture.Width + x) * sizeof(float32),
                sizeof(float32));
    return value;
}
} // namespace

TEST_CASE("Texture viewer names every generation texture and resolves canvas nodes", "[editor][texture-viewer]")
{
    const QByteArray bytes = ReadFixture();
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())}) ==
            renderer::RenderGraphPackageStatus::Ready);
    const QByteArray names = ReadFixtureNames();
    const RenderGraphTargetCatalogue catalogue =
        MakeTargetCatalogue(package.GetPlan(), package.GetGenerationIdentity(), names);
    INFO(catalogue.NamesProblem.toStdString());
    REQUIRE(catalogue.HasNames == true);
    const QByteArray runtimeIdentity = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    const RenderGraphTargetCatalogue exact =
        MakeTargetCatalogue(package.GetPlan(), package.GetGenerationIdentity(), names, runtimeIdentity);
    CHECK(exact.HasExactRuntimeNames == true);
    QJsonObject otherConfiguration = QJsonDocument::fromJson(names).object();
    otherConfiguration.insert(QStringLiteral("runtime_sha256"), QStringLiteral("another-runtime"));
    const RenderGraphTargetCatalogue mismatched = MakeTargetCatalogue(package.GetPlan(),
                                                                      package.GetGenerationIdentity(),
                                                                      QJsonDocument(otherConfiguration).toJson(),
                                                                      runtimeIdentity);
    CHECK(mismatched.HasNames == false);
    CHECK(mismatched.NamesProblem.contains(QStringLiteral("configuration")) == true);
    otherConfiguration.remove(QStringLiteral("runtime_sha256"));
    const RenderGraphTargetCatalogue legacy = MakeTargetCatalogue(package.GetPlan(),
                                                                  package.GetGenerationIdentity(),
                                                                  QJsonDocument(otherConfiguration).toJson(),
                                                                  runtimeIdentity);
    CHECK(legacy.HasNames == true);
    CHECK(legacy.HasExactRuntimeNames == false);

    // Every texture, not only the export: the transient depth target is listed too.
    REQUIRE(catalogue.Targets.size() == 2);
    const qsizetype color = catalogue.Find(QStringLiteral("Main/color"));
    const qsizetype depth = catalogue.Find(QStringLiteral("Main/depth"));
    REQUIRE(color >= 0);
    REQUIRE(depth >= 0);
    CHECK(catalogue.Targets[color].IsExported == true);
    CHECK(catalogue.Targets[depth].IsDepth == true);
    CHECK(catalogue.Targets[depth].IsExported == false);
    CHECK(DescribeTarget(catalogue.Targets[color]) ==
          QStringLiteral("96 × 64 · RGBA8Unorm · 1 mip, 1 layer · colour attachment of Main/raster/draw (Clear → "
                         "Store) · exported by Main/output"));
    CHECK(DescribeTarget(catalogue.Targets[depth])
              .contains(QStringLiteral(
                  "Depth32Float · 1 mip, 1 layer · depth attachment of Main/raster/draw (Clear → Store)")) == true);
    CHECK(DescribeTarget(catalogue.Targets[depth]).contains(QStringLiteral("transient")) == true);

    // Canvas nodes of the root document: Target, Subgraph (its Raster's attachments), Export, Camera.
    const QStringList both{QStringLiteral("Main/color"), QStringLiteral("Main/depth")};
    CHECK(ResolveCanvasTargets(catalogue, QStringLiteral("Main"), QStringLiteral("depth")) ==
          QStringList{QStringLiteral("Main/depth")});
    CHECK(ResolveCanvasTargets(catalogue, QStringLiteral("Main"), QStringLiteral("raster")) == both);
    CHECK(ResolveCanvasTargets(catalogue, QStringLiteral("Main"), QStringLiteral("output")) ==
          QStringList{QStringLiteral("Main/color")});
    CHECK(ResolveCanvasTargets(catalogue, QStringLiteral("Main"), QStringLiteral("camera")) ==
          QStringList{QStringLiteral("Main/color")});
    CHECK(ResolveCanvasTargets(catalogue, QStringLiteral("Main"), QStringLiteral("scene")).isEmpty() == true);
    // The Raster node inside the subgraph document, opened on its own, matches every instance.
    CHECK(ResolveCanvasTargets(catalogue, QStringLiteral("RasterizeGraph"), QStringLiteral("draw")) == both);

    // A sidecar from another generation is refused; textures keep numeric names and uses.
    QByteArray stale = names;
    const qsizetype at = stale.indexOf("\"generation\": \"") + qsizetype{sizeof("\"generation\": \"")} - 1;
    stale[at] = stale.at(at) == '0' ? '1' : '0';
    const RenderGraphTargetCatalogue numeric =
        MakeTargetCatalogue(package.GetPlan(), package.GetGenerationIdentity(), stale);
    CHECK(numeric.HasNames == false);
    CHECK(numeric.NamesProblem.contains(QStringLiteral("another generation")) == true);
    REQUIRE(numeric.Targets.size() == 2);
    CHECK(numeric.Targets[0].Name == QStringLiteral("Resource 0"));
    CHECK(DescribeTarget(numeric.Targets[depth]).contains(QStringLiteral("depth attachment of pass 0")) == true);
    CHECK(ResolveCanvasTargets(numeric, QStringLiteral("Main"), QStringLiteral("depth")).isEmpty() == true);
    CHECK(MakeTargetCatalogue(package.GetPlan(), package.GetGenerationIdentity(), QByteArrayLiteral("[]"))
              .NamesProblem.isEmpty() == false);
}

TEST_CASE("Texture viewer reads colour and depth targets back without stalling", "[editor][texture-viewer]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({.25F, .5F, .75F, 1}, 1));
    // Selected before any generation exists: resolved by name once one is active.
    preview.SetInspectedTarget(QStringLiteral("Main/depth"));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetInspectedTexture().IsValid() == true; }) == true);
    INFO(preview.GetInspectionStatus().toStdString());
    CHECK(preview.GetTargetCatalogue().HasNames == true);
    const InspectedTexture depth = preview.GetInspectedTexture();
    REQUIRE(depth.Encoding == TexelEncoding::Depth32Float);
    REQUIRE(depth.Width == static_cast<uint32>(WIDTH));
    REQUIRE(depth.Height == static_cast<uint32>(HEIGHT));
    CHECK(preview.GetInspectedGeneration() == 1);
    CHECK(preview.GetInspectedRevision() == 1);
    // The triangle's plane depth is exact (no colour quantization); the cleared background is 1.
    const float32 expected = GetDepth(renderer::ViewDescription{}, {0, 0, .5F});
    CHECK(GetInspectedDepth(depth, WIDTH / 2, HEIGHT / 2) == expected);
    CHECK(GetInspectedDepth(depth, 0, 0) == 1.0F);
    CHECK(DescribeTexel(depth, WIDTH / 2, HEIGHT / 2).contains(QStringLiteral("depth 0.5 (0x3f000000)")) == true);

    // The colour target (here also the export) equals the presented frame byte for byte.
    preview.SetInspectedTarget(QStringLiteral("Main/color"));
    REQUIRE(TickUntil(preview, [&]() {
                return preview.GetInspectedTexture().Encoding == TexelEncoding::Rgba8Unorm &&
                       preview.GetInspectedTexture().IsValid() == true;
            }) == true);
    const QImage& image = preview.GetImage();
    const InspectedTexture color = preview.GetInspectedTexture();
    REQUIRE(color.Bytes.size() == image.sizeInBytes());
    CHECK(std::memcmp(color.Bytes.constData(), image.constBits(), static_cast<usize>(color.Bytes.size())) == 0);

    // An unknown name reports itself instead of showing a stale texture.
    preview.SetInspectedTarget(QStringLiteral("Main/missing"));
    REQUIRE(TickUntil(preview, [&]() { return preview.GetInspectionStatus().isEmpty() == false; }) == true);
    CHECK(preview.GetInspectionStatus().contains(QStringLiteral("not a texture")) == true);
    CHECK(preview.GetInspectedTexture().IsValid() == false);
    preview.Detach();
}

TEST_CASE("Texture viewer readback is polled across ticks and reports undefined contents", "[editor][texture-viewer]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; }) == true);
    // The reader is driven directly here: Begin only records requests and returns.
    RenderTargetReader reader;
    rhi::TextureDescription description;
    description.Width = WIDTH;
    description.Height = HEIGHT;
    description.Attachment = true;
    rhi::TextureHandle never;
    REQUIRE(rhi::CreateTexture(device.Device, description, {}, never) == rhi::RasterStatus::Ready);
    // A never-written attachment has no stored colour or depth to show.
    CHECK(reader.Begin(device.Device, device.Surface, never, rhi::TextureAspect::Color, WIDTH, HEIGHT) ==
          RenderTargetReadStatus::Undefined);
    CHECK(reader.Begin(device.Device, device.Surface, never, rhi::TextureAspect::Depth, WIDTH, HEIGHT) ==
          RenderTargetReadStatus::Undefined);
    CHECK(reader.IsBusy() == false);

    // A written attachment: the depth pack draw and band copies complete over later polls.
    rhi::TextureHandle written;
    REQUIRE(rhi::CreateTexture(device.Device, description, {}, written) == rhi::RasterStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.Color = written;
    pass.DepthStore = rhi::RasterStore::Store;
    pass.ClearDepth = .25F;
    REQUIRE(rhi::BeginFrame(device.Device, device.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginRasterPass(device.Device, pass) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(device.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(reader.Begin(device.Device, device.Surface, written, rhi::TextureAspect::Depth, WIDTH, HEIGHT) ==
            RenderTargetReadStatus::Pending);
    CHECK(reader.IsBusy() == true);
    RenderTargetReadStatus status = reader.Poll();
    for (usize attempt = 0; status == RenderTargetReadStatus::Pending && attempt < ATTEMPTS; ++attempt)
    {
        QThread::usleep(200);
        status = reader.Poll();
    }
    REQUIRE(status == RenderTargetReadStatus::Complete);
    // At least the first poll only issued band requests: the read never waited inside a call.
    CHECK(reader.GetPendingPolls() >= 1);
    const InspectedTexture depth = reader.TakeTexture();
    CHECK(reader.IsBusy() == false);
    CHECK(GetInspectedDepth(depth, 0, 0) == .25F);
    CHECK(GetInspectedDepth(depth, WIDTH - 1, HEIGHT - 1) == .25F);
    reader.Release();
    static_cast<void>(rhi::Destroy(device.Device, written));
    static_cast<void>(rhi::Destroy(device.Device, never));
    preview.Detach();
}

TEST_CASE("Diagnostic readback yields shared staging capacity to viewport output",
          "[editor][texture-viewer][inspection-scheduling]")
{
    constexpr uint32 POLL_MICROSECONDS = 200;
    rhi::StorageLimits storage;
    storage.TransferBytes = SINGLE_ROW_TRANSFER_BYTES;
    storage.TransferSlices = 1;
    DeviceFixture device(storage);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    rhi::TextureDescription description;
    description.Width = WIDTH;
    description.Height = HEIGHT;
    description.Attachment = true;
    rhi::TextureHandle source;
    REQUIRE(rhi::CreateTexture(device.Device, description, {}, source) == rhi::RasterStatus::Ready);
    rhi::RasterPassDescription pass;
    pass.Color = source;
    pass.ColorStore = rhi::RasterStore::Store;
    REQUIRE(rhi::BeginFrame(device.Device, device.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginRasterPass(device.Device, pass) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken producer;
    REQUIRE(rhi::EndFrame(device.Device, producer) == rhi::RasterStatus::Ready);
    rhi::RasterStatus producerStatus = rhi::GetStatus(device.Device, producer);
    for (usize attempt = 0; producerStatus == rhi::RasterStatus::Pending && attempt < ATTEMPTS; ++attempt)
    {
        QThread::usleep(POLL_MICROSECONDS);
        producerStatus = rhi::GetStatus(device.Device, producer);
    }
    REQUIRE(producerStatus == rhi::RasterStatus::Ready);

    RenderTargetReader inspectionReader;
    RenderTargetReader outputReader;
    REQUIRE(
        inspectionReader.Begin(device.Device, device.Surface, source, rhi::TextureAspect::Color, WIDTH, HEIGHT, true) ==
        RenderTargetReadStatus::Pending);
    for (usize attempt = 0; inspectionReader.IsSourcePending() == true && attempt < ATTEMPTS; ++attempt)
    {
        REQUIRE(inspectionReader.Poll() == RenderTargetReadStatus::Pending);
        QThread::usleep(POLL_MICROSECONDS);
    }
    REQUIRE(inspectionReader.IsSourcePending() == false);
    REQUIRE(outputReader.Begin(device.Device, device.Surface, source, rhi::TextureAspect::Color, WIDTH, HEIGHT) ==
            RenderTargetReadStatus::Pending);
    RenderTargetReadStatus outputStatus = RenderTargetReadStatus::Pending;
    for (usize attempt = 0; outputStatus == RenderTargetReadStatus::Pending && attempt < ATTEMPTS; ++attempt)
    {
        REQUIRE(inspectionReader.Poll(false) == RenderTargetReadStatus::Pending);
        outputStatus = outputReader.Poll();
        QThread::usleep(POLL_MICROSECONDS);
    }
    REQUIRE(outputStatus == RenderTargetReadStatus::Complete);
    CHECK(outputReader.TakeTexture().IsValid() == true);
    CHECK(inspectionReader.IsBusy() == true);
    CHECK(inspectionReader.Poll(false) == RenderTargetReadStatus::Pending);
    inspectionReader.Release();
    outputReader.Release();
    static_cast<void>(rhi::Destroy(device.Device, source));
}

TEST_CASE("Hidden texture viewers stop inspection and resume the preserved selection",
          "[editor][texture-viewer][inspection-scheduling]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    RenderGraphPreview preview = MakePreview();
    QWidget container;
    TextureViewer viewer(&container);
    viewer.show();
    viewer.SetPreview(&preview);
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    viewer.SelectTarget(QStringLiteral("Main/color"));
    QSignalSpy inspected(&preview, &RenderGraphPreview::TargetInspected);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 1; }) == true);
    CHECK(inspected.count() == 0);
    CHECK(viewer.GetDisplayImage().isNull() == true);
    CHECK(preview.GetStatistics().GetInspectionReadbackMilliseconds().GetCount() == 0);

    // A completion handler can hide an ancestor synchronously. Its completed timing must
    // remain valid even though hiding invalidates the next refresh and cancels reader ownership.
    QObject::connect(&viewer, &TextureViewer::DisplayUpdated, &container, &QWidget::hide, Qt::SingleShotConnection);
    QElapsedTimer observationTimer;
    observationTimer.start();
    container.show();
    REQUIRE(TickUntilDisplay(preview, [&]() { return viewer.GetDisplayImage().isNull() == false; }) == true);
    const isize completed = inspected.count();
    REQUIRE(completed == 1);
    CHECK(container.isVisible() == false);
    CHECK(preview.GetStatistics().IsInspectionEnabled() == false);
    CHECK(preview.GetStatistics().GetInspectionCpuMilliseconds().GetCount() == 1);
    CHECK(preview.GetStatistics().GetInspectionCpuMilliseconds().GetAverage() > 0);
    CHECK(preview.GetStatistics().GetInspectionReadbackMilliseconds().GetAverage() > 0);
    CHECK(preview.GetStatistics().GetConversionWorkMilliseconds().GetCount() == 1);
    CHECK(preview.GetStatistics().GetConversionElapsedMilliseconds().GetAverage() >=
          preview.GetStatistics().GetConversionWorkMilliseconds().GetAverage());
    CHECK(preview.GetStatistics().GetInspectionReadbackMilliseconds().GetAverage() <=
          static_cast<float64>(observationTimer.nsecsElapsed()) / 1'000'000.0);
    const QImage accepted = viewer.GetDisplayImage();

    // Cancel a newly requested read, then keep rendering with the viewer hidden.
    container.show();
    viewer.SelectTarget(QStringLiteral("Main/depth"));
    preview.Tick();
    container.hide();
    preview.SetExtraction(MakeExtraction({1, 0, 0, 1}, 2));
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 2; }) == true);
    CHECK(inspected.count() == completed);
    CHECK(viewer.GetSelectedTarget() == QStringLiteral("Main/depth"));
    CHECK(viewer.GetDisplayImage() == accepted);
    container.show();
    REQUIRE(TickUntilDisplay(preview, [&]() { return viewer.GetDisplayedRevision() == 2; }) == true);
    CHECK(preview.GetInspectedTexture().Encoding == TexelEncoding::Depth32Float);
    CHECK(preview.GetStatistics().IsInspectionEnabled() == true);
    preview.Detach();
}

TEST_CASE("Texture inspection snapshots preserve source pixels while later frames render",
          "[editor][texture-viewer][inspection-scheduling]")
{
    rhi::StorageLimits storage;
    storage.TransferBytes = SINGLE_ROW_TRANSFER_BYTES;
    storage.TransferSlices = 1;
    DeviceFixture device(storage);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    RenderGraphPreview preview = MakePreview();
    preview.SetDirectPresentation(true);
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 1; }) == true);
    QString target = QStringLiteral("Main/color");
    bool isDepth = false;
    SECTION("colour snapshot") {}
    SECTION("depth snapshot")
    {
        target = QStringLiteral("Main/depth");
        isDepth = true;
    }
    QSignalSpy submitted(&preview, &RenderGraphPreview::SubmissionRecorded);
    preview.SetInspectedTarget(target);
    preview.Tick(); // Start the independent snapshot of revision 1.
    RenderScene nextScene = MakeScene({1, 0, 0, 1});
    for (math::Vector3& positionMesh : nextScene.Meshes[0].Positions)
    {
        positionMesh.Z = .25F;
    }
    RenderGraphExtraction nextExtraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(nextScene, {}, 2, nextExtraction, error) == true);
    preview.SetExtraction(nextExtraction);
    REQUIRE(TickUntil(preview, [&]() { return submitted.count() > 0; }) == true);
    // Submission proceeds before the one-row band readback finishes, regardless of GPU speed.
    CHECK(preview.GetInspectedTexture().IsValid() == false);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetInspectedTexture().IsValid() == true; }) == true);
    CHECK(preview.GetInspectedRevision() == 1);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 2; }) == true);
    if (isDepth == true)
    {
        CHECK(GetInspectedDepth(preview.GetInspectedTexture(), WIDTH / 2, HEIGHT / 2) == .5F);
        CHECK(GetInspectedDepth(preview.GetInspectedTexture(), 0, 0) == 1.0F);
    }
    else
    {
        TextureValueRange range;
        const QImage captured = MakeDisplayImage(preview.GetInspectedTexture(), {}, range);
        CHECK(captured.pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(0, 255, 0));
    }
    preview.Detach();
}

TEST_CASE("Texture viewer follows Live Preview frames and rebuilt generations by name", "[editor][texture-viewer]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    RenderGraphPreview preview = MakePreview();
    TextureViewer viewer;
    viewer.SetPreview(&preview);
    viewer.show();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntilDisplay(preview, [&]() { return preview.GetPresentedGeneration() == 1; }) == true);
    // Canvas selection: the Raster subgraph node selects its colour attachment, then its depth.
    REQUIRE(viewer.SelectCanvasNode(QStringLiteral("Main"), QStringLiteral("raster")) == true);
    CHECK(viewer.GetSelectedTarget() == QStringLiteral("Main/color"));
    REQUIRE(viewer.SelectCanvasNode(QStringLiteral("Main"), QStringLiteral("raster")) == true);
    CHECK(viewer.GetSelectedTarget() == QStringLiteral("Main/depth"));
    CHECK(viewer.SelectCanvasNode(QStringLiteral("Main"), QStringLiteral("scene")) == false);
    viewer.SelectCanvasNode(QStringLiteral("Main"), QStringLiteral("color"));
    REQUIRE(viewer.GetSelectedTarget() == QStringLiteral("Main/color"));
    REQUIRE(TickUntilDisplay(preview, [&]() { return viewer.GetDisplayImage().isNull() == false; }) == true);
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(0, 255, 0));
    CHECK(viewer.GetInfoText().startsWith(QStringLiteral("Main/color · 96 × 64 · RGBA8Unorm")) == true);
    viewer.ProbeTexel(WIDTH / 2, HEIGHT / 2);
    CHECK(viewer.GetProbeText() ==
          QStringLiteral("Pinned (48, 32)  R 0 (0.0000)  G 255 (1.0000)  B 0 (0.0000)  A 255 (1.0000)"));

    // A new World revision re-reads the selected target after its frame is presented.
    preview.SetExtraction(MakeExtraction({1, 0, 0, 1}, 2));
    REQUIRE(TickUntilDisplay(preview, [&]() { return viewer.GetDisplayedRevision() == 2; }) == true);
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(255, 0, 0));
    CHECK(viewer.GetProbeText().contains(QStringLiteral("R 255")) == true);

    // A rebuilt generation (Live Preview republishes the runtime) keeps the selection by name.
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    const uint64 rebuilt = preview.GetReport().CandidateGeneration;
    REQUIRE(TickUntilDisplay(preview, [&]() { return viewer.GetDisplayedGeneration() == rebuilt; }) == true);
    CHECK(viewer.GetSelectedTarget() == QStringLiteral("Main/color"));
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(255, 0, 0));

    // Depth remapped to its automatic range: the flat triangle is the range floor (black) and the
    // background, left out of the range, clamps to white.
    viewer.SelectTarget(QStringLiteral("Main/depth"));
    REQUIRE(TickUntilDisplay(preview, [&]() {
                return preview.GetInspectedTexture().Encoding == TexelEncoding::Depth32Float &&
                       preview.GetInspectedTexture().IsValid() == true &&
                       viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(0, 0, 0);
            }) == true);
    CHECK(viewer.GetDisplaySettings().Range == TextureRangeMode::Automatic);
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(0, 0, 0));
    CHECK(viewer.GetDisplayImage().pixelColor(0, 0) == QColor(255, 255, 255));
    // Back on colour: the measured depth range does not leak into the normalized colour range.
    viewer.SelectTarget(QStringLiteral("Main/color"));
    REQUIRE(TickUntilDisplay(preview, [&]() {
                return preview.GetInspectedTexture().Encoding == TexelEncoding::Rgba8Unorm &&
                       preview.GetInspectedTexture().IsValid() == true &&
                       viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(255, 0, 0);
            }) == true);
    CHECK(viewer.GetDisplaySettings().Range == TextureRangeMode::Manual);
    CHECK(viewer.GetDisplaySettings().Minimum == 0.0F);
    CHECK(viewer.GetDisplaySettings().Maximum == 1.0F);
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(255, 0, 0));
    // Queued control changes cannot publish after hiding or detaching the preview.
    const QImage retained = viewer.GetDisplayImage();
    QSignalSpy displayed(&viewer, &TextureViewer::DisplayUpdated);
    TextureDisplaySettings controls = viewer.GetDisplaySettings();
    controls.Channel = TextureChannel::Green;
    viewer.SetDisplaySettings(controls);
    controls.Channel = TextureChannel::Red;
    viewer.SetDisplaySettings(controls);
    viewer.hide();
    QApplication::processEvents();
    CHECK(displayed.count() == 0);
    CHECK(viewer.GetDisplayImage() == retained);
    viewer.show();
    REQUIRE(TickUntilDisplay(preview, [&]() { return displayed.count() == 1; }) == true);
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(255, 255, 255));
    controls.Channel = TextureChannel::Green;
    viewer.SetDisplaySettings(controls);
    preview.Detach();
    QApplication::processEvents();
    CHECK(displayed.count() == 1);
    CHECK(preview.GetStatistics().GetConversionWorkMilliseconds().GetCount() == 0);
}

TEST_CASE("canvas resource inspection opens the existing viewer and shares its GPU readback",
          "[editor][render-graph][resource-inspection-preview]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QTemporaryDir project;
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT) + QStringLiteral("/tests/sdk_consumer/rg2/offscreen"));
    for (const QString& file : {QStringLiteral("main.lurender.json"), QStringLiteral("rasterize.lurender.json")})
    {
        REQUIRE(QFile::copy(source.filePath(file), QDir(project.path()).filePath(file)) == true);
    }
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    REQUIRE(workspace.OpenGraph(QDir(project.path()).filePath(QStringLiteral("main.lurender.json"))) == true);
    workspace.show();
    RenderGraphPreview* previewOrNull = workspace.GetPreviewOrNull();
    TextureViewer* viewerOrNull = workspace.findChild<TextureViewer*>();
    RenderGraphResourceInspector* inspectorOrNull = workspace.findChild<RenderGraphResourceInspector*>();
    REQUIRE(previewOrNull != nullptr);
    REQUIRE(viewerOrNull != nullptr);
    REQUIRE(inspectorOrNull != nullptr);
    RenderGraphPreview& preview = *previewOrNull;
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Waiting);
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() != 0; }) == true);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    // Settle the initial show/resize fit before taking the navigation baseline.
    QApplication::processEvents();
    canvas.SelectNode(QStringLiteral("color"));
    RenderGraphNavigation* navigationOrNull = canvas.findChild<RenderGraphNavigation*>();
    REQUIRE(navigationOrNull != nullptr);
    navigationOrNull->SetTraceDirection(RenderGraphTraceDirection::Downstream);
    const RenderGraphViewState state = canvas.CaptureViewState();
    const QByteArray before = workspace.GetDocument().GetDraft(QStringLiteral("main.lurender.json"));
    Q_EMIT canvas.ResourceInspectionRequested(QStringLiteral("raster.depth"), true);
    CHECK(inspectorOrNull->CanOpenInTextures() == true);
    CHECK(inspectorOrNull->GetStatusText().startsWith(QStringLiteral("Unverified")) == true);
    QPushButton* openOrNull = inspectorOrNull->findChild<QPushButton*>(QStringLiteral("renderGraphOpenInTextures"));
    REQUIRE(openOrNull != nullptr);
    openOrNull->click();
    CHECK(viewerOrNull->GetSelectedTarget() == QStringLiteral("Main/depth"));
    REQUIRE(TickUntilDisplay(preview, [&]() {
                return preview.GetInspectedTarget() == QStringLiteral("Main/depth") &&
                       preview.GetInspectedTexture().Encoding == TexelEncoding::Depth32Float &&
                       preview.GetInspectedTexture().IsValid() == true &&
                       viewerOrNull->GetDisplayImage().isNull() == false;
            }) == true);
    CHECK(viewerOrNull->GetDisplayImage().isNull() == false);
    inspectorOrNull->SetPinned(true);
    Q_EMIT canvas.NodeSelected(QStringLiteral("color"));
    CHECK(viewerOrNull->GetSelectedTarget() == QStringLiteral("Main/depth"));
    CHECK(canvas.GetSelectedNodes() == state.Selection);
    CHECK(canvas.CaptureViewState().Trace == state.Trace);
    CHECK(canvas.CaptureViewState().CentreScene == state.CentreScene);
    CHECK(canvas.CaptureViewState().Zoom == state.Zoom);
    CHECK(workspace.GetDocument().GetDraft(QStringLiteral("main.lurender.json")) == before);
    inspectorOrNull->SetPinned(false);
    Q_EMIT canvas.NodeSelected(QStringLiteral("color"));
    CHECK(viewerOrNull->GetSelectedTarget() == QStringLiteral("Main/color"));
    const QImage retained = preview.GetImage();
    preview.Detach();
    CHECK(inspectorOrNull->CanOpenInTextures() == false);
    CHECK(preview.GetImage() == retained);
    inspectorOrNull->close();
}

TEST_CASE("Cold project shader timeout keeps the default rendering the current World",
          "[editor][render-graph][world-graph][fallback]")
{
    constexpr int32 TIMEOUT_MILLISECONDS = 60000;
    const QString compiler = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/shader-tools/slang/bin/slangc");
    const QString python = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/host-tools/venv/bin/python");
    if (QFileInfo(compiler).isExecutable() == false || QFileInfo(python).isExecutable() == false)
    {
        SKIP("pinned shader tools are required for the project recovery test");
    }
    rhi::Shutdown();
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const ToolingPaths tooling{python, {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)};
    {
        RendererWorkspace seed(tooling);
        seed.SetProject(project.path(), 1);
        REQUIRE(seed.CustomizeBuiltIn() == true);
    }
    const QString helper = QDir(project.path()).filePath(QStringLiteral("shader-helper"));
    REQUIRE(QDir().mkpath(helper) == true);
    const QDir originalHelper(QDir(QFileInfo(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)).absolutePath())
                                  .filePath(QStringLiteral("helper")));
    for (const QString& file : originalHelper.entryList(QDir::Files))
    {
        REQUIRE(QFile::copy(originalHelper.filePath(file), QDir(helper).filePath(file)) == true);
    }
    const QString helperScript = QDir(helper).filePath(QStringLiteral("compile_shader.py"));
    QFile originalScript(helperScript);
    REQUIRE(originalScript.open(QIODevice::ReadOnly) == true);
    const QByteArray script = originalScript.readAll();
    originalScript.close();
    const auto writeHelper = [&](const QByteArray& bytes) {
        QSaveFile output(helperScript);
        REQUIRE(output.open(QIODevice::WriteOnly) == true);
        REQUIRE(output.write(bytes) == bytes.size());
        REQUIRE(output.commit() == true);
    };
    // Exercise the real job/error mapping without waiting for the worker's 120-second deadline.
    writeHelper(
        QByteArrayLiteral("import sys\nprint('Ludus shader build: timed out', file=sys.stderr)\nsys.exit(1)\n"));
    RendererWorkspace workspace(tooling);
    RenderPreview source;
    PreviewViewport viewport(&source);
    viewport.resize(800, 600);
    viewport.show();
    viewport.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&source);
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.IsBuiltIn() == false);
    workspace.SetBuildTools(helper, compiler);
    // SPIR-V profiles require the validator; without it the build stops at
    // RenderGraphBuildTools before the shader timeout this case exercises.
    const QString validator = QStringLiteral(LUDUS_EDITOR_TEST_SPIRV_VALIDATOR);
    if (QFileInfo(validator).isExecutable() == true)
    {
        RenderGraphBuildSettings settings = workspace.GetBuildSettings();
        settings.Tools.Validator = validator;
        workspace.SetBuildSettings(settings);
    }
    workspace.EnableWorldRendering();
    QSignalSpy failed(&source, &RenderPreview::Failed);
    const auto waitFor = [&](const auto& isReady) {
        QElapsedTimer timer;
        timer.start();
        while (isReady() == false && timer.elapsed() < TIMEOUT_MILLISECONDS)
        {
            QTest::qWait(1);
            workspace.ServicePreview();
        }
        INFO(workspace.GetStatusText().toStdString());
        INFO(workspace.GetPreviewOrNull()->GetStatus().toStdString());
        return isReady();
    };
    REQUIRE(workspace.BuildAndPreview() == true);
    REQUIRE(waitFor([&]() {
                return workspace.GetLastResult().Request.Request != 0 && workspace.IsJobActive() == false;
            }) == true);
    REQUIRE(workspace.GetLastResult().IsSuccessful == false);
    CHECK(workspace.GetLastResult().Code == QStringLiteral("RenderGraphBuildShader"));
    CHECK(workspace.GetRenderProblem().contains(QStringLiteral("timed out")) == true);
    CHECK(workspace.GetScenePreparationProblem().isEmpty() == true);
    REQUIRE(source.SetScene(MakeScene({1, 0, 0, 1})) == true);
    REQUIRE(source.Start() == true);
    WorldGraphView* graphViewOrNull = viewport.findChild<WorldGraphView*>();
    REQUIRE(graphViewOrNull != nullptr);
    REQUIRE(waitFor([&]() { return graphViewOrNull->GetImage().isNull() == false; }) == true);
    CHECK(workspace.IsUsingDefaultRenderer() == true);
    CHECK(source.FrameAvailable() == true);
    CHECK(failed.isEmpty() == true);
    QLabel* statusOrNull = viewport.findChild<QLabel*>(QStringLiteral("worldGraphStatus"));
    REQUIRE(statusOrNull != nullptr);
    CHECK(statusOrNull->text().contains(QStringLiteral("timed out")) == true);
    CHECK(statusOrNull->text().contains(QStringLiteral("Engine default renderer fallback")) == true);
    const QImage initial = graphViewOrNull->GetImage();
    const uint64 generation = workspace.GetPreviewOrNull()->GetPresentedGeneration();
    const uint64 revision = workspace.GetPreviewOrNull()->GetPresentedRevision();
    RenderScene changed = MakeScene({0, 1, 0, 1});
    changed.Identity = QByteArrayLiteral("changed-world-during-shader-failure");
    REQUIRE(source.SetScene(changed) == true);
    REQUIRE(waitFor([&]() {
                return workspace.GetPreviewOrNull()->GetPresentedRevision() > revision &&
                       graphViewOrNull->GetImage() != initial;
            }) == true);
    CHECK(workspace.GetPreviewOrNull()->GetPresentedGeneration() == generation);
    const uint64 beforeNavigation = workspace.GetPreviewOrNull()->GetPresentedRevision();
    source.Orbit(20, 10);
    REQUIRE(waitFor([&]() { return workspace.GetPreviewOrNull()->GetPresentedRevision() > beforeNavigation; }) == true);
    source.Stop();
    QCoreApplication::processEvents();
    REQUIRE(source.Start() == true);
    REQUIRE(waitFor([&]() { return source.FrameAvailable() == true; }) == true);
    CHECK(workspace.GetRenderProblem().contains(QStringLiteral("timed out")) == true);
    CHECK(failed.isEmpty() == true);
    writeHelper(script);
    REQUIRE(workspace.BuildAndPreview() == true);
    REQUIRE(waitFor([&]() { return workspace.GetPreviewOrNull()->GetPresentedGeneration() > generation; }) == true);
    CHECK(workspace.IsUsingDefaultRenderer() == false);
    CHECK(workspace.GetRenderProblem().isEmpty() == true);
    CHECK(workspace.GetScenePreparationProblem().isEmpty() == true);
    CHECK(failed.isEmpty() == true);
    source.Stop();
    workspace.SetPreviewSource(nullptr);
    rhi::Shutdown();
}

TEST_CASE("Graph viewport Stats toggle is visible and persists with navigation",
          "[editor][render-graph][graph-statistics]")
{
    constexpr int32 STATISTICS_VIEWPORT_WIDTH = 1200;
    constexpr int32 STATISTICS_VIEWPORT_HEIGHT = 600;
    RendererWorkspace workspace({});
    RenderPreview source;
    PreviewViewport viewport(&source);
    viewport.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&source);
    QToolButton* statistics = viewport.findChild<QToolButton*>(QStringLiteral("viewportStatistics"));
    QLabel* overlay = viewport.findChild<QLabel*>(QStringLiteral("worldGraphStatisticsOverlay"));
    REQUIRE(statistics != nullptr);
    REQUIRE(overlay != nullptr);
    // The controls live in a toolbar, which only reveals its widgets once it is laid out.
    viewport.resize(STATISTICS_VIEWPORT_WIDTH, STATISTICS_VIEWPORT_HEIGHT);
    viewport.show();
    QApplication::processEvents();
    CHECK(statistics->isVisible() == true);
    CHECK(overlay->isHidden() == true);
    statistics->click();
    CHECK(source.IsStatisticsVisible() == true);
    CHECK(overlay->isHidden() == false);
    CHECK(overlay->testAttribute(Qt::WA_TransparentForMouseEvents) == true);
    CHECK(viewport.GetStatisticsText().contains(QStringLiteral("FPS (updates):")) == true);
    CHECK(viewport.GetStatisticsText().contains(QStringLiteral("Output readback:")) == true);
    const QJsonObject navigation = source.Navigation();
    statistics->click();
    CHECK(overlay->isHidden() == true);
    CHECK(viewport.GetStatisticsText().isEmpty() == true);
    source.RestoreNavigation(navigation);
    CHECK(statistics->isChecked() == true);
    CHECK(overlay->isHidden() == false);
    workspace.SetPreviewSource(nullptr);
}

TEST_CASE("World viewport follows project shaders and keeps drawable output after a failed edit",
          "[editor][render-graph][world-graph]")
{
    // Each edit recooks the whole deferred default, as render_graph_default_tests.cpp's build does.
    constexpr int32 TIMEOUT_MILLISECONDS = 120000;
    // Display codes a sample may lose to grid blending and lighting below the mapped primary emission,
    // or gain above the mapped other channels.
    constexpr int32 PRIMARY_MARGIN = 55;
    constexpr int32 DARK_MARGIN = 10;
    constexpr int32 SAMPLE_STRIDE = 4;
    constexpr usize COLORED_SAMPLE_MINIMUM = 100;
    const QString compiler = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/shader-tools/slang/bin/slangc");
    const QString python = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/host-tools/venv/bin/python");
    if (QFileInfo(compiler).isExecutable() == false || QFileInfo(python).isExecutable() == false)
    {
        SKIP("pinned shader tools are required for the real project reload test");
    }
    // The World grid draws over part of the triangle, so count an area of the edited primary colour
    // instead of reading one pixel a grid line may cross. The default graph's ToneMap desaturates the
    // emission, so its red channel is lifted above zero.
    const auto countPrimarySamples = [](const QImage& image, bool isBlue) {
        const QColor expected = MapDefaultGraphColor(isBlue == true ? math::Vector3{0, 0, 1} : math::Vector3{0, 1, 0});
        const int32 expectedPrimary = isBlue == true ? expected.blue() : expected.green();
        usize count = 0;
        for (int32 y = 0; y < image.height(); y += SAMPLE_STRIDE)
        {
            for (int32 x = 0; x < image.width(); x += SAMPLE_STRIDE)
            {
                const QColor pixel = image.pixelColor(x, y);
                const int32 primary = isBlue == true ? pixel.blue() : pixel.green();
                if (primary > expectedPrimary - PRIMARY_MARGIN && pixel.red() < expected.red() + DARK_MARGIN)
                {
                    ++count;
                }
            }
        }
        return count;
    };
    rhi::Shutdown();
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({python, {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)});
    RenderPreview source;
    PreviewViewport viewport(&source);
    viewport.resize(800, 600);
    viewport.show();
    viewport.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&source);
    workspace.EnableWorldRendering();
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.IsBuiltIn() == true);
    REQUIRE(source.SetScene(MakeScene({1, 1, 1, 1})) == true);
    source.SetStatisticsVisible(true);
    REQUIRE(source.Start() == true);
    const auto waitFor = [&](const auto& isReady) {
        QElapsedTimer timer;
        timer.start();
        while (isReady() == false && timer.elapsed() < TIMEOUT_MILLISECONDS)
        {
            QTest::qWait(1);
            workspace.ServicePreview();
        }
        INFO(workspace.GetStatusText().toStdString());
        INFO(workspace.GetPreviewOrNull()->GetStatus().toStdString());
        return isReady();
    };
    WorldGraphView* graphView = viewport.findChild<WorldGraphView*>();
    REQUIRE(graphView != nullptr);
    REQUIRE(waitFor([&]() { return graphView->GetImage().isNull() == false; }) == true);
    CHECK(source.FrameAvailable() == true);
    CHECK(source.Frames() != 0);
    const GraphViewportStatistics& measured = workspace.GetPreviewOrNull()->GetStatistics();
    REQUIRE(measured.GetCpuMilliseconds().GetCount() != 0);
    CHECK(measured.GetCpuMilliseconds().GetAverage() > 0);
    REQUIRE(measured.GetReadbackMilliseconds().GetCount() != 0);
    CHECK(measured.GetReadbackMilliseconds().GetAverage() > 0);
    QLabel* overlay = viewport.findChild<QLabel*>(QStringLiteral("worldGraphStatisticsOverlay"));
    REQUIRE(overlay != nullptr);
    CHECK(overlay->isVisible() == true);
    REQUIRE(waitFor([&]() { return source.GetStatistics().GpuState != ViewportGpuTimeState::Waiting; }) == true);
    CHECK(QFileInfo::exists(QDir(project.path()).filePath(QStringLiteral("graphs"))) == false);
    REQUIRE(workspace.CustomizeBuiltIn() == true);
    workspace.SetBuildTools(QDir(QFileInfo(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)).absolutePath())
                                .filePath(QStringLiteral("helper")),
                            compiler);
    const QString validator = QStringLiteral(LUDUS_EDITOR_TEST_SPIRV_VALIDATOR);
    if (QFileInfo(validator).isExecutable() == true)
    {
        RenderGraphBuildSettings settings = workspace.GetBuildSettings();
        settings.Tools.Validator = validator;
        workspace.SetBuildSettings(settings);
    }
    const QString file =
        QDir(workspace.GetDocument().GetPackageRoot()).filePath(QStringLiteral("main_shaders/color.slang"));
    QFile original(file);
    REQUIRE(original.open(QIODevice::ReadOnly) == true);
    const QByteArray shader = original.readAll();
    original.close();
    const auto saveShader = [&](const QByteArray& text) {
        QSaveFile output(file);
        REQUIRE(output.open(QIODevice::WriteOnly) == true);
        REQUIRE(output.write(text) == text.size());
        REQUIRE(output.commit() == true);
    };
    // The default's material callback feeds both its forward and deferred profiles.
    const auto makeEmissive = [&shader](const char* emission) {
        QByteArray edited = shader;
        edited.replace("return SampleMaterial(value);",
                       QByteArray("SurfaceSample sample = SampleMaterial(value);\n"
                                  "    sample.BaseColor = float4(0, 0, 0, 1);\n"
                                  "    sample.Emission = ") +
                           emission + ";\n    return sample;");
        return edited;
    };
    const QByteArray green = makeEmissive("float3(0, 1, 0)");
    REQUIRE(green != shader);
    saveShader(green);
    REQUIRE(waitFor([&]() {
                return workspace.GetDocument().GetDraft(QStringLiteral("main_shaders/color.slang")) == green &&
                       countPrimarySamples(graphView->GetImage(), false) >= COLORED_SAMPLE_MINIMUM;
            }) == true);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(viewport.grab().save(capture) == true);
    }
    const QImage lastGood = graphView->GetImage();
    const uint64 generation = workspace.GetPreviewOrNull()->GetPresentedGeneration();
    const QString graphPath =
        QDir(workspace.GetDocument().GetPackageRoot()).filePath(workspace.GetDocument().GetRootGraph());
    const QByteArray graphBytes = workspace.GetDocument().GetSaved(workspace.GetDocument().GetRootGraph());
    QFile graphFile(graphPath);
    REQUIRE(graphFile.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    const QByteArray oversizedGraph(RenderGraphDocument::MAX_FILE_BYTES + 1, ' ');
    REQUIRE(graphFile.write(oversizedGraph) == oversizedGraph.size());
    graphFile.close();
    REQUIRE(waitFor([&]() { return workspace.GetRenderProblem().isEmpty() == false; }) == true);
    CHECK(graphView->GetImage() == lastGood);
    REQUIRE(graphFile.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    REQUIRE(graphFile.write(graphBytes) == graphBytes.size());
    graphFile.close();
    REQUIRE(waitFor([&]() { return workspace.GetRenderProblem().isEmpty() == true; }) == true);
    saveShader(QByteArrayLiteral("float4 PSMain(RasterVarying value) { return brokenIdentifier; }"));
    REQUIRE(waitFor([&]() {
                return workspace.IsJobActive() == false && workspace.GetLastResult().IsSuccessful == false &&
                       workspace.GetLastResult().Request.Request != 0;
            }) == true);
    CHECK(graphView->GetImage() == lastGood);
    CHECK(workspace.GetPreviewOrNull()->GetPresentedGeneration() == generation);
    saveShader(shader);
    REQUIRE(waitFor([&]() { return workspace.GetPreviewOrNull()->GetPresentedGeneration() > generation; }) == true);
    QComboBox* sourceFiles = workspace.findChild<QComboBox*>(QStringLiteral("renderGraphSourceFile"));
    QPlainTextEdit* sourceText = workspace.findChild<QPlainTextEdit*>(QStringLiteral("renderGraphSource"));
    REQUIRE(sourceFiles != nullptr);
    REQUIRE(sourceText != nullptr);
    sourceFiles->setCurrentText(QStringLiteral("main_shaders/color.slang"));
    const QByteArray blue = makeEmissive("float3(0, 0, 1)");
    REQUIRE(blue != shader);
    sourceText->setPlainText(QString::fromUtf8(blue));
    const uint64 beforeSave = workspace.GetPreviewOrNull()->GetPresentedGeneration();
    QTest::qWait(600);
    CHECK(workspace.GetPreviewOrNull()->GetPresentedGeneration() == beforeSave);
    REQUIRE(workspace.Save() == true);
    REQUIRE(waitFor([&]() { return countPrimarySamples(graphView->GetImage(), true) >= COLORED_SAMPLE_MINIMUM; }) ==
            true);
    REQUIRE(original.open(QIODevice::ReadOnly) == true);
    CHECK(original.readAll() == blue);
    original.close();
    const QImage savedImage = graphView->GetImage();
    const uint64 savedGeneration = workspace.GetPreviewOrNull()->GetPresentedGeneration();
    const QByteArray savedGraph = workspace.GetDocument().GetSaved(workspace.GetDocument().GetRootGraph());
    const uint64 beforeInvalidGraph = workspace.GetLastResult().Request.Request;
    REQUIRE(workspace.GetDocument().SetText(workspace.GetDocument().GetRootGraph(), QByteArrayLiteral("{}")) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.Save() == true);
    REQUIRE(waitFor([&]() {
                return workspace.IsJobActive() == false && workspace.GetLastResult().IsSuccessful == false &&
                       workspace.GetLastResult().Request.Request > beforeInvalidGraph;
            }) == true);
    CHECK(graphView->GetImage() == savedImage);
    CHECK(workspace.GetPreviewOrNull()->GetPresentedGeneration() == savedGeneration);
    REQUIRE(workspace.GetDocument().SetText(workspace.GetDocument().GetRootGraph(), savedGraph) ==
            RenderGraphDocumentStatus::Ok);
    // Direct document edits above bypass the source controller. Rebind through the selector
    // so the next shader edit uses the restored graph's current revision.
    sourceFiles->setCurrentText(workspace.GetDocument().GetRootGraph());
    sourceFiles->setCurrentText(QStringLiteral("main_shaders/color.slang"));
    REQUIRE(sourceFiles->currentText() == QStringLiteral("main_shaders/color.slang"));
    const uint64 beforeInvalidShader = workspace.GetLastResult().Request.Request;
    sourceText->setPlainText(QStringLiteral("broken shader source"));
    REQUIRE(workspace.GetDocument().GetDraft(QStringLiteral("main_shaders/color.slang")) ==
            QByteArrayLiteral("broken shader source"));
    REQUIRE(workspace.Save() == true);
    REQUIRE(waitFor([&]() {
                return workspace.IsJobActive() == false && workspace.GetLastResult().IsSuccessful == false &&
                       workspace.GetLastResult().Request.Request > beforeInvalidShader;
            }) == true);
    CHECK(graphView->GetImage() == savedImage);
    CHECK(workspace.GetPreviewOrNull()->GetPresentedGeneration() == savedGeneration);
    sourceText->setPlainText(QString::fromUtf8(blue));
    REQUIRE(workspace.Save() == true);
    REQUIRE(waitFor([&]() { return workspace.GetPreviewOrNull()->GetPresentedGeneration() > savedGeneration; }) ==
            true);
    const uint64 revision = workspace.GetPreviewOrNull()->GetPresentedRevision();
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::AltModifier, QPoint(100, 200));
    QMouseEvent movement(QEvent::MouseMove,
                         QPointF(140, 220),
                         QPointF(viewport.mapToGlobal(QPoint(140, 220))),
                         Qt::NoButton,
                         Qt::LeftButton,
                         Qt::AltModifier);
    QCoreApplication::sendEvent(&viewport, &movement);
    QTest::mouseRelease(&viewport, Qt::LeftButton, Qt::AltModifier, QPoint(140, 220));
    REQUIRE(waitFor([&]() { return workspace.GetPreviewOrNull()->GetPresentedRevision() > revision; }) == true);
    QComboBox* targets = viewport.findChild<QComboBox*>(QStringLiteral("worldGraphTargets"));
    REQUIRE(targets != nullptr);
    CHECK(targets->count() >= 3);
    targets->setCurrentIndex(2);
    REQUIRE(waitFor([&]() { return graphView->GetImage() != lastGood; }) == true);
    source.Stop();
    workspace.SetPreviewSource(nullptr);
    rhi::Shutdown();
}

TEST_CASE("Selected project World edits reach the delegated graph viewport", "[editor][render-graph][world-graph]")
{
    constexpr int32 TIMEOUT_MILLISECONDS = 30000;
    rhi::Shutdown();
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const std::string_view level = ludus::world_demo::ExampleLevel();
    const QString path = QFileInfo(WriteBytes(project,
                                              QStringLiteral("room.world.json"),
                                              QByteArray(level.data(), static_cast<qsizetype>(level.size()))))
                             .canonicalFilePath();
    RendererWorkspace workspace({});
    WorldWorkspace world;
    world.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&world.GetRenderPreview());
    QObject::connect(&world, &WorldWorkspace::RenderPreviewChanged, &workspace, [&]() {
        workspace.SetPreviewSource(&world.GetRenderPreview());
    });
    workspace.EnableWorldRendering();
    workspace.SetProject(project.path(), 1);
    world.SetProject(project.path(), 1);
    world.resize(1000, 700);
    world.show();
    REQUIRE(world.Open(path) == true);
    world.ResumePreview();
    QWidget* viewport = world.findChild<QWidget*>(QStringLiteral("worldViewport"));
    REQUIRE(viewport != nullptr);
    WorldGraphView* graphView = viewport->findChild<WorldGraphView*>();
    REQUIRE(graphView != nullptr);
    const auto waitFor = [&](const auto& isReady) {
        QElapsedTimer timer;
        timer.start();
        while (isReady() == false && timer.elapsed() < TIMEOUT_MILLISECONDS)
        {
            QTest::qWait(1);
            workspace.ServicePreview();
        }
        return isReady();
    };
    REQUIRE(waitFor([&]() { return graphView->GetImage().isNull() == false; }) == true);
    const QByteArray identity = world.GetRenderPreview().Scene().Identity;
    const QImage initial = graphView->GetImage();
    QListWidget* entities = world.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    REQUIRE(entities != nullptr);
    const QString player = world.Document().FindEntityKey(QStringLiteral("player-start"));
    for (int32 row = 0; row < entities->count(); ++row)
    {
        if (entities->item(row)->data(Qt::UserRole).toString() == player)
        {
            entities->setCurrentRow(row);
        }
    }
    QLineEdit* position = world.findChild<QLineEdit*>(QStringLiteral("worldField:transform/x"));
    REQUIRE(position != nullptr);
    position->setFocus();
    position->selectAll();
    QTest::keyClicks(position, QStringLiteral("-3"));
    REQUIRE(world.Apply() == true);
    REQUIRE(waitFor([&]() {
                return world.GetRenderPreview().Scene().Identity != identity && graphView->GetImage() != initial;
            }) == true);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_WORLD_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(world.grab().save(capture) == true);
    }
    world.StopPreview();
    workspace.SetPreviewSource(nullptr);
    rhi::Shutdown();
}

TEST_CASE("Reference capture window names the viewport's render graph and follows its toolbar",
          "[editor][render-graph][world-graph][reference-capture]")
{
    constexpr int32 TIMEOUT_MILLISECONDS = 30000;
    rhi::Shutdown();
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const std::string_view level = ludus::world_demo::ExampleLevel();
    const QString path = QFileInfo(WriteBytes(project,
                                              QStringLiteral("room.world.json"),
                                              QByteArray(level.data(), static_cast<qsizetype>(level.size()))))
                             .canonicalFilePath();
    RendererWorkspace workspace({});
    WorldWorkspace world;
    world.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&world.GetRenderPreview());
    QObject::connect(&world, &WorldWorkspace::RenderPreviewChanged, &workspace, [&]() {
        workspace.SetPreviewSource(&world.GetRenderPreview());
    });
    workspace.EnableWorldRendering();
    workspace.SetProject(project.path(), 1);
    world.SetProject(project.path(), 1);
    world.resize(1000, 700);
    world.show();
    REQUIRE(world.Open(path) == true);
    world.ResumePreview();
    QWidget* viewport = world.findChild<QWidget*>(QStringLiteral("worldViewport"));
    REQUIRE(viewport != nullptr);
    WorldGraphView* graphView = viewport->findChild<WorldGraphView*>();
    REQUIRE(graphView != nullptr);
    const auto waitFor = [&](const auto& isReady) {
        QElapsedTimer timer;
        timer.start();
        while (isReady() == false && timer.elapsed() < TIMEOUT_MILLISECONDS)
        {
            QTest::qWait(1);
            workspace.ServicePreview();
        }
        return isReady();
    };
    REQUIRE(waitFor([&]() { return graphView->GetImage().isNull() == false; }) == true);
    // A project without graphs draws the engine default; the capture window must say so instead of leaving the
    // Render graph source's graph implicit.
    REQUIRE(workspace.IsUsingDefaultRenderer() == true);
    QToolButton* open = viewport->findChild<QToolButton*>(QStringLiteral("viewportReference"));
    REQUIRE(open != nullptr);
    open->click();
    ReferenceCaptureWindow* window = world.findChild<ReferenceCaptureWindow*>();
    REQUIRE(window != nullptr);
    CHECK(window->GetGraphText() == QStringLiteral("Engine default renderer · Final output"));
    QComboBox* source = window->findChild<QComboBox*>(QStringLiteral("referenceSource"));
    REQUIRE(source != nullptr);
    const QStandardItemModel* sources = qobject_cast<const QStandardItemModel*>(source->model());
    REQUIRE(sources != nullptr);
    CHECK(sources->item(CAPTURE_SOURCE_GRAPH_INDEX)->isEnabled() == true);
    // The engine default draws its output with raster passes. The window says so before any click and keeps
    // Capture disabled for the Render graph source, while the RT1 reference stays available.
    CHECK(window->GetGraphCaptureProblem().contains(QStringLiteral("raster passes")) == true);
    QPushButton* capture = window->findChild<QPushButton*>(QStringLiteral("referenceCapture"));
    REQUIRE(capture != nullptr);
    QLabel* problem = window->findChild<QLabel*>(QStringLiteral("referenceGraphProblem"));
    REQUIRE(problem != nullptr);
    CHECK(capture->isEnabled() == true);
    CHECK(problem->isVisible() == false);
    source->setCurrentIndex(CAPTURE_SOURCE_GRAPH_INDEX);
    CHECK(capture->isEnabled() == false);
    CHECK(problem->isVisible() == true);
    CHECK(problem->text().contains(window->GetGraphCaptureProblem()) == true);
    // Force raster is explained the same way and clears again when it is turned off.
    workspace.GetPreviewOrNull()->SetForceRaster(true);
    CHECK(window->GetGraphCaptureProblem().startsWith(QStringLiteral("Force raster")) == true);
    workspace.GetPreviewOrNull()->SetForceRaster(false);
    CHECK(window->GetGraphCaptureProblem().contains(QStringLiteral("raster passes")) == true);
    source->setCurrentIndex(CAPTURE_SOURCE_REFERENCE_INDEX);
    CHECK(capture->isEnabled() == true);
    CHECK(problem->isVisible() == false);
    // Choosing another attachment in the viewport toolbar renames the output while the window stays open.
    QComboBox* targets = viewport->findChild<QComboBox*>(QStringLiteral("worldGraphTargets"));
    REQUIRE(targets != nullptr);
    REQUIRE(targets->count() >= 2);
    targets->setCurrentIndex(1);
    CHECK(window->GetGraphText() ==
          QStringLiteral("Engine default renderer · output %1").arg(targets->currentData().toString()));
    // Reopening reuses the window and keeps describing the current choice.
    open->click();
    CHECK(world.findChildren<ReferenceCaptureWindow*>().size() == 1);
    CHECK(window->GetGraphText().endsWith(targets->currentData().toString()) == true);
    window->close();
    world.StopPreview();
    workspace.SetPreviewSource(nullptr);
    rhi::Shutdown();
}

TEST_CASE("Reference capture window offers the render graph source once the viewport requests its graph",
          "[editor][render-graph][world-graph][reference-capture]")
{
    constexpr int32 TIMEOUT_MILLISECONDS = 30000;
    rhi::Shutdown();
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const std::string_view level = ludus::world_demo::ExampleLevel();
    const QString path = QFileInfo(WriteBytes(project,
                                              QStringLiteral("room.world.json"),
                                              QByteArray(level.data(), static_cast<qsizetype>(level.size()))))
                             .canonicalFilePath();
    RendererWorkspace workspace({});
    WorldWorkspace world;
    world.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&world.GetRenderPreview());
    QObject::connect(&world, &WorldWorkspace::RenderPreviewChanged, &workspace, [&]() {
        workspace.SetPreviewSource(&world.GetRenderPreview());
    });
    // Open the window before the viewport has requested any graph, as when Reference… is clicked early.
    ReferenceCaptureWindow window(&world.GetRenderPreview());
    window.SetProjectGraph(&workspace);
    REQUIRE(workspace.GetPreviewOrNull()->GetRequestedPackagePath().isEmpty() == true);
    QComboBox* source = window.findChild<QComboBox*>(QStringLiteral("referenceSource"));
    REQUIRE(source != nullptr);
    const QStandardItemModel* sources = qobject_cast<const QStandardItemModel*>(source->model());
    REQUIRE(sources != nullptr);
    CHECK(sources->item(CAPTURE_SOURCE_GRAPH_INDEX)->isEnabled() == false);
    CHECK(window.GetGraphText().startsWith(QStringLiteral("None.")) == true);
    workspace.EnableWorldRendering();
    workspace.SetProject(project.path(), 1);
    world.SetProject(project.path(), 1);
    world.resize(1000, 700);
    world.show();
    REQUIRE(world.Open(path) == true);
    world.ResumePreview();
    QWidget* viewport = world.findChild<QWidget*>(QStringLiteral("worldViewport"));
    REQUIRE(viewport != nullptr);
    WorldGraphView* graphView = viewport->findChild<WorldGraphView*>();
    REQUIRE(graphView != nullptr);
    QElapsedTimer timer;
    timer.start();
    while (graphView->GetImage().isNull() == true && timer.elapsed() < TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(1);
        workspace.ServicePreview();
    }
    REQUIRE(graphView->GetImage().isNull() == false);
    // The open window followed the viewport without SetProjectGraph being called again.
    CHECK(sources->item(CAPTURE_SOURCE_GRAPH_INDEX)->isEnabled() == true);
    CHECK(window.GetGraphText() == QStringLiteral("Engine default renderer · Final output"));
    world.StopPreview();
    workspace.SetPreviewSource(nullptr);
    rhi::Shutdown();
}

namespace
{
/// Assemble the selected World's actual catalog closure and keep its immutable geometry alive.
RenderScene LoadSuppliedWorldScene(const QString& root, const QString& worldPath)
{
    WorldDocument world;
    QString error;
    REQUIRE(world.LoadSchema(QByteArrayLiteral("{\"version\":1,\"id\":\"ludus.world.default.v1\","
                                               "\"label\":\"World\",\"components\":[],\"assets\":[]}"),
                             error) == true);
    QFile file(QDir(root).filePath(worldPath));
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    REQUIRE(world.Load(file.readAll(), error) == true);
    const QJsonObject level = world.SceneLevel();
    const QByteArray levelBytes = QJsonDocument(level).toJson(QJsonDocument::Compact);
    const QByteArray projectRoot = root.toUtf8();
    Array<uint8> bundle;
    ludus::content::scene_cook::AssetError assetError;
    const bool hasBundle = ludus::content::scene_cook::AssembleLevel(
        {projectRoot.constData(), static_cast<usize>(projectRoot.size())},
        {reinterpret_cast<const uint8*>(levelBytes.constData()), static_cast<usize>(levelBytes.size())},
        {},
        bundle,
        assetError);
    INFO(assetError.Message.View());
    REQUIRE(hasBundle == true);
    const QByteArray cooked(reinterpret_cast<const char*>(bundle.GetData()), static_cast<qsizetype>(bundle.GetSize()));
    RenderScene scene;
    const bool hasScene = RenderScene::LoadCooked(cooked, level, scene, error, {});
    INFO(error.toStdString());
    REQUIRE(hasScene == true);
    if (worldPath == QStringLiteral("assets/sponza.world.json"))
    {
        REQUIRE(scene.Objects.size() == 28);
    }
    return scene;
}
} // namespace

TEST_CASE("Prepare the complete supplied Intel Sponza World", "[.prepare-full-sponza]")
{
    namespace cook = ludus::content::scene_cook;
    const QString root = qEnvironmentVariable("LUDUS_FULL_SPONZA_PROJECT");
    const QFileInfo entry(qEnvironmentVariable("LUDUS_FULL_SPONZA_ENTRY"));
    if (root.isEmpty() == true || entry.isFile() == false)
    {
        SKIP("Set LUDUS_FULL_SPONZA_PROJECT to a new isolated project directory and LUDUS_FULL_SPONZA_ENTRY to the "
             "supplied glTF.");
    }
    REQUIRE(QFileInfo::exists(QDir(root).filePath(QStringLiteral("assets/catalog.json"))) == false);
    REQUIRE(QDir().mkpath(root) == true);
    const QByteArray rootBytes = root.toUtf8();
    const QByteArray sourceBytes = entry.absolutePath().toUtf8();
    const QByteArray entryBytes = entry.fileName().toUtf8();
    const identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    threading::JobSystem jobs;
    REQUIRE(jobs.Initialize({ .WorkerCount = 4 }) == threading::JobStatus::Ok);
    cook::AssetOptions options;
    options.EntropyOrNull = &entropy;
    options.JobsOrNull = &jobs;
    options.SourceEntry = {entryBytes.constData(), static_cast<usize>(entryBytes.size())};
    const QByteArray environment = qEnvironmentVariable("LUDUS_FULL_SPONZA_ENVIRONMENT").toUtf8();
    options.EnvironmentFile = {environment.constData(), static_cast<usize>(environment.size())};
    cook::AssetError assetError;
    Array<uint8> created;
    REQUIRE(cook::CreateLevel("Intel Sponza", options, created, assetError) == true);
    cook::EditorSceneRequest request;
    request.Root = {rootBytes.constData(), static_cast<usize>(rootBytes.size())};
    request.Source = {sourceBytes.constData(), static_cast<usize>(sourceBytes.size())};
    request.Level = {created.GetData(), created.GetSize()};
    request.Operation = cook::EditorSceneOperation::Import;
    cook::EditorSceneResult imported;
    const bool isImported = cook::OperateEditorScene(request, options, imported, assetError);
    INFO(assetError.Message.View());
    REQUIRE(isImported == true);
    const QJsonObject response =
        QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(imported.Response.GetData()),
                                           static_cast<qsizetype>(imported.Response.GetSize())))
            .object();
    const QJsonObject level = response.value(QStringLiteral("level")).toObject();
    const QByteArray levelBytes = QJsonDocument(level).toJson(QJsonDocument::Compact);
    request.Operation = cook::EditorSceneOperation::CommitImport;
    request.Level = {reinterpret_cast<const uint8*>(levelBytes.constData()), static_cast<usize>(levelBytes.size())};
    request.Catalog = {imported.Catalog.GetData(), imported.Catalog.GetSize()};
    cook::EditorSceneResult committed;
    REQUIRE(cook::OperateEditorScene(request, options, committed, assetError) == true);
    WorldDocument world;
    QString error;
    REQUIRE(world.LoadSchema(QByteArrayLiteral("{\"version\":1,\"id\":\"ludus.world.default.v1\",\"label\":\"World\","
                                               "\"components\":[],\"assets\":[]}"),
                             error) == true);
    REQUIRE(world.Load(QByteArrayLiteral("{\"version\":1,\"schema\":\"ludus.world.default.v1\",\"entities\":[]}"),
                       error) == true);
    REQUIRE(world.ImportScene(level, error) == true);
    QSaveFile file(QDir(root).filePath(QStringLiteral("assets/sponza.world.json")));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(world.Encode()) > 0);
    REQUIRE(file.commit() == true);
}

TEST_CASE("Supplied World replaces a drawable frame through its authored ray graph",
          "[.sponza-ray-project][.supplied-ray-project]")
{
    const QString root =
        qEnvironmentVariable("LUDUS_RAY_TEST_PROJECT", qEnvironmentVariable("LUDUS_SPONZA_TEST_PROJECT"));
    const QString runtime =
        qEnvironmentVariable("LUDUS_RAY_TEST_RUNTIME", qEnvironmentVariable("LUDUS_SPONZA_TEST_RUNTIME"));
    if (root.isEmpty() == true || runtime.isEmpty() == true)
    {
        SKIP("Set LUDUS_SPONZA_TEST_PROJECT to an isolated project copy and LUDUS_SPONZA_TEST_RUNTIME to its ray "
             "package.");
    }
    const QString worldPath = qEnvironmentVariable("LUDUS_RAY_TEST_WORLD", QStringLiteral("assets/sponza.world.json"));
    DeviceFixture device(MakeViewportDeviceDescription().Storage, true);
    REQUIRE(device.IsAvailable == true);
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 1; }) == true);
    const QImage previous = preview.GetImage();
    const RenderScene scene = LoadSuppliedWorldScene(root, worldPath);
    RenderPreview viewport;
    REQUIRE(viewport.SetScene(scene) == true);
    constexpr uint32 TARGET_WIDTH = 640;
    constexpr uint32 TARGET_HEIGHT = 480;
    preview.SetReferenceSize(QSize(TARGET_WIDTH, TARGET_HEIGHT));
    QString error;
    renderer::ViewDescription view;
    REQUIRE(viewport.DescribeConventionalView(TARGET_WIDTH, TARGET_HEIGHT, view, error) == true);
    RenderGraphExtraction extraction;
    REQUIRE(MakeRenderGraphExtraction(scene, view, 2, extraction, error) == true);
    extraction.ContentRevision = 2;
    usize triangleCount = 0;
    constexpr usize CORNERS_PER_TRIANGLE = 3;
    for (const renderer::RenderGraphMesh& mesh : extraction.Meshes)
    {
        triangleCount += mesh.Geometry.IndexCount / CORNERS_PER_TRIANGLE;
    }
    REQUIRE(triangleCount != 0);
    if (worldPath == QStringLiteral("assets/sponza.world.json"))
    {
        REQUIRE(triangleCount == 3747018);
    }
    QFile runtimeFile(runtime);
    REQUIRE(runtimeFile.open(QIODevice::ReadOnly) == true);
    const QByteArray runtimeBytes = runtimeFile.readAll();
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({reinterpret_cast<const uint8*>(runtimeBytes.constData()),
                            static_cast<usize>(runtimeBytes.size())}) == renderer::RenderGraphPackageStatus::Ready);
    bool hasRayPass = false;
    usize rayPassCount = 0;
    const renderer::RenderGraphPlan plan = package.GetPlan();
    for (usize passIndex = 0; passIndex < plan.Passes.GetSize(); ++passIndex)
    {
        hasRayPass = hasRayPass == true || plan.Passes[passIndex].Program == format::Technique::RayTracing;
        rayPassCount += plan.Passes[passIndex].Program == format::Technique::RayTracing ? 1 : 0;
    }
    REQUIRE(hasRayPass == true);
    WARN("Supplied graph ray passes: " << rayPassCount);
    REQUIRE(preview.Load(runtime) == RenderGraphPreviewLoad::Staged);
    const uint64 rayGeneration = preview.GetReport().CandidateGeneration;
    preview.SetExtraction(extraction);
    CHECK(preview.GetImage() == previous);
    constexpr int32 TIMEOUT_MILLISECONDS = 600000;
    QElapsedTimer timer;
    timer.start();
    while ((preview.GetPresentedRevision() != 2 || preview.GetPresentedGeneration() != rayGeneration) &&
           timer.elapsed() < TIMEOUT_MILLISECONDS &&
           preview.GetReport().Candidate != renderer::RenderGraphCandidateState::Failed &&
           preview.GetStatus().contains(QStringLiteral("CapacityExceeded")) == false)
    {
        preview.Tick();
        QTest::qWait(1);
    }
    INFO(preview.GetStatus().toStdString());
    REQUIRE(preview.GetPresentedRevision() == 2);
    REQUIRE(preview.GetPresentedGeneration() == rayGeneration);
    CHECK(preview.GetImage().isNull() == false);
    CHECK(preview.GetImage() != previous);
    const QImage firstRayFrame = preview.GetImage();
    viewport.Look(20, 0);
    REQUIRE(viewport.DescribeConventionalView(TARGET_WIDTH, TARGET_HEIGHT, extraction.View, error) == true);
    extraction.Revision = 3;
    preview.SetExtraction(extraction);
    CHECK(preview.GetImage() == firstRayFrame);
    timer.restart();
    while (preview.GetPresentedRevision() != 3 && timer.elapsed() < TIMEOUT_MILLISECONDS &&
           preview.GetReport().Candidate != renderer::RenderGraphCandidateState::Failed &&
           preview.GetStatus().contains(QStringLiteral("CapacityExceeded")) == false)
    {
        preview.Tick();
        QTest::qWait(1);
    }
    INFO(preview.GetStatus().toStdString());
    REQUIRE(preview.GetPresentedRevision() == 3);
    CHECK(preview.GetPresentedGeneration() == rayGeneration);
    // A physically lit authored graph can remain black when the World has no lights.
    // The declared diagnostic graph must visibly follow the camera.
    if (runtime == QStringLiteral(LUDUS_EDITOR_RAY_GRAPH_FIXTURE))
    {
        CHECK(preview.GetImage() != firstRayFrame);
    }
    // A content edit retains two complete GPU scenes until the replacement is ready.
    // Camera-only navigation above reuses the uploaded scene and cannot prove this peak.
    const QImage navigated = preview.GetImage();
    extraction.Items[0].MeshToWorld.Translation.X += .5F;
    extraction.ContentRevision += 1;
    extraction.Revision = 4;
    preview.SetExtraction(extraction);
    CHECK(preview.GetImage() == navigated);
    uint64 peakResourceBytes = 0;
    bool hasPreservedPendingFrame = true;
    timer.restart();
    while (preview.GetPresentedRevision() != 4 && timer.elapsed() < TIMEOUT_MILLISECONDS &&
           preview.GetReport().Candidate != renderer::RenderGraphCandidateState::Failed &&
           preview.GetStatus().contains(QStringLiteral("CapacityExceeded")) == false)
    {
        preview.Tick();
        rhi::StorageUsage usage;
        REQUIRE(rhi::GetStorageUsage(device.Device, usage) == rhi::RasterStatus::Ready);
        peakResourceBytes = math::Max(peakResourceBytes, usage.PeakResourceBytes);
        if (preview.GetPresentedRevision() != 4)
        {
            // Progressive authored graphs can keep refining the old view while the
            // replacement prepares. Its presented identity and drawable image must survive.
            hasPreservedPendingFrame = hasPreservedPendingFrame == true && preview.GetPresentedRevision() == 3 &&
                                       preview.GetImage().isNull() == false;
        }
        QTest::qWait(1);
    }
    INFO(preview.GetStatus().toStdString());
    REQUIRE(preview.GetPresentedRevision() == 4);
    CHECK(hasPreservedPendingFrame == true);
    rhi::StorageUsage finalUsage;
    REQUIRE(rhi::GetStorageUsage(device.Device, finalUsage) == rhi::RasterStatus::Ready);
    WARN("Full scene replacement peak logical RHI bytes: " << peakResourceBytes << "; peak host metadata/index bytes: "
                                                           << finalUsage.PeakHostBytes);
    if (worldPath == QStringLiteral("assets/sponza.world.json"))
    {
        constexpr uint64 PREVIOUS_DEVICE_BYTES = uint64{2} * 1024 * 1024 * 1024;
        CHECK(peakResourceBytes > PREVIOUS_DEVICE_BYTES);
    }
    const QString capture =
        qEnvironmentVariable("LUDUS_RAY_TEST_IMAGE", qEnvironmentVariable("LUDUS_SPONZA_TEST_IMAGE"));
    if (capture.isEmpty() == false)
    {
        REQUIRE(preview.GetImage().save(capture) == true);
    }
}

TEST_CASE("Full Intel Sponza presents through the native editor viewport", "[.full-sponza-native]")
{
    const QString root = qEnvironmentVariable("LUDUS_SPONZA_TEST_PROJECT");
    if (root.isEmpty() == true)
    {
        SKIP("Set LUDUS_SPONZA_TEST_PROJECT to an isolated full-scene project prepared with [.prepare-full-sponza].");
    }
    REQUIRE(PreviewViewport::GetPresentation() == ViewportPresentation::SharedDevice);
    rhi::Shutdown();
    const RenderScene scene = LoadSuppliedWorldScene(root, QStringLiteral("assets/sponza.world.json"));
    const bool shouldExpectEnvironment = qEnvironmentVariableIntValue("LUDUS_SPONZA_TEST_EXPECT_ENVIRONMENT") == 1;
    if (shouldExpectEnvironment == true)
    {
        REQUIRE(scene.Definition.Get() != nullptr);
        CHECK(scene.Definition->Environment.Width == 4096);
        CHECK(scene.Definition->Environment.Height == 2048);
        float32 peakRadiance = 0;
        for (const math::Vector3 pixel : scene.Definition->Environment.Pixels)
        {
            peakRadiance = math::Max(peakRadiance, pixel.X * scene.Definition->Environment.Rgb.X);
        }
        CHECK(peakRadiance > 10000);
        REQUIRE(scene.Definition->Lights.GetSize() == 24);
        for (const ludus::resources::SceneLight& light : scene.Definition->Lights)
        {
            CHECK(light.IsEnabled == false);
            CHECK(light.IntensityRgb == math::Vector3{});
        }
        CHECK(scene.AuthoredLights.Count == 0);
    }
    usize triangles = 0;
    REQUIRE(scene.Definition.Get() != nullptr);
    for (const ludus::resources::SceneMesh& mesh : scene.Definition->Meshes)
    {
        triangles += mesh.Indices.GetSize() / 3;
    }
    REQUIRE(triangles == 3747018);
    for (const ludus::resources::SceneMaterial& material : scene.Definition->Materials)
    {
        // Material acceptance uses a newly imported v7 project, rather than an old
        // v6 fixture whose texture reductions and factors cannot exercise this path.
        REQUIRE(material.HasSourceMaterial == true);
    }
    RendererWorkspace workspace({});
    workspace.SetProject(root, 1);
    RenderPreview source;
    PreviewViewport viewport(&source);
    viewport.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&source);
    REQUIRE(workspace.PreviewRuntime() == true);
    RenderGraphPreview& graph = *workspace.GetPreviewOrNull();
    viewport.resize(640, 480);
    viewport.show();
    REQUIRE(source.SetScene(scene) == true);
    if (shouldExpectEnvironment == true)
    {
        // Inspect the imported World from its stored courtyard camera, through the
        // same editor navigation restore operation that the viewport persists.
        const ludus::resources::SceneDefinition& definition = *scene.Definition;
        math::Vector3 forwardWorld;
        REQUIRE(math::TryNormalize(definition.Target - definition.Eye, forwardWorld) == math::MathStatus::Success);
        math::Quaternion forwardOrientationWorld;
        REQUIRE(math::TryRotationBetween({0, 0, -1}, forwardWorld, forwardOrientationWorld) ==
                math::MathStatus::Success);
        const math::Vector3 cameraUpWorld = math::Rotate(forwardOrientationWorld, {0, 1, 0});
        math::Vector3 upWorld;
        REQUIRE(math::TryNormalize(definition.Up - forwardWorld * math::Dot(definition.Up, forwardWorld), upWorld) ==
                math::MathStatus::Success);
        math::Quaternion rollWorld;
        REQUIRE(math::TryFromAxisAngle(forwardWorld,
                                       math::Atan2(math::Dot(forwardWorld, math::Cross(cameraUpWorld, upWorld)),
                                                   math::Dot(cameraUpWorld, upWorld)),
                                       rollWorld) == math::MathStatus::Success);
        math::Quaternion orientationWorld;
        REQUIRE(math::TryNormalize(rollWorld * forwardOrientationWorld, orientationWorld) == math::MathStatus::Success);
        QJsonObject navigation = source.Navigation();
        navigation.insert(QStringLiteral("eyeX"), static_cast<float64>(definition.Eye.X));
        navigation.insert(QStringLiteral("eyeY"), static_cast<float64>(definition.Eye.Y));
        navigation.insert(QStringLiteral("eyeZ"), static_cast<float64>(definition.Eye.Z));
        navigation.insert(QStringLiteral("rotationX"), static_cast<float64>(orientationWorld.X));
        navigation.insert(QStringLiteral("rotationY"), static_cast<float64>(orientationWorld.Y));
        navigation.insert(QStringLiteral("rotationZ"), static_cast<float64>(orientationWorld.Z));
        navigation.insert(QStringLiteral("rotationW"), static_cast<float64>(orientationWorld.W));
        navigation.insert(QStringLiteral("fov"), static_cast<float64>(definition.VerticalFov));
        navigation.insert(QStringLiteral("distance"),
                          static_cast<float64>(math::Length(definition.Target - definition.Eye)));
        source.RestoreNavigation(navigation);
        CHECK(source.GetVisibleCameraSample().Pose.Position.X == static_cast<float64>(definition.Eye.X));
    }
    REQUIRE(source.Start() == true);
    QQuickWindow* windowOrNull = nullptr;
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport") && window->isVisible() == true)
        {
            windowOrNull = static_cast<QQuickWindow*>(window);
        }
    }
    REQUIRE(windowOrNull != nullptr);
    REQUIRE(QTest::qWaitForWindowExposed(windowOrNull) == true);
    const auto waitFor = [&graph, windowOrNull](auto predicate) {
        QElapsedTimer timer;
        timer.start();
        constexpr int32 TIMEOUT_MILLISECONDS = 600000;
        while (predicate() == false && timer.elapsed() < TIMEOUT_MILLISECONDS &&
               graph.GetReport().Candidate != renderer::RenderGraphCandidateState::Failed &&
               graph.GetStatus().contains(QStringLiteral("CapacityExceeded")) == false)
        {
            QTest::qWait(1);
            // Drive the native consumer even when the test window is occluded.
            static_cast<void>(windowOrNull->grabWindow());
        }
        return predicate();
    };
    const bool hasPresented = waitFor([&]() { return viewport.PresentedFrames() > 0; });
    INFO(source.Status().toStdString());
    INFO(graph.GetStatus().toStdString());
    REQUIRE(hasPresented == true);
    REQUIRE(graph.HasGpuFrame() == true);
    CHECK(graph.GetImage().isNull() == true);
    CHECK(viewport.HasRetainedGraphFrame() == true);
    // Ordinary raster navigation admits the complete World independently of ray setup.
    const QImage raster = windowOrNull->grabWindow();
    REQUIRE(raster.isNull() == false);
    if (shouldExpectEnvironment == true)
    {
        usize litPixels = 0;
        for (int32 y = raster.height() / 4; y < raster.height() * 3 / 4; ++y)
        {
            for (int32 x = raster.width() / 4; x < raster.width() * 3 / 4; ++x)
            {
                const QRgb pixel = raster.pixel(x, y);
                litPixels += qRed(pixel) + qGreen(pixel) + qBlue(pixel) > 48 ? 1 : 0;
            }
        }
        // Exclude the corner overlays; a grid over the dark source cannot pass.
        CHECK(litPixels > static_cast<usize>(raster.width()) * static_cast<usize>(raster.height()) / 40);
    }

    const QString capture = qEnvironmentVariable("LUDUS_SPONZA_NATIVE_IMAGE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(raster.save(capture + QStringLiteral(".raster.png")) == true);
    }
    const uint64 rasterRevision = graph.GetPresentedRevision();
    const uint64 beforeRasterNavigation = viewport.PresentedFrames();
    source.Look(10, 0);
    REQUIRE(waitFor([&]() {
                return graph.GetPresentedRevision() > rasterRevision &&
                       viewport.PresentedFrames() > beforeRasterNavigation;
            }) == true);
    // The legacy material-average ray fixture is optional for this raster/material
    // acceptance. Its full-scene navigation is not reference-path parity evidence;
    // dedicated opacity tests exercise CPU, compute-BVH and native-query coverage.
    if (qEnvironmentVariableIsSet("LUDUS_SPONZA_TEST_LEGACY_RAY") == true)
    {
        const uint64 rasterFrames = viewport.PresentedFrames();
        // The declared diagnostic ray graph shows authored base-color averages without
        // inventing illumination for Sponza's zero-intensity punctual lights.
        const uint64 beforeRayGeneration = graph.GetPresentedGeneration();
        REQUIRE(graph.Load(QStringLiteral(LUDUS_EDITOR_RAY_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
        REQUIRE(waitFor([&]() {
                    static_cast<void>(windowOrNull->grabWindow());
                    return graph.GetPresentedGeneration() > beforeRayGeneration &&
                           viewport.PresentedFrames() > rasterFrames;
                }) == true);
        const QImage first = windowOrNull->grabWindow();
        REQUIRE(first.isNull() == false);
        bool hasVariation = false;
        usize errorPixels = 0;
        const QRgb firstPixel = first.pixel(0, 0);
        for (int32 y = 0; y < first.height(); ++y)
        {
            for (int32 x = 0; x < first.width(); ++x)
            {
                const QRgb pixel = first.pixel(x, y);
                hasVariation = hasVariation == true || pixel != firstPixel;
                errorPixels += qRed(pixel) == 255 && qGreen(pixel) == 0 && qBlue(pixel) == 255 ? 1 : 0;
            }
        }
        CHECK(hasVariation == true);
        CHECK(errorPixels == 0);
        const uint64 initialRevision = graph.GetPresentedRevision();
        const uint64 initialFrame = viewport.PresentedFrames();
        source.Look(20, 0);
        REQUIRE(waitFor([&]() {
                    static_cast<void>(windowOrNull->grabWindow());
                    return graph.GetPresentedRevision() > initialRevision && viewport.PresentedFrames() > initialFrame;
                }) == true);
        const QImage navigated = windowOrNull->grabWindow();
        CHECK(navigated != first);
    }
    const uint64 beforeResize = viewport.PresentedFrames();
    const QSize beforeResizeSize = windowOrNull->grabWindow().size();
    viewport.resize(800, 550);
    REQUIRE(waitFor([&]() {
                // Drive the native capture before checking its consumer counter;
                // a static raster view need not schedule another continuous frame.
                const QImage resized = windowOrNull->grabWindow();
                return viewport.PresentedFrames() > beforeResize && resized.size() != beforeResizeSize;
            }) == true);
    CHECK(graph.GetStatistics().GetReadbackMilliseconds().GetAverage() == 0);
    if (capture.isEmpty() == false)
    {
        REQUIRE(windowOrNull->grabWindow().save(capture) == true);
    }
    // Inspect all imported base maps through textured raster shading independently of
    // the source's illumination. This chart is a diagnostic World, not a relit Sponza.
    RenderScene chart = MakeScene({1, 1, 1, 1});
    chart.Identity = QByteArrayLiteral("sponza-material-chart");
    chart.Textures = scene.Textures;
    chart.Surfaces = scene.Surfaces;
    PreviewMesh& tile = chart.Meshes[0];
    tile.Positions = {{-.5F, -.5F, 0}, {.5F, -.5F, 0}, {.5F, .5F, 0}, {-.5F, .5F, 0}};
    tile.Uvs = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    tile.Normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    tile.Indices = {0, 1, 2, 0, 2, 3};
    chart.Objects.clear();
    constexpr int32 CHART_COLUMNS = 5;
    constexpr float32 TILE_SPACING = 1.2F;
    constexpr float32 CHART_TOP_ROW_CENTER = 2.5F;
    constexpr int32 CHART_EYE_DISTANCE_WORLD = 7;
    for (int32 surface = 0; surface < chart.Surfaces.size(); ++surface)
    {
        PreviewObject object;
        object.Id = QString::number(surface);
        object.Surface = surface;
        object.Texture = chart.Surfaces[surface].Texture;
        object.Transform.Translation = {static_cast<float32>(surface % CHART_COLUMNS - 2) * TILE_SPACING,
                                        (CHART_TOP_ROW_CENTER - static_cast<float32>(surface / CHART_COLUMNS)) *
                                            TILE_SPACING,
                                        0};
        chart.Objects.append(object);
    }
    const uint64 beforeChartRevision = graph.GetPresentedRevision();
    REQUIRE(source.SetScene(chart) == true);
    QJsonObject navigation = source.Navigation();
    navigation.insert(QStringLiteral("camera"), QString{});
    navigation.insert(QStringLiteral("eyeX"), 0);
    navigation.insert(QStringLiteral("eyeY"), 0);
    navigation.insert(QStringLiteral("eyeZ"), CHART_EYE_DISTANCE_WORLD);
    navigation.insert(QStringLiteral("rotationX"), 0);
    navigation.insert(QStringLiteral("rotationY"), 0);
    navigation.insert(QStringLiteral("rotationZ"), 0);
    navigation.insert(QStringLiteral("rotationW"), 1);
    navigation.insert(QStringLiteral("distance"), CHART_EYE_DISTANCE_WORLD);
    source.RestoreNavigation(navigation);
    const uint64 beforeChart = viewport.PresentedFrames();
    const uint64 beforeChartGeneration = graph.GetPresentedGeneration();
    REQUIRE(graph.Load(GetDefaultRenderGraphRuntime()) == RenderGraphPreviewLoad::Staged);
    REQUIRE(waitFor([&]() {
                return graph.GetPresentedGeneration() > beforeChartGeneration &&
                       graph.GetPresentedRevision() > beforeChartRevision && viewport.PresentedFrames() > beforeChart;
            }) == true);
    const uint64 chartSerial = graph.GetDisplayFrames().GetLatestSerial();
    REQUIRE(waitFor([&]() {
                static_cast<void>(windowOrNull->grabWindow());
                return viewport.GetPresentedGraphFrameSerial() >= chartSerial;
            }) == true);
    const QImage chartImage = windowOrNull->grabWindow();
    // Flat material averages have only tile-boundary transitions. Texture detail
    // must occupy a substantial part of the chart, away from the corner overlays.
    usize textureTransitions = 0;
    constexpr int32 CHART_CENTRAL_DIVISOR = 4;
    constexpr usize MINIMUM_TEXTURE_TRANSITION_DIVISOR = 20;
    for (int32 y = 0; y < chartImage.height(); ++y)
    {
        for (int32 x = chartImage.width() / CHART_CENTRAL_DIVISOR;
             x < chartImage.width() - chartImage.width() / CHART_CENTRAL_DIVISOR;
             ++x)
        {
            textureTransitions += chartImage.pixel(x, y) != chartImage.pixel(x - 1, y) ? 1 : 0;
        }
    }
    CHECK(textureTransitions > static_cast<usize>(chartImage.width()) * static_cast<usize>(chartImage.height()) /
                                   MINIMUM_TEXTURE_TRANSITION_DIVISOR);
    if (capture.isEmpty() == false)
    {
        REQUIRE(chartImage.save(capture + QStringLiteral(".materials.png")) == true);
    }
    QFile chartRuntime(GetDefaultRenderGraphRuntime());
    REQUIRE(chartRuntime.open(QIODevice::ReadOnly) == true);
    const QByteArray chartBytes = chartRuntime.readAll();
    renderer::RenderGraphPackage chartPackage;
    REQUIRE(chartPackage.Decode({reinterpret_cast<const uint8*>(chartBytes.constData()),
                                 static_cast<usize>(chartBytes.size())}) == renderer::RenderGraphPackageStatus::Ready);
    const renderer::RenderGraphPrograms chartPrograms = chartPackage.GetPrograms({}, {});
    CHECK(chartPrograms.ForwardVertex.Inputs.empty() == false);
    CHECK(graph.GetRequestedPackagePath() == GetDefaultRenderGraphRuntime());
    source.Stop();
    workspace.SetPreviewSource(nullptr);
}

TEST_CASE("Supplied staircase2 World stays drawable through the engine default fallback", "[.staircase2-project]")
{
    constexpr int32 TIMEOUT_MILLISECONDS = 30000;
    const QString root = qEnvironmentVariable("LUDUS_EDITOR_STAIRCASE_PROJECT");
    if (root.isEmpty() == true)
    {
        SKIP("Set LUDUS_EDITOR_STAIRCASE_PROJECT to an isolated copy of the supplied project.");
    }
    const QString python = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/host-tools/venv/bin/python");
    const QString compiler = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/shader-tools/slang/bin/slangc");
    const ToolingPaths tooling{python, {}, QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)};
    RendererWorkspace renderer(tooling);
    WorldWorkspace world(tooling, nullptr);
    world.UseProjectGraph(renderer);
    renderer.SetPreviewSource(&world.GetRenderPreview());
    QObject::connect(&world, &WorldWorkspace::RenderPreviewChanged, &renderer, [&]() {
        renderer.SetPreviewSource(&world.GetRenderPreview());
    });
    renderer.SetProject(root, 1);
    renderer.SetBuildTools(QDir(QFileInfo(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)).absolutePath())
                               .filePath(QStringLiteral("helper")),
                           compiler);
    renderer.EnableWorldRendering();
    // The generic timeout regression exercises build failure/recovery. This supplied-scene
    // check isolates fallback geometry from the project's potentially stalled custom compiler.
    renderer.CancelJob();
    world.SetProject(root, 1);
    world.resize(1000, 700);
    world.show();
    QElapsedTimer timer;
    timer.start();
    REQUIRE(world.Open(QDir(root).filePath(QStringLiteral("assets/staircase2.world.json"))) == true);
    while ((world.CanSave() == false || world.GetRenderPreview().Frames() == 0) &&
           timer.elapsed() < TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(1);
        renderer.ServicePreview();
    }
    INFO(renderer.GetStatusText().toStdString());
    INFO(renderer.GetRenderProblem().toStdString());
    REQUIRE(world.CanSave() == true);
    REQUIRE(world.GetRenderPreview().Frames() != 0);
    CHECK(world.GetRenderPreview().Scene().Objects.size() == 17);
    const QImage& image = renderer.GetPreviewOrNull()->GetImage();
    REQUIRE(image.isNull() == false);
    CHECK(renderer.IsUsingDefaultRenderer() == true);
    bool hasGeometry = false;
    // A white wall can cover the complete frame. Compare against the default raster clear,
    // rather than assuming that geometry must contain more than one flat material color.
    const QRgb background = qRgb(0, 0, 0);
    for (int32 y = 0; y < image.height(); ++y)
    {
        for (int32 x = 0; x < image.width(); ++x)
        {
            hasGeometry = hasGeometry == true || image.pixel(x, y) != background;
        }
    }
    CHECK(hasGeometry == true);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_WORLD_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(world.grab().save(capture) == true);
    }
    QString depthTarget;
    for (const RenderGraphTarget& target : renderer.GetPreviewOrNull()->GetTargetCatalogue().Targets)
    {
        if (target.IsDepth == true)
        {
            depthTarget = target.Name;
            break;
        }
    }
    REQUIRE(depthTarget.isEmpty() == false);
    const QImage color = image;
    renderer.GetPreviewOrNull()->SetOutputTarget(depthTarget);
    timer.restart();
    while (renderer.GetPreviewOrNull()->GetImage() == color && timer.elapsed() < TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(1);
        renderer.ServicePreview();
    }
    const QImage depth = renderer.GetPreviewOrNull()->GetImage();
    REQUIRE(depth != color);
    bool hasDepthVariation = false;
    const QRgb firstDepth = depth.pixel(0, 0);
    for (int32 y = 0; y < depth.height(); ++y)
    {
        for (int32 x = 0; x < depth.width(); ++x)
        {
            hasDepthVariation = hasDepthVariation == true || depth.pixel(x, y) != firstDepth;
        }
    }
    CHECK(hasDepthVariation == true);
    if (capture.isEmpty() == false)
    {
        REQUIRE(depth.save(capture + QStringLiteral(".depth.png")) == true);
    }
    world.StopPreview();
    renderer.SetPreviewSource(nullptr);
}

TEST_CASE("World graph frames wait for import publication without erasing the last view",
          "[editor][render-graph][world-graph]")
{
    rhi::Shutdown();
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({});
    RenderPreview source;
    PreviewViewport viewport(&source, static_cast<QWidget*>(nullptr), false);
    viewport.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&source);
    workspace.EnableWorldRendering();
    workspace.SetProject(project.path(), 1);
    REQUIRE(source.SetScene(MakeScene({1, 0, 0, 1})) == true);
    REQUIRE(source.Start() == true);
    const auto waitForRevision = [&](uint64 previous) {
        for (usize attempt = 0; attempt < ATTEMPTS; ++attempt)
        {
            QTest::qWait(1);
            workspace.ServicePreview();
            if (workspace.GetPreviewOrNull()->GetPresentedRevision() > previous)
            {
                return true;
            }
        }
        return false;
    };
    REQUIRE(waitForRevision(0) == true);
    WorldGraphView* graphView = viewport.findChild<WorldGraphView*>();
    REQUIRE(graphView != nullptr);
    CHECK(graphView->GetImage().isNull() == true);
    viewport.ShowFrame();
    const QImage shown = graphView->GetImage();
    REQUIRE(shown.isNull() == false);
    const uint64 revision = workspace.GetPreviewOrNull()->GetPresentedRevision();
    viewport.HoldFrame();
    RenderScene replacement = MakeScene({0, 0, 1, 1});
    replacement.Identity = QByteArrayLiteral("replacement-candidate");
    REQUIRE(source.SetScene(replacement) == true);
    REQUIRE(waitForRevision(revision) == true);
    CHECK(graphView->GetImage() == shown);
    CHECK(workspace.GetPreviewOrNull()->GetImage() != shown);
    viewport.ShowFrame();
    CHECK(graphView->GetImage() == workspace.GetPreviewOrNull()->GetImage());
    source.Stop();
    workspace.SetPreviewSource(nullptr);
    rhi::Shutdown();
}

TEST_CASE("Changing graph output during readback never publishes the superseded attachment",
          "[editor][render-graph][world-graph]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("no headless GPU adapter");
    }
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    preview.SetExtraction(MakeExtraction({1, 0, 0, 1}, 1));
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() != 0; }) == true);
    const QImage color = preview.GetImage();
    QString depth;
    for (const RenderGraphTarget& target : preview.GetTargetCatalogue().Targets)
    {
        if (target.IsDepth == true)
        {
            depth = target.Name;
        }
    }
    REQUIRE(depth.isEmpty() == false);
    preview.SetOutputTarget(depth);
    REQUIRE(TickUntil(preview, [&]() { return preview.IsCopyingOutput() == true; }) == true);
    QSignalSpy presented(&preview, &RenderGraphPreview::FramePresented);
    preview.SetOutputTarget({});
    REQUIRE(TickUntil(preview, [&]() { return presented.isEmpty() == false; }) == true);
    CHECK(preview.GetImage() == color);
    preview.Detach();
}

TEST_CASE("World grid uses scene depth without writing it in the project graph", "[editor][grid][render-graph-preview]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    RenderScene scene = MakeScene({.9F, .2F, .8F, 1});
    scene.Meshes[0].Positions = {{-.6F, -.6F, 1}, {.6F, -.6F, 1}, {.6F, .6F, 1}, {-.6F, .6F, 1}};
    scene.Meshes[0].Indices = {0, 1, 2, 2, 3, 0};
    scene.Meshes[0].Normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    renderer::ViewDescription view;
    view.OrthographicCamera = true;
    view.CameraPosition = {0, 0, 5};
    // Conventional finite camera: Z=1 is in front of the grid's Z=0 plane.
    view.WorldToClip.Columns[2].Z = -.1F;
    view.WorldToClip.Columns[3].Z = .5F;
    uint64 revision = 0;
    const auto render = [&](bool isGridVisible) {
        view.WorldGrid = { .IsVisible = isGridVisible, .FocusDistance = 5, .Width = WIDTH, .Height = HEIGHT };
        RenderGraphExtraction extraction;
        QString error;
        REQUIRE(MakeRenderGraphExtraction(scene, view, ++revision, extraction, error) == true);
        preview.SetExtraction(extraction);
        REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == revision; }) == true);
        return preview.GetImage().copy();
    };
    const QImage sceneOnly = render(false);
    preview.SetInspectedTarget(QStringLiteral("Main/depth"));
    REQUIRE(TickUntil(preview, [&]() { return preview.GetInspectedRevision() == revision; }) == true);
    REQUIRE(preview.GetInspectedTexture().IsValid() == true);
    const QByteArray depthWithoutGrid = preview.GetInspectedTexture().Bytes;
    const QImage withGrid = render(true);
    preview.SetInspectedTarget(QStringLiteral("Main/depth"));
    REQUIRE(TickUntil(preview, [&]() { return preview.GetInspectedRevision() == revision; }) == true);
    REQUIRE(preview.GetInspectedTexture().IsValid() == true);
    CHECK(preview.GetInspectedTexture().Bytes == depthWithoutGrid);
    usize changedOutside = 0;
    for (int32 y = 0; y < HEIGHT; ++y)
    {
        for (int32 x = 0; x < WIDTH; ++x)
        {
            const bool isInterior = x > WIDTH / 4 && x < WIDTH * 3 / 4 && y > HEIGHT / 4 && y < HEIGHT * 3 / 4;
            if (isInterior == true)
            {
                CHECK(withGrid.pixel(x, y) == sceneOnly.pixel(x, y));
            }
            else if (withGrid.pixel(x, y) != sceneOnly.pixel(x, y))
            {
                ++changedOutside;
            }
        }
    }
    CHECK(changedOutside > 20);
    // Geometry behind the grid must remain visible between lines, while the axes cross it.
    for (math::Vector3& positionWorld : scene.Meshes[0].Positions)
    {
        positionWorld.Z = -1;
    }
    const QImage behindOnly = render(false);
    const QImage behindGrid = render(true);
    CHECK(behindGrid.pixel(WIDTH / 2, HEIGHT / 2) != behindOnly.pixel(WIDTH / 2, HEIGHT / 2));
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_SCENE_GRID_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(withGrid.save(capture) == true);
        REQUIRE(behindGrid.save(capture + QStringLiteral("-behind.png")) == true);
    }
    preview.Detach();
}

TEST_CASE("view-relative preview resizes without losing its last completed image",
          "[editor][render-graph][preview][size][resource-inspection-preview]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No headless GPU adapter is available");
    }
    QByteArray relative = ReadFixture();
    constexpr qsizetype VIEW_NUMERATOR_FIELD = 6;
    constexpr qsizetype VIEW_DENOMINATOR_FIELD = 7;
    for (qsizetype resource = 0; resource < static_cast<qsizetype>(host::RESOURCE_COUNT); ++resource)
    {
        const qsizetype start = static_cast<qsizetype>(format::HEADER_BYTES) +
                                resource * static_cast<qsizetype>(format::RESOURCE_RECORD_BYTES);
        // Fixture is little-endian; a one-byte change suffices for the small test ratios.
        relative[start + VIEW_NUMERATOR_FIELD * static_cast<qsizetype>(sizeof(uint32))] = 1;
        relative[start + VIEW_DENOMINATOR_FIELD * static_cast<qsizetype>(sizeof(uint32))] = 2;
    }
    QTemporaryDir directory;
    const QString runtime = WriteBytes(directory, QStringLiteral("relative.lurgrt"), relative);
    RenderGraphPreview preview = MakePreview();
    preview.SetReferenceSize({192, 128});
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(preview.Load(runtime) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetImage().size() == QSize(96, 64); }) == true);
    const QImage first = preview.GetImage();
    CHECK(preview.HasPresentedCatalogue() == true);
    bool hasAwaitedPresentation = false;
    QObject resizeObserver;
    QObject::connect(&preview, &RenderGraphPreview::TargetsChanged, &resizeObserver, [&]() {
        CHECK(preview.HasPresentedCatalogue() == false);
        hasAwaitedPresentation = true;
    });
    preview.SetReferenceSize({193, 129});
    CHECK(preview.GetImage() == first);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetImage().size() == QSize(97, 65); }) == true);
    CHECK(hasAwaitedPresentation == true);
    CHECK(preview.HasPresentedCatalogue() == true);
    CHECK(preview.GetPresentedGeneration() == 1);
    CHECK(preview.GetPresentedRevision() == 1);
    const QImage resized = preview.GetImage();
    preview.SetReferenceSize({0, 0});
    preview.Tick();
    CHECK(preview.GetImage() == resized);
    preview.SetReferenceSize({100000, 100000});
    preview.Tick();
    CHECK(preview.GetImage() == resized);
    CHECK(preview.GetDisplayStatus().contains(QStringLiteral("rejected")) == true);
    preview.SetReferenceSize({257, 193});
    REQUIRE(TickUntil(preview, [&]() { return preview.GetImage().size() == QSize(129, 97); }) == true);
    // Another preview owns its own reference; a fixed export does not inherit the first view's size.
    RenderGraphPreview fixed = MakePreview();
    fixed.SetReferenceSize({3840, 2160});
    REQUIRE(fixed.Attach(device.Device, device.Surface) == true);
    fixed.SetExtraction(MakeExtraction({1, 0, 0, 1}, 1));
    REQUIRE(fixed.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(fixed, [&]() { return fixed.GetImage().size() == QSize(WIDTH, HEIGHT); }) == true);
    CHECK(preview.GetImage().size() == QSize(129, 97));
}

TEST_CASE("drawable reference pixels are retained before the edit preview cap", "[editor][render-graph][preview][size]")
{
    RenderPreview viewport;
    viewport.Resize(3840, 2160);
    CHECK(viewport.GetDrawableSize() == QSize(3840, 2160));
    CHECK(viewport.WindowInfo().Width < 3840);
    viewport.Resize(0, 0);
    CHECK(viewport.GetDrawableSize().isEmpty() == true);
}

TEST_CASE("Viewport forces authored raster before attaching a device", "[editor][render-graph][selection]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    RenderGraphPreview preview(
        {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
        shaders::renderer_composite::Vertex(),
        shaders::renderer_composite::Fragment());
    preview.SetForceRaster(true);
    const QString ray = QStringLiteral(LUDUS_EDITOR_RAY_GRAPH_FIXTURE);
    const QString raster = GetDefaultRenderGraphRuntime();
    REQUIRE(preview.Load(ray, raster) == RenderGraphPreviewLoad::Waiting);
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({.25F, .5F, .75F, 1}, 1));
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() != 0; }) == true);
    CHECK(preview.GetReport().ActiveRenderer == renderer::RenderGraphRenderer::Raster);
    CHECK(preview.GetStatus().contains(QStringLiteral("Force raster")) == true);
    const QColor center =
        preview.GetImage().pixelColor(preview.GetImage().width() / 2, preview.GetImage().height() / 2);
    // The forced raster fallback is the engine default graph, whose ToneMap maps the unlit tint's emission.
    CheckColor(center, MapDefaultGraphColor({.25F, .5F, .75F}));
    const QImage last = preview.GetImage();
    CHECK(preview.Load(ray, ray) == RenderGraphPreviewLoad::Unsupported);
    CHECK(preview.GetImage().cacheKey() == last.cacheKey());
    CHECK(preview.Load(ray) == RenderGraphPreviewLoad::Unsupported);
    CHECK(preview.GetImage().cacheKey() == last.cacheKey());
    preview.Detach();
}
TEST_CASE("Force raster switches the live hardware renderer and restores automatic selection",
          "[editor][render-graph][selection]")
{
    DeviceFixture device({}, true);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    renderer::RenderGraphRenderer automatic;
    REQUIRE(renderer::SelectRenderGraphRenderer(device.Device, renderer::RenderGraphPreference::Automatic, automatic) ==
            rhi::RasterStatus::Ready);
    RenderGraphPreview preview(
        {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
        shaders::renderer_composite::Vertex(),
        shaders::renderer_composite::Fragment());
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    RenderGraphExtraction extraction = MakeExtraction({1, 0, 0, 1}, 1);
    // Identity WorldToClip describes an orthographic camera for native camera-ray generation.
    extraction.View.OrthographicCamera = true;
    preview.SetExtraction(extraction);
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RAY_GRAPH_FIXTURE), GetDefaultRenderGraphRuntime()) ==
            RenderGraphPreviewLoad::Staged);
    const bool hasFirstFrame = TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; });
    INFO(preview.GetStatus().toStdString());
    REQUIRE(hasFirstFrame == true);
    CHECK(preview.GetReport().ActiveRenderer == automatic);
    const QImage initial = preview.GetImage();
    preview.SetForceRaster(true);
    CHECK(preview.GetImage().cacheKey() == initial.cacheKey());
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 2; }) == true);
    CHECK(preview.GetReport().ActiveRenderer == renderer::RenderGraphRenderer::Raster);
    CHECK(preview.GetStatus().contains(QStringLiteral("Force raster")) == true);
    const QImage raster = preview.GetImage();
    preview.SetForceRaster(false);
    CHECK(preview.GetImage().cacheKey() == raster.cacheKey());
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 3; }) == true);
    CHECK(preview.GetReport().ActiveRenderer == automatic);
    CHECK(preview.GetPresentedRevision() == 1);
    preview.Detach();
}

TEST_CASE("Runtime activation selects the authored hardware or raster package", "[editor][render-graph][selection]")
{
    bool shouldRequestRay = false;
    SECTION("Default device features") {}
    SECTION("Native ray features preferred")
    {
        shouldRequestRay = true;
    }
    DeviceFixture device({}, shouldRequestRay);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    const auto loadPackage = [](const QString& path) {
        QFile file(path);
        REQUIRE(file.open(QIODevice::ReadOnly) == true);
        const QByteArray bytes = file.readAll();
        renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
        ludus::foundation::UniquePtr<renderer::RenderGraphPackage> package(allocationOrNull);
        REQUIRE(package.Get() != nullptr);
        REQUIRE(package->Decode({reinterpret_cast<const uint8*>(bytes.constData()),
                                 static_cast<usize>(bytes.size())}) == renderer::RenderGraphPackageStatus::Ready);
        return package;
    };
    renderer::RenderGraphActivation activation;
    REQUIRE(activation.Initialize(
                device.Device,
                {default_graph::INTERFACE_IDENTITY.GetData(), default_graph::INTERFACE_IDENTITY.GetSize()},
                shaders::renderer_composite::Vertex(),
                shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    REQUIRE(activation.Resize(WIDTH, HEIGHT) == rhi::RasterStatus::Ready);
    renderer::RenderGraphRenderer automatic;
    REQUIRE(renderer::SelectRenderGraphRenderer(device.Device, renderer::RenderGraphPreference::Automatic, automatic) ==
            rhi::RasterStatus::Ready);
    rhi::RasterStatus staged = activation.StageSelected(loadPackage(GetDefaultRenderGraphRuntime()),
                                                        loadPackage(QStringLiteral(LUDUS_EDITOR_RAY_GRAPH_FIXTURE)));
    REQUIRE((staged == rhi::RasterStatus::Ready || staged == rhi::RasterStatus::Pending));
    CHECK(activation.GetReport().CandidateRenderer == automatic);
    staged = activation.StageSelected(loadPackage(GetDefaultRenderGraphRuntime()),
                                      loadPackage(QStringLiteral(LUDUS_EDITOR_RAY_GRAPH_FIXTURE)),
                                      renderer::RenderGraphPreference::ForceRaster);
    REQUIRE((staged == rhi::RasterStatus::Ready || staged == rhi::RasterStatus::Pending));
    CHECK(activation.GetReport().CandidateRenderer == renderer::RenderGraphRenderer::Raster);
    staged = activation.StageSelected(loadPackage(GetDefaultRenderGraphRuntime()), {});
    REQUIRE((staged == rhi::RasterStatus::Ready || staged == rhi::RasterStatus::Pending));
    CHECK(activation.GetReport().CandidateRenderer == renderer::RenderGraphRenderer::Raster);
    activation.Reset();
}

TEST_CASE("World renderer follows the viewport Force raster preference", "[editor][render-graph][selection]")
{
    RendererWorkspace workspace({});
    RenderPreview source;
    source.SetForceRaster(true);
    workspace.SetPreviewSource(&source);
    RenderGraphPreview* previewOrNull = workspace.GetPreviewOrNull();
    REQUIRE(previewOrNull != nullptr);
    CHECK(previewOrNull->ShouldForceRaster() == true);
    source.SetForceRaster(false);
    CHECK(previewOrNull->ShouldForceRaster() == false);
    source.SetForceRaster(true);
    CHECK(previewOrNull->ShouldForceRaster() == true);
    workspace.SetPreviewSource(nullptr);
}

TEST_CASE("Graph GPU display snapshots retain consumers and bypass frame readback",
          "[editor][render-graph][preview][gpu-display]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No headless GPU adapter");
    }
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetDirectPresentation(true);
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    GraphDisplayFrames& display = preview.GetDisplayFrames();
    struct Reads final
    {
        GraphDisplayFrames& Display;
        rhi::NativeDeviceLease Device;
        StaticArray<ViewportFrameRead, DISPLAY_READ_COUNT> Held;
        QMetaObject::Connection ResetConnection;
        void Drain() noexcept
        {
            for (ViewportFrameRead& read : Held)
            {
                if (read.Token != 0)
                {
                    Display.DrainFrameRead(read);
                }
            }
        }
        ~Reads()
        {
            QObject::disconnect(ResetConnection);
            Drain();
            static_cast<void>(rhi::ReleaseNativeDevice(Device));
        }
    } reads{display, {}, {}, {}};
    REQUIRE(rhi::AcquireNativeDevice(device.Device, reads.Device) == rhi::RasterStatus::Ready);
    rhi::NativeDeviceInfo native;
    REQUIRE(rhi::GetNativeDeviceInfo(reads.Device, native) == rhi::RasterStatus::Ready);
    reads.ResetConnection =
        QObject::connect(&display, &ViewportFrameSource::ResettingFrames, &display, [&reads]() { reads.Drain(); });
    QSignalSpy released(&display, &GraphDisplayFrames::CapacityAvailable);
    for (usize i = 0; i < reads.Held.GetSize(); ++i)
    {
        const uint64 revision = i + 1;
        preview.SetExtraction(MakeExtraction({1, 0, 0, 1}, revision));
        REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == revision; }) == true);
        REQUIRE(display.TakeFrameRead(reads.Device, native.Identity, reads.Held[i]) == rhi::RasterStatus::Ready);
        CHECK(reads.Held[i].Image.Width == WIDTH);
        CHECK(reads.Held[i].Image.Height == HEIGHT);
        CHECK(preview.GetImage().isNull() == true);
        CHECK(preview.HasGpuFrame() == true);
        CHECK(preview.GetDisplayStatus().isEmpty() == true);
    }
    const uint64 retainedSerial = display.GetLatestSerial();
    constexpr uint64 BLOCKED_REVISION = 4;
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, BLOCKED_REVISION));
    constexpr usize BACKPRESSURE_POLLS = 100;
    for (usize i = 0; i < BACKPRESSURE_POLLS; ++i)
    {
        preview.Tick();
        QThread::usleep(200);
    }
    CHECK(display.GetLatestSerial() == retainedSerial);
    CHECK(preview.GetPresentedRevision() == reads.Held.GetSize());
    display.DrainFrameRead(reads.Held[0]);
    CHECK(released.count() == 1);
    const bool hasResumed = TickUntil(preview, [&]() { return preview.GetPresentedRevision() == BLOCKED_REVISION; });
    INFO(preview.GetStatus().toStdString());
    INFO("copying=" << display.IsCopying() << ", latest=" << display.GetLatestSerial());
    REQUIRE(hasResumed == true);
    CHECK(display.GetLatestSerial() > retainedSerial);
    for (usize i = 1; i < reads.Held.GetSize(); ++i)
    {
        rhi::NativeTextureInfo retained;
        REQUIRE(rhi::GetNativeTextureInfo(reads.Held[i].Read, retained) == rhi::RasterStatus::Ready);
        CHECK(retained.Revision == reads.Held[i].Image.Revision);
        CHECK(retained.ProducerOrdinal == reads.Held[i].Image.ProducerOrdinal);
    }
    CHECK(preview.GetStatistics().GetReadbackMilliseconds().GetAverage() == 0);
    const uint64 lastGood = display.GetLatestSerial();
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CHECK(preview.Load(WriteBytes(directory, QStringLiteral("bad.lurgrt"), QByteArrayLiteral("bad"))) ==
          RenderGraphPreviewLoad::Malformed);
    CHECK(display.GetLatestSerial() == lastGood);
    preview.Detach();
    CHECK(reads.Held[1].Token == 0);
    CHECK(reads.Held[2].Token == 0);
}

TEST_CASE("Live graph viewport presents GPU images through navigation and resize", "[.render-native][gpu-display]")
{
    REQUIRE(PreviewViewport::GetPresentation() == ViewportPresentation::SharedDevice);
    rhi::Shutdown();
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    RenderPreview source;
    PreviewViewport viewport(&source);
    viewport.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&source);
    REQUIRE(workspace.PreviewRuntime() == true);
    viewport.resize(640, 480);
    viewport.show();
    REQUIRE(source.SetScene(MakeScene({0, 1, 0, 1})) == true);
    REQUIRE(source.Start() == true);
    viewport.raise();
    viewport.activateWindow();
    QQuickWindow* windowOrNull = nullptr;
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport") && window->isVisible() == true)
        {
            windowOrNull = static_cast<QQuickWindow*>(window);
        }
    }
    REQUIRE(windowOrNull != nullptr);
    REQUIRE(QTest::qWaitForWindowExposed(windowOrNull) == true);
    const auto waitFor = [](auto predicate) {
        QElapsedTimer timer;
        timer.start();
        constexpr int32 DEADLINE_MILLISECONDS = 10'000;
        while (predicate() == false && timer.elapsed() < DEADLINE_MILLISECONDS)
        {
            QTest::qWait(1);
        }
        return predicate();
    };
    RenderGraphPreview& graph = *workspace.GetPreviewOrNull();
    const bool hasPresented = waitFor([&]() { return viewport.PresentedFrames() > 0; });
    INFO(source.Status().toStdString());
    INFO(graph.GetStatus().toStdString());
    INFO("Qt initialized=" << windowOrNull->isSceneGraphInitialized() << ", exposed=" << windowOrNull->isExposed());
    REQUIRE(hasPresented == true);
    CHECK(graph.HasGpuFrame() == true);
    CHECK(graph.GetImage().isNull() == true);
    CHECK(viewport.HasRetainedGraphFrame() == true);
    // Test-only framebuffer capture independently checks the actual shared GPU presentation.
    const QImage first = windowOrNull->grabWindow();
    REQUIRE(first.isNull() == false);
    CHECK(first.pixelColor(first.width() / 2, first.height() / 2) != first.pixelColor(0, 0));
    const uint64 firstRevision = graph.GetPresentedRevision();
    const uint64 firstPaint = viewport.PresentedFrames();
    source.Orbit(120, 40);
    REQUIRE(waitFor([&]() {
                return graph.GetPresentedRevision() > firstRevision && viewport.PresentedFrames() > firstPaint;
            }) == true);
    const QImage navigated = windowOrNull->grabWindow();
    CHECK(navigated != first);
    viewport.resize(800, 550);
    const uint64 beforeResize = viewport.PresentedFrames();
    source.Orbit(20, 10);
    REQUIRE(waitFor([&]() { return viewport.PresentedFrames() > beforeResize; }) == true);
    CHECK(windowOrNull->grabWindow().size() != first.size());
    CHECK(graph.GetImage().isNull() == true);
    CHECK(graph.GetStatistics().GetReadbackMilliseconds().GetAverage() == 0);
    QString depthTarget;
    for (const RenderGraphTarget& target : graph.GetTargetCatalogue().Targets)
    {
        if (target.IsDepth == true)
        {
            depthTarget = target.Name;
            break;
        }
    }
    REQUIRE(depthTarget.isEmpty() == false);
    const uint64 beforeDepth = viewport.PresentedFrames();
    graph.SetOutputTarget(depthTarget);
    REQUIRE(waitFor([&]() { return graph.HasGpuFrame() == false && viewport.PresentedFrames() > beforeDepth; }) ==
            true);
    CHECK(windowOrNull->grabWindow().isNull() == false);
    const uint64 beforeColor = viewport.PresentedFrames();
    graph.SetOutputTarget({});
    REQUIRE(waitFor([&]() { return graph.HasGpuFrame() == true && viewport.PresentedFrames() > beforeColor; }) == true);
    source.SetStatisticsVisible(true);
    constexpr int32 IDLE_STATISTICS_MILLISECONDS = 1'250;
    QTest::qWait(IDLE_STATISTICS_MILLISECONDS);
    CHECK(viewport.GetStatisticsText().contains(QStringLiteral("FPS (updates): 0")) == true);
    const uint64 lastGeneration = graph.GetPresentedGeneration();
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    CHECK(graph.Load(WriteBytes(directory, QStringLiteral("broken.lurgrt"), QByteArrayLiteral("bad"))) ==
          RenderGraphPreviewLoad::Malformed);
    CHECK(graph.GetPresentedGeneration() == lastGeneration);
    CHECK(viewport.HasRetainedGraphFrame() == true);
    source.Stop();
    workspace.SetPreviewSource(nullptr);
    rhi::Shutdown();
}

TEST_CASE("Forward-lit template follows World materials camera and viewport extent",
          "[editor][render-graph][preview][forward-lit]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    RenderScene scene = MakeScene({1, 1, 1, 1});
    scene.Lit = true;
    PreviewSurface surface;
    surface.Emission = {1, 0, 0};
    scene.Surfaces.append(surface);
    scene.Objects[0].Surface = 0;
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(scene, {}, 1, extraction, error) == true);
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetReferenceSize({WIDTH, HEIGHT});
    preview.SetExtraction(extraction);
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_FORWARD_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    INFO(preview.GetStatus().toStdString());
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 1; }) == true);
    CheckCenter(preview.GetImage(), 255, 0, 0);
    const QImage previous = preview.GetImage();
    scene.Surfaces[0].Emission = {0, 1, 0};
    REQUIRE(MakeRenderGraphExtraction(scene, {}, 2, extraction, error) == true);
    preview.SetExtraction(extraction);
    CHECK(preview.GetImage() == previous);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 2; }) == true);
    CheckCenter(preview.GetImage(), 0, 255, 0);
    // The existing camera path must update the actual extraction and leave asset preparation reusable.
    renderer::ViewDescription view;
    view.WorldToClip.Columns[3].X = 2;
    preview.SetCamera(view, 3);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 3; }) == true);
    CheckCenter(preview.GetImage(), 0, 0, 0);
    preview.SetCamera({}, 4);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedRevision() == 4; }) == true);
    CheckCenter(preview.GetImage(), 0, 255, 0);
    preview.SetReferenceSize({WIDTH * 2, HEIGHT * 2});
    REQUIRE(TickUntil(preview, [&]() { return preview.GetImage().size() == QSize(WIDTH * 2, HEIGHT * 2); }) == true);
    const QImage retained = preview.GetImage();
    scene.Meshes[0].Normals.clear();
    REQUIRE(MakeRenderGraphExtraction(scene, {}, 5, extraction, error) == true);
    preview.SetExtraction(extraction);
    REQUIRE(TickUntil(preview, [&]() { return preview.HasPendingWork() == false; }) == true);
    CHECK(preview.GetPresentedRevision() == 4);
    CHECK(preview.GetImage() == retained);
    preview.Detach();
}

TEST_CASE("A viewport graph capture converges an accumulating ray output and saves the linear mean",
          "[editor][render-graph][render-graph-capture]")
{
    DeviceFixture device({}, true);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    renderer::RenderGraphRenderer automatic;
    REQUIRE(renderer::SelectRenderGraphRenderer(device.Device, renderer::RenderGraphPreference::Automatic, automatic) ==
            rhi::RasterStatus::Ready);
    if (automatic != renderer::RenderGraphRenderer::HardwareRayTracing)
    {
        SKIP("The progressive ray fixture needs native ray queries");
    }
    // A lit World, so the fixture's material scatters stochastic samples.
    RenderScene scene = MakeScene({1, 0, 0, 1});
    scene.Lit = true;
    renderer::ViewDescription view{};
    view.OrthographicCamera = true;
    ViewportRasterSource source;
    source.PackagePath = QStringLiteral(LUDUS_EDITOR_PROGRESSIVE_RAY_FIXTURE);
    constexpr uint32 SAMPLES = 4;
    const QSize requested(32, 32);
    ViewportGraphCapture capture;
    QSize extent;
    QString error;
    REQUIRE(capture.Load(device.Device, device.Surface, source, {}, requested, SAMPLES, extent, error) == true);
    CHECK(extent.isEmpty() == false);
    CHECK(capture.IsCaptureProfile() == false);
    REQUIRE(capture.Begin(scene, view, error) == true);
    constexpr int64 CAPTURE_TIMEOUT_MILLISECONDS = 30000;
    QElapsedTimer timer;
    timer.start();
    while (capture.IsRunning() == true && timer.elapsed() < CAPTURE_TIMEOUT_MILLISECONDS)
    {
        static_cast<void>(capture.Service());
    }
    INFO(capture.GetError().toStdString());
    REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Complete);
    const renderer::RenderGraphCaptureInfo info = capture.GetInfo();
    CHECK(info.SubmittedSamples == SAMPLES);
    CHECK(info.MinimumSamples == SAMPLES);
    REQUIRE(capture.GetPixels().GetSize() == static_cast<usize>(extent.width()) * extent.height());
    const QImage image = capture.MakeDisplayImage();
    CHECK(image.size() == extent);

    QTemporaryDir directory;
    const QString path = QDir(directory.path()).filePath(QStringLiteral("capture.lurgcap"));
    REQUIRE(capture.Save(path, error) == true);
    QFile saved(path);
    REQUIRE(saved.open(QIODevice::ReadOnly) == true);
    const QByteArray bytes = saved.readAll();
    CHECK(bytes.startsWith(QByteArray("LURGCAP\0", 8)) == true);
    CHECK(static_cast<usize>(bytes.size()) == format::CAPTURE_HEADER_BYTES + static_cast<usize>(extent.width()) *
                                                                                 extent.height() *
                                                                                 format::CAPTURE_PIXEL_BYTES);
    // The package identity is the SHA-256 of the cooked bytes the capture decoded.
    QFile package(source.PackagePath);
    REQUIRE(package.open(QIODevice::ReadOnly) == true);
    CHECK(bytes.mid(format::CAPTURE_PACKAGE_IDENTITY_OFFSET, format::CAPTURE_IDENTITY_BYTES) ==
          QCryptographicHash::hash(package.readAll(), QCryptographicHash::Sha256));
    CHECK(QImage(ViewportGraphCapture::GetDisplayImagePath(path)).size() == extent);
    capture.Release();
    CHECK(capture.GetPhase() == renderer::RenderGraphCapturePhase::Idle);
}

TEST_CASE("A library capture names its captured Export rather than the package's first exported colour",
          "[editor][render-graph][render-graph-capture]")
{
    DeviceFixture device({}, true);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    renderer::RenderGraphRenderer automatic;
    REQUIRE(renderer::SelectRenderGraphRenderer(device.Device, renderer::RenderGraphPreference::Automatic, automatic) ==
            rhi::RasterStatus::Ready);
    if (automatic == renderer::RenderGraphRenderer::Raster)
    {
        SKIP("The triangle-loop template needs GPU compute");
    }
    const QString package = QStringLiteral(LUDUS_EDITOR_LIBRARY_CAPTURE_FIXTURE);
    const QString capturedExport = QString::fromLatin1(LIBRARY_CAPTURE_EXPORT);
    uint32 captured = format::ABSENT;
    uint32 required = format::ABSENT;
    QString error;
    const bool hasCaptured = ViewportGraphCapture::FindExportResource(package, capturedExport, captured, error);
    INFO(error.toStdString());
    REQUIRE(hasCaptured == true);
    // The library's required colour output has its own Export, which runs first: an unnamed output would capture it.
    REQUIRE(ViewportGraphCapture::FindExportResource(package,
                                                     QStringLiteral("LibraryCapture/required_color"),
                                                     required,
                                                     error) == true);
    CHECK(captured != required);
    uint32 missing = format::ABSENT;
    CHECK(ViewportGraphCapture::FindExportResource(package, QStringLiteral("LibraryCapture/missing"), missing, error) ==
          false);
    CHECK(error.contains(QStringLiteral("has no Export LibraryCapture/missing")) == true);
    CHECK(missing == format::ABSENT);

    RenderScene scene = MakeScene({1, 0, 0, 1});
    scene.Lit = true;
    renderer::ViewDescription view{};
    view.OrthographicCamera = true;
    ViewportRasterSource source;
    source.PackagePath = package;
    source.OutputTarget = capturedExport;
    source.OutputResource = captured;
    constexpr uint32 SAMPLES = 2;
    ViewportGraphCapture capture;
    QSize extent;
    REQUIRE(capture.Load(device.Device, device.Surface, source, {}, QSize(32, 32), SAMPLES, extent, error) == true);
    REQUIRE(capture.Begin(scene, view, error) == true);
    constexpr int64 CAPTURE_TIMEOUT_MILLISECONDS = 60000;
    QElapsedTimer timer;
    timer.start();
    while (capture.IsRunning() == true && timer.elapsed() < CAPTURE_TIMEOUT_MILLISECONDS)
    {
        static_cast<void>(capture.Service());
    }
    INFO(capture.GetError().toStdString());
    REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Complete);
    CHECK(capture.GetInfo().MinimumSamples == SAMPLES);
    capture.Release();
}

TEST_CASE("A viewport graph capture decodes its generation's capture profile in place of the runtime package",
          "[editor][render-graph][render-graph-capture]")
{
    DeviceFixture device({}, true);
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    renderer::RenderGraphRenderer automatic;
    REQUIRE(renderer::SelectRenderGraphRenderer(device.Device, renderer::RenderGraphPreference::Automatic, automatic) ==
            rhi::RasterStatus::Ready);
    if (automatic != renderer::RenderGraphRenderer::HardwareRayTracing)
    {
        SKIP("The progressive ray fixture needs native ray queries");
    }
    RenderScene scene = MakeScene({1, 0, 0, 1});
    scene.Lit = true;
    renderer::ViewDescription view{};
    view.OrthographicCamera = true;
    ViewportRasterSource source;
    source.PackagePath = QStringLiteral(LUDUS_EDITOR_PROGRESSIVE_RAY_FIXTURE);
    // The Reference window requests the profile of this identity from the workspace.
    QByteArray identity;
    QString error;
    REQUIRE(ViewportGraphCapture::ReadGenerationIdentity(source, identity, error) == true);
    CHECK(identity.isEmpty() == false);
    constexpr uint32 SAMPLES = 2;
    const QString profilePath = QStringLiteral(LUDUS_EDITOR_PROGRESSIVE_CAPTURE_PROFILE_FIXTURE);
    ViewportGraphCapture capture;
    QSize extent;
    REQUIRE(capture.Load(device.Device, device.Surface, source, profilePath, QSize(32, 32), SAMPLES, extent, error) ==
            true);
    CHECK(capture.IsCaptureProfile() == true);
    REQUIRE(capture.Begin(scene, view, error) == true);
    constexpr int64 CAPTURE_TIMEOUT_MILLISECONDS = 30000;
    QElapsedTimer timer;
    timer.start();
    while (capture.IsRunning() == true && timer.elapsed() < CAPTURE_TIMEOUT_MILLISECONDS)
    {
        static_cast<void>(capture.Service());
    }
    INFO(capture.GetError().toStdString());
    REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Complete);
    CHECK(capture.GetInfo().MinimumSamples == SAMPLES);
    // The saved provenance names the profile's bytes, which the capture decoded.
    QTemporaryDir directory;
    const QString path = QDir(directory.path()).filePath(QStringLiteral("profile.lurgcap"));
    REQUIRE(capture.Save(path, error) == true);
    QFile saved(path);
    REQUIRE(saved.open(QIODevice::ReadOnly) == true);
    QFile profile(profilePath);
    REQUIRE(profile.open(QIODevice::ReadOnly) == true);
    CHECK(saved.readAll().mid(format::CAPTURE_PACKAGE_IDENTITY_OFFSET, format::CAPTURE_IDENTITY_BYTES) ==
          QCryptographicHash::hash(profile.readAll(), QCryptographicHash::Sha256));
    capture.Release();
    CHECK(capture.IsCaptureProfile() == false);
}

TEST_CASE("The viewport names the check that turned graph pixels magenta",
          "[editor][render-graph][render-graph-failures]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    // A lit receiver and ten small emissive triangles: the polygonal-light template's interactive budget covers
    // eight, so every shaded pixel latches the visibility budget.
    RenderScene scene = MakeScene({1, 1, 1, 1});
    scene.Lit = true;
    PreviewSurface receiver;
    PreviewSurface lamp;
    constexpr float32 LAMP_RADIANCE = 4;
    lamp.Emission = {LAMP_RADIANCE, LAMP_RADIANCE, LAMP_RADIANCE};
    scene.Surfaces = {receiver, lamp};
    scene.Objects[0].Surface = 0;
    PreviewMesh lamps;
    lamps.Id = QStringLiteral("lamps");
    constexpr uint32 LAMP_COUNT = 10;
    constexpr float32 LAMP_SPACING = .08F;
    constexpr float32 LAMP_SIZE = .05F;
    constexpr float32 LAMP_DEPTH = .2F;
    for (uint32 index = 0; index < LAMP_COUNT; ++index)
    {
        const float32 x = -.4F + LAMP_SPACING * static_cast<float32>(index);
        const uint32 first = static_cast<uint32>(lamps.Positions.size());
        lamps.Positions.append(
            {{x, .4F, LAMP_DEPTH}, {x + LAMP_SIZE, .4F, LAMP_DEPTH}, {x, .4F + LAMP_SIZE, LAMP_DEPTH}});
        lamps.Normals.append({{0, 0, 1}, {0, 0, 1}, {0, 0, 1}});
        lamps.Indices.append({first, first + 1, first + 2});
    }
    scene.Meshes.append(lamps);
    PreviewObject lampObject;
    lampObject.Id = QStringLiteral("lamps");
    lampObject.Mesh = 1;
    lampObject.Surface = 1;
    scene.Objects.append(lampObject);
    RenderGraphExtraction extraction;
    QString error;
    REQUIRE(MakeRenderGraphExtraction(scene, {}, 1, extraction, error) == true);
    extraction.View.OrthographicCamera = true;
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(extraction);
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_POLYGONAL_LIGHT_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    // The preview reads the failure summaries about twice a second, one ray pass at a time.
    constexpr int64 FAILURE_TIMEOUT_MILLISECONDS = 30000;
    QElapsedTimer timer;
    timer.start();
    while (preview.GetFailureText().isEmpty() == true && timer.elapsed() < FAILURE_TIMEOUT_MILLISECONDS)
    {
        preview.Tick();
        QApplication::processEvents();
        QThread::msleep(1);
    }
    INFO(preview.GetStatus().toStdString());
    REQUIRE(preview.GetFailureText().isEmpty() == false);
    CHECK(preview.GetFailureText().contains(QStringLiteral("visibility_queries")) == true);
    // A replacement candidate may be preparing meanwhile; the active graph's explanation stays in the status.
    INFO(preview.GetDisplayStatus().toStdString());
    CHECK(preview.GetDisplayStatus().endsWith(preview.GetFailureText()) == true);
    preview.Detach();
}

TEST_CASE("A viewport graph capture explains outputs it cannot capture before using the device",
          "[editor][render-graph][render-graph-capture]")
{
    ViewportRasterSource source;
    source.PackagePath = QStringLiteral(LUDUS_EDITOR_PROGRESSIVE_RAY_FIXTURE);
    QString profilePath;
    QString expected;
    SECTION("a runtime package named as the capture profile")
    {
        // Only RenderGraphPackage::DecodeCaptureProfile admits the profile's magic, and it refuses a runtime package.
        profilePath = source.PackagePath;
        expected = QStringLiteral("capture profile");
    }
    SECTION("a missing capture profile")
    {
        profilePath = QStringLiteral("missing.lurgcp");
        expected = QStringLiteral("capture again to rebuild it");
    }
    SECTION("Force raster")
    {
        source.ShouldForceRaster = true;
        expected = QStringLiteral("Force raster");
    }
    SECTION("an output name the active generation does not have")
    {
        source.OutputTarget = QStringLiteral("missing");
        expected = QStringLiteral("not part of the active graph generation");
    }
    SECTION("a raster graph")
    {
        source.PackagePath = QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE);
        expected = QStringLiteral("raster passes");
    }
    ViewportGraphCapture capture;
    QSize extent;
    QString error;
    CHECK(capture.Load({}, {}, source, profilePath, QSize(16, 16), 1, extent, error) == false);
    INFO(error.toStdString());
    CHECK(error.contains(expected) == true);
    CHECK(capture.GetPhase() == renderer::RenderGraphCapturePhase::Idle);
    CHECK(extent.isEmpty() == true);
}
