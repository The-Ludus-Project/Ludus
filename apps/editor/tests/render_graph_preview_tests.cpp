// RG3b trusted render-graph preview on a real headless RHI device. The frozen fixture is the
// checked-in RendererStatic offscreen graph, cooked by the editor build both as the host's
// compiled header (the live runtime path) and as a .lurgrt runtime generation (the preview
// path). Tests prove pending, failed and successful publication keep or replace the last
// drawable view as documented, and that preview pixels equal the live path's pixels. Hosts
// without a GPU adapter skip; this is not native Qt presentation evidence.
#include "editor_default_graph.h"
#include "editor_renderer_static.h"
#include "internal/offscreen_frame_reader.h"
#include "internal/preview_viewport.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_preview.h"
#include "internal/render_graph_targets.h"
#include "internal/render_preview.h"
#include "internal/render_target_reader.h"
#include "internal/renderer_workspace.h"
#include "internal/texture_inspection.h"
#include "internal/texture_viewer.h"
#include "internal/world_demo_adapter.h"
#include "internal/world_graph_view.h"
#include "internal/world_workspace.h"
#include "renderer_composite.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>

#include <cstring>
#include <initializer_list>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

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
constexpr usize MATRIX_COLUMNS = 4;
/// Bytes per RGBA8 test texel.
constexpr usize TEXEL_BYTES = 4;
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
    DeviceFixture()
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
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
    PreviewSurface surface;
    // The lit tint is not the static profile's color; the flat approximation is.
    surface.Tint = {.25F, 1, 1, 1};
    surface.FlatTint = {.5F, 1, 1, 1};
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

    RenderGraphExtraction unchanged = extraction;
    CHECK(MakeRenderGraphExtraction(RenderScene{}, {}, 10, unchanged, error) == false);
    CHECK(unchanged.Revision == 9);
    CHECK(MakeRenderGraphExtraction(scene, {}, 0, unchanged, error) == false);
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
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; }) == true);
    CHECK(preview.GetPresentedRevision() == 1);
    // The framed triangle covers the center; the corner stays the graph's clear color.
    CheckCenter(preview.GetImage(), 255, 0, 0);
    CHECK(preview.GetImage().pixelColor(0, 0) != preview.GetImage().pixelColor(WIDTH / 2, HEIGHT / 2));
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

