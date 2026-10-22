#include "renderer_composite.h"
#include "rg2_raytracing.h"
#include "rg2_scene_ray.h"
#include "rg2_storage_ray.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>
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
    explicit GraphGpuFixture(bool shouldEnableGpuTiming = false)
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.EnableGpuTiming = shouldEnableGpuTiming;
        description.Preferred.RayQueries = true;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus started =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface, &failure);
        if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            FAIL("Ray pipeline acceptance requires a GPU adapter");
        }
        REQUIRE(started == rhi::DeviceStatus::Ready);
        rhi::RayCapabilities capabilities;
        if (rhi::GetRayCapabilities(Device, capabilities) == rhi::RasterStatus::Unsupported)
        {
            SKIP("Hardware raytracing is unavailable; no software traversal is selected");
        }
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

TEST_CASE("Scene callbacks preserve material and scene generation through native GPU rendering",
          "[render-graph][gpu][scene-shader]")
{
    GraphGpuFixture fixture;
    const renderer::RenderGraphPrograms programs =
        packages::rg2_scene_ray::MakePrograms(shaders::renderer_composite::Vertex(),
                                              shaders::renderer_composite::Fragment());
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_SCENE_RAY_DIRECTORY, "rg2_scene_ray.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    CHECK(programs.HasSceneShaderInterface == true);
    CHECK(decoded.HasSceneShaderInterface == true);
    REQUIRE(package->GetPlan().Passes[0].Program == format::Technique::RayTracing);
    CheckSameStage(decoded.RayCompute, programs.RayCompute);
    CheckSameStage(decoded.RayImageVertex, programs.RayImageVertex);
    CheckSameStage(decoded.RayImageFragment, programs.RayImageFragment);
    uint32 selection = format::SCENE_DATA_ALL;
    SECTION("full Scene") {}
    SECTION("Geometry only")
    {
        selection = format::SCENE_DATA_GEOMETRY;
    }
    SECTION("Geometry and Materials")
    {
        selection = format::SCENE_DATA_GEOMETRY | format::SCENE_DATA_MATERIALS;
    }
    SECTION("Geometry and Lights")
    {
        selection = format::SCENE_DATA_GEOMETRY | format::SCENE_DATA_LIGHTS;
    }
    format::Pass operation = package->GetPlan().Passes[0];
    operation.SceneData = selection;
    renderer::RenderGraphPlan plan = package->GetPlan();
    plan.Passes = {&operation, 1};
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, plan, decoded);
    const uint64 started = time::NowTicks();
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - started < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = fixture.Graph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    fixture.Cameras[0].OrthographicCamera = true;
    StaticArray<renderer::SceneShaderMaterial, 2> materials{};
    renderer::SceneShaderMaterial& material = materials[0];
    materials[1].Emission = {2, 1, 0};
    materials[1].IsEmissionTwoSided = true;
    StaticArray<renderer::RenderGraphMesh, 2> meshes{};
    StaticArray<renderer::RenderGraphItem, 2> items{};
    uint32 emitterBinding = 1;
    meshes[1] = fixture.Meshes[0];
    meshes[1].MaterialBindings = {&emitterBinding, 1};
    items[1].SourceId = 2;
    items[1].MeshIndex = 1;
    items[1].MeshToWorld.Translation.X = 5;
    StaticArray<renderer::DirectLight, 3> lights{};
    lights[0].SourceId = 1;
    lights[0].Kind = renderer::LightKind::Point;
    lights[0].Position = {0, 0, 1};
    lights[1].SourceId = 2;
    lights[1].Kind = renderer::LightKind::Directional;
    lights[2].SourceId = 3;
    lights[2].Kind = renderer::LightKind::Spot;
    lights[2].Position = {0, 0, 1};
    uint32 binding = 0;
    fixture.Meshes[0].MaterialBindings = {&binding, 1};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    inputs.Lights = {lights.GetData(), lights.GetSize()};
    inputs.Environment = {0, 0, 1};
    const auto update = [&fixture, &inputs, &material, &meshes, &items](uint64 revision,
                                                                        bool hasItems = true,
                                                                        bool shouldUseDiagnosticColor = false) {
        renderer::RenderGraphReport report;
        meshes[0] = fixture.Meshes[0];
        items[0] = fixture.Items[0];
        material.BaseColor = {fixture.Items[0].Material.Color.X,
                              fixture.Items[0].Material.Color.Y,
                              fixture.Items[0].Material.Color.Z};
        rhi::RasterStatus result = fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                                        {items.GetData(), hasItems == true ? items.GetSize() : 0},
                                                        {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                                        revision,
                                                        shouldUseDiagnosticColor == true
                                                            ? renderer::RenderGraphCoveragePolicy::DiagnosticColor
                                                            : renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                        report,
                                                        &inputs);
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
    CHECK(fixture.Graph.GetReport().Covered == 2);
    read();
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    if (selection != format::SCENE_DATA_ALL)
    {
        CHECK(fixture.Pixels[center] == ((selection & format::SCENE_DATA_MATERIALS) != 0 ? 188 : 0));
        CHECK(fixture.Pixels[center + 1] == 0);
        CHECK(fixture.Pixels[center + 2] == 0);
        CHECK(fixture.Pixels[GetPixel(0, 0) + 2] == ((selection & format::SCENE_DATA_LIGHTS) != 0 ? 188 : 0));
        return;
    }
    CHECK(fixture.Pixels[center] == 188);
    CHECK(fixture.Pixels[center + 2] == 0);
    CHECK(fixture.Pixels[GetPixel(0, 0) + 2] == 188);
    fixture.Items[0].MeshToWorld.Translation.X = 2;
    REQUIRE(update(2) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 2] == 188);
    fixture.Cameras[0].WorldToClip.Columns[3].X = -2;
    REQUIRE(update(3) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[center] == 188);
    fixture.Items[0].Material.Color = {0, 1, 0, 1};
    REQUIRE(update(4) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 1] == 188);
    fixture.Items[0].Coverage = renderer::RenderGraphMaterial::Transparent;
    CHECK(update(5) == rhi::RasterStatus::Unsupported);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 4);
    read();
    CHECK(fixture.Pixels[center + 1] == 188);
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
    CHECK(fixture.Pixels[center + 2] == 188);
    fixture.Items[0].Coverage = renderer::RenderGraphMaterial::Opaque;
    binding = renderer::DEFAULT_SCENE_MATERIAL;
    REQUIRE(update(7) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Graph.GetReport().SceneDiagnostics.DefaultBindings == 1);
    CHECK(fixture.Pixels[center] == 156);
    CHECK(fixture.Pixels[center + 1] == 156);
    binding = 37;
    CHECK(update(8) == rhi::RasterStatus::InvalidDescription);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 7);
    REQUIRE(update(9, true, true) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().SceneDiagnostics.ErrorBindings == 1);
    read();
    CHECK(fixture.Pixels[center] == 188);
    CHECK(fixture.Pixels[center + 1] == 0);
    CHECK(fixture.Pixels[center + 2] == 188);
}

