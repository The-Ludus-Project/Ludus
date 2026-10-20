#include "renderer_composite.h"
#include "rg2_raytracing.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>
#include <new>
#include <string_view>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace format = ludus::content::render_graph_format;
namespace
{
constexpr uint32 WIDTH = 96;
constexpr uint32 HEIGHT = 64;
constexpr usize PIXEL_BYTES = static_cast<usize>(WIDTH) * HEIGHT * 4;
constexpr usize VERTEX_COUNT = 3;
constexpr usize INDEX_COUNT = 3;
constexpr usize TABLE_COUNT = 1;
struct RhiGuard final
{
    LUDUS_INLINE RhiGuard() noexcept
    {
        rhi::Shutdown();
    }
    LUDUS_INLINE ~RhiGuard() noexcept
    {
        rhi::Shutdown();
    }
};
struct GraphGpuFixture final
{
    RhiGuard Session;
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    renderer::RenderGraph Graph;
    StaticArray<math::Vector3, VERTEX_COUNT> Positions{{{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}}};
    StaticArray<uint32, INDEX_COUNT> Indices{{0, 1, 2}};
    StaticArray<renderer::RenderGraphMesh, TABLE_COUNT> Meshes{};
    StaticArray<renderer::RenderGraphItem, TABLE_COUNT> Items{};
    StaticArray<renderer::ViewDescription, TABLE_COUNT> Cameras{};
    StaticArray<uint8, PIXEL_BYTES> Pixels{};
    GraphGpuFixture()
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus started =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface, &failure);
        if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            FAIL("Ray pipeline acceptance requires a GPU adapter");
        }
        REQUIRE(started == rhi::DeviceStatus::Ready);
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Items[0].SourceId = 1;
        Items[0].Material.Color = {1, 0, 0, 1};
    }
};
/// Build-tree location of one runtime generation cooked by ludus_compile_render_graph(... RUNTIME_PACKAGE).
struct PackageLocation final
{
    std::string_view Directory; ///< Absolute output directory of the graph.
    std::string_view File;      ///< `<NAME>.lurgrt` inside Directory.
};
core::UniquePtr<renderer::RenderGraphPackage> LoadPackage(const PackageLocation& location)
{
    filesystem::Directory directory;
    REQUIRE(directory.Open(location.Directory).Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead(location.File, file).Succeeded() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
    const filesystem::ReadResult read = file.ReadAt(0, bytes.AsSpan());
    REQUIRE(read.Outcome.Succeeded() == true);
    REQUIRE(read.BytesRead == bytes.GetSize());
    renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
    REQUIRE(package.Get() != nullptr);
    REQUIRE(package->Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    return package;
}
/// Compare one runtime stage description with the compiled header's, target by target.
void CheckSameStage(const rhi::RasterShaderDescription& runtime, const rhi::RasterShaderDescription& compiled)
{
    CHECK(runtime.Artifact.Stage == compiled.Artifact.Stage);
    REQUIRE(runtime.Artifact.Spirv.size() == compiled.Artifact.Spirv.size());
    for (usize word = 0; word < compiled.Artifact.Spirv.size(); ++word)
    {
        REQUIRE(runtime.Artifact.Spirv[word] == compiled.Artifact.Spirv[word]);
    }
    CHECK(runtime.Artifact.SpirvEntry == compiled.Artifact.SpirvEntry);
    CHECK(runtime.Artifact.Msl == compiled.Artifact.Msl);
    CHECK(runtime.Artifact.MslEntry == compiled.Artifact.MslEntry);
    CHECK(runtime.Artifact.Wgsl == compiled.Artifact.Wgsl);
    CHECK(runtime.Artifact.WgslEntry == compiled.Artifact.WgslEntry);
    CHECK(runtime.Artifact.GlslEs == compiled.Artifact.GlslEs);
    CHECK(runtime.Artifact.GlslEsEntry == compiled.Artifact.GlslEsEntry);
    REQUIRE(runtime.Inputs.size() == compiled.Inputs.size());
    for (usize input = 0; input < compiled.Inputs.size(); ++input)
    {
        CHECK(runtime.Inputs[input].Location == compiled.Inputs[input].Location);
        CHECK(runtime.Inputs[input].Format == compiled.Inputs[input].Format);
    }
    REQUIRE(runtime.Bindings.size() == compiled.Bindings.size());
    for (usize binding = 0; binding < compiled.Bindings.size(); ++binding)
    {
        CHECK(runtime.Bindings[binding].Binding == compiled.Bindings[binding].Binding);
        CHECK(runtime.Bindings[binding].Kind == compiled.Bindings[binding].Kind);
        CHECK(runtime.Bindings[binding].Visibility == compiled.Bindings[binding].Visibility);
        CHECK(runtime.Bindings[binding].MinSize == compiled.Bindings[binding].MinSize);
    }
    for (usize slot = 0; slot < format::UNIFORM_SLOT_COUNT; ++slot)
    {
        CHECK(runtime.UniformBlocks[slot] == compiled.UniformBlocks[slot]);
    }
}
constexpr usize GetPixel(usize column, usize row) noexcept
{
    return (row * WIDTH + column) * 4;
}
} // namespace

TEST_CASE("Authored ray graph shades World triangles and preserves the drawable scene on rejection",
          "[render-graph][gpu][ray-pipeline]")
{
    GraphGpuFixture fixture;
    const renderer::RenderGraphPrograms programs =
        packages::rg2_raytracing::MakePrograms(shaders::renderer_composite::Vertex(),
                                               shaders::renderer_composite::Fragment());
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_RAYTRACING_DIRECTORY, "rg2_raytracing.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(package->GetPlan().Passes[0].Program == format::Technique::RayTracing);
    CheckSameStage(decoded.RayCompute, programs.RayCompute);
    CheckSameStage(decoded.RayImageVertex, programs.RayImageVertex);
    CheckSameStage(decoded.RayImageFragment, programs.RayImageFragment);
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, package->GetPlan(), decoded);
    const uint64 started = time::NowTicks();
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - started < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = fixture.Graph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    fixture.Cameras[0].OrthographicCamera = true;
    const auto update = [&fixture](uint64 revision, bool hasItems = true) {
        renderer::RenderGraphReport report;
        rhi::RasterStatus result =
            fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                 {fixture.Items.GetData(), hasItems == true ? fixture.Items.GetSize() : 0},
                                 {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                 revision,
                                 renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                 report);
        const uint64 beginning = time::NowTicks();
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            result = fixture.Graph.Poll();
        }
        return result;
    };
    const auto read = [&fixture]() {
        rhi::SubmissionToken completion;
        REQUIRE(fixture.Graph.Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
        const uint64 beginning = time::NowTicks();
        rhi::RasterStatus result = rhi::GetStatus(fixture.Device, completion);
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            result = rhi::GetStatus(fixture.Device, completion);
        }
        REQUIRE(result == rhi::RasterStatus::Ready);
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect;
        REQUIRE(fixture.Graph.GetExport(0, texture, aspect) == rhi::RasterStatus::Ready);
        rhi::ReadbackTicket ticket;
        result = rhi::RequestTextureReadback(fixture.Device, texture, {0, 0, 0, WIDTH, HEIGHT}, WIDTH * 4, ticket);
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            result = rhi::GetStatus(fixture.Device, ticket);
        }
        REQUIRE(result == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CopyReadback(fixture.Device, ticket, fixture.Pixels.GetData(), fixture.Pixels.GetSize()) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::Release(fixture.Device, ticket) == rhi::RasterStatus::Ready);
    };
    REQUIRE(update(1) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Covered == 1);
    read();
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    CHECK(fixture.Pixels[center] == 255);
    CHECK(fixture.Pixels[center + 2] == 0);
    CHECK(fixture.Pixels[GetPixel(0, 0) + 2] == 255);
    fixture.Items[0].MeshToWorld.Translation.X = 2;
    REQUIRE(update(2) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 2] == 255);
    fixture.Cameras[0].WorldToClip.Columns[3].X = -2;
    REQUIRE(update(3) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[center] == 255);
    fixture.Items[0].Material.Color = {0, 1, 0, 1};
    REQUIRE(update(4) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 1] == 255);
    fixture.Items[0].Coverage = renderer::RenderGraphMaterial::Transparent;
    CHECK(update(5) == rhi::RasterStatus::Unsupported);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 4);
    read();
    CHECK(fixture.Pixels[center + 1] == 255);
    CHECK(fixture.Graph.GetOutputSceneRevision() == 4);
    REQUIRE(fixture.Graph.PrepareEncoding() == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
    const rhi::RasterStatus confirmed = fixture.Graph.ConfirmSubmission(completion);
    REQUIRE((confirmed == rhi::RasterStatus::Ready || confirmed == rhi::RasterStatus::Pending));
    const uint64 beginning = time::NowTicks();
    while (rhi::GetStatus(fixture.Device, completion) == rhi::RasterStatus::Pending &&
           time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
    {
    }
    REQUIRE(update(6, false) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 2] == 255);
}
