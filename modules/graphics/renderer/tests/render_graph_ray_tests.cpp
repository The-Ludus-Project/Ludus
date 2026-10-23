#include "renderer_composite.h"
#include "rg2_ordered_ray.h"
#include "rg2_raytracing.h"
#include "rg2_restir_di.h"
#include "rg2_scene_ray.h"
#include "rg2_storage_ray.h"
#include "rg2_visibility.h"
#include "rg2_visibility_buffer_template.h"
#include "rg2_visibility_secondary_rays_template.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/reference.hpp>
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
        description.Storage.ResourcesPerGraph = renderer::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
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
void ReadGraphFrame(GraphGpuFixture& inoutFixture, uint32 exportResource = 0)
{
    rhi::SubmissionToken completion;
    REQUIRE(inoutFixture.Graph.Render(inoutFixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
    const uint64 beginning = time::NowTicks();
    rhi::RasterStatus result = rhi::GetStatus(inoutFixture.Device, completion);
    while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
    {
        result = rhi::GetStatus(inoutFixture.Device, completion);
    }
    REQUIRE(result == rhi::RasterStatus::Ready);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(inoutFixture.Graph.GetExport(exportResource, texture, aspect) == rhi::RasterStatus::Ready);
    rhi::ReadbackTicket ticket;
    result = rhi::RequestTextureReadback(inoutFixture.Device,
                                         texture,
                                         {0, 0, 0, WIDTH, HEIGHT},
                                         static_cast<usize>(WIDTH) * 4,
                                         ticket);
    while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
    {
        result = rhi::GetStatus(inoutFixture.Device, ticket);
    }
    REQUIRE(result == rhi::RasterStatus::Ready);
    REQUIRE(
        rhi::CopyReadback(inoutFixture.Device, ticket, inoutFixture.Pixels.GetData(), inoutFixture.Pixels.GetSize()) ==
        rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(inoutFixture.Device, ticket) == rhi::RasterStatus::Ready);
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
    const auto read = [&fixture]() { ReadGraphFrame(fixture); };
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
    const auto read = [&fixture]() { ReadGraphFrame(fixture); };
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
    // Version 11 appends one per-pixel stride word to every resource. Remove in reverse order.
    for (usize i = resourceCount; i > 0; --i)
    {
        const usize word = format::HEADER_BYTES + i * format::RESOURCE_RECORD_BYTES - sizeof(uint32);
        bytes.RemoveRange(word, word + sizeof(uint32));
    }
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
                                      report,
                                      nullptr,
                                      1);
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            status = fixture.Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
        constexpr usize FRAME_COUNT = 4;
        constexpr usize CAMERA_FRAME_INDEX = 3;
        constexpr StaticArray<uint8, FRAME_COUNT> EXPECTED_RED{{188, 213, 225, 0}};
        uint8 previousRed = 0;
        for (usize frame = 0; frame < FRAME_COUNT; ++frame)
        {
            if (frame == CAMERA_FRAME_INDEX)
            {
                status = fixture.Graph.Update({},
                                              {},
                                              {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                              2,
                                              renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                              report,
                                              nullptr,
                                              1);
                while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
                {
                    status = fixture.Graph.Poll();
                }
                REQUIRE(status == rhi::RasterStatus::Ready);
            }
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
            if (frame < CAMERA_FRAME_INDEX)
            {
                CHECK(fixture.Pixels[center] > previousRed);
            }
            CHECK(fixture.Pixels[center] == EXPECTED_RED[frame]);
            CHECK(fixture.Graph.HasPendingSamples() == true);
            // This fixture diagnoses a retained frame counter with a restarted sample index as green.
            // It also proves camera epochs preserve the ordered invocation index.
            CHECK(fixture.Pixels[center + 1] == (frame == CAMERA_FRAME_INDEX ? 188 : 0));
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
    CHECK(render() == 225); // Sample zero resumes from retained storage; camera history and its sample index reset.
    update(3);
    CHECK(render() == first);
}

TEST_CASE("Visibility raster IDs reconstruct instance materials and launch secondary rays",
          "[render-graph][gpu][ray-pipeline][visibility]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_VISIBILITY_DIRECTORY, "rg2_visibility.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
        packages::rg2_visibility::MakePrograms(shaders::renderer_composite::Vertex(),
                                               shaders::renderer_composite::Fragment());
    CheckSameStage(decoded.ColorVertex, compiled.ColorVertex);
    CheckSameStage(decoded.RayCompute, compiled.RayCompute);
    REQUIRE(decoded.HasVisibilityBuffer == true);
    REQUIRE(decoded.HasSceneShaderInterface == true);
    REQUIRE(package->GetPlan().Passes.GetSize() == 2);
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, package->GetPlan(), decoded);
    const uint64 started = time::NowTicks();
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - started < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = fixture.Graph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    constexpr usize QUAD_VERTICES = 4;
    constexpr usize QUAD_INDICES = 6;
    StaticArray<math::Vector3, QUAD_VERTICES> positions{
        {{-.8F, -.6F, .3F}, {0, -.6F, .7F}, {-.8F, .6F, .3F}, {0, .6F, .7F}}};
    StaticArray<uint32, QUAD_INDICES> indices{{0, 1, 2, 2, 1, 3}};
    // Slot order deliberately differs from primitive order; instance 1 overrides only slot 1.
    StaticArray<uint32, 2> slots{{1, 0}};
    StaticArray<uint32, 2> bindings{{0, 1}};
    StaticArray<uint32, 2> overrides{{renderer::INHERIT_SCENE_MATERIAL, 2}};
    constexpr usize MESH_COUNT = 3;
    StaticArray<renderer::RenderGraphMesh, MESH_COUNT> meshes{};
    meshes[0].Geometry = {positions.GetData(), positions.GetSize(), indices.GetData(), indices.GetSize()};
    meshes[0].MaterialSlots = {slots.GetData(), slots.GetSize()};
    meshes[0].MaterialBindings = {bindings.GetData(), bindings.GetSize()};
    meshes[1] = meshes[0];
    meshes[2] = meshes[0];
    constexpr usize ITEM_COUNT = 4;
    StaticArray<renderer::RenderGraphItem, ITEM_COUNT> items{};
    items[0].SourceId = 11;
    items[1].SourceId = 22;
    items[1].MeshIndex = 0;
    items[2].SourceId = 33;
    items[2].MeshIndex = 1;
    items[3].SourceId = 44;
    items[3].MeshIndex = 2;
    items[3].MeshToWorld.Translation = {.55F, 1.5F, 0};
    items[2].MeshToWorld.Translation = {.45F, 1.4F, 0};
    items[1].MeshToWorld.Translation.X = .9F;
    items[1].MaterialOverrides = {overrides.GetData(), overrides.GetSize()};
    constexpr usize MATERIAL_COUNT = 3;
    StaticArray<renderer::SceneShaderMaterial, MATERIAL_COUNT> materials{};
    materials[0].BaseColor = {1, 0, 0};
    materials[1].BaseColor = {0, 1, 0};
    materials[2].BaseColor = {1, 1, 0};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    fixture.Cameras[0].OrthographicCamera = true;
    const auto update = [&fixture, &meshes, &items, &inputs](uint64 revision) {
        renderer::RenderGraphReport report;
        rhi::RasterStatus result = fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                                        {items.GetData(), items.GetSize()},
                                                        {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                                        revision,
                                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                        report,
                                                        &inputs,
                                                        revision);
        const uint64 beginning = time::NowTicks();
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            result = fixture.Graph.Poll();
        }
        return result;
    };
    const auto read = [&fixture]() { ReadGraphFrame(fixture); };

    REQUIRE(update(1) == rhi::RasterStatus::Ready);
    read();
    const usize leftBottom = GetPixel(17, 43);
    const usize leftTop = GetPixel(36, 20);
    const usize rightBottom = GetPixel(62, 43);
    CHECK(fixture.Pixels[leftBottom] == 0);
    CHECK(fixture.Pixels[leftBottom + 1] == 188);
    CHECK(fixture.Pixels[leftTop] == 188);
    CHECK(fixture.Pixels[leftTop + 1] == 0);
    CHECK(fixture.Pixels[rightBottom] == 188);
    CHECK(fixture.Pixels[rightBottom + 1] == 188);
    CHECK(fixture.Pixels[GetPixel(0, 0) + 2] == 188);
    // A second frame must rebuild imports after the first accepted producer revision.
    read();
    CHECK(fixture.Pixels[rightBottom + 1] == 188);
    // Camera-only updates retain geometry but replace both raster transforms and ray unprojection.
    const auto updateCamera = [&fixture](uint64 revision) {
        renderer::RenderGraphReport report;
        rhi::RasterStatus result = fixture.Graph.Update({},
                                                        {},
                                                        {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                                        revision,
                                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                        report,
                                                        nullptr,
                                                        1);
        const uint64 beginning = time::NowTicks();
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            result = fixture.Graph.Poll();
        }
        REQUIRE(result == rhi::RasterStatus::Ready);
    };
    fixture.Cameras[0].WorldToClip.Columns[3].X = 2;
    updateCamera(2);
    read();
    CHECK(fixture.Pixels[leftBottom + 2] == 188);
    CHECK(fixture.Pixels[rightBottom + 2] == 188);
    fixture.Cameras[0].WorldToClip.Columns[3].X = 0;
    updateCamera(3);
    read();
    CHECK(fixture.Pixels[rightBottom] == 188);
    CHECK(fixture.Pixels[rightBottom + 1] == 188);
    // Exclusion preserves original extraction instance identities rather than compacting them.
    items[0].IsIntentionallyExcluded = true;
    REQUIRE(update(4) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[leftBottom + 2] == 188);
    CHECK(fixture.Pixels[rightBottom] == 188);
    CHECK(fixture.Pixels[rightBottom + 1] == 188);
    items[1].Coverage = renderer::RenderGraphMaterial::Transparent;
    CHECK(update(5) == rhi::RasterStatus::Unsupported);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 4);
    read();
    CHECK(fixture.Pixels[rightBottom + 1] == 188);
}