TEST_CASE("Legacy software ray packages require recooking", "[render-graph][ray-pipeline][activation]")
{
    filesystem::Directory directory;
    REQUIRE(directory.Open(LUDUS_RG3_RAYTRACING_DIRECTORY).Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead("rg2_raytracing.lurgrt", file).Succeeded() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
    const filesystem::ReadResult read = file.ReadAt(0, bytes.AsSpan());
    REQUIRE(read.Outcome.Succeeded() == true);
    REQUIRE(read.BytesRead == bytes.GetSize());
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    REQUIRE(TryWriteLittleEndian(format::SCENE_PACKAGE_VERSION, {bytes.GetData() + sizeof(uint32), sizeof(uint32)}) ==
            true);
    renderer::RenderGraphPackage legacy;
    CHECK(legacy.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::UnsupportedVersion);
    CHECK(package.GetPlan().Passes[0].Program == format::Technique::RayTracing);
}

TEST_CASE("Camera revisions reuse ray scene resources and reset history transactionally",
          "[render-graph][gpu][ray-pipeline][camera-reuse]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_RAYTRACING_DIRECTORY, "rg2_raytracing.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs);
    const auto finish = [&fixture](rhi::RasterStatus pending) {
        const uint64 beginning = time::NowTicks();
        while (pending == rhi::RasterStatus::Pending &&
               time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            pending = fixture.Graph.Poll();
        }
        REQUIRE(pending == rhi::RasterStatus::Ready);
    };
    finish(status);
    fixture.Cameras[0].OrthographicCamera = true;
    renderer::RenderGraphReport report;
    finish(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                {fixture.Items.GetData(), fixture.Items.GetSize()},
                                {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                1,
                                renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                report,
                                nullptr,
                                1));
    const auto read = [&fixture]() {
        rhi::SubmissionToken completion;
        REQUIRE(fixture.Graph.Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
        const uint64 beginning = time::NowTicks();
        rhi::RasterStatus completed = rhi::GetStatus(fixture.Device, completion);
        while (completed == rhi::RasterStatus::Pending &&
               time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            completed = rhi::GetStatus(fixture.Device, completion);
        }
        REQUIRE(completed == rhi::RasterStatus::Ready);
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect;
        REQUIRE(fixture.Graph.GetExport(0, texture, aspect) == rhi::RasterStatus::Ready);
        rhi::ReadbackTicket ticket;
        completed = rhi::RequestTextureReadback(fixture.Device, texture, {0, 0, 0, WIDTH, HEIGHT}, WIDTH * 4, ticket);
        while (completed == rhi::RasterStatus::Pending &&
               time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            completed = rhi::GetStatus(fixture.Device, ticket);
        }
        REQUIRE(completed == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CopyReadback(fixture.Device, ticket, fixture.Pixels.GetData(), fixture.Pixels.GetSize()) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::Release(fixture.Device, ticket) == rhi::RasterStatus::Ready);
    };
    read();
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    CHECK(fixture.Pixels[center] == 255);
    CHECK(fixture.Pixels[center + 2] == 0);
    // Geometry bytes are not borrowed or read again for an unchanged content identity.
    // This also makes a hidden full-scene rebuild fail instead of passing accidentally.
    fixture.Cameras[0].WorldToClip.Columns[3].X = 2;
    status = fixture.Graph.Update({},
                                  {},
                                  {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                  2,
                                  renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                  report,
                                  nullptr,
                                  1);
    REQUIRE(status == rhi::RasterStatus::Pending);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 1);
    finish(status);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 2);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 2] == 255);
    fixture.Cameras[0].WorldToClip = {};
    fixture.Cameras[0].WorldToClip.Columns[0] = {};
    CHECK(fixture.Graph.Update({},
                               {},
                               {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                               3,
                               renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                               report,
                               nullptr,
                               1) == rhi::RasterStatus::InvalidDescription);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 2);
    read();
    CHECK(fixture.Pixels[center] == 0);
    fixture.Cameras[0].WorldToClip = math::Matrix4::Identity();
    finish(fixture.Graph.Update({},
                                {},
                                {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                4,
                                renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                report,
                                nullptr,
                                1));
    read();
    CHECK(fixture.Pixels[center] == 255);
    CHECK(fixture.Pixels[center + 2] == 0);
    fixture.Items[0].Material.Color = {0, 1, 0, 1};
    finish(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                {fixture.Items.GetData(), fixture.Items.GetSize()},
                                {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                5,
                                renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                report,
                                nullptr,
                                2));
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 1] == 255);
    REQUIRE(fixture.Graph.Update({},
                                 {},
                                 {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                 6,
                                 renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                 report,
                                 nullptr,
                                 2) == rhi::RasterStatus::Pending);
    fixture.Graph.Reset(); // Cancels staged camera bindings before releasing their scene.
}

