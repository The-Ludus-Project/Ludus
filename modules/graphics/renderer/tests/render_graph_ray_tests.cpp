#include "renderer_composite.h"
#include "rg2_ordered_ray.h"
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
    // Versions 6 through 8 lack the three per-pass words added in version 9.
    uint32 resourceCount = 0;
    uint32 cameraCount = 0;
    constexpr usize RESOURCE_COUNT_OFFSET = 2 * sizeof(uint32);
    constexpr usize CAMERA_COUNT_OFFSET = 4 * sizeof(uint32);
    REQUIRE(TryReadLittleEndian({bytes.GetData() + RESOURCE_COUNT_OFFSET, sizeof(uint32)}, resourceCount) == true);
    REQUIRE(TryReadLittleEndian({bytes.GetData() + CAMERA_COUNT_OFFSET, sizeof(uint32)}, cameraCount) == true);
    constexpr usize LEGACY_PASS_FIELDS = 10;
    const usize extension = format::HEADER_BYTES + resourceCount * format::RESOURCE_RECORD_BYTES +
                            cameraCount * format::CAMERA_RECORD_BYTES + LEGACY_PASS_FIELDS * sizeof(uint32);
    bytes.RemoveRange(extension, extension + (format::PASS_FIELD_COUNT - LEGACY_PASS_FIELDS) * sizeof(uint32));
    REQUIRE(TryWriteLittleEndian(format::STORAGE_PACKAGE_VERSION, {bytes.GetData() + sizeof(uint32), sizeof(uint32)}) ==
            true);
    renderer::RenderGraphPackage storageGeneration;
    REQUIRE(storageGeneration.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    constexpr usize WORD_BYTES = sizeof(uint32);
    const usize nameBytes = package.GetPlan().PassNames[0].GetSize();
    const usize paddedNameBytes = ((nameBytes + WORD_BYTES - 1) / WORD_BYTES) * WORD_BYTES;
    const usize storageCountOffset = bytes.GetSize() - paddedNameBytes - 2 * WORD_BYTES;
    bytes.RemoveRange(storageCountOffset, storageCountOffset + WORD_BYTES);
    REQUIRE(TryWriteLittleEndian(format::NATIVE_RAY_PACKAGE_VERSION,
                                 {bytes.GetData() + sizeof(uint32), sizeof(uint32)}) == true);
    renderer::RenderGraphPackage hardware;
    REQUIRE(hardware.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    CHECK(hardware.GetPlan().Passes[0].RayProgram == 0);
    CHECK(hardware.GetPlan().Passes[0].Buffer0 == format::ABSENT);
    CHECK(hardware.GetPlan().Passes[0].Buffer1 == format::ABSENT);
    REQUIRE(TryWriteLittleEndian(format::SCENE_PACKAGE_VERSION, {bytes.GetData() + sizeof(uint32), sizeof(uint32)}) ==
            true);
    renderer::RenderGraphPackage legacy;
    CHECK(legacy.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::UnsupportedVersion);
    CHECK(package.GetPlan().Passes[0].Program == format::Technique::RayTracing);
}

TEST_CASE("Ordered ray callbacks share zeroed storage across passes and frames", "[render-graph][gpu][ray-history]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_ORDERED_RAY_DIRECTORY, "rg2_ordered_ray.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
        packages::rg2_ordered_ray::MakePrograms(shaders::renderer_composite::Vertex(),
                                                shaders::renderer_composite::Fragment());
    REQUIRE(decoded.HasPersistentRayBuffers == true);
    REQUIRE(decoded.HasSceneShaderInterface == true);
    CheckSameStage(decoded.RayCompute, compiled.RayCompute);
    constexpr usize GENERATION_COUNT = 2;
    for (usize generation = 0; generation < GENERATION_COUNT; ++generation)
    {
        fixture.Graph.Reset();
        rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, package->GetPlan(), decoded);
        constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
        const uint64 beginning = time::NowTicks();
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            status = fixture.Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
        fixture.Cameras[0].OrthographicCamera = true;
        renderer::RenderGraphReport report;
        status = fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                      {fixture.Items.GetData(), fixture.Items.GetSize()},
                                      {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                      1,
                                      renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                      report);
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            status = fixture.Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
        constexpr usize FRAME_COUNT = 3;
        constexpr StaticArray<uint8, FRAME_COUNT> EXPECTED_RED{{188, 213, 225}};
        uint8 previousRed = 0;
        for (usize frame = 0; frame < FRAME_COUNT; ++frame)
        {
            rhi::SubmissionToken completion;
            REQUIRE(fixture.Graph.Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
            status = rhi::GetStatus(fixture.Device, completion);
            while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
            {
                status = rhi::GetStatus(fixture.Device, completion);
            }
            REQUIRE(status == rhi::RasterStatus::Ready);
            rhi::TextureHandle texture;
            rhi::TextureAspect aspect;
            uint32 resource = 0;
            for (uint32 index = 0; index < package->GetPlan().Resources.GetSize(); ++index)
            {
                if (package->GetPlan().Resources[index].IsExported == 1)
                {
                    resource = index;
                }
            }
            status = fixture.Graph.GetExport(resource, texture, aspect);
            while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
            {
                status = fixture.Graph.GetExport(resource, texture, aspect);
            }
            REQUIRE(status == rhi::RasterStatus::Ready);
            rhi::ReadbackTicket ticket;
            status = rhi::RequestTextureReadback(fixture.Device, texture, {0, 0, 0, WIDTH, HEIGHT}, WIDTH * 4, ticket);
            REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
            while (rhi::GetStatus(fixture.Device, ticket) == rhi::RasterStatus::Pending &&
                   time::NowTicks() - beginning < TIMEOUT)
            {
            }
            REQUIRE(rhi::CopyReadback(fixture.Device, ticket, fixture.Pixels.GetData(), fixture.Pixels.GetSize()) ==
                    rhi::RasterStatus::Ready);
            const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
            CHECK(fixture.Pixels[center] > previousRed);
            CHECK(fixture.Pixels[center] == EXPECTED_RED[frame]);
            CHECK(fixture.Graph.HasPendingSamples() == true);
            CHECK(fixture.Pixels[center + 1] == 0);
            CHECK(fixture.Pixels[center + 2] == 0);
            previousRed = fixture.Pixels[center];
            REQUIRE(rhi::Release(fixture.Device, ticket) == rhi::RasterStatus::Ready);
        }
    }
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
    const auto update = [&fixture](uint64 revision) {
        renderer::RenderGraphReport report;
        rhi::RasterStatus result = fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                                        {fixture.Items.GetData(), fixture.Items.GetSize()},
                                                        {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                                        revision,
                                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                        report);
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
    update(2);
    CHECK(render() == first);
}