TEST_CASE("Three ReSTIR shader passes shade mesh emitters and invalidate stale history", "[render-graph][gpu][restir]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_RESTIR_DIRECTORY, "rg2_restir_di.lurgrt"});
    REQUIRE(package->GetPlan().Passes.GetSize() == 3);
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
        packages::rg2_restir_di::MakePrograms(shaders::renderer_composite::Vertex(),
                                              shaders::renderer_composite::Fragment());
    CheckSameStage(programs.RayCompute, compiled.RayCompute);
    bool hasFullCapacity = true;
    SECTION("complete reservoirs") {}
    SECTION("undersized reservoirs fail before indexing")
    {
        hasFullCapacity = false;
    }
    renderer::RenderGraphPlan plan = package->GetPlan();
    StaticArray<format::Resource, format::MAX_RESOURCES> resources{};
    for (usize i = 0; i < plan.Resources.GetSize(); ++i)
    {
        resources[i] = plan.Resources[i];
        if (hasFullCapacity == false && resources[i].Kind == format::Aspect::Buffer)
        {
            resources[i].Width = sizeof(uint32) * 4;
            resources[i].BytesPerPixel = 0;
            resources[i].ViewNumerator = 0;
        }
    }
    plan.Resources = {resources.GetData(), plan.Resources.GetSize()};
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, plan, programs, nullptr, WIDTH, HEIGHT);
    const uint64 started = time::NowTicks();
    constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
    const auto wait = [&fixture, started](rhi::RasterStatus initial) {
        rhi::RasterStatus result = initial;
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - started < TIMEOUT)
        {
            result = fixture.Graph.Poll();
        }
        REQUIRE(result == rhi::RasterStatus::Ready);
    };
    wait(status);
    fixture.Indices = {{0, 2, 1}};
    fixture.Cameras[0].OrthographicCamera = true;
    constexpr usize MATERIAL_COUNT = 2;
    StaticArray<renderer::SceneShaderMaterial, MATERIAL_COUNT> materials{};
    materials[0].BaseColor = {1, 0, 0};
    materials[1].Emission = {8, 8, 8};
    materials[1].IsEmissionTwoSided = true;
    uint32 receiverBinding = 0;
    uint32 emitterBinding = 1;
    StaticArray<renderer::RenderGraphMesh, MATERIAL_COUNT> meshes{};
    meshes[0] = fixture.Meshes[0];
    meshes[0].MaterialBindings = {&receiverBinding, 1};
    meshes[1] = fixture.Meshes[0];
    meshes[1].MaterialBindings = {&emitterBinding, 1};
    constexpr usize ITEM_COUNT = 3;
    StaticArray<renderer::RenderGraphItem, ITEM_COUNT> items{};
    items[0] = fixture.Items[0];
    items[1].SourceId = 2;
    items[1].MeshIndex = 1;
    items[1].MeshToWorld.Translation = {1, 0, -.4F};
    items[2].SourceId = 3;
    items[2].MeshToWorld.Translation.X = 5;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    const auto update = [&fixture, &meshes, &items, &inputs, &wait](uint64 revision) {
        renderer::RenderGraphReport report;
        wait(fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                  {items.GetData(), items.GetSize()},
                                  {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                  revision,
                                  renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                  report,
                                  &inputs));
    };
    const auto read = [&fixture, &package, started, hasFullCapacity]() {
        rhi::SubmissionToken completion;
        REQUIRE(fixture.Graph.Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
        rhi::RasterStatus result = rhi::GetStatus(fixture.Device, completion);
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - started < TIMEOUT)
        {
            result = rhi::GetStatus(fixture.Device, completion);
        }
        REQUIRE(result == rhi::RasterStatus::Ready);
        uint32 resource = 0;
        for (uint32 index = 0; index < package->GetPlan().Resources.GetSize(); ++index)
        {
            if (package->GetPlan().Resources[index].IsExported == 1)
            {
                resource = index;
            }
        }
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect;
        REQUIRE(fixture.Graph.GetExport(resource, texture, aspect) == rhi::RasterStatus::Ready);
        rhi::ReadbackTicket ticket;
        result = rhi::RequestTextureReadback(fixture.Device, texture, {0, 0, 0, WIDTH, HEIGHT}, WIDTH * 4, ticket);
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - started < TIMEOUT)
        {
            result = rhi::GetStatus(fixture.Device, ticket);
        }
        REQUIRE(result == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CopyReadback(fixture.Device, ticket, fixture.Pixels.GetData(), fixture.Pixels.GetSize()) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::Release(fixture.Device, ticket) == rhi::RasterStatus::Ready);
        // Shader failure is magenta. Check the whole image, including miss/boundary invocations.
        for (usize pixel = 0; pixel < WIDTH * HEIGHT; ++pixel)
        {
            constexpr usize CHANNEL_COUNT = 4;
            const usize address = pixel * CHANNEL_COUNT;
            const bool isFailureColor = fixture.Pixels[address] == 255 && fixture.Pixels[address + 1] == 0 &&
                                        fixture.Pixels[address + 2] == 255;
            CHECK(isFailureColor == (hasFullCapacity == false));
        }
    };
    update(1);
    if (hasFullCapacity == false)
    {
        read();
        return;
    }
    // Independent deterministic area quadrature of the CPU reference BSDF.
    renderer::ReferenceBsdf reference;
    reference.BaseColor = materials[0].BaseColor;
    reference.Roughness = materials[0].Roughness;
    constexpr uint32 QUADRATURE_SIDE = 32;
    float32 expectedRed = 0;
    const math::Vector3 receiverPositionWorld{1.0F / static_cast<float32>(WIDTH),
                                              -1.0F / static_cast<float32>(HEIGHT),
                                              .5F};
    for (uint32 row = 0; row < QUADRATURE_SIDE; ++row)
    {
        for (uint32 column = 0; column < QUADRATURE_SIDE; ++column)
        {
            const float32 root = math::Sqrt((static_cast<float32>(row) + .5F) / static_cast<float32>(QUADRATURE_SIDE));
            const float32 v = (static_cast<float32>(column) + .5F) / static_cast<float32>(QUADRATURE_SIDE);
            const float32 first = root * (1 - v);
            const float32 second = root * v;
            const math::Vector3 endpointWorld{.5F + .5F * first + second, -.5F + first, .1F};
            const math::Vector3 differenceWorld = endpointWorld - receiverPositionWorld;
            const float32 distanceSquaredWorld = math::Dot(differenceWorld, differenceWorld);
            const float32 distanceWorld = math::Sqrt(distanceSquaredWorld);
            const math::Vector3 directionWorld = differenceWorld / distanceWorld;
            renderer::ReferenceBsdfEvaluation evaluation;
            REQUIRE(renderer::EvaluateReferenceBsdf(reference, {0, 0, -1}, {0, 0, -1}, directionWorld, evaluation) ==
                    rhi::RasterStatus::Ready);
            const float32 cosine = -directionWorld.Z;
            expectedRed += materials[1].Emission.X * evaluation.Value.X * cosine * cosine / distanceSquaredWorld;
        }
    }
    expectedRed *= .5F / static_cast<float32>(QUADRATURE_SIDE * QUADRATURE_SIDE);
    constexpr usize FRAME_COUNT = 16;
    float32 measuredRed = 0;
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    for (usize frame = 0; frame < FRAME_COUNT; ++frame)
    {
        read();
        CHECK(fixture.Pixels[center] > 0);
        CHECK(fixture.Pixels[center] > fixture.Pixels[center + 1]);
        CHECK(fixture.Pixels[GetPixel(0, 0)] == 0);
        constexpr float32 MAXIMUM_CHANNEL = 255;
        const float32 mapped = math::DecodeSrgb(static_cast<float32>(fixture.Pixels[center]) / MAXIMUM_CHANNEL);
        measuredRed += mapped / (1 - mapped);
    }
    measuredRed /= static_cast<float32>(FRAME_COUNT);
    INFO("area quadrature=" << expectedRed << ", decoded GPU mean=" << measuredRed);
    constexpr float32 RELATIVE_ENERGY_TOLERANCE = .2F;
    CHECK(math::Abs(measuredRed - expectedRed) < RELATIVE_ENERGY_TOLERANCE * expectedRed);
    // Resize both reservoirs with the output, and retain published dimensions while pending.
    REQUIRE(fixture.Graph.Resize(WIDTH * 2, HEIGHT * 2) == rhi::RasterStatus::Pending);
    for (uint32 i = 0; i < plan.Resources.GetSize(); ++i)
    {
        if (plan.Resources[i].Kind == format::Aspect::Buffer)
        {
            format::Resource description{};
            REQUIRE(fixture.Graph.GetResourceDescription(i, description) == true);
            CHECK(description.Width == WIDTH * HEIGHT * 64);
        }
    }
    update(1);
    for (uint32 i = 0; i < plan.Resources.GetSize(); ++i)
    {
        if (plan.Resources[i].Kind == format::Aspect::Buffer)
        {
            format::Resource description{};
            REQUIRE(fixture.Graph.GetResourceDescription(i, description) == true);
            CHECK(description.Width == WIDTH * HEIGHT * 64 * 4);
        }
    }
    read();
    CHECK(fixture.Graph.Resize(3840, 2160) == rhi::RasterStatus::CapacityExceeded);
    read(); // A rejected oversized replacement keeps the last drawable generation.
    REQUIRE(fixture.Graph.Resize(WIDTH, HEIGHT) == rhi::RasterStatus::Pending);
    update(1);
    read();
    // A displaced opaque copy blocks every receiver-to-light ray but not the camera ray.
    items[2].MeshToWorld.Translation = {.5F, 0, -.2F};
    update(2);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 1] == 0);
    CHECK(fixture.Pixels[center + 2] == 0);
    // Buffers survive replacement: stale emitter samples must not relight this frame.
    materials[1].Emission = {0, 0, 0};
    update(3);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 1] == 0);
    CHECK(fixture.Pixels[center + 2] == 0);
    materials[0].Emission = {2, 1, 0};
    materials[0].IsEmissionTwoSided = true;
    update(4);
    read();
    CHECK(fixture.Pixels[center] == 213);
    CHECK(fixture.Pixels[center + 1] == 188);
    CHECK(fixture.Pixels[center + 2] == 0);
    fixture.Cameras[0].WorldToClip.Columns[3].X = -2;
    update(5);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 1] == 0);
    CHECK(fixture.Pixels[center + 2] == 0);
}