TEST_CASE("Measure engine-only ray navigation and image output at 640 by 480", "[.viewport-performance]")
{
    constexpr uint32 IMAGE_WIDTH = 640;
    constexpr uint32 IMAGE_HEIGHT = 480;
    constexpr uint32 GPU_ITERATIONS = 40;
    constexpr uint32 CAMERA_ITERATIONS = 12;
    constexpr float64 NANOSECONDS_PER_MILLISECOND = 1'000'000;
    GraphGpuFixture fixture(true);
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_RAYTRACING_DIRECTORY, "rg2_raytracing.lurgrt"});
    renderer::RenderGraphPlan plan = package->GetPlan();
    StaticArray<format::Resource, format::MAX_RESOURCES> resources{};
    for (usize i = 0; i < plan.Resources.GetSize(); ++i)
    {
        resources[i] = plan.Resources[i];
        resources[i].Width = IMAGE_WIDTH;
        resources[i].Height = IMAGE_HEIGHT;
    }
    plan.Resources = {resources.GetData(), plan.Resources.GetSize()};
    const auto finish = [&fixture](rhi::RasterStatus pending) {
        const uint64 beginning = time::NowTicks();
        while (pending == rhi::RasterStatus::Pending &&
               time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            pending = fixture.Graph.Poll();
        }
        REQUIRE(pending == rhi::RasterStatus::Ready);
    };
    finish(fixture.Graph.Initialize(
        fixture.Device,
        plan,
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment())));
    fixture.Cameras[0].OrthographicCamera = true;
    renderer::RenderGraphReport report;
    uint64 revision = 1;
    uint64 contentRevision = 1;
    const auto update = [&fixture, &report, &revision, &contentRevision](bool shouldReuseScene) {
        return fixture.Graph.Update(
            shouldReuseScene == true
                ? BorrowedView<const renderer::RenderGraphMesh>{}
                : BorrowedView<const renderer::RenderGraphMesh>{fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
            shouldReuseScene == true
                ? BorrowedView<const renderer::RenderGraphItem>{}
                : BorrowedView<const renderer::RenderGraphItem>{fixture.Items.GetData(), fixture.Items.GetSize()},
            {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
            ++revision,
            renderer::RenderGraphCoveragePolicy::RejectIncomplete,
            report,
            nullptr,
            contentRevision);
    };
    finish(update(false));
    float64 rayNanoseconds = 0;
    float64 imageNanoseconds = 0;
    for (uint32 i = 0; i < GPU_ITERATIONS; ++i)
    {
        REQUIRE(rhi::ArmGpuPassTimings(fixture.Device) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(fixture.Graph.Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
        const uint64 beginning = time::NowTicks();
        while (rhi::GetStatus(fixture.Device, completion) == rhi::RasterStatus::Pending &&
               time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
        }
        REQUIRE(rhi::GetStatus(fixture.Device, completion) == rhi::RasterStatus::Ready);
        rhi::GpuPassTimings timings;
        REQUIRE(rhi::GetGpuPassTimings(fixture.Device, timings) == rhi::RasterStatus::Ready);
        REQUIRE(timings.Count == 2);
        REQUIRE(timings.Passes[0].Timing.State == rhi::GpuTimingState::Available);
        REQUIRE(timings.Passes[1].Timing.State == rhi::GpuTimingState::Available);
        rayNanoseconds += timings.Passes[0].Timing.DurationNanoseconds;
        imageNanoseconds += timings.Passes[1].Timing.DurationNanoseconds;
    }
    float64 fullUpdateNanoseconds = 0;
    float64 cameraUpdateNanoseconds = 0;
    for (uint32 i = 0; i < CAMERA_ITERATIONS; ++i)
    {
        ++contentRevision;
        uint64 beginning = time::NowTicks();
        rhi::RasterStatus status = update(false);
        fullUpdateNanoseconds += static_cast<float64>(time::NowTicks() - beginning);
        finish(status);
        fixture.Cameras[0].WorldToClip.Columns[3].X = float32(i) / float32(CAMERA_ITERATIONS);
        beginning = time::NowTicks();
        status = update(true);
        cameraUpdateNanoseconds += static_cast<float64>(time::NowTicks() - beginning);
        finish(status);
    }
    WARN("640x480 engine fixture: ray GPU ms="
         << rayNanoseconds / GPU_ITERATIONS / NANOSECONDS_PER_MILLISECOND
         << ", fullscreen image GPU ms=" << imageNanoseconds / GPU_ITERATIONS / NANOSECONDS_PER_MILLISECOND
         << ", full Update call ms=" << fullUpdateNanoseconds / CAMERA_ITERATIONS / NANOSECONDS_PER_MILLISECOND
         << ", retained camera Update call ms="
         << cameraUpdateNanoseconds / CAMERA_ITERATIONS / NANOSECONDS_PER_MILLISECOND);
}

TEST_CASE("Two graph reservoirs retain alternating shader writes and reset on replacement",
          "[render-graph][gpu][ray-pipeline][storage]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_STORAGE_RAY_DIRECTORY, "rg2_storage_ray.lurgrt"});
    const renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(plan.StorageBuffers.GetSize() == 2);
    CHECK(plan.StorageBuffers[0].Count == 0);
    CHECK(plan.StorageBuffers[0].Stride == 32);
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
        packages::rg2_storage_ray::MakePrograms(shaders::renderer_composite::Vertex(),
                                                shaders::renderer_composite::Fragment());
    CheckSameStage(decoded.RayCompute, compiled.RayCompute);
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, plan, decoded);
    const uint64 started = time::NowTicks();
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - started < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = fixture.Graph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    fixture.Cameras[0].OrthographicCamera = true;
    const auto update = [&fixture](uint64 revision, bool shouldReuseScene = false) {
        renderer::RenderGraphReport report;
        rhi::RasterStatus result = fixture.Graph.Update(
            shouldReuseScene == true
                ? BorrowedView<const renderer::RenderGraphMesh>{}
                : BorrowedView<const renderer::RenderGraphMesh>{fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
            shouldReuseScene == true
                ? BorrowedView<const renderer::RenderGraphItem>{}
                : BorrowedView<const renderer::RenderGraphItem>{fixture.Items.GetData(), fixture.Items.GetSize()},
            {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
            revision,
            renderer::RenderGraphCoveragePolicy::RejectIncomplete,
            report,
            nullptr,
            shouldReuseScene == true ? 1 : revision);
        const uint64 beginning = time::NowTicks();
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            result = fixture.Graph.Poll();
        }
        REQUIRE(result == rhi::RasterStatus::Ready);
    };
    const auto render = [&fixture]() {
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
        result =
            rhi::RequestTextureReadback(fixture.Device, texture, {0, 0, 0, WIDTH, HEIGHT}, usize{WIDTH} * 4, ticket);
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            result = rhi::GetStatus(fixture.Device, ticket);
        }
        REQUIRE(result == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CopyReadback(fixture.Device, ticket, fixture.Pixels.GetData(), fixture.Pixels.GetSize()) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::Release(fixture.Device, ticket) == rhi::RasterStatus::Ready);
        return fixture.Pixels[GetPixel(WIDTH / 2, HEIGHT / 2)];
    };
    update(1);
    const uint8 first = render();
    const uint8 second = render();
    const uint8 third = render();
    CHECK(first > 0);
    CHECK(second > first);
    CHECK(third > second);
    update(2, true);
    CHECK(render() > third); // Camera history resets; authored storage and its bindings survive.
    update(3);
    CHECK(render() == first);
}
