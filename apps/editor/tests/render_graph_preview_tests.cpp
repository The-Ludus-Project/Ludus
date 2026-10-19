// RG3b trusted render-graph preview on a real headless RHI device. The frozen fixture is the
// checked-in RendererStatic offscreen graph, cooked by the editor build both as the host's
// compiled header (the live runtime path) and as a .lurgrt runtime generation (the preview
// path). Tests prove pending, failed and successful publication keep or replace the last
// drawable view as documented, and that preview pixels equal the live path's pixels. Hosts
// without a GPU adapter skip; this is not native Qt presentation evidence.
#include "editor_default_graph.h"
#include "editor_renderer_static.h"
#include "internal/offscreen_frame_reader.h"
#include "internal/render_graph_defaults.h"
#include "internal/render_graph_preview.h"
#include "internal/renderer_workspace.h"
#include "renderer_composite.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/rhi/device.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
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
    surface.Tint = {.5F, 1, 1, 1};
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