TEST_CASE("Built-in visibility templates preserve IDs and reconstruct reflected emitter hits",
          "[render-graph][gpu][visibility-template]")
{
    GraphGpuFixture fixture;
    bool hasSecondaryRays = false;
    SECTION("visibility and depth exports") {}
    SECTION("reconstructed surface and secondary rays")
    {
        hasSecondaryRays = true;
    }
    const PackageLocation location =
        hasSecondaryRays == true
            ? PackageLocation{LUDUS_RG3_VISIBILITY_SECONDARY_RAYS_TEMPLATE_DIRECTORY,
                              "rg2_visibility_secondary_rays_template.lurgrt"}
            : PackageLocation{LUDUS_RG3_VISIBILITY_BUFFER_TEMPLATE_DIRECTORY, "rg2_visibility_buffer_template.lurgrt"};
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(location);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
        hasSecondaryRays == true
            ? packages::rg2_visibility_secondary_rays_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                             shaders::renderer_composite::Fragment())
            : packages::rg2_visibility_buffer_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                     shaders::renderer_composite::Fragment());
    CheckSameStage(programs.ColorVertex, compiled.ColorVertex);
    if (hasSecondaryRays == true)
    {
        CheckSameStage(programs.RayCompute, compiled.RayCompute);
    }
    constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
    const auto wait = [&fixture](rhi::RasterStatus initial, const char* operation) {
        INFO(operation);
        rhi::RasterStatus status = initial;
        const uint64 beginning = time::NowTicks();
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            status = fixture.Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    };
    wait(fixture.Graph.Initialize(fixture.Device, plan, programs, nullptr, WIDTH, HEIGHT), "initialize");
    fixture.Cameras[0].OrthographicCamera = true;
    constexpr usize MATERIAL_COUNT = 2;
    StaticArray<renderer::SceneShaderMaterial, MATERIAL_COUNT> materials{};
    materials[0].BaseColor = {1, 1, 1};
    materials[1].Emission = {2, 1, 0};
    materials[1].IsEmissionTwoSided = true;
    StaticArray<renderer::RenderGraphMesh, MATERIAL_COUNT> meshes{};
    StaticArray<renderer::RenderGraphItem, MATERIAL_COUNT> items{};
    uint32 receiverBinding = 0;
    uint32 emitterBinding = 1;
    meshes[0] = fixture.Meshes[0];
    meshes[0].MaterialBindings = {&receiverBinding, 1};
    meshes[1] = fixture.Meshes[0];
    meshes[1].MaterialBindings = {&emitterBinding, 1};
    items[0].SourceId = 1;
    items[1].SourceId = 2;
    items[1].MeshIndex = 1;
    // Behind the near clip plane: absent from raster visibility, reachable by reflection.
    items[1].MeshToWorld.Translation.Z = -.75F;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    inputs.Environment = {0, 0, 1};
    const auto update = [&fixture, &wait, &meshes, &items, &inputs](uint64 revision) {
        renderer::RenderGraphReport report;
        wait(fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                  {items.GetData(), items.GetSize()},
                                  {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                  revision,
                                  renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                  report,
                                  &inputs,
                                  revision),
             "update");
    };
    const uint32 colorResource = hasSecondaryRays == true ? plan.Passes[1].Color : plan.Passes[0].Color;
    update(1);
    ReadGraphFrame(fixture, colorResource);
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    if (hasSecondaryRays == false)
    {
        CHECK(fixture.Pixels[center] == 0);
        CHECK(fixture.Pixels[center + 1] == 0);
        CHECK(fixture.Pixels[center + 2] == 0);
        CHECK(fixture.Pixels[center + 3] == 1);
        CHECK(fixture.Pixels[GetPixel(0, 0) + 3] == 0);
        rhi::TextureHandle depth;
        rhi::TextureAspect aspect;
        REQUIRE(fixture.Graph.GetExport(plan.Passes[0].Depth, depth, aspect) == rhi::RasterStatus::Ready);
        CHECK(aspect == rhi::TextureAspect::Depth);
        return;
    }
    // A reconstructed first hit leaves the one permitted surface query for the hidden emitter.
    CHECK(fixture.Pixels[center] == 213);
    CHECK(fixture.Pixels[center + 1] == 188);
    CHECK(fixture.Pixels[center + 2] == 0);
    CHECK(fixture.Pixels[GetPixel(0, 0) + 2] == 188);
    materials[1].Emission = {0, 0, 0};
    update(2);
    ReadGraphFrame(fixture, colorResource);
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 1] == 0);
    CHECK(fixture.Pixels[center + 2] == 0);
    items[1].MeshToWorld.Translation.X = 2;
    update(3);
    ReadGraphFrame(fixture, colorResource);
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 2] == 188);
}

