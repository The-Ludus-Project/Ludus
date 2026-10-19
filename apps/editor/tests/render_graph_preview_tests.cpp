// RG3b trusted render-graph preview on a real headless RHI device. The frozen fixture is the
// checked-in RendererStatic offscreen graph, cooked by the editor build both as the host's
// compiled header (the live runtime path) and as a .lurgrt runtime generation (the preview
// path). Tests prove pending, failed and successful publication keep or replace the last
// drawable view as documented, and that preview pixels equal the live path's pixels. Hosts
// without a GPU adapter skip; this is not native Qt presentation evidence.
#include "editor_renderer_static.h"
#include "internal/offscreen_frame_reader.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/world_demo_adapter.h"
#include "renderer_composite.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/device.h>

#include <initializer_list>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::render_graph_format;
namespace host = ludus::render_graph_packages::editor_renderer_static;
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

TEST_CASE("RG3b Renderer workspace previews only explicitly cooked generations", "[editor][render-graph][preview]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.GetPreviewOrNull() != nullptr);
    // Nothing is open: no implicit preview.
    CHECK(workspace.PreviewRuntime() == false);
    CHECK(workspace.GetPreviewOrNull()->GetPresentedGeneration() == 0);
}