TEST_CASE("RG3b failed and pending candidates keep the last drawable view", "[editor][render-graph][preview]")
{
    DeviceFixture device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    QTemporaryDir directory;
    const QByteArray bytes = ReadFixture();
    RenderGraphPreview preview = MakePreview();
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; }) == true);
    const QImage green = preview.GetImage();
    CheckCenter(green, 0, 255, 0);
    const auto expectKept = [&]() {
        for (int32 tick = 0; tick < 50; ++tick)
        {
            preview.Tick();
        }
        CHECK(preview.GetImage() == green);
        CHECK(preview.GetPresentedGeneration() == 1);
        CHECK(preview.GetReport().ActiveGeneration == 1);
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
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
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

    // Stopping the viewport device keeps the last presented frame and reports it as such.
    preview.Detach();
    CHECK(preview.IsAttached() == false);
    CHECK(preview.GetImage() == red);
    CHECK(preview.GetStatus().contains(QStringLiteral("last presented frame")) == true);
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
    CHECK(qAbs(center.red() - 64) <= COLOR_TOLERANCE);
    CHECK(qAbs(center.green() - 128) <= COLOR_TOLERANCE);
    CHECK(qAbs(center.blue() - 191) <= COLOR_TOLERANCE);
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
    REQUIRE(preview.Attach(device.Device, device.Surface) == true);
    preview.SetExtraction(MakeExtraction({0, 1, 0, 1}, 1));
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    REQUIRE(TickUntil(preview, [&]() { return preview.GetPresentedGeneration() == 1; }) == true);
    // Canvas selection: the Raster subgraph node selects its colour attachment, then its depth.
    REQUIRE(viewer.SelectCanvasNode(QStringLiteral("Main"), QStringLiteral("raster")) == true);
    CHECK(viewer.GetSelectedTarget() == QStringLiteral("Main/color"));
    REQUIRE(viewer.SelectCanvasNode(QStringLiteral("Main"), QStringLiteral("raster")) == true);
    CHECK(viewer.GetSelectedTarget() == QStringLiteral("Main/depth"));
    CHECK(viewer.SelectCanvasNode(QStringLiteral("Main"), QStringLiteral("scene")) == false);
    viewer.SelectCanvasNode(QStringLiteral("Main"), QStringLiteral("color"));
    REQUIRE(viewer.GetSelectedTarget() == QStringLiteral("Main/color"));
    REQUIRE(TickUntil(preview, [&]() { return viewer.GetDisplayImage().isNull() == false; }) == true);
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(0, 255, 0));
    CHECK(viewer.GetInfoText().startsWith(QStringLiteral("Main/color · 96 × 64 · RGBA8Unorm")) == true);
    viewer.ProbeTexel(WIDTH / 2, HEIGHT / 2);
    CHECK(viewer.GetProbeText() ==
          QStringLiteral("Pinned (48, 32)  R 0 (0.0000)  G 255 (1.0000)  B 0 (0.0000)  A 255 (1.0000)"));

    // A new World revision re-reads the selected target after its frame is presented.
    preview.SetExtraction(MakeExtraction({1, 0, 0, 1}, 2));
    REQUIRE(TickUntil(preview, [&]() { return preview.GetInspectedRevision() == 2; }) == true);
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(255, 0, 0));
    CHECK(viewer.GetProbeText().contains(QStringLiteral("R 255")) == true);

    // A rebuilt generation (Live Preview republishes the runtime) keeps the selection by name.
    REQUIRE(preview.Load(QStringLiteral(LUDUS_EDITOR_RENDER_GRAPH_FIXTURE)) == RenderGraphPreviewLoad::Staged);
    const uint64 rebuilt = preview.GetReport().CandidateGeneration;
    REQUIRE(TickUntil(preview, [&]() { return preview.GetInspectedGeneration() == rebuilt; }) == true);
    CHECK(viewer.GetSelectedTarget() == QStringLiteral("Main/color"));
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(255, 0, 0));

    // Depth remapped to its automatic range: the flat triangle is the range floor (black) and the
    // background, left out of the range, clamps to white.
    viewer.SelectTarget(QStringLiteral("Main/depth"));
    REQUIRE(TickUntil(preview, [&]() {
                return preview.GetInspectedTexture().Encoding == TexelEncoding::Depth32Float &&
                       preview.GetInspectedTexture().IsValid() == true;
            }) == true);
    CHECK(viewer.GetDisplaySettings().Range == TextureRangeMode::Automatic);
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(0, 0, 0));
    CHECK(viewer.GetDisplayImage().pixelColor(0, 0) == QColor(255, 255, 255));
    // Back on colour: the measured depth range does not leak into the normalized colour range.
    viewer.SelectTarget(QStringLiteral("Main/color"));
    REQUIRE(TickUntil(preview, [&]() {
                return preview.GetInspectedTexture().Encoding == TexelEncoding::Rgba8Unorm &&
                       preview.GetInspectedTexture().IsValid() == true;
            }) == true);
    CHECK(viewer.GetDisplaySettings().Range == TextureRangeMode::Manual);
    CHECK(viewer.GetDisplaySettings().Minimum == 0.0F);
    CHECK(viewer.GetDisplaySettings().Maximum == 1.0F);
    CHECK(viewer.GetDisplayImage().pixelColor(WIDTH / 2, HEIGHT / 2) == QColor(255, 0, 0));
    preview.Detach();
}

TEST_CASE("World viewport follows project shaders and keeps drawable output after a failed edit",
          "[editor][render-graph][world-graph]")
{
    constexpr int32 TIMEOUT_MILLISECONDS = 60000;
    constexpr int32 GREEN_MINIMUM = 200;
    constexpr int32 DARK_MAXIMUM = 10;
    const QString compiler = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/shader-tools/slang/bin/slangc");
    const QString python = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/out/host-tools/venv/bin/python");
    if (QFileInfo(compiler).isExecutable() == false || QFileInfo(python).isExecutable() == false)
    {
        SKIP("pinned shader tools are required for the real project reload test");
    }
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
    QByteArray green = shader;
    REQUIRE(green.replace("return value.Color;", "return float4(0, 1, 0, 1);") != shader);
    saveShader(green);
    REQUIRE(waitFor([&]() {
                const QImage& image = graphView->GetImage();
                const QColor pixel = image.pixelColor(image.width() / 2, image.height() / 2);
                return workspace.GetDocument().GetDraft(QStringLiteral("main_shaders/color.slang")) == green &&
                       pixel.green() > GREEN_MINIMUM && pixel.red() < DARK_MAXIMUM;
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
    QByteArray blue = shader;
    blue.replace("return value.Color;", "return float4(0, 0, 1, 1);");
    sourceText->setPlainText(QString::fromUtf8(blue));
    REQUIRE(waitFor([&]() {
                const QImage& image = graphView->GetImage();
                const QColor pixel = image.pixelColor(image.width() / 2, image.height() / 2);
                return pixel.blue() > GREEN_MINIMUM && pixel.red() < DARK_MAXIMUM;
            }) == true);
    REQUIRE(original.open(QIODevice::ReadOnly) == true);
    CHECK(original.readAll() == shader);
    original.close();
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

TEST_CASE("Supplied staircase2 World opens through its project raster graph", "[.staircase2-project]")
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
    bool hasGeometry = false;
    const QRgb background = image.pixel(0, 0);
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