TEST_CASE("Explicit Accumulate averages linear ray output and stops at its authored limit",
          "[render-graph][gpu][accumulation]")
{
    GraphGpuFixture fixture;
    const renderer::RenderGraphPrograms programs =
        packages::rg2_ordered_ray::MakePrograms(shaders::renderer_composite::Vertex(),
                                                shaders::renderer_composite::Fragment());
    renderer::RenderGraphPlan plan = packages::rg2_ordered_ray::MakePlan();
    StaticArray<format::Pass, format::MAX_RAY_PASSES> passes{};
    REQUIRE(plan.Passes.GetSize() <= passes.GetSize());
    for (usize pass = 0; pass < plan.Passes.GetSize(); ++pass)
    {
        passes[pass] = plan.Passes[pass];
    }
    constexpr uint32 SAMPLE_LIMIT = 2;
    passes[plan.Passes.GetSize() - 1].AccumulationSamples = SAMPLE_LIMIT;
    plan.Passes = {passes.GetData(), plan.Passes.GetSize()};
    const auto finish = [&fixture](rhi::RasterStatus status) {
        const uint64 beginning = time::NowTicks();
        constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            status = fixture.Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    };
    finish(fixture.Graph.Initialize(fixture.Device, plan, programs));
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
    uint32 output = 0;
    for (uint32 resource = 0; resource < plan.Resources.GetSize(); ++resource)
    {
        if (plan.Resources[resource].IsExported == 1)
        {
            output = resource;
        }
    }
    REQUIRE(fixture.Graph.HasPendingSamples() == true);
    ReadGraphFrame(fixture, output);
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    CHECK(fixture.Pixels[center] == 188); // Reinhard/sRGB of linear sample 1.
    CHECK(fixture.Graph.HasPendingSamples() == true);
    ReadGraphFrame(fixture, output);
    CHECK(fixture.Pixels[center] == 203); // Display conversion of mean(1, 2) = 1.5.
    CHECK(fixture.Pixels[center + 1] == 0);
    CHECK(fixture.Graph.HasPendingSamples() == false);
    ReadGraphFrame(fixture, output);
    CHECK(fixture.Pixels[center] == 203);
    // An accepted camera revision restarts the averaging budget without clearing shared buffers.
    finish(fixture.Graph.Update({},
                                {},
                                {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                2,
                                renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                report,
                                nullptr,
                                1));
    CHECK(fixture.Graph.HasPendingSamples() == true);
}
