#include "../src/internal/gpu_denoiser.h"

#include "renderer_composite.h"
#include "rg2_compute_mixed.h"
#include "rg2_compute_ray_tracing_template.h"
#include "rg2_compute_split.h"
#include "rg2_direct_lighting_reference.h"
#include "rg2_line_sampling_template.h"
#include "rg2_line_visibility_loop.h"
#include "rg2_ordered_ray.h"
#include "rg2_raytracing.h"
#include "rg2_restir_di.h"
#include "rg2_salesin_jarosz_lines_template.h"
#include "rg2_scene_compute.h"
#include "rg2_scene_ray.h"
#include "rg2_shared_ray_computebvh.h"
#if defined(__APPLE__)
#    include "rg2_mixed_invocation.h"
#    include "rg2_native_ray.h"
#    include "rg2_native_visibility.h"
#endif
#include "rg2_shared_ray_default.h"
#include "rg2_simple_compute_ray_tracing_template.h"
#include "rg2_storage_ray.h"
#include "rg2_tiled_vbuffer_nee_template.h"
#include "rg2_veach_guibas_mis_template.h"
#include "rg2_visibility.h"
#include "rg2_visibility_buffer_template.h"
#include "rg2_visibility_ordinary.h"
#include "rg2_visibility_secondary_rays_template.h"
#include "rg2_visibility_sequence.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>
#include <new>
#include <string>
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
    explicit GraphGpuFixture(bool shouldEnableGpuTiming = false, bool shouldRequireNativeRays = true)
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Storage.ResourcesPerGraph = renderer::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
        description.EnableGpuTiming = shouldEnableGpuTiming;
        description.Preferred.RayQueries = shouldRequireNativeRays;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus started =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface, &failure);
        if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            FAIL("Ray pipeline acceptance requires a GPU adapter");
        }
        REQUIRE(started == rhi::DeviceStatus::Ready);
        rhi::RayCapabilities capabilities;
        if (shouldRequireNativeRays == true &&
            rhi::GetRayCapabilities(Device, capabilities) == rhi::RasterStatus::Unsupported)
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
    renderer::RenderGraphPlan plan = package->GetPlan();
    format::Pass operation = plan.Passes[0];
    SECTION("clamped linear output") {}
    SECTION("explicit ToneMap")
    {
        operation.ToneMappingExposure = 1;
    }
    const uint8 expected = operation.ToneMappingExposure == 0 ? 255 : 188;
    plan.Passes = {&operation, 1};
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, plan, decoded);
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
    CHECK(fixture.Pixels[center] == expected);
    CHECK(fixture.Pixels[center + 2] == 0);
    CHECK(fixture.Pixels[GetPixel(0, 0) + 2] == expected);
    fixture.Items[0].MeshToWorld.Translation.X = 2;
    REQUIRE(update(2) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 2] == expected);
    fixture.Cameras[0].WorldToClip.Columns[3].X = -2;
    REQUIRE(update(3) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[center] == expected);
    fixture.Items[0].Material.Color = {0, 1, 0, 1};
    REQUIRE(update(4) == rhi::RasterStatus::Ready);
    read();
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 1] == expected);
    fixture.Items[0].Coverage = renderer::RenderGraphMaterial::Transparent;
    CHECK(update(5) == rhi::RasterStatus::Unsupported);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 4);
    read();
    CHECK(fixture.Pixels[center + 1] == expected);
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
    CHECK(fixture.Pixels[center + 2] == expected);
}

TEST_CASE("Scene callbacks preserve material and scene generation through native GPU rendering",
          "[render-graph][gpu][scene-shader]")
{
    format::RayTraversal traversal = format::RayTraversal::NativeQuery;
    bool shouldUseNativeStages = false;
    SECTION("native traversal") {}
#if defined(__APPLE__)
    SECTION("native ray stage function tables")
    {
        shouldUseNativeStages = true;
    }
#endif
    SECTION("triangle-loop traversal without requested ray features")
    {
        traversal = format::RayTraversal::TriangleLoop;
    }
    SECTION("compute BVH traversal without requested ray features")
    {
        traversal = format::RayTraversal::ComputeBvh;
    }
    const bool shouldUseSoftwareTraversal = traversal != format::RayTraversal::NativeQuery;
    GraphGpuFixture fixture(false, shouldUseSoftwareTraversal == false);
    const renderer::RenderGraphPrograms programs =
#if defined(__APPLE__)
        shouldUseNativeStages == true ? packages::rg2_native_ray::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                               shaders::renderer_composite::Fragment())
        :
#endif
        traversal == format::RayTraversal::ComputeBvh
            ? packages::rg2_shared_ray_computebvh::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                shaders::renderer_composite::Fragment())
        : traversal == format::RayTraversal::TriangleLoop
            ? packages::rg2_shared_ray_default::MakePrograms(shaders::renderer_composite::Vertex(),
                                                             shaders::renderer_composite::Fragment())
            : packages::rg2_scene_ray::MakePrograms(shaders::renderer_composite::Vertex(),
                                                    shaders::renderer_composite::Fragment());
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(
#if defined(__APPLE__)
        shouldUseNativeStages == true ? PackageLocation{LUDUS_RG3_NATIVE_RAY_DIRECTORY, "rg2_native_ray.lurgrt"} :
#endif
        traversal == format::RayTraversal::ComputeBvh
            ? PackageLocation{LUDUS_RG3_SHARED_RAY_COMPUTEBVH_DIRECTORY, "rg2_shared_ray_computebvh.lurgrt"}
        : traversal == format::RayTraversal::TriangleLoop
            ? PackageLocation{LUDUS_RG3_SHARED_RAY_DEFAULT_DIRECTORY, "rg2_shared_ray_default.lurgrt"}
            : PackageLocation{LUDUS_RG3_SCENE_RAY_DIRECTORY, "rg2_scene_ray.lurgrt"});
    CHECK((shouldUseNativeStages == true || package->GetPlan().Passes[0].Traversal == traversal));
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    CHECK(renderer::RequiresHardwareRayTracing(package->GetPlan()) == (shouldUseSoftwareTraversal == false));
    for (const rhi::RasterBinding& binding : decoded.RayCompute.Bindings)
    {
        if (shouldUseSoftwareTraversal == true)
        {
            CHECK(binding.Kind != rhi::RasterBindingKind::RayScene);
        }
    }
    CHECK(programs.HasSceneShaderInterface == true);
    CHECK(decoded.HasSceneShaderInterface == true);
    REQUIRE(package->GetPlan().Passes[0].Program == format::Technique::RayTracing);
    CheckSameStage(decoded.RayCompute, programs.RayCompute);
    CheckSameStage(decoded.RayImageVertex, programs.RayImageVertex);
    CheckSameStage(decoded.RayImageFragment, programs.RayImageFragment);
    CHECK(decoded.NativeRayStages.Artifact.HasNativeRayStages == shouldUseNativeStages);
    if (shouldUseNativeStages == true)
    {
        CheckSameStage(decoded.NativeRayStages, programs.NativeRayStages);
        for (const usize axis : {usize{0}, usize{1}, usize{2}})
        {
            CHECK(decoded.NativeRayStages.WorkgroupSize[axis] == programs.NativeRayStages.WorkgroupSize[axis]);
            CHECK(decoded.NativeRayStages.WorkgroupSize[axis] > 0);
        }
        CHECK(package->GetPlan().Passes[0].Traversal == format::RayTraversal::HardwarePipeline);
    }
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

#if defined(__APPLE__)
TEST_CASE("Native Metal intersection callbacks read bound scene data and reject opaque candidates",
          "[render-graph][gpu][native-ray]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_NATIVE_RAY_DIRECTORY, "rg2_native_ray.lurgrt"});
    renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    // Replace only the intersection callback's final predicate. Reading the first
    // packed World position proves its private buffer binding reaches the callback;
    // rejecting an opaque triangle proves the intersector invokes the function table.
    std::string source(programs.NativeRayStages.Artifact.Msl);
    const usize predicate = source.rfind("return LudusNativeAcceptTriangle_");
    REQUIRE(predicate != std::string::npos);
    const usize end = source.find(';', predicate);
    REQUIRE(end != std::string::npos);
    source.replace(predicate, end - predicate + 1, "return as_type<float>(sceneWords_1[0].x) != -0.5f;");
    programs.NativeRayStages.Artifact.Msl = source;
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs);
    const uint64 beginning = time::NowTicks();
    constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        status = fixture.Graph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    fixture.Cameras[0].OrthographicCamera = true;
    renderer::SceneShaderInputs inputs;
    inputs.Environment = {0, 0, 1};
    renderer::RenderGraphReport report;
    status = fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                  {fixture.Items.GetData(), fixture.Items.GetSize()},
                                  {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                  1,
                                  renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                  report,
                                  &inputs);
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        status = fixture.Graph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    ReadGraphFrame(fixture);
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    CHECK(fixture.Pixels[center] == 0);
    constexpr uint8 EXPECTED_BLUE = 188;
    CHECK(fixture.Pixels[center + 2] == EXPECTED_BLUE);
}
#endif

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
    constexpr usize WORD_BYTES = sizeof(uint32);
    const auto readWord = [&bytes](usize offset) {
        uint32 value = 0;
        REQUIRE(TryReadLittleEndian({bytes.GetData() + offset, WORD_BYTES}, value) == true);
        return value;
    };
    const auto padWord = [](uint32 byteCount) { return ((byteCount + WORD_BYTES - 1) / WORD_BYTES) * WORD_BYTES; };
    constexpr usize RESOURCE_COUNT_OFFSET = 2 * WORD_BYTES;
    constexpr usize PASS_COUNT_OFFSET = 3 * WORD_BYTES;
    constexpr usize CAMERA_COUNT_OFFSET = 4 * WORD_BYTES;
    constexpr usize UNIFORM_BINDING_COUNT_OFFSET = 6 * WORD_BYTES;
    constexpr usize UNIFORM_BLOCK_COUNT_OFFSET = 7 * WORD_BYTES;
    constexpr usize UNIFORM_FIELD_COUNT_OFFSET = 8 * WORD_BYTES;
    constexpr usize STAGE_COUNT_OFFSET = 9 * WORD_BYTES;
    const uint32 resourceCount = readWord(RESOURCE_COUNT_OFFSET);
    const uint32 cameraCount = readWord(CAMERA_COUNT_OFFSET);
    // Version 21 inserts workgroup words into each ray compute stage and version 20 admits a
    // native ray-stage record. Stages follow every fixed-width record; strip them first.
    const uint32 stageCount = readWord(STAGE_COUNT_OFFSET);
    usize stage = format::HEADER_BYTES + resourceCount * format::RESOURCE_RECORD_BYTES +
                  cameraCount * format::CAMERA_RECORD_BYTES + readWord(PASS_COUNT_OFFSET) * format::PASS_RECORD_BYTES +
                  readWord(UNIFORM_BINDING_COUNT_OFFSET) * format::UNIFORM_BINDING_RECORD_BYTES +
                  readWord(UNIFORM_BLOCK_COUNT_OFFSET) * format::UNIFORM_BLOCK_RECORD_BYTES +
                  readWord(UNIFORM_FIELD_COUNT_OFFSET) * format::UNIFORM_FIELD_RECORD_BYTES;
    uint32 legacyStageCount = 0;
    for (uint32 i = 0; i < stageCount; ++i)
    {
        const uint32 role = readWord(stage);
        const uint32 inputCount = readWord(stage + 2 * WORD_BYTES);
        const uint32 entryBytes = readWord(stage + 3 * WORD_BYTES);
        const uint32 artifactBytes = readWord(stage + 4 * WORD_BYTES);
        const uint32 blockCount = readWord(stage + 5 * WORD_BYTES);
        // Forward stages carry a texture-name table this walk does not skip; none are expected here.
        REQUIRE((role < format::FORWARD_STAGE_ROLE_FIRST || role == format::NATIVE_RAY_STAGE_ROLE) == true);
        usize end = stage + format::STAGE_FIELD_COUNT * WORD_BYTES;
        if (role == static_cast<uint32>(format::StageRole::RayCompute) ||
            role == static_cast<uint32>(format::StageRole::SceneRayCompute) ||
            role == static_cast<uint32>(format::StageRole::PersistentRayCompute))
        {
            bytes.RemoveRange(end, end + format::RAY_WORKGROUP_DIMENSIONS * WORD_BYTES);
        }
        end += inputCount * format::INPUT_RECORD_BYTES;
        for (uint32 block = 0; block < blockCount; ++block)
        {
            end += 2 * WORD_BYTES + padWord(readWord(end + WORD_BYTES));
        }
        end += padWord(entryBytes) + padWord(artifactBytes);
        if (role == format::NATIVE_RAY_STAGE_ROLE)
        {
            bytes.RemoveRange(stage, end);
        }
        else
        {
            stage = end;
            ++legacyStageCount;
        }
    }
    REQUIRE(TryWriteLittleEndian(legacyStageCount, {bytes.GetData() + STAGE_COUNT_OFFSET, WORD_BYTES}) == true);
    // Versions 6 through 8 lack the three per-pass words added in version 9.
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
    bool hasMixedInvocations = false;
    SECTION("compute invocations") {}
#if defined(__APPLE__)
    SECTION("native stages, inline query compute and software compute")
    {
        hasMixedInvocations = true;
    }
#endif
    GraphGpuFixture fixture(hasMixedInvocations);
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(
#if defined(__APPLE__)
        hasMixedInvocations == true
            ? PackageLocation{LUDUS_RG3_MIXED_INVOCATION_DIRECTORY, "rg2_mixed_invocation.lurgrt"}
            :
#endif
            PackageLocation{LUDUS_RG3_ORDERED_RAY_DIRECTORY, "rg2_ordered_ray.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
#if defined(__APPLE__)
        hasMixedInvocations == true
            ? packages::rg2_mixed_invocation::MakePrograms(shaders::renderer_composite::Vertex(),
                                                           shaders::renderer_composite::Fragment())
            :
#endif
            packages::rg2_ordered_ray::MakePrograms(shaders::renderer_composite::Vertex(),
                                                    shaders::renderer_composite::Fragment());
    CHECK(decoded.NativeRayStages.Artifact.HasNativeRayStages == hasMixedInvocations);
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
            if (hasMixedInvocations == true)
            {
                REQUIRE(rhi::ArmGpuPassTimings(fixture.Device) == rhi::RasterStatus::Ready);
            }
            rhi::SubmissionToken completion;
            REQUIRE(fixture.Graph.Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
            status = rhi::GetStatus(fixture.Device, completion);
            while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
            {
                status = rhi::GetStatus(fixture.Device, completion);
            }
            REQUIRE(status == rhi::RasterStatus::Ready);
            if (hasMixedInvocations == true)
            {
                rhi::GpuPassTimings timings;
                REQUIRE(rhi::GetGpuPassTimings(fixture.Device, timings) == rhi::RasterStatus::Ready);
                bool hasNativeTiming = false;
                bool hasComputeTiming = false;
                bool hasRasterTiming = false;
                for (usize index = 0; index < timings.Count; ++index)
                {
                    hasNativeTiming =
                        hasNativeTiming == true || timings.Passes[index].Kind == rhi::GpuPassKind::RayTracing;
                    hasComputeTiming =
                        hasComputeTiming == true || timings.Passes[index].Kind == rhi::GpuPassKind::Compute;
                    hasRasterTiming = hasRasterTiming == true || timings.Passes[index].Kind == rhi::GpuPassKind::Raster;
                }
                CHECK(hasNativeTiming == true);
                CHECK(hasComputeTiming == true);
                CHECK(hasRasterTiming == true);
            }
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

namespace
{
/// Render a two-pass hit-record package (compute_split, compute_mixed) and require that every
/// packed triangle index recorded by pass 0 resolves to the same hit in pass 1. See their shade.slang.
void CheckRecordedHitsResolveInLaterPass(const PackageLocation& location, const renderer::RenderGraphPrograms& compiled)
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(location);
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(decoded.HasPersistentRayBuffers == true);
    REQUIRE(decoded.HasSceneShaderInterface == true);
    CheckSameStage(decoded.RayCompute, compiled.RayCompute);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    usize rayPassCount = 0;
    uint32 exportResource = 0;
    for (usize pass = 0; pass < plan.Passes.GetSize(); ++pass)
    {
        rayPassCount += plan.Passes[pass].Program == format::Technique::RayTracing ? 1 : 0;
    }
    for (uint32 index = 0; index < plan.Resources.GetSize(); ++index)
    {
        exportResource = plan.Resources[index].IsExported == 1 ? index : exportResource;
    }
    REQUIRE(rayPassCount == 2);
    // More triangles than one BVH leaf holds, in two instances, so the native order differs from
    // the packed triangle order that the record stores.
    constexpr usize GRID_COLUMNS = 3;
    constexpr usize GRID_ROWS = 2;
    constexpr usize GRID_VERTEX_COUNT = (GRID_COLUMNS + 1) * (GRID_ROWS + 1);
    constexpr usize QUAD_INDEX_COUNT = 6;
    constexpr usize GRID_INDEX_COUNT = GRID_COLUMNS * GRID_ROWS * QUAD_INDEX_COUNT;
    constexpr usize GRID_ITEM_COUNT = 2;
    constexpr float32 CELL_WIDTH = .2F;
    constexpr float32 CELL_HEIGHT = .6F;
    StaticArray<math::Vector3, GRID_VERTEX_COUNT> positions{};
    StaticArray<uint32, GRID_INDEX_COUNT> indices{};
    for (usize row = 0; row <= GRID_ROWS; ++row)
    {
        for (usize column = 0; column <= GRID_COLUMNS; ++column)
        {
            positions[row * (GRID_COLUMNS + 1) + column] = {float32(column) * CELL_WIDTH,
                                                            float32(row) * CELL_HEIGHT - CELL_HEIGHT,
                                                            .5F};
        }
    }
    usize index = 0;
    for (usize row = 0; row < GRID_ROWS; ++row)
    {
        for (usize column = 0; column < GRID_COLUMNS; ++column)
        {
            const uint32 corner = static_cast<uint32>(row * (GRID_COLUMNS + 1) + column);
            const uint32 above = corner + static_cast<uint32>(GRID_COLUMNS + 1);
            const StaticArray<uint32, QUAD_INDEX_COUNT> quad{{corner, corner + 1, above + 1, corner, above + 1, above}};
            for (const uint32 vertex : quad)
            {
                indices[index] = vertex;
                ++index;
            }
        }
    }
    StaticArray<renderer::RenderGraphMesh, 1> meshes{};
    meshes[0].Geometry = {positions.GetData(), positions.GetSize(), indices.GetData(), indices.GetSize()};
    StaticArray<renderer::RenderGraphItem, GRID_ITEM_COUNT> items{};
    items[0].SourceId = 1;
    items[0].MeshToWorld.Translation.X = .05F;
    items[1].SourceId = 2;
    items[1].MeshToWorld.Translation.X = -.7F;
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, plan, decoded);
    constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
    const uint64 beginning = time::NowTicks();
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        status = fixture.Graph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    fixture.Cameras[0].OrthographicCamera = true;
    renderer::RenderGraphReport report;
    const auto update = [&fixture, &meshes, &items, &report, beginning](uint64 revision, bool shouldReuseScene) {
        rhi::RasterStatus result = fixture.Graph.Update(
            shouldReuseScene == true
                ? BorrowedView<const renderer::RenderGraphMesh>{}
                : BorrowedView<const renderer::RenderGraphMesh>{meshes.GetData(), meshes.GetSize()},
            shouldReuseScene == true ? BorrowedView<const renderer::RenderGraphItem>{}
                                     : BorrowedView<const renderer::RenderGraphItem>{items.GetData(), items.GetSize()},
            {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
            revision,
            renderer::RenderGraphCoveragePolicy::RejectIncomplete,
            report,
            nullptr,
            1);
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            result = fixture.Graph.Poll();
        }
        return result;
    };
    REQUIRE(update(1, false) == rhi::RasterStatus::Ready);
    constexpr usize FRAME_COUNT = 4;
    constexpr usize CAMERA_FRAME_INDEX = 2;
    // ToneMap exposure 1 encodes a linear 1 as Reinhard 0.5, which is sRGB byte 188.
    constexpr uint8 TONE_MAPPED_ONE = 188;
    constexpr usize BYTE_VALUE_COUNT = 256;
    for (usize frame = 0; frame < FRAME_COUNT; ++frame)
    {
        if (frame == CAMERA_FRAME_INDEX)
        {
            fixture.Cameras[0].WorldToClip.Columns[3].Y = .1F;
            REQUIRE(update(2, true) == rhi::RasterStatus::Ready);
        }
        ReadGraphFrame(fixture, exportResource);
        usize hitCount = 0;
        usize mismatchCount = 0;
        usize uncorrelatedCount = 0;
        StaticArray<bool, BYTE_VALUE_COUNT> hasTriangleShade{};
        for (usize pixel = 0; pixel < static_cast<usize>(WIDTH) * HEIGHT; ++pixel)
        {
            const uint8 identity = fixture.Pixels[pixel * 4];
            const uint8 triangle = fixture.Pixels[pixel * 4 + 1];
            const uint8 correlation = fixture.Pixels[pixel * 4 + 2];
            mismatchCount += identity == TONE_MAPPED_ONE ? 0 : 1;
            uncorrelatedCount += correlation == TONE_MAPPED_ONE ? 0 : 1;
            hitCount += triangle != 0 ? 1 : 0;
            hasTriangleShade[triangle] = true;
        }
        usize distinctTriangleShades = 0;
        for (usize shade = 1; shade < hasTriangleShade.GetSize(); ++shade)
        {
            distinctTriangleShades += hasTriangleShade[shade] == true ? 1 : 0;
        }
        CAPTURE(frame, hitCount, distinctTriangleShades);
        // Every pass-0 triangle index and miss resolves to the same hit in pass 1.
        CHECK(mismatchCount == 0);
        // Every pass draws its first automatic dimension from the same address; explicit offsets do not.
        CHECK(uncorrelatedCount == 0);
        CHECK(hitCount > 0);
        CHECK(hitCount < static_cast<usize>(WIDTH) * HEIGHT);
        CHECK(distinctTriangleShades > 2);
    }
}
} // namespace

TEST_CASE("A raygen-only pass hands packed triangle hits to a later pass",
          "[render-graph][gpu][ray-history][compute-split]")
{
    // Compute ray-tracing phase C0: pass 0 traces and records a packed triangle index; pass 1
    // resolves it in its own separately built scene and BLAS/TLAS.
    CheckRecordedHitsResolveInLaterPass(
        {LUDUS_RG3_COMPUTE_SPLIT_DIRECTORY, "rg2_compute_split.lurgrt"},
        packages::rg2_compute_split::MakePrograms(shaders::renderer_composite::Vertex(),
                                                  shaders::renderer_composite::Fragment()));
}

TEST_CASE("A scene compute pass hands packed triangle hits to a later ray pass",
          "[render-graph][gpu][compute-ray-tracing][compute-mixed]")
{
    // Compute ray-tracing C2: pass 0 is a ComputePipeline kernel and pass 1 a RayTracingPipeline in one
    // persistent wrapper. The ray pass's TraceAndShade must still dispatch its own hit and miss callbacks.
    CheckRecordedHitsResolveInLaterPass(
        {LUDUS_RG3_COMPUTE_MIXED_DIRECTORY, "rg2_compute_mixed.lurgrt"},
        packages::rg2_compute_mixed::MakePrograms(shaders::renderer_composite::Vertex(),
                                                  shaders::renderer_composite::Fragment()));
}

namespace
{
/// Display byte of linear radiance 1 after the fused ToneMap: exposure 0 bypasses it, 1 and 2 apply Reinhard + sRGB.
constexpr uint8 GetDisplayedOne(float32 exposure) noexcept
{
    return exposure == 0 ? 255 : exposure == 1 ? 188 : 213;
}
/// Display byte of the accumulated mean of the linear samples 1 and 2, encoded like GetDisplayedOne.
constexpr uint8 GetDisplayedMean(float32 exposure) noexcept
{
    return exposure == 0 ? 255 : exposure == 1 ? 203 : 225;
}
} // namespace

TEST_CASE("A scene compute pass traces the World and feeds Accumulate and ToneMap",
          "[render-graph][gpu][compute-ray-tracing][accumulation][tone-mapping]")
{
    // scene_info.slang: red = sample number, green = camera-ray hit, blue = GetSceneInfo triangle count.
    // Its leftmost column calls TraceAndShade, which a compute pass must turn into the error color.
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_SCENE_COMPUTE_DIRECTORY, "rg2_scene_compute.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
        packages::rg2_scene_compute::MakePrograms(shaders::renderer_composite::Vertex(),
                                                  shaders::renderer_composite::Fragment());
    // One compute pass still uses the persistent wrapper, which owns GraphBuffer0/1 and the pass index.
    REQUIRE(decoded.HasPersistentRayBuffers == true);
    REQUIRE(decoded.HasSceneShaderInterface == true);
    CheckSameStage(decoded.RayCompute, compiled.RayCompute);
    renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(renderer::RequiresHardwareRayTracing(plan) == true);
    constexpr usize PASS_COUNT = 1;
    REQUIRE(plan.Passes.GetSize() == PASS_COUNT);
    REQUIRE(plan.Passes[0].Program == format::Technique::RayTracing);
    constexpr uint32 SAMPLE_LIMIT = 2;
    REQUIRE(plan.Passes[0].AccumulationSamples == SAMPLE_LIMIT);
    REQUIRE(plan.Passes[0].ToneMappingExposure == 1);
    StaticArray<format::Pass, PASS_COUNT> passes{{plan.Passes[0]}};
    float32 exposure = 1;
    SECTION("authored unit exposure") {}
    SECTION("bypass")
    {
        exposure = 0;
    }
    SECTION("double exposure")
    {
        exposure = 2;
    }
    passes[0].ToneMappingExposure = exposure;
    plan.Passes = {passes.GetData(), passes.GetSize()};
    const auto finish = [&fixture](rhi::RasterStatus status) {
        const uint64 beginning = time::NowTicks();
        constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            status = fixture.Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    };
    finish(fixture.Graph.Initialize(fixture.Device, plan, decoded));
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
        output = plan.Resources[resource].IsExported == 1 ? resource : output;
    }
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    const usize corner = GetPixel(WIDTH - 1, 0);
    const usize failure = GetPixel(0, HEIGHT / 2);
    const uint8 one = GetDisplayedOne(exposure);
    const uint8 mean = GetDisplayedMean(exposure);
    REQUIRE(fixture.Graph.HasPendingSamples() == true);
    ReadGraphFrame(fixture, output);
    CHECK(fixture.Pixels[center] == one);     // Sample 1.
    CHECK(fixture.Pixels[center + 1] == one); // The camera ray hits the triangle.
    CHECK(fixture.Pixels[center + 2] == one); // GetSceneInfo reports the one World triangle.
    CHECK(fixture.Pixels[corner] == one);
    CHECK(fixture.Pixels[corner + 1] == 0); // Miss.
    CHECK(fixture.Pixels[corner + 2] == one);
    // TraceAndShade has no hit or miss callback in a compute pass: the error color, not the payload.
    CHECK(fixture.Pixels[failure] == 255);
    CHECK(fixture.Pixels[failure + 1] == 0);
    CHECK(fixture.Pixels[failure + 2] == 255);
    CHECK(fixture.Graph.HasPendingSamples() == true);
    ReadGraphFrame(fixture, output);
    CHECK(fixture.Pixels[center] == mean); // Accumulate averages samples 1 and 2 before ToneMap.
    CHECK(fixture.Pixels[center + 1] == one);
    CHECK(fixture.Pixels[corner + 1] == 0);
    CHECK(fixture.Pixels[failure + 1] == 0);
    CHECK(fixture.Graph.HasPendingSamples() == false);
    ReadGraphFrame(fixture, output);
    CHECK(fixture.Pixels[center] == mean); // The sample limit stops the compute pass.
    finish(fixture.Graph.Update({},
                                {},
                                {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                2,
                                renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                report,
                                nullptr,
                                1));
    CHECK(fixture.Graph.HasPendingSamples() == true);
    ReadGraphFrame(fixture, output);
    CHECK(fixture.Pixels[center] == one); // A camera revision restarts at sample 1.
}

TEST_CASE("Emitter bounds read on the GPU match their packed emitter triangles",
          "[render-graph][gpu][compute-ray-tracing][scene-shader]")
{
    // emitter_bounds.slang recomputes every emitter's bounds from GetEmitterTriangle and GetMaterial:
    // green columns agree with GetEmitterBounds, red columns do not, and the column past the last
    // emitter is blue when that out-of-range read reports IsValid=false.
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_EMITTER_BOUNDS_DIRECTORY, "rg2_emitter_bounds.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(decoded.HasSceneShaderInterface == true);
    renderer::RenderGraphPlan plan = package->GetPlan();
    constexpr usize PASS_COUNT = 1;
    REQUIRE(plan.Passes.GetSize() == PASS_COUNT);
    StaticArray<format::Pass, PASS_COUNT> passes{{plan.Passes[0]}};
    passes[0].ToneMappingExposure = 0; // Bypass ToneMap: linear 1 reads back as 255.
    plan.Passes = {passes.GetData(), passes.GetSize()};
    const auto finish = [&fixture](rhi::RasterStatus status) {
        const uint64 beginning = time::NowTicks();
        constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            status = fixture.Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    };
    finish(fixture.Graph.Initialize(fixture.Device, plan, decoded));
    fixture.Cameras[0].OrthographicCamera = true;
    // A one-sided triangle emitter, a folded two-sided quad emitter under a mirrored transform,
    // and a non-emissive copy of the triangle that must not become an emitter.
    constexpr usize MATERIAL_COUNT = 3;
    constexpr uint32 SURFACE_MATERIAL = 0;
    constexpr uint32 ONE_SIDED_MATERIAL = 1;
    constexpr uint32 TWO_SIDED_MATERIAL = 2;
    StaticArray<renderer::SceneShaderMaterial, MATERIAL_COUNT> materials{};
    materials[ONE_SIDED_MATERIAL].Emission = {1, 1, 1};
    materials[TWO_SIDED_MATERIAL].Emission = {1, 1, 1};
    materials[TWO_SIDED_MATERIAL].IsEmissionTwoSided = true;
    constexpr usize QUAD_VERTEX_COUNT = 4;
    constexpr usize QUAD_INDEX_COUNT = 6;
    const StaticArray<math::Vector3, QUAD_VERTEX_COUNT> quadPositions{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, .5F}}};
    const StaticArray<uint32, QUAD_INDEX_COUNT> quadIndices{{0, 1, 2, 1, 3, 2}};
    constexpr usize MESH_COUNT = 3;
    StaticArray<uint32, MESH_COUNT> bindings{{ONE_SIDED_MATERIAL, TWO_SIDED_MATERIAL, SURFACE_MATERIAL}};
    StaticArray<renderer::RenderGraphMesh, MESH_COUNT> meshes{};
    StaticArray<renderer::RenderGraphItem, MESH_COUNT> items{};
    for (usize i = 0; i < MESH_COUNT; ++i)
    {
        meshes[i] = fixture.Meshes[0];
        meshes[i].MaterialBindings = {&bindings[i], 1};
        items[i].SourceId = i + 1;
        items[i].MeshIndex = static_cast<uint32>(i);
    }
    meshes[1].Geometry = {quadPositions.GetData(),
                          quadPositions.GetSize(),
                          quadIndices.GetData(),
                          quadIndices.GetSize()};
    items[0].MeshToWorld.Translation = {3, 0, 0};
    items[0].MeshToWorld.Columns[1].Y = 2;
    items[1].MeshToWorld.Columns[0].X = -2;
    items[2].MeshToWorld.Translation = {-3, 0, 0};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    renderer::RenderGraphReport report;
    finish(fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                {items.GetData(), items.GetSize()},
                                {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                1,
                                renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                report,
                                &inputs));
    uint32 output = 0;
    for (uint32 resource = 0; resource < plan.Resources.GetSize(); ++resource)
    {
        output = plan.Resources[resource].IsExported == 1 ? resource : output;
    }
    ReadGraphFrame(fixture, output);
    constexpr usize EMITTER_COUNT = 2;
    const usize row = HEIGHT / 2;
    for (usize column = 0; column < EMITTER_COUNT; ++column)
    {
        CAPTURE(column);
        CHECK(fixture.Pixels[GetPixel(column, row)] == 0);
        CHECK(fixture.Pixels[GetPixel(column, row) + 1] == 255);
    }
    CHECK(fixture.Pixels[GetPixel(EMITTER_COUNT, row) + 1] == 0);
    CHECK(fixture.Pixels[GetPixel(EMITTER_COUNT, row) + 2] == 255);
}

TEST_CASE("Scene compute packages are ray packages for renderer selection",
          "[render-graph][gpu][compute-ray-tracing][selection]")
{
    bool shouldRequestRay = false;
    SECTION("default device features") {}
    SECTION("native ray features preferred")
    {
        shouldRequestRay = true;
    }
    RhiGuard session;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Storage.ResourcesPerGraph = renderer::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
    description.Preferred.RayQueries = shouldRequestRay;
    rhi::DeviceHandle device{};
    rhi::SurfaceHandle surface{};
    rhi::StartupInfo failure;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, device, surface, &failure);
    if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        FAIL("Renderer selection acceptance requires a GPU adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    core::UniquePtr<renderer::RenderGraphPackage> compute =
        LoadPackage({LUDUS_RG3_SCENE_COMPUTE_DIRECTORY, "rg2_scene_compute.lurgrt"});
    core::UniquePtr<renderer::RenderGraphPackage> raster =
        LoadPackage({LUDUS_RG3_SCENE_COMPUTE_RASTER_DIRECTORY, "rg2_scene_compute_raster.lurgrt"});
    REQUIRE(renderer::RequiresHardwareRayTracing(compute->GetPlan()) == true);
    REQUIRE(renderer::RequiresHardwareRayTracing(raster->GetPlan()) == false);
    // Metal enables native rays on every capable device; a Vulkan device without the preference may
    // not, which exercises the raster fallback with this real compute package.
    rhi::RayCapabilities capabilities;
    const bool hasNativeRays = rhi::GetRayCapabilities(device, capabilities) != rhi::RasterStatus::Unsupported;
    CAPTURE(shouldRequestRay, hasNativeRays);
    renderer::RenderGraphRenderer automatic = renderer::RenderGraphRenderer::Raster;
    REQUIRE(renderer::SelectRenderGraphRenderer(device, renderer::RenderGraphPreference::Automatic, automatic) ==
            rhi::RasterStatus::Ready);
    CHECK(automatic == (hasNativeRays == true ? renderer::RenderGraphRenderer::HardwareRayTracing
                                              : renderer::RenderGraphRenderer::Raster));
    renderer::RenderGraphActivation activation;
    REQUIRE(activation.Initialize(device,
                                  {packages::rg2_scene_compute::INTERFACE_IDENTITY.GetData(),
                                   packages::rg2_scene_compute::INTERFACE_IDENTITY.GetSize()},
                                  shaders::renderer_composite::Vertex(),
                                  shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    REQUIRE(activation.Resize(WIDTH, HEIGHT) == rhi::RasterStatus::Ready);
    // Automatic stages the compute package as the ray graph; without native rays it is released
    // before preparation and the raster package is staged instead.
    rhi::RasterStatus staged = activation.StageSelected(Move(raster), Move(compute));
    REQUIRE((staged == rhi::RasterStatus::Ready || staged == rhi::RasterStatus::Pending));
    CHECK(activation.GetReport().CandidateRenderer == automatic);
    staged = activation.StageSelected(
        LoadPackage({LUDUS_RG3_SCENE_COMPUTE_RASTER_DIRECTORY, "rg2_scene_compute_raster.lurgrt"}),
        LoadPackage({LUDUS_RG3_SCENE_COMPUTE_DIRECTORY, "rg2_scene_compute.lurgrt"}),
        renderer::RenderGraphPreference::ForceRaster);
    REQUIRE((staged == rhi::RasterStatus::Ready || staged == rhi::RasterStatus::Pending));
    CHECK(activation.GetReport().CandidateRenderer == renderer::RenderGraphRenderer::Raster);
    activation.Reset();
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
    bool hasVisibility = true;
    bool hasNativeStages = false;
#if defined(__APPLE__)
    SECTION("native stages reconstruct visibility and launch secondary rays")
    {
        hasNativeStages = true;
    }
#endif
    SECTION("visibility configuration") {}
    SECTION("visibility configuration with editor grid")
    {
        fixture.Cameras[0].WorldGrid = { .IsVisible = true, .FocusDistance = 1, .Width = WIDTH, .Height = HEIGHT };
    }
    SECTION("ordinary configuration in the same package")
    {
        hasVisibility = false;
    }
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(
#if defined(__APPLE__)
        hasNativeStages == true ? PackageLocation{LUDUS_RG3_NATIVE_VISIBILITY_DIRECTORY, "rg2_native_visibility.lurgrt"}
        :
#endif
        hasVisibility == true
            ? PackageLocation{LUDUS_RG3_VISIBILITY_DIRECTORY, "rg2_visibility.lurgrt"}
            : PackageLocation{LUDUS_RG3_VISIBILITY_ORDINARY_DIRECTORY, "rg2_visibility_ordinary.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
#if defined(__APPLE__)
        hasNativeStages == true ? packages::rg2_native_visibility::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                                shaders::renderer_composite::Fragment())
        :
#endif
        hasVisibility == true
            ? packages::rg2_visibility::MakePrograms(shaders::renderer_composite::Vertex(),
                                                     shaders::renderer_composite::Fragment())
            : packages::rg2_visibility_ordinary::MakePrograms(shaders::renderer_composite::Vertex(),
                                                              shaders::renderer_composite::Fragment());
    CHECK(decoded.NativeRayStages.Artifact.HasNativeRayStages == hasNativeStages);
    CheckSameStage(decoded.ColorVertex, compiled.ColorVertex);
    CheckSameStage(decoded.RayCompute, compiled.RayCompute);
    REQUIRE(decoded.HasVisibilityBuffer == hasVisibility);
    REQUIRE(compiled.HasVisibilityBuffer == hasVisibility);
    REQUIRE(decoded.HasSceneShaderInterface == true);
    REQUIRE(package->GetPlan().Passes.GetSize() == (hasVisibility == true ? 2 : 1));
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
    if (hasVisibility == false)
    {
        return;
    }
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

TEST_CASE("One visibility raster feeds every pass of an ordered ray sequence",
          "[render-graph][gpu][ray-history][visibility]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_VISIBILITY_SEQUENCE_DIRECTORY, "rg2_visibility_sequence.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
        packages::rg2_visibility_sequence::MakePrograms(shaders::renderer_composite::Vertex(),
                                                        shaders::renderer_composite::Fragment());
    CheckSameStage(decoded.ColorVertex, compiled.ColorVertex);
    CheckSameStage(decoded.RayCompute, compiled.RayCompute);
    REQUIRE(decoded.HasVisibilityBuffer == true);
    REQUIRE(decoded.HasPersistentRayBuffers == true);
    // One raster producer, then three ray passes that all read its visibility/depth image.
    constexpr usize PASS_COUNT = 4;
    const renderer::RenderGraphPlan& source = package->GetPlan();
    REQUIRE(source.Passes.GetSize() == PASS_COUNT);
    StaticArray<format::Pass, PASS_COUNT> passes{};
    for (usize i = 0; i < PASS_COUNT; ++i)
    {
        passes[i] = source.Passes[i];
    }
    for (usize i = 1; i < PASS_COUNT; ++i)
    {
        REQUIRE(passes[i].Program == format::Technique::RayTracing);
        REQUIRE(passes[i].Visibility == passes[0].Color);
        REQUIRE(passes[i].VisibilityDepth == passes[0].Depth);
    }
    renderer::RenderGraphPlan plan = source;
    plan.Passes = {passes.GetData(), passes.GetSize()};
    bool isMixed = false;
    SECTION("every pass reads the shared image") {}
    SECTION("an ordinary pass inside the sequence is rejected")
    {
        isMixed = true;
        passes[2].Visibility = format::ABSENT;
        passes[2].VisibilityDepth = format::ABSENT;
    }
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, plan, decoded);
    if (isMixed == true)
    {
        CHECK(status == rhi::RasterStatus::Unsupported);
        return;
    }
    const uint64 started = time::NowTicks();
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - started < 10 * time::NANOSECONDS_PER_SECOND)
    {
        status = fixture.Graph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    StaticArray<uint32, 1> slots{{0}};
    StaticArray<uint32, 1> bindings{{0}};
    fixture.Meshes[0].MaterialSlots = {slots.GetData(), slots.GetSize()};
    fixture.Meshes[0].MaterialBindings = {bindings.GetData(), bindings.GetSize()};
    StaticArray<renderer::SceneShaderMaterial, 1> materials{};
    materials[0].BaseColor = {0, 1, 0};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    fixture.Cameras[0].OrthographicCamera = true;
    const auto update = [&fixture, &inputs](uint64 revision, bool isCameraOnly) {
        renderer::RenderGraphReport report;
        rhi::RasterStatus result = fixture.Graph.Update(
            isCameraOnly == true
                ? BorrowedView<const renderer::RenderGraphMesh>{}
                : BorrowedView<const renderer::RenderGraphMesh>{fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
            isCameraOnly == true
                ? BorrowedView<const renderer::RenderGraphItem>{}
                : BorrowedView<const renderer::RenderGraphItem>{fixture.Items.GetData(), fixture.Items.GetSize()},
            {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
            revision,
            renderer::RenderGraphCoveragePolicy::RejectIncomplete,
            report,
            isCameraOnly == true ? nullptr : &inputs,
            1);
        const uint64 beginning = time::NowTicks();
        while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < 10 * time::NANOSECONDS_PER_SECOND)
        {
            result = fixture.Graph.Poll();
        }
        REQUIRE(result == rhi::RasterStatus::Ready);
    };
    // Tone-mapped unit radiance; magenta would mean a pass reconstructed a different surface.
    constexpr uint8 UNIT = 188;
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    const usize corner = GetPixel(0, 0);
    const auto checkFrame = [&fixture](usize surface) {
        ReadGraphFrame(fixture);
        CHECK(fixture.Pixels[surface] == 0);
        CHECK(fixture.Pixels[surface + 1] == UNIT);
        CHECK(fixture.Pixels[surface + 2] == 0);
        CHECK(fixture.Pixels[corner] == 0);
        CHECK(fixture.Pixels[corner + 1] == 0);
        CHECK(fixture.Pixels[corner + 2] == UNIT);
    };
    update(1, false);
    checkFrame(center);
    // A second frame rebuilds the sequence from the retained producer.
    checkFrame(center);
    // Camera-only updates move the shared raster image and every pass's unprojection together.
    constexpr usize SHIFTED_COLUMN = WIDTH / 2 + WIDTH / 4;
    fixture.Cameras[0].WorldToClip.Columns[3].X = .5F;
    update(2, true);
    checkFrame(GetPixel(SHIFTED_COLUMN, HEIGHT / 2));
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
    // Navigation overlays cannot alter either exported visibility IDs or the ray producer's inputs.
    fixture.Cameras[0].WorldGrid = { .IsVisible = true, .FocusDistance = 1, .Width = WIDTH, .Height = HEIGHT };
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

TEST_CASE("Tiled visibility-buffer NEE matches an area quadrature of two mesh emitters",
          "[render-graph][gpu][tiled-nee]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_TILED_VBUFFER_NEE_TEMPLATE_DIRECTORY, "rg2_tiled_vbuffer_nee_template.lurgrt"});
    const renderer::RenderGraphPlan plan = package->GetPlan();
    // One raster producer, then the tiles, prefix and shade ray passes.
    constexpr usize PASS_COUNT = 4;
    REQUIRE(plan.Passes.GetSize() == PASS_COUNT);
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
        packages::rg2_tiled_vbuffer_nee_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                               shaders::renderer_composite::Fragment());
    CheckSameStage(programs.ColorVertex, compiled.ColorVertex);
    CheckSameStage(programs.RayCompute, compiled.RayCompute);
    constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
    const auto wait = [&fixture](rhi::RasterStatus initial) {
        rhi::RasterStatus status = initial;
        const uint64 beginning = time::NowTicks();
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            status = fixture.Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    };
    wait(fixture.Graph.Initialize(fixture.Device, plan, programs, nullptr, WIDTH, HEIGHT));
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
    // Two emitters at different distances, so the tile table's PMF is not uniform.
    constexpr usize ITEM_COUNT = 3;
    constexpr StaticArray<float32, ITEM_COUNT - 1> EMITTER_OFFSETS_WORLD{{1, -2}};
    StaticArray<renderer::RenderGraphItem, ITEM_COUNT> items{};
    items[0] = fixture.Items[0];
    for (usize emitter = 0; emitter < EMITTER_OFFSETS_WORLD.GetSize(); ++emitter)
    {
        items[emitter + 1].SourceId = static_cast<uint32>(emitter + 2);
        items[emitter + 1].MeshIndex = 1;
        items[emitter + 1].MeshToWorld.Translation = {EMITTER_OFFSETS_WORLD[emitter], 0, -.4F};
    }
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
                                  &inputs,
                                  revision));
    };
    // Independent deterministic area quadrature of the CPU reference BSDF over both emitters.
    renderer::ReferenceBsdf reference;
    reference.BaseColor = materials[0].BaseColor;
    reference.Roughness = materials[0].Roughness;
    constexpr uint32 QUADRATURE_SIDE = 32;
    float32 expectedRed = 0;
    const math::Vector3 receiverPositionWorld{1.0F / static_cast<float32>(WIDTH),
                                              -1.0F / static_cast<float32>(HEIGHT),
                                              .5F};
    for (const float32 offsetWorld : EMITTER_OFFSETS_WORLD)
    {
        for (uint32 row = 0; row < QUADRATURE_SIDE; ++row)
        {
            for (uint32 column = 0; column < QUADRATURE_SIDE; ++column)
            {
                const float32 root =
                    math::Sqrt((static_cast<float32>(row) + .5F) / static_cast<float32>(QUADRATURE_SIDE));
                const float32 v = (static_cast<float32>(column) + .5F) / static_cast<float32>(QUADRATURE_SIDE);
                const float32 first = root * (1 - v);
                const float32 second = root * v;
                const math::Vector3 endpointWorld{offsetWorld - .5F + .5F * first + second, -.5F + first, .1F};
                const math::Vector3 differenceWorld = endpointWorld - receiverPositionWorld;
                const float32 distanceSquaredWorld = math::Dot(differenceWorld, differenceWorld);
                const math::Vector3 directionWorld = differenceWorld / math::Sqrt(distanceSquaredWorld);
                renderer::ReferenceBsdfEvaluation evaluation;
                REQUIRE(
                    renderer::EvaluateReferenceBsdf(reference, {0, 0, -1}, {0, 0, -1}, directionWorld, evaluation) ==
                    rhi::RasterStatus::Ready);
                const float32 cosine = -directionWorld.Z;
                expectedRed += materials[1].Emission.X * evaluation.Value.X * cosine * cosine / distanceSquaredWorld;
            }
        }
    }
    // Each emitter triangle has area one half.
    expectedRed *= .5F / static_cast<float32>(QUADRATURE_SIDE * QUADRATURE_SIDE);
    update(1);
    const uint32 colorResource = plan.Passes[PASS_COUNT - 1].Color;
    // The final Accumulate node averages every frame; read its running mean.
    constexpr usize FRAME_COUNT = 64;
    for (usize frame = 0; frame < FRAME_COUNT; ++frame)
    {
        ReadGraphFrame(fixture, colorResource);
    }
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    for (usize pixel = 0; pixel < static_cast<usize>(WIDTH) * HEIGHT; ++pixel)
    {
        constexpr usize CHANNEL_COUNT = 4;
        const usize address = pixel * CHANNEL_COUNT;
        const bool isFailureColor =
            fixture.Pixels[address] == 255 && fixture.Pixels[address + 1] == 0 && fixture.Pixels[address + 2] == 255;
        CHECK(isFailureColor == false);
    }
    CHECK(fixture.Pixels[center] > fixture.Pixels[center + 1]);
    constexpr float32 MAXIMUM_CHANNEL = 255;
    const float32 mapped = math::DecodeSrgb(static_cast<float32>(fixture.Pixels[center]) / MAXIMUM_CHANNEL);
    const float32 measuredRed = mapped / (1 - mapped);
    INFO("area quadrature=" << expectedRed << ", decoded GPU mean=" << measuredRed);
    // Light-only, BSDF-only and MIS variants of this shader converge to the same 8-bit value,
    // 4.5% below this quadrature; the tolerance covers that GPU/reference BSDF difference.
    constexpr float32 RELATIVE_ENERGY_TOLERANCE = .1F;
    CHECK(math::Abs(measuredRed - expectedRed) < RELATIVE_ENERGY_TOLERANCE * expectedRed);
    // The table is rebuilt each frame: removing the emission must not relight the receiver.
    materials[1].Emission = {0, 0, 0};
    update(2);
    ReadGraphFrame(fixture, colorResource);
    CHECK(fixture.Pixels[center] == 0);
    CHECK(fixture.Pixels[center + 1] == 0);
    CHECK(fixture.Pixels[center + 2] == 0);
}

namespace
{
constexpr usize CORNELL_QUAD_COUNT = 7;
constexpr usize CORNELL_QUAD_CORNERS = 4;
constexpr usize CORNELL_VERTEX_COUNT = CORNELL_QUAD_COUNT * CORNELL_QUAD_CORNERS;
constexpr usize CORNELL_TRIANGLE_COUNT = CORNELL_QUAD_COUNT * 2;
constexpr usize CORNELL_INDEX_COUNT = CORNELL_TRIANGLE_COUNT * 3;
constexpr usize CORNELL_MATERIAL_COUNT = 4;
constexpr uint32 CORNELL_WHITE = 0;
constexpr uint32 CORNELL_RED = 1;
constexpr uint32 CORNELL_GREEN = 2;
constexpr uint32 CORNELL_LIGHT = 3;
/// A Cornell-box-style room spanning [-1, 1] on every axis, open toward a camera on +Z. Each quad's
/// corners wind counterclockwise seen from inside the room, so every geometric normal faces inward.
constexpr StaticArray<math::Vector3, CORNELL_VERTEX_COUNT> CORNELL_CORNERS_WORLD{{
    // Floor, ceiling and back wall.
    {-1, -1, 1},
    {1, -1, 1},
    {1, -1, -1},
    {-1, -1, -1},
    {-1, 1, -1},
    {1, 1, -1},
    {1, 1, 1},
    {-1, 1, 1},
    {-1, -1, -1},
    {1, -1, -1},
    {1, 1, -1},
    {-1, 1, -1},
    // Left and right walls.
    {-1, -1, 1},
    {-1, -1, -1},
    {-1, 1, -1},
    {-1, 1, 1},
    {1, -1, -1},
    {1, -1, 1},
    {1, 1, 1},
    {1, 1, -1},
    // A one-sided ceiling light facing down, then the top of a block that shadows the floor.
    {-.35F, .98F, -.35F},
    {.35F, .98F, -.35F},
    {.35F, .98F, .35F},
    {-.35F, .98F, .35F},
    {-.5F, -.3F, 0},
    {0, -.3F, 0},
    {0, -.3F, -.5F},
    {-.5F, -.3F, -.5F},
}};
constexpr StaticArray<uint32, CORNELL_QUAD_COUNT> CORNELL_QUAD_MATERIALS{
    {CORNELL_WHITE, CORNELL_WHITE, CORNELL_WHITE, CORNELL_RED, CORNELL_GREEN, CORNELL_LIGHT, CORNELL_WHITE}};
constexpr float32 CORNELL_EYE_Z_WORLD = 3.4F;

/// The lit room, its materials and a conventional-depth perspective camera. The mesh borrows this
/// object's arrays, so it is neither copied nor moved.
struct CornellBox final
{
    StaticArray<math::Vector3, CORNELL_VERTEX_COUNT> Positions = CORNELL_CORNERS_WORLD;
    StaticArray<uint32, CORNELL_INDEX_COUNT> Indices{};
    StaticArray<uint32, CORNELL_TRIANGLE_COUNT> Slots{};
    StaticArray<uint32, CORNELL_MATERIAL_COUNT> Bindings{{CORNELL_WHITE, CORNELL_RED, CORNELL_GREEN, CORNELL_LIGHT}};
    StaticArray<renderer::SceneShaderMaterial, CORNELL_MATERIAL_COUNT> Materials{};
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::RenderGraphItem, 1> Items{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    renderer::SceneShaderInputs Inputs;
    CornellBox()
    {
        for (usize quad = 0; quad < CORNELL_QUAD_COUNT; ++quad)
        {
            const uint32 first = static_cast<uint32>(quad * CORNELL_QUAD_CORNERS);
            constexpr usize QUAD_INDEX_COUNT = 6;
            const StaticArray<uint32, QUAD_INDEX_COUNT> corners{{0, 1, 2, 0, 2, 3}};
            for (usize corner = 0; corner < QUAD_INDEX_COUNT; ++corner)
            {
                Indices[quad * QUAD_INDEX_COUNT + corner] = first + corners[corner];
            }
            Slots[quad * 2] = CORNELL_QUAD_MATERIALS[quad];
            Slots[quad * 2 + 1] = CORNELL_QUAD_MATERIALS[quad];
        }
        Materials[CORNELL_WHITE].BaseColor = {.8F, .8F, .8F};
        Materials[CORNELL_RED].BaseColor = {.8F, .1F, .1F};
        Materials[CORNELL_GREEN].BaseColor = {.1F, .8F, .1F};
        Materials[CORNELL_LIGHT].BaseColor = {.8F, .8F, .8F};
        Materials[CORNELL_LIGHT].Emission = {12, 12, 12};
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Meshes[0].MaterialSlots = {Slots.GetData(), Slots.GetSize()};
        Meshes[0].MaterialBindings = {Bindings.GetData(), Bindings.GetSize()};
        Items[0].SourceId = 1;
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        // Right-handed view down -Z from the eye; clip depth 0 at the near plane and 1 at the far plane.
        constexpr float32 FOCAL_LENGTH = 2.2F;
        constexpr float32 NEAR_WORLD = .5F;
        constexpr float32 FAR_WORLD = 10;
        constexpr float32 DEPTH_SCALE = FAR_WORLD / (NEAR_WORLD - FAR_WORLD);
        const float32 aspect = static_cast<float32>(WIDTH) / static_cast<float32>(HEIGHT);
        math::Matrix4& worldToClip = Cameras[0].WorldToClip;
        worldToClip.Columns[0] = {FOCAL_LENGTH / aspect, 0, 0, 0};
        worldToClip.Columns[1] = {0, FOCAL_LENGTH, 0, 0};
        worldToClip.Columns[2] = {0, 0, DEPTH_SCALE, -1};
        worldToClip.Columns[3] = {0, 0, DEPTH_SCALE * (NEAR_WORLD - CORNELL_EYE_Z_WORLD), CORNELL_EYE_Z_WORLD};
        Cameras[0].CameraPosition = {0, 0, CORNELL_EYE_Z_WORLD};
    }
    CornellBox(const CornellBox&) = delete;
    CornellBox& operator=(const CornellBox&) = delete;
};

/// Pixel dimensions of an exported color resource.
struct ExportExtent final
{
    uint32 Width = WIDTH;
    uint32 Height = HEIGHT;
};

/// Render one frame and wait for its completion.
void RenderFrame(renderer::RenderGraph& inoutGraph, const GraphGpuFixture& fixture)
{
    constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
    rhi::SubmissionToken completion;
    REQUIRE(inoutGraph.Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
    const uint64 beginning = time::NowTicks();
    rhi::RasterStatus result = rhi::GetStatus(fixture.Device, completion);
    while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        result = rhi::GetStatus(fixture.Device, completion);
    }
    REQUIRE(result == rhi::RasterStatus::Ready);
}

/// Copy an RGBA8 export of the given extent, as the last rendered frame left it, into outPixels.
void ReadExport(renderer::RenderGraph& inoutGraph,
                const GraphGpuFixture& fixture,
                uint32 exportResource,
                ExportExtent extent,
                Array<uint8>& outPixels)
{
    const uint32 width = extent.Width;
    const uint32 height = extent.Height;
    constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
    const uint64 beginning = time::NowTicks();
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(inoutGraph.GetExport(exportResource, texture, aspect) == rhi::RasterStatus::Ready);
    const usize rowBytes = static_cast<usize>(width) * 4;
    REQUIRE(outPixels.TryResize(rowBytes * height) == true);
    rhi::ReadbackTicket ticket;
    rhi::RasterStatus result =
        rhi::RequestTextureReadback(fixture.Device, texture, {0, 0, 0, width, height}, rowBytes, ticket);
    while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        result = rhi::GetStatus(fixture.Device, ticket);
    }
    REQUIRE(result == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CopyReadback(fixture.Device, ticket, outPixels.GetData(), outPixels.GetSize()) ==
            rhi::RasterStatus::Ready);
    REQUIRE(rhi::Release(fixture.Device, ticket) == rhi::RasterStatus::Ready);
}

/// Render one frame and copy an RGBA8 export of the given extent into outPixels.
void RenderExport(renderer::RenderGraph& inoutGraph,
                  const GraphGpuFixture& fixture,
                  uint32 exportResource,
                  ExportExtent extent,
                  Array<uint8>& outPixels)
{
    RenderFrame(inoutGraph, fixture);
    ReadExport(inoutGraph, fixture, exportResource, extent, outPixels);
}

/// Number of RGBA8 pixels showing the renderer's magenta shader-failure color.
usize CountFailurePixels(const Array<uint8>& pixels)
{
    usize count = 0;
    for (usize address = 0; address + 3 < pixels.GetSize(); address += 4)
    {
        count += pixels[address] == 255 && pixels[address + 1] == 0 && pixels[address + 2] == 255 ? 1 : 0;
    }
    return count;
}

/// Linear radiance of one ToneMap channel at exposure 1 (Reinhard x / (1 + x), then sRGB).
float32 DecodeToneMapped(uint8 value)
{
    constexpr float32 MAXIMUM_CHANNEL = 255;
    // The brightest byte is clamped below one, so the inverse stays finite.
    constexpr float32 MAXIMUM_MAPPED = .999F;
    const float32 mapped = math::Min(math::DecodeSrgb(static_cast<float32>(value) / MAXIMUM_CHANNEL), MAXIMUM_MAPPED);
    return mapped / (1 - mapped);
}

/// Mean linear RGB radiance of a tone-mapped RGBA8 image.
math::Vector3 ComputeMeanRadiance(const Array<uint8>& pixels)
{
    math::Vector3 sum{};
    for (usize address = 0; address + 3 < pixels.GetSize(); address += 4)
    {
        sum = sum + math::Vector3{DecodeToneMapped(pixels[address]),
                                  DecodeToneMapped(pixels[address + 1]),
                                  DecodeToneMapped(pixels[address + 2])};
    }
    const usize pixelCount = pixels.GetSize() / 4;
    return sum / static_cast<float32>(pixelCount);
}
} // namespace

TEST_CASE("The simple triangle-loop template keeps the nearest instance in either input order",
          "[render-graph][gpu][simple-compute-template]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(
        {LUDUS_RG3_SIMPLE_COMPUTE_RAY_TRACING_TEMPLATE_DIRECTORY, "rg2_simple_compute_ray_tracing_template.lurgrt"});
    const renderer::RenderGraphPlan plan = package->GetPlan();
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    CheckSameStage(
        programs.RayCompute,
        packages::rg2_simple_compute_ray_tracing_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                        shaders::renderer_composite::Fragment())
            .RayCompute);
    REQUIRE(plan.Passes.GetSize() == 1);
    CHECK(plan.Passes[0].AccumulationSamples == 0);
    uint32 output = 0;
    for (uint32 resource = 0; resource < plan.Resources.GetSize(); ++resource)
    {
        CHECK(plan.Resources[resource].Kind != format::Aspect::Buffer);
        output = plan.Resources[resource].IsExported == 1 ? resource : output;
    }
    constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
    const auto wait = [&fixture](rhi::RasterStatus initial) {
        rhi::RasterStatus status = initial;
        const uint64 beginning = time::NowTicks();
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            status = fixture.Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    };
    wait(fixture.Graph.Initialize(fixture.Device, plan, programs, nullptr, WIDTH, HEIGHT));
    constexpr usize INSTANCE_COUNT = 2;
    StaticArray<renderer::RenderGraphItem, INSTANCE_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
    items[1].SourceId = 2;
    items[1].MeshToWorld.Translation.Z = .25F;
    items[1].MeshIndex = 1;
    StaticArray<renderer::RenderGraphMesh, INSTANCE_COUNT> meshes{{fixture.Meshes[0], fixture.Meshes[0]}};
    StaticArray<uint32, INSTANCE_COUNT> bindings{{0, 1}};
    StaticArray<renderer::SceneShaderMaterial, INSTANCE_COUNT> materials{};
    materials[0].BaseColor = {1, 0, 0};
    materials[1].BaseColor = {0, 1, 0};
    for (usize index = 0; index < INSTANCE_COUNT; ++index)
    {
        meshes[index].MaterialBindings = {&bindings[index], 1};
    }
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    fixture.Cameras[0].OrthographicCamera = true;
    const auto update = [&fixture, &items, &meshes, &inputs, &wait](uint64 revision, usize count) {
        renderer::RenderGraphReport report;
        wait(fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                  {items.GetData(), count},
                                  {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                  revision,
                                  renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                  report,
                                  &inputs));
    };
    Array<uint8> pixels;
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    constexpr uint8 TONEMAPPED_UNIT_COLOR = 188;
    update(1, INSTANCE_COUNT);
    RenderExport(fixture.Graph, fixture, output, {}, pixels);
    CHECK(pixels[center] == TONEMAPPED_UNIT_COLOR);
    CHECK(pixels[center + 1] == 0);
    CHECK(CountFailurePixels(pixels) == 0);
    const renderer::RenderGraphItem first = items[0];
    items[0] = items[1];
    items[1] = first;
    update(2, INSTANCE_COUNT);
    RenderExport(fixture.Graph, fixture, output, {}, pixels);
    CHECK(pixels[center] == TONEMAPPED_UNIT_COLOR);
    CHECK(pixels[center + 1] == 0);
    // Reverse winding: the intersection remains two-sided.
    fixture.Indices[1] = 2;
    fixture.Indices[2] = 1;
    update(3, INSTANCE_COUNT);
    RenderExport(fixture.Graph, fixture, output, {}, pixels);
    CHECK(pixels[center] == TONEMAPPED_UNIT_COLOR);
    // A miss and an empty scene use the authored environment (black by default).
    CHECK(pixels[GetPixel(0, 0)] == 0);
    CHECK(pixels[GetPixel(0, 0) + 1] == 0);
    // Put one triangle behind the near plane and the other beyond the far plane.
    items[0].MeshToWorld.Translation.Z = -1;
    items[1].MeshToWorld.Translation.Z = 1;
    update(4, INSTANCE_COUNT);
    RenderExport(fixture.Graph, fixture, output, {}, pixels);
    CHECK(pixels[center] == 0);
    CHECK(pixels[center + 1] == 0);
    CHECK(CountFailurePixels(pixels) == 0);
    update(5, 0);
    RenderExport(fixture.Graph, fixture, output, {}, pixels);
    CHECK(pixels[center] == 0);
    CHECK(pixels[center + 1] == 0);
    CHECK(pixels[center + 2] == 0);
    CHECK(CountFailurePixels(pixels) == 0);
}

TEST_CASE("The compute ray-tracing template matches a single-pass direct-lighting reference",
          "[render-graph][gpu][compute-ray-tracing][template]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_COMPUTE_RAY_TRACING_TEMPLATE_DIRECTORY, "rg2_compute_ray_tracing_template.lurgrt"});
    const renderer::RenderGraphPlan plan = package->GetPlan();
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    CheckSameStage(programs.RayCompute,
                   packages::rg2_compute_ray_tracing_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                            shaders::renderer_composite::Fragment())
                       .RayCompute);
    REQUIRE(programs.HasPersistentRayBuffers == true);
    REQUIRE(renderer::RequiresHardwareRayTracing(plan) == true);
    // The primary-hits and shade scene compute passes; Accumulate and ToneMap fuse into shade.
    constexpr usize PASS_COUNT = 2;
    REQUIRE(plan.Passes.GetSize() == PASS_COUNT);
    REQUIRE(plan.Passes[0].Program == format::Technique::RayTracing);
    REQUIRE(plan.Passes[1].Program == format::Technique::RayTracing);
    CHECK(plan.Passes[0].AccumulationSamples == 0);
    CHECK(plan.Passes[1].AccumulationSamples > 0);
    CHECK(plan.Passes[1].ToneMappingExposure == 1);
    uint32 hits = 0;
    uint32 output = 0;
    for (uint32 resource = 0; resource < plan.Resources.GetSize(); ++resource)
    {
        hits = plan.Resources[resource].Kind == format::Aspect::Buffer ? resource : hits;
        output = plan.Resources[resource].IsExported == 1 ? resource : output;
    }
    constexpr uint32 HIT_RECORD_BYTES = 32;
    REQUIRE(plan.Resources[hits].BytesPerPixel == HIT_RECORD_BYTES);

    const core::UniquePtr<renderer::RenderGraphPackage> referencePackage =
        LoadPackage({LUDUS_RG3_DIRECT_LIGHTING_REFERENCE_DIRECTORY, "rg2_direct_lighting_reference.lurgrt"});
    const renderer::RenderGraphPlan referencePlan = referencePackage->GetPlan();
    const renderer::RenderGraphPrograms referencePrograms =
        referencePackage->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(referencePlan.Passes.GetSize() == 1);
    uint32 referenceOutput = 0;
    for (uint32 resource = 0; resource < referencePlan.Resources.GetSize(); ++resource)
    {
        referenceOutput = referencePlan.Resources[resource].IsExported == 1 ? resource : referenceOutput;
    }

    renderer::RenderGraph reference;
    constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
    const auto wait = [](renderer::RenderGraph& inoutGraph, rhi::RasterStatus initial) {
        rhi::RasterStatus status = initial;
        const uint64 beginning = time::NowTicks();
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
        {
            status = inoutGraph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    };
    wait(fixture.Graph, fixture.Graph.Initialize(fixture.Device, plan, programs, nullptr, WIDTH, HEIGHT));
    wait(reference, reference.Initialize(fixture.Device, referencePlan, referencePrograms, nullptr, WIDTH, HEIGHT));
    const CornellBox room;
    const auto update = [&room, &wait](renderer::RenderGraph& inoutGraph, uint64 revision) {
        renderer::RenderGraphReport report;
        wait(inoutGraph,
             inoutGraph.Update({room.Meshes.GetData(), room.Meshes.GetSize()},
                               {room.Items.GetData(), room.Items.GetSize()},
                               {room.Cameras.GetData(), room.Cameras.GetSize()},
                               revision,
                               renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                               report,
                               &room.Inputs,
                               revision));
    };
    update(fixture.Graph, 1);
    update(reference, 1);

    // Both graphs accumulate a running mean; compare after the same number of samples.
    constexpr usize FRAME_COUNT = 64;
    Array<uint8> pixels;
    Array<uint8> referencePixels;
    for (usize frame = 0; frame < FRAME_COUNT; ++frame)
    {
        RenderExport(fixture.Graph, fixture, output, {}, pixels);
        RenderExport(reference, fixture, referenceOutput, {}, referencePixels);
        // Invalid hit records, exhausted budgets and failed queries would show magenta.
        REQUIRE(CountFailurePixels(pixels) == 0);
        REQUIRE(CountFailurePixels(referencePixels) == 0);
    }
    const math::Vector3 mean = ComputeMeanRadiance(pixels);
    const math::Vector3 referenceMean = ComputeMeanRadiance(referencePixels);
    INFO("template mean=" << mean.X << ',' << mean.Y << ',' << mean.Z << ", reference mean=" << referenceMean.X << ','
                          << referenceMean.Y << ',' << referenceMean.Z);
    // The room is lit: the back wall at the center is white, the left wall red and the right wall green.
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    const usize left = GetPixel(WIDTH / 2 - WIDTH * 3 / 10, HEIGHT / 2);
    const usize right = GetPixel(WIDTH / 2 + WIDTH * 3 / 10, HEIGHT / 2);
    CHECK(pixels[center] > 0);
    CHECK(pixels[left] > pixels[left + 1]);
    CHECK(pixels[right + 1] > pixels[right]);
    constexpr float32 MINIMUM_MEAN_RADIANCE = .01F;
    CHECK(mean.X > MINIMUM_MEAN_RADIANCE);
    // Both estimate emission plus one shadowed light sample per pixel and frame. The reference draws
    // jitter, light selection and light shape from its automatic stream in the order of the
    // template's explicit ranges ([0, 2), then [2, 6)), so each sample sees the same random values:
    // on Metal the two images are byte-identical. The tolerances allow other backends to contract
    // floating-point operations differently in the two kernels. A 2% mean difference also exceeds
    // the Monte Carlo error of 64 samples per pixel over this image.
    constexpr float32 RELATIVE_MEAN_TOLERANCE = .02F;
    CHECK(math::Abs(mean.X - referenceMean.X) < RELATIVE_MEAN_TOLERANCE * referenceMean.X);
    CHECK(math::Abs(mean.Y - referenceMean.Y) < RELATIVE_MEAN_TOLERANCE * referenceMean.Y);
    CHECK(math::Abs(mean.Z - referenceMean.Z) < RELATIVE_MEAN_TOLERANCE * referenceMean.Z);
    constexpr uint8 CHANNEL_TOLERANCE = 2;
    usize differingPixels = 0;
    for (usize address = 0; address < pixels.GetSize(); address += 4)
    {
        bool isDifferent = false;
        for (usize channel = 0; channel < 3; ++channel)
        {
            const uint8 high = math::Max(pixels[address + channel], referencePixels[address + channel]);
            const uint8 low = math::Min(pixels[address + channel], referencePixels[address + channel]);
            isDifferent = isDifferent == true || high - low > CHANNEL_TOLERANCE;
        }
        differingPixels += isDifferent == true ? 1 : 0;
    }
    // At most one pixel in a thousand, for shadow rays that graze an edge differently.
    constexpr usize DIFFERING_PIXEL_DIVISOR = 1000;
    CHECK(differingPixels <= static_cast<usize>(WIDTH) * HEIGHT / DIFFERING_PIXEL_DIVISOR);

    // A resize replaces the hits buffer with a zeroed one sized for the new extent. The first
    // samples after it must not read a stale or unwritten record.
    constexpr uint32 RESIZED_WIDTH = WIDTH * 2;
    constexpr uint32 RESIZED_HEIGHT = HEIGHT * 2;
    REQUIRE(fixture.Graph.Resize(RESIZED_WIDTH, RESIZED_HEIGHT) == rhi::RasterStatus::Pending);
    update(fixture.Graph, 1);
    format::Resource description{};
    REQUIRE(fixture.Graph.GetResourceDescription(hits, description) == true);
    CHECK(description.Width == RESIZED_WIDTH * RESIZED_HEIGHT * HIT_RECORD_BYTES);
    constexpr usize RESIZED_FRAME_COUNT = 4;
    for (usize frame = 0; frame < RESIZED_FRAME_COUNT; ++frame)
    {
        RenderExport(fixture.Graph, fixture, output, {RESIZED_WIDTH, RESIZED_HEIGHT}, pixels);
        INFO("frame " << frame << " after resize");
        CHECK(CountFailurePixels(pixels) == 0);
        CHECK(ComputeMeanRadiance(pixels).X > MINIMUM_MEAN_RADIANCE);
    }
}

namespace
{
constexpr uint64 TRAVERSAL_TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
/// The room plus smaller copies of it inside, which add occluders and deepen the BVH.
constexpr usize TRAVERSAL_ITEM_COUNT = 3;
/// Uniform scale and World translations of the nested copies.
constexpr float32 TRAVERSAL_BLOCK_SCALE = .3F;
constexpr StaticArray<math::Vector3, TRAVERSAL_ITEM_COUNT - 1> TRAVERSAL_BLOCK_OFFSETS_WORLD{{
    {-.45F, -.65F, -.2F},
    {.4F, -.55F, .25F},
}};

/// Poll one graph operation until it is ready.
void WaitForGraph(renderer::RenderGraph& inoutGraph, rhi::RasterStatus initial)
{
    rhi::RasterStatus status = initial;
    const uint64 beginning = time::NowTicks();
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TRAVERSAL_TIMEOUT)
    {
        status = inoutGraph.Poll();
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
}

/// Initialize a graph from a decoded package, submit the World and return its exported resource.
uint32 PrepareRoomGraph(renderer::RenderGraph& inoutGraph,
                        const GraphGpuFixture& fixture,
                        const renderer::RenderGraphPackage& package,
                        const CornellBox& room,
                        const StaticArray<renderer::RenderGraphItem, TRAVERSAL_ITEM_COUNT>& items,
                        usize itemCount)
{
    const renderer::RenderGraphPlan plan = package.GetPlan();
    const renderer::RenderGraphPrograms programs =
        package.GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    WaitForGraph(inoutGraph, inoutGraph.Initialize(fixture.Device, plan, programs, nullptr, WIDTH, HEIGHT));
    renderer::RenderGraphReport report;
    constexpr uint64 REVISION = 1;
    WaitForGraph(inoutGraph,
                 inoutGraph.Update({room.Meshes.GetData(), room.Meshes.GetSize()},
                                   {items.GetData(), itemCount},
                                   {room.Cameras.GetData(), room.Cameras.GetSize()},
                                   REVISION,
                                   renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                   report,
                                   &room.Inputs,
                                   REVISION));
    uint32 output = 0;
    for (uint32 resource = 0; resource < plan.Resources.GetSize(); ++resource)
    {
        output = plan.Resources[resource].IsExported == 1 ? resource : output;
    }
    return output;
}

/// The room's item followed by its nested copies.
StaticArray<renderer::RenderGraphItem, TRAVERSAL_ITEM_COUNT> MakeTraversalItems(const CornellBox& room)
{
    StaticArray<renderer::RenderGraphItem, TRAVERSAL_ITEM_COUNT> items{};
    items[0] = room.Items[0];
    for (usize block = 1; block < TRAVERSAL_ITEM_COUNT; ++block)
    {
        items[block] = room.Items[0];
        items[block].SourceId = block + 1;
        items[block].MeshToWorld.Columns[0] = {TRAVERSAL_BLOCK_SCALE, 0, 0};
        items[block].MeshToWorld.Columns[1] = {0, TRAVERSAL_BLOCK_SCALE, 0};
        items[block].MeshToWorld.Columns[2] = {0, 0, TRAVERSAL_BLOCK_SCALE};
        items[block].MeshToWorld.Translation = TRAVERSAL_BLOCK_OFFSETS_WORLD[block - 1];
    }
    return items;
}

/// Pixels of two equally sized RGBA8 images with an RGB byte difference above channelTolerance.
usize CountDifferentPixels(const Array<uint8>& left, const Array<uint8>& right, uint8 channelTolerance)
{
    usize count = 0;
    for (usize address = 0; address + 3 < left.GetSize(); address += 4)
    {
        bool isDifferent = false;
        for (usize channel = 0; channel < 3; ++channel)
        {
            const uint8 high = math::Max(left[address + channel], right[address + channel]);
            const uint8 low = math::Min(left[address + channel], right[address + channel]);
            isDifferent = isDifferent == true || high - low > channelTolerance;
        }
        count += isDifferent == true ? 1 : 0;
    }
    return count;
}
} // namespace

TEST_CASE("Compute BVH traversal reports the same hits and visibility as the other strategies",
          "[render-graph][gpu][ray-traversal]")
{
    bool shouldCompareNative = false;
    SECTION("triangle loop without requested ray features") {}
    SECTION("native ray queries")
    {
        shouldCompareNative = true;
    }
    GraphGpuFixture fixture(false, shouldCompareNative);
    const core::UniquePtr<renderer::RenderGraphPackage> bvhPackage =
        LoadPackage({LUDUS_RG3_TRAVERSAL_PARITY_COMPUTEBVH_DIRECTORY, "rg2_traversal_parity_computebvh.lurgrt"});
    const core::UniquePtr<renderer::RenderGraphPackage> baselinePackage =
        LoadPackage(shouldCompareNative == true ? PackageLocation{LUDUS_RG3_TRAVERSAL_PARITY_NATIVE_DIRECTORY,
                                                                  "rg2_traversal_parity_native.lurgrt"}
                                                : PackageLocation{LUDUS_RG3_TRAVERSAL_PARITY_TRIANGLELOOP_DIRECTORY,
                                                                  "rg2_traversal_parity_triangleloop.lurgrt"});
    REQUIRE(bvhPackage->GetPlan().Passes[0].Traversal == format::RayTraversal::ComputeBvh);
    CHECK(baselinePackage->GetPlan().Passes[0].Traversal ==
          (shouldCompareNative == true ? format::RayTraversal::NativeQuery : format::RayTraversal::TriangleLoop));
    CHECK(renderer::RequiresHardwareRayTracing(bvhPackage->GetPlan()) == false);
    const renderer::RenderGraphPrograms bvhPrograms =
        bvhPackage->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    for (const rhi::RasterBinding& binding : bvhPrograms.RayCompute.Bindings)
    {
        CHECK(binding.Kind != rhi::RasterBindingKind::RayScene);
    }

    const CornellBox room;
    const StaticArray<renderer::RenderGraphItem, TRAVERSAL_ITEM_COUNT> items = MakeTraversalItems(room);
    usize itemCount = TRAVERSAL_ITEM_COUNT;
    SECTION("room with nested blocks") {}
    SECTION("empty World")
    {
        itemCount = 0;
    }
    renderer::RenderGraph baseline;
    const uint32 bvhOutput = PrepareRoomGraph(fixture.Graph, fixture, *bvhPackage, room, items, itemCount);
    const uint32 baselineOutput = PrepareRoomGraph(baseline, fixture, *baselinePackage, room, items, itemCount);
    Array<uint8> pixels;
    Array<uint8> baselinePixels;
    RenderExport(fixture.Graph, fixture, bvhOutput, {}, pixels);
    RenderExport(baseline, fixture, baselineOutput, {}, baselinePixels);
    REQUIRE(pixels.GetSize() == baselinePixels.GetSize());
    // Failed queries, exhausted budgets and traversal stack overflow show magenta.
    REQUIRE(CountFailurePixels(pixels) == 0);
    REQUIRE(CountFailurePixels(baselinePixels) == 0);
    constexpr uint8 VISIBLE_LEVEL = 255;
    constexpr uint8 OCCLUDED_LEVEL = 128;
    usize primaryHits = 0;
    usize secondaryHits = 0;
    usize visible = 0;
    usize occluded = 0;
    for (usize address = 0; address + 3 < pixels.GetSize(); address += 4)
    {
        primaryHits += pixels[address] != 0 ? 1 : 0;
        secondaryHits += pixels[address + 1] != 0 ? 1 : 0;
        visible += pixels[address] != 0 && pixels[address + 2] == VISIBLE_LEVEL ? 1 : 0;
        occluded += pixels[address] != 0 && pixels[address + 2] == OCCLUDED_LEVEL ? 1 : 0;
    }
    // Query outcomes are encoded exactly, so any byte difference is a different hit or visibility.
    const usize differences = CountDifferentPixels(pixels, baselinePixels, 0);
    INFO("primary hits=" << primaryHits << ", secondary hits=" << secondaryHits << ", visible=" << visible
                         << ", occluded=" << occluded << ", different pixels=" << differences);
    if (itemCount == 0)
    {
        CHECK(primaryHits == 0);
        CHECK(differences == 0);
        return;
    }
    // The fixture exercises surface hits, occluded and unoccluded shadow rays.
    const usize pixelCount = pixels.GetSize() / 4;
    CHECK(primaryHits > pixelCount / 2);
    CHECK(secondaryHits > 0);
    CHECK(visible > 0);
    CHECK(occluded > 0);
    // Watertight, Moller-Trumbore and hardware intersection can differ only where a query grazes an
    // edge or ties two triangles; allow one pixel in a thousand for those.
    constexpr usize DIFFERING_PIXEL_DIVISOR = 1000;
    CHECK(differences <= static_cast<usize>(WIDTH) * HEIGHT / DIFFERING_PIXEL_DIVISOR);
}

TEST_CASE("The compute ray-tracing template runs its ComputeBvh configuration without native rays",
          "[render-graph][gpu][compute-ray-tracing][template][ray-traversal]")
{
    bool shouldCompareNative = false;
    SECTION("without requested ray features") {}
    SECTION("against the native default configuration")
    {
        shouldCompareNative = true;
    }
    GraphGpuFixture fixture(false, shouldCompareNative);
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(
        {LUDUS_RG3_COMPUTE_RAY_TRACING_BVH_TEMPLATE_DIRECTORY, "rg2_compute_ray_tracing_bvh_template.lurgrt"});
    const renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(renderer::RequiresHardwareRayTracing(plan) == false);
    constexpr usize PASS_COUNT = 2;
    REQUIRE(plan.Passes.GetSize() == PASS_COUNT);
    for (usize index = 0; index < plan.Passes.GetSize(); ++index)
    {
        CHECK(plan.Passes[index].Traversal == format::RayTraversal::ComputeBvh);
    }
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    for (const rhi::RasterBinding& binding : programs.RayCompute.Bindings)
    {
        CHECK(binding.Kind != rhi::RasterBindingKind::RayScene);
    }
    const CornellBox room;
    const StaticArray<renderer::RenderGraphItem, TRAVERSAL_ITEM_COUNT> items = MakeTraversalItems(room);
    const uint32 output = PrepareRoomGraph(fixture.Graph, fixture, *package, room, items, 1);
    constexpr usize FRAME_COUNT = 64;
    Array<uint8> pixels;
    if (shouldCompareNative == false)
    {
        for (usize frame = 0; frame < FRAME_COUNT; ++frame)
        {
            RenderExport(fixture.Graph, fixture, output, {}, pixels);
            REQUIRE(CountFailurePixels(pixels) == 0);
        }
        // The room is lit: the back wall at the center is white, the left wall red and the right wall green.
        const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
        const usize left = GetPixel(WIDTH / 2 - WIDTH * 3 / 10, HEIGHT / 2);
        const usize right = GetPixel(WIDTH / 2 + WIDTH * 3 / 10, HEIGHT / 2);
        CHECK(pixels[center] > 0);
        CHECK(pixels[left] > pixels[left + 1]);
        CHECK(pixels[right + 1] > pixels[right]);
        return;
    }
    const core::UniquePtr<renderer::RenderGraphPackage> nativePackage =
        LoadPackage({LUDUS_RG3_COMPUTE_RAY_TRACING_TEMPLATE_DIRECTORY, "rg2_compute_ray_tracing_template.lurgrt"});
    REQUIRE(renderer::RequiresHardwareRayTracing(nativePackage->GetPlan()) == true);
    renderer::RenderGraph native;
    const uint32 nativeOutput = PrepareRoomGraph(native, fixture, *nativePackage, room, items, 1);
    Array<uint8> nativePixels;
    for (usize frame = 0; frame < FRAME_COUNT; ++frame)
    {
        RenderExport(fixture.Graph, fixture, output, {}, pixels);
        RenderExport(native, fixture, nativeOutput, {}, nativePixels);
        REQUIRE(CountFailurePixels(pixels) == 0);
        REQUIRE(CountFailurePixels(nativePixels) == 0);
    }
    // Both configurations draw the same random values; only the traversal differs, so the
    // accumulated images agree except where a shadow ray grazes an edge differently.
    const math::Vector3 mean = ComputeMeanRadiance(pixels);
    const math::Vector3 nativeMean = ComputeMeanRadiance(nativePixels);
    INFO("ComputeBvh mean=" << mean.X << ',' << mean.Y << ',' << mean.Z << ", native mean=" << nativeMean.X << ','
                            << nativeMean.Y << ',' << nativeMean.Z);
    constexpr float32 RELATIVE_MEAN_TOLERANCE = .02F;
    CHECK(math::Abs(mean.X - nativeMean.X) < RELATIVE_MEAN_TOLERANCE * nativeMean.X);
    CHECK(math::Abs(mean.Y - nativeMean.Y) < RELATIVE_MEAN_TOLERANCE * nativeMean.Y);
    CHECK(math::Abs(mean.Z - nativeMean.Z) < RELATIVE_MEAN_TOLERANCE * nativeMean.Z);
    // Hardware and watertight barycentrics differ in their last bits, so shading may differ by a byte.
    constexpr uint8 CHANNEL_TOLERANCE = 2;
    constexpr usize DIFFERING_PIXEL_DIVISOR = 1000;
    CHECK(CountDifferentPixels(pixels, nativePixels, CHANNEL_TOLERANCE) <=
          static_cast<usize>(WIDTH) * HEIGHT / DIFFERING_PIXEL_DIVISOR);
}

namespace
{
// Veach and Guibas MIS template tests. Thanks to Eric Veach and Leonidas J. Guibas, "Optimally
// Combining Sampling Techniques for Monte Carlo Rendering", SIGGRAPH 1995, pp. 419-428,
// https://doi.org/10.1145/218380.218498 : the template combines their light and BSDF strategies and
// exports each strategy alone, so these tests check the estimator's unbiasedness and its lower noise.

/// Shade passes of the template, in its output order: MIS (the graph's color output), then each strategy alone.
constexpr usize VEACH_GUIBAS_OUTPUT_COUNT = 3;
/// Index of the first shade pass; pass 0 records the primary hits.
constexpr usize VEACH_GUIBAS_FIRST_SHADE_PASS = 1;
constexpr usize VEACH_GUIBAS_MIS = 0;
constexpr usize VEACH_GUIBAS_LIGHT_SAMPLING = 1;
constexpr usize VEACH_GUIBAS_BSDF_SAMPLING = 2;
constexpr uint64 VEACH_GUIBAS_TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;

/// Glossy metal on the white surfaces and a grey sky through the open front. For highlights of the
/// ceiling light and for the sky, BSDF sampling has the higher density, while light sampling suits
/// the diffuse red and green walls: as in Veach and Guibas's glossy-plate scene, neither strategy is
/// good everywhere.
void MakeRoomGlossy(CornellBox& inoutRoom)
{
    constexpr float32 GLOSSY_ROUGHNESS = .3F;
    constexpr float32 SKY_RADIANCE = .5F;
    inoutRoom.Materials[CORNELL_WHITE].Metallic = 1;
    inoutRoom.Materials[CORNELL_WHITE].Roughness = GLOSSY_ROUGHNESS;
    inoutRoom.Inputs.Environment = {SKY_RADIANCE, SKY_RADIANCE, SKY_RADIANCE};
}

/// A runtime graph initialized for one room, with the package storage its programs borrow.
struct PreparedGraph final
{
    core::UniquePtr<renderer::RenderGraphPackage> Package;
    renderer::RenderGraphPlan Plan;
    renderer::RenderGraphPrograms Programs;
    renderer::RenderGraph Graph;

    /// @param revision Scene and camera revision; a different revision reseeds sampling.
    PreparedGraph(const GraphGpuFixture& fixture,
                  const PackageLocation& location,
                  const CornellBox& room,
                  uint64 revision)
        : Package(LoadPackage(location)), Plan(Package->GetPlan()),
          Programs(Package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()))
    {
        wait(Graph.Initialize(fixture.Device, Plan, Programs, nullptr, WIDTH, HEIGHT));
        renderer::RenderGraphReport report;
        wait(Graph.Update({room.Meshes.GetData(), room.Meshes.GetSize()},
                          {room.Items.GetData(), room.Items.GetSize()},
                          {room.Cameras.GetData(), room.Cameras.GetSize()},
                          revision,
                          renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                          report,
                          &room.Inputs,
                          revision));
    }
    PreparedGraph(const PreparedGraph&) = delete;
    PreparedGraph& operator=(const PreparedGraph&) = delete;

    /// The single exported color resource of a reference graph.
    uint32 GetOnlyExport() const
    {
        uint32 output = 0;
        usize exportCount = 0;
        for (uint32 resource = 0; resource < Plan.Resources.GetSize(); ++resource)
        {
            output = Plan.Resources[resource].IsExported == 1 ? resource : output;
            exportCount += Plan.Resources[resource].IsExported == 1 ? 1 : 0;
        }
        REQUIRE(exportCount == 1);
        return output;
    }

    /// The template's exports, identified by the shade pass that writes each one.
    StaticArray<uint32, VEACH_GUIBAS_OUTPUT_COUNT> GetVeachGuibasExports() const
    {
        REQUIRE(Plan.Passes.GetSize() == VEACH_GUIBAS_FIRST_SHADE_PASS + VEACH_GUIBAS_OUTPUT_COUNT);
        StaticArray<uint32, VEACH_GUIBAS_OUTPUT_COUNT> exports{};
        for (usize output = 0; output < VEACH_GUIBAS_OUTPUT_COUNT; ++output)
        {
            exports[output] = Plan.Passes[VEACH_GUIBAS_FIRST_SHADE_PASS + output].Color;
            REQUIRE(exports[output] < Plan.Resources.GetSize());
            REQUIRE(Plan.Resources[exports[output]].IsExported == 1);
        }
        REQUIRE(exports[VEACH_GUIBAS_MIS] != exports[VEACH_GUIBAS_LIGHT_SAMPLING]);
        REQUIRE(exports[VEACH_GUIBAS_MIS] != exports[VEACH_GUIBAS_BSDF_SAMPLING]);
        REQUIRE(exports[VEACH_GUIBAS_LIGHT_SAMPLING] != exports[VEACH_GUIBAS_BSDF_SAMPLING]);
        return exports;
    }

private:
    void wait(rhi::RasterStatus initial)
    {
        rhi::RasterStatus status = initial;
        const uint64 beginning = time::NowTicks();
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < VEACH_GUIBAS_TIMEOUT)
        {
            status = Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    }
};

/// Render frameCount frames of the template and read each output's running mean after the last one.
void RenderVeachGuibasOutputs(PreparedGraph& inoutTemplate,
                              const GraphGpuFixture& fixture,
                              usize frameCount,
                              StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT>& outPixels)
{
    const StaticArray<uint32, VEACH_GUIBAS_OUTPUT_COUNT> exports = inoutTemplate.GetVeachGuibasExports();
    for (usize frame = 0; frame < frameCount; ++frame)
    {
        RenderFrame(inoutTemplate.Graph, fixture);
    }
    for (usize output = 0; output < VEACH_GUIBAS_OUTPUT_COUNT; ++output)
    {
        ReadExport(inoutTemplate.Graph, fixture, exports[output], {}, outPixels[output]);
        // Invalid hit records, exhausted budgets and failed queries would show magenta.
        REQUIRE(CountFailurePixels(outPixels[output]) == 0);
    }
}

/// Number of pixels whose RGB channels differ by more than channelTolerance.
usize CountDifferingPixels(const Array<uint8>& first, const Array<uint8>& second, uint8 channelTolerance)
{
    REQUIRE(first.GetSize() == second.GetSize());
    usize differingPixels = 0;
    for (usize address = 0; address + 3 < first.GetSize(); address += 4)
    {
        bool isDifferent = false;
        for (usize channel = 0; channel < 3; ++channel)
        {
            const uint8 high = math::Max(first[address + channel], second[address + channel]);
            const uint8 low = math::Min(first[address + channel], second[address + channel]);
            isDifferent = isDifferent == true || high - low > channelTolerance;
        }
        differingPixels += isDifferent == true ? 1 : 0;
    }
    return differingPixels;
}

/// Mean squared error of decoded linear radiance against a converged image, over pixels and RGB channels.
float64 ComputeMeanSquaredError(const Array<uint8>& pixels, const Array<uint8>& converged)
{
    REQUIRE(pixels.GetSize() == converged.GetSize());
    float64 sum = 0;
    for (usize address = 0; address + 3 < pixels.GetSize(); address += 4)
    {
        for (usize channel = 0; channel < 3; ++channel)
        {
            const float64 difference = static_cast<float64>(DecodeToneMapped(pixels[address + channel])) -
                                       static_cast<float64>(DecodeToneMapped(converged[address + channel]));
            sum += difference * difference;
        }
    }
    constexpr float64 CHANNEL_COUNT = 3;
    return sum / (static_cast<float64>(pixels.GetSize() / 4) * CHANNEL_COUNT);
}

/// Image means agree within a relative tolerance in every channel.
void CheckMeansAgree(const math::Vector3& mean, const math::Vector3& expected, float32 relativeTolerance)
{
    CHECK(math::Abs(mean.X - expected.X) < relativeTolerance * expected.X);
    CHECK(math::Abs(mean.Y - expected.Y) < relativeTolerance * expected.Y);
    CHECK(math::Abs(mean.Z - expected.Z) < relativeTolerance * expected.Z);
}
} // namespace

TEST_CASE("The Veach and Guibas MIS template matches single-pass references for each of its three outputs",
          "[render-graph][gpu][veach-guibas][template]")
{
    GraphGpuFixture fixture;
    CornellBox room;
    MakeRoomGlossy(room);
    PreparedGraph mis(fixture,
                      {LUDUS_RG3_VEACH_GUIBAS_MIS_TEMPLATE_DIRECTORY, "rg2_veach_guibas_mis_template.lurgrt"},
                      room,
                      1);
    CheckSameStage(mis.Programs.RayCompute,
                   packages::rg2_veach_guibas_mis_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                         shaders::renderer_composite::Fragment())
                       .RayCompute);
    REQUIRE(mis.Programs.HasPersistentRayBuffers == true);
    REQUIRE(renderer::RequiresHardwareRayTracing(mis.Plan) == true);
    // The primary-hits pass, then one shade pass per output with its own Accumulate and ToneMap.
    for (usize pass = 0; pass < mis.Plan.Passes.GetSize(); ++pass)
    {
        INFO("pass " << pass);
        CHECK(mis.Plan.Passes[pass].Program == format::Technique::RayTracing);
        CHECK((mis.Plan.Passes[pass].AccumulationSamples > 0) == (pass >= VEACH_GUIBAS_FIRST_SHADE_PASS));
        CHECK((mis.Plan.Passes[pass].ToneMappingExposure == 1) == (pass >= VEACH_GUIBAS_FIRST_SHADE_PASS));
    }
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> pixels;
    // Both graphs accumulate a running mean; compare after the same number of samples.
    constexpr usize FRAME_COUNT = 64;
    RenderVeachGuibasOutputs(mis, fixture, FRAME_COUNT, pixels);

    const StaticArray<PackageLocation, VEACH_GUIBAS_OUTPUT_COUNT> references{{
        {LUDUS_RG3_VEACH_GUIBAS_REFERENCE_MIS_DIRECTORY, "rg2_veach_guibas_reference_mis.lurgrt"},
        {LUDUS_RG3_VEACH_GUIBAS_REFERENCE_LIGHT_SAMPLING_DIRECTORY, "rg2_veach_guibas_reference_light_sampling.lurgrt"},
        {LUDUS_RG3_VEACH_GUIBAS_REFERENCE_BSDF_SAMPLING_DIRECTORY, "rg2_veach_guibas_reference_bsdf_sampling.lurgrt"},
    }};
    for (usize output = 0; output < VEACH_GUIBAS_OUTPUT_COUNT; ++output)
    {
        PreparedGraph reference(fixture, references[output], room, 1);
        REQUIRE(reference.Plan.Passes.GetSize() == 1);
        const uint32 referenceOutput = reference.GetOnlyExport();
        Array<uint8> referencePixels;
        for (usize frame = 0; frame < FRAME_COUNT; ++frame)
        {
            RenderExport(reference.Graph, fixture, referenceOutput, {}, referencePixels);
        }
        REQUIRE(CountFailurePixels(referencePixels) == 0);
        const math::Vector3 mean = ComputeMeanRadiance(pixels[output]);
        const math::Vector3 referenceMean = ComputeMeanRadiance(referencePixels);
        INFO("output " << output << ": template mean=" << mean.X << ',' << mean.Y << ',' << mean.Z
                       << ", reference mean=" << referenceMean.X << ',' << referenceMean.Y << ',' << referenceMean.Z);
        constexpr float32 MINIMUM_MEAN_RADIANCE = .01F;
        CHECK(mean.X > MINIMUM_MEAN_RADIANCE);
        // Each reference draws jitter, light selection, light shape and BSDF direction from its
        // automatic stream in the order of the template's explicit ranges ([0, 2), then [2, 9)), so
        // each sample sees the same random values: on Metal the images are byte-identical. The
        // tolerances allow other backends to contract floating-point operations differently in the
        // two kernels. A 2% mean difference also exceeds the Monte Carlo error of 64 samples per pixel.
        constexpr float32 RELATIVE_MEAN_TOLERANCE = .02F;
        CheckMeansAgree(mean, referenceMean, RELATIVE_MEAN_TOLERANCE);
        // At most one pixel in a thousand, for shadow or BSDF rays that graze an edge differently.
        constexpr uint8 CHANNEL_TOLERANCE = 2;
        constexpr usize DIFFERING_PIXEL_DIVISOR = 1000;
        CHECK(CountDifferingPixels(pixels[output], referencePixels, CHANNEL_TOLERANCE) <=
              static_cast<usize>(WIDTH) * HEIGHT / DIFFERING_PIXEL_DIVISOR);
    }
}

TEST_CASE("Every MIS heuristic of the Veach and Guibas template estimates the same integral as each strategy alone",
          "[render-graph][gpu][veach-guibas][template][mis]")
{
    // Veach and Guibas's multi-sample estimator is unbiased whenever the strategy weights sum to one.
    // Each heuristic variant (the same sources with another MIS_HEURISTIC setting) must therefore
    // converge to what light sampling alone estimates; so must BSDF sampling alone, since the room has
    // no delta light that BSDF sampling could miss.
    GraphGpuFixture fixture;
    CornellBox room;
    MakeRoomGlossy(room);
    PackageLocation heuristic{LUDUS_RG3_VEACH_GUIBAS_MIS_TEMPLATE_DIRECTORY, "rg2_veach_guibas_mis_template.lurgrt"};
    SECTION("power (default)") {}
    SECTION("balance")
    {
        heuristic = {LUDUS_RG3_VEACH_GUIBAS_MIS_BALANCE_DIRECTORY, "rg2_veach_guibas_mis_balance.lurgrt"};
    }
    SECTION("cutoff")
    {
        heuristic = {LUDUS_RG3_VEACH_GUIBAS_MIS_CUTOFF_DIRECTORY, "rg2_veach_guibas_mis_cutoff.lurgrt"};
    }
    SECTION("maximum")
    {
        heuristic = {LUDUS_RG3_VEACH_GUIBAS_MIS_MAXIMUM_DIRECTORY, "rg2_veach_guibas_mis_maximum.lurgrt"};
    }
    PreparedGraph graph(fixture, heuristic, room, 1);
    constexpr usize FRAME_COUNT = 256;
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> pixels;
    RenderVeachGuibasOutputs(graph, fixture, FRAME_COUNT, pixels);
    const math::Vector3 mis = ComputeMeanRadiance(pixels[VEACH_GUIBAS_MIS]);
    const math::Vector3 lightSampling = ComputeMeanRadiance(pixels[VEACH_GUIBAS_LIGHT_SAMPLING]);
    const math::Vector3 bsdfSampling = ComputeMeanRadiance(pixels[VEACH_GUIBAS_BSDF_SAMPLING]);
    // MIS and light sampling share the light strategy's random values, so the images differ only through
    // the weights and the BSDF strategy. A difference in many pixels shows that the BSDF strategy contributes.
    constexpr uint8 CHANNEL_TOLERANCE = 2;
    const usize differingPixels =
        CountDifferingPixels(pixels[VEACH_GUIBAS_MIS], pixels[VEACH_GUIBAS_LIGHT_SAMPLING], CHANNEL_TOLERANCE);
    INFO("MIS mean=" << mis.X << ',' << mis.Y << ',' << mis.Z << ", light sampling mean=" << lightSampling.X << ','
                     << lightSampling.Y << ',' << lightSampling.Z << ", BSDF sampling mean=" << bsdfSampling.X << ','
                     << bsdfSampling.Y << ',' << bsdfSampling.Z << ", differing pixels=" << differingPixels);
    constexpr usize DIFFERING_PIXEL_DIVISOR = 10;
    CHECK(differingPixels > static_cast<usize>(WIDTH) * HEIGHT / DIFFERING_PIXEL_DIVISOR);
    constexpr float32 MINIMUM_MEAN_RADIANCE = .01F;
    REQUIRE(lightSampling.X > MINIMUM_MEAN_RADIANCE);
    // Estimates of one integral, compared after decoding the tone-mapped running means. With 256
    // samples per pixel the image-mean Monte Carlo difference stays well below 2%.
    constexpr float32 RELATIVE_MEAN_TOLERANCE = .02F;
    CheckMeansAgree(mis, lightSampling, RELATIVE_MEAN_TOLERANCE);
    CheckMeansAgree(bsdfSampling, lightSampling, RELATIVE_MEAN_TOLERANCE);
}

TEST_CASE("The Veach and Guibas MIS output has less error than either sampling strategy alone",
          "[render-graph][gpu][veach-guibas][template][mis]")
{
    // The paper's central result: in a scene where each strategy fails somewhere (glossy highlights
    // for light sampling, the small light on diffuse walls for BSDF sampling), MIS keeps the error of
    // whichever strategy is better at each sample. Compare low-sample images of all three outputs
    // with an independent converged image: another graph instance whose scene revision reseeds sampling.
    GraphGpuFixture fixture;
    CornellBox room;
    MakeRoomGlossy(room);
    const PackageLocation location{LUDUS_RG3_VEACH_GUIBAS_MIS_TEMPLATE_DIRECTORY,
                                   "rg2_veach_guibas_mis_template.lurgrt"};
    PreparedGraph graph(fixture, location, room, 1);
    constexpr usize LOW_FRAME_COUNT = 16;
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> pixels;
    RenderVeachGuibasOutputs(graph, fixture, LOW_FRAME_COUNT, pixels);
    constexpr uint64 CONVERGED_REVISION = 2;
    PreparedGraph convergedGraph(fixture, location, room, CONVERGED_REVISION);
    constexpr usize CONVERGED_FRAME_COUNT = 1024;
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> converged;
    RenderVeachGuibasOutputs(convergedGraph, fixture, CONVERGED_FRAME_COUNT, converged);
    StaticArray<float64, VEACH_GUIBAS_OUTPUT_COUNT> errors{};
    for (usize output = 0; output < VEACH_GUIBAS_OUTPUT_COUNT; ++output)
    {
        errors[output] = ComputeMeanSquaredError(pixels[output], converged[VEACH_GUIBAS_MIS]);
    }
    INFO("mean squared error at " << LOW_FRAME_COUNT << " samples: MIS=" << errors[VEACH_GUIBAS_MIS]
                                  << ", light sampling=" << errors[VEACH_GUIBAS_LIGHT_SAMPLING]
                                  << ", BSDF sampling=" << errors[VEACH_GUIBAS_BSDF_SAMPLING]);
    CHECK(errors[VEACH_GUIBAS_MIS] < errors[VEACH_GUIBAS_LIGHT_SAMPLING]);
    CHECK(errors[VEACH_GUIBAS_MIS] < errors[VEACH_GUIBAS_BSDF_SAMPLING]);
}

namespace
{
// Line visibility query tests (point-line spec PL1). Thanks to Niels Billen and Philip Dutre, "Line
// Sampling for Direct Illumination", Computer Graphics Forum 35(4) (EGSR 2016), pp. 93-102,
// https://doi.org/10/f84z2h : FindVisibleLineIntervals clips each occluder against their shadow
// triangle, so the visible parts of a segment have closed forms for these planar occluders.
//
// The line_visibility kernel's receiver is the World origin and its segment runs from (-1, 2, 0) to
// (1, 2, 0). An occluder crossing the shadow plane z = 0 at height y over [xa, xb] hides segment
// parameters [(xa / y + 1) / 2, (xb / y + 1) / 2]. Each kernel row is one query form; columns hold
// the status, count and HasMaskedOccluders bytes, the visible length and then each interval, as exact
// 24-bit fixed point (no Accumulate or ToneMap).
constexpr usize LINE_ROW_PLAIN = 0;
constexpr usize LINE_ROW_RECEIVER_EXCLUDED = 1;
constexpr usize LINE_ROW_SEGMENT_EXCLUDED = 2;
constexpr usize LINE_FIRST_FAILING_ROW = 3;
constexpr usize LINE_LAST_FAILING_ROW = 5;
constexpr usize LINE_FIRST_INTERVAL_COLUMN = 2;
constexpr usize LINE_INTERVAL_CAPACITY = renderer::SCENE_LINE_INTERVAL_CAPACITY;
constexpr float64 LINE_SEGMENT_LENGTH_WORLD = 2;
constexpr float64 LINE_FIXED_POINT_SCALE = 16777215;
constexpr uint8 LINE_QUERY_MISS = 0;
constexpr uint8 LINE_QUERY_SURFACE = 1;
/// Parameter tolerance against the analytic intervals: float32 rounding of the occluder vertices and
/// of the clip, plus 6e-8 of 24-bit readout quantization, stays below it; equal to the merge tolerance.
constexpr float64 LINE_PARAMETER_TOLERANCE = static_cast<float64>(renderer::SCENE_LINE_INTERVAL_MERGE_TOLERANCE);
constexpr usize LINE_MAXIMUM_TRIANGLES = 96;

/// Triangle soup for the line visibility kernel, with one neutral material and a camera the kernel
/// ignores. The mesh borrows this object's arrays, so it is neither copied nor moved.
struct LineScene final
{
    Array<math::Vector3> Positions;
    Array<uint32> Indices;
    StaticArray<uint32, 1> Bindings{{0}};
    StaticArray<renderer::SceneShaderMaterial, 1> Materials{};
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::RenderGraphItem, 1> Items{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    renderer::SceneShaderInputs Inputs;
    LineScene()
    {
        REQUIRE(Positions.TryEnsureCapacity(LINE_MAXIMUM_TRIANGLES * 3) == true);
        REQUIRE(Indices.TryEnsureCapacity(LINE_MAXIMUM_TRIANGLES * 3) == true);
        // Occluders behind the receiver (y = -1) and beyond the segment (y = 3) are in every scene.
        AddTriangle({-1, -1, -1}, {1, -1, -1}, {0, -1, 1});
        AddTriangle({-1, 3, -1}, {1, 3, -1}, {0, 3, 1});
    }
    LineScene(const LineScene&) = delete;
    LineScene& operator=(const LineScene&) = delete;
    void AddTriangle(math::Vector3 position0World, math::Vector3 position1World, math::Vector3 position2World)
    {
        const uint32 first = static_cast<uint32>(Positions.GetSize());
        REQUIRE(Positions.TryAdd(position0World) == true);
        REQUIRE(Positions.TryAdd(position1World) == true);
        REQUIRE(Positions.TryAdd(position2World) == true);
        for (uint32 corner = 0; corner < 3; ++corner)
        {
            REQUIRE(Indices.TryAdd(first + corner) == true);
        }
    }
    /// An occluder at height y whose cross-section with z = 0 is [xa, xb]; its third vertex lies in
    /// the shadow plane, so it also covers a vertex that contributes itself to the clip.
    void AddOccluder(float32 heightWorld, float32 minimumWorld, float32 maximumWorld)
    {
        AddTriangle({minimumWorld, heightWorld, -1}, {minimumWorld, heightWorld, 1}, {maximumWorld, heightWorld, 0});
    }
    void Finish()
    {
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Meshes[0].MaterialBindings = {Bindings.GetData(), Bindings.GetSize()};
        Items[0].SourceId = 1;
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        constexpr float32 FOCAL_LENGTH = 2.2F;
        constexpr float32 NEAR_WORLD = .5F;
        constexpr float32 FAR_WORLD = 10;
        constexpr float32 EYE_Z_WORLD = 3.4F;
        constexpr float32 DEPTH_SCALE = FAR_WORLD / (NEAR_WORLD - FAR_WORLD);
        const float32 aspect = static_cast<float32>(WIDTH) / static_cast<float32>(HEIGHT);
        math::Matrix4& worldToClip = Cameras[0].WorldToClip;
        worldToClip.Columns[0] = {FOCAL_LENGTH / aspect, 0, 0, 0};
        worldToClip.Columns[1] = {0, FOCAL_LENGTH, 0, 0};
        worldToClip.Columns[2] = {0, 0, DEPTH_SCALE, -1};
        worldToClip.Columns[3] = {0, 0, DEPTH_SCALE * (NEAR_WORLD - EYE_Z_WORLD), EYE_Z_WORLD};
        Cameras[0].CameraPosition = {0, 0, EYE_Z_WORLD};
    }
};

/// Expected result of one kernel row: status and visible intervals as segment parameters.
struct LineExpectation final
{
    uint8 Status = LINE_QUERY_MISS;
    usize IntervalCount = 1;
    StaticArray<float64, LINE_INTERVAL_CAPACITY * 2> Bounds{{0, 1}};
};

/// K equal slats at y = 1 across the segment, each hiding the middle half of its 1 / K share.
void AddSlats(LineScene& inoutScene, usize slatCount, LineExpectation& outExpected)
{
    outExpected = {};
    outExpected.Status = LINE_QUERY_SURFACE;
    outExpected.IntervalCount = slatCount + 1;
    const float64 pitch = 1.0 / static_cast<float64>(slatCount);
    for (usize slat = 0; slat < slatCount; ++slat)
    {
        const float64 start = (static_cast<float64>(slat) + .25) * pitch;
        const float64 end = (static_cast<float64>(slat) + .75) * pitch;
        // On y = 1, segment parameter t covers x = t - 0.5.
        inoutScene.AddOccluder(1, static_cast<float32>(start - .5), static_cast<float32>(end - .5));
        if (slat + 1 < LINE_INTERVAL_CAPACITY)
        {
            outExpected.Bounds[slat * 2 + 1] = start;
            outExpected.Bounds[slat * 2 + 2] = end;
        }
    }
    if (slatCount < LINE_INTERVAL_CAPACITY)
    {
        outExpected.Bounds[slatCount * 2 + 1] = 1;
    }
}

float64 DecodeLineFixed(const Array<uint8>& pixels, usize column, usize row)
{
    const usize address = GetPixel(column, row);
    const uint32 bits = (static_cast<uint32>(pixels[address]) << 16) | (static_cast<uint32>(pixels[address + 1]) << 8) |
                        static_cast<uint32>(pixels[address + 2]);
    return static_cast<float64>(bits) / LINE_FIXED_POINT_SCALE;
}

bool IsFailurePixel(const Array<uint8>& pixels, usize column, usize row)
{
    const usize address = GetPixel(column, row);
    return pixels[address] == 255 && pixels[address + 1] == 0 && pixels[address + 2] == 255;
}

/// Compare one kernel row with its analytic intervals; returns the largest parameter error.
float64 CheckLineRow(const Array<uint8>& pixels, usize row, const LineExpectation& expected)
{
    INFO("row " << row);
    REQUIRE(IsFailurePixel(pixels, 0, row) == false);
    const usize header = GetPixel(0, row);
    CHECK(pixels[header] == expected.Status);
    CHECK(pixels[header + 1] == expected.IntervalCount);
    // Render-graph scenes are opaque, so no masked occluder is ever reported.
    CHECK(pixels[header + 2] == 0);
    float64 largestError = 0;
    float64 visibleParameter = 0;
    for (usize interval = 0; interval < expected.IntervalCount; ++interval)
    {
        for (usize bound = 0; bound < 2; ++bound)
        {
            const float64 measured = DecodeLineFixed(pixels, LINE_FIRST_INTERVAL_COLUMN + interval * 2 + bound, row);
            const float64 error = math::Abs(measured - expected.Bounds[interval * 2 + bound]);
            INFO("interval " << interval << " bound " << bound << ": measured " << measured << ", expected "
                             << expected.Bounds[interval * 2 + bound]);
            CHECK(error <= LINE_PARAMETER_TOLERANCE);
            largestError = math::Max(largestError, error);
        }
        visibleParameter += expected.Bounds[interval * 2 + 1] - expected.Bounds[interval * 2];
    }
    // Later interval slots are zero.
    for (usize column = LINE_FIRST_INTERVAL_COLUMN + expected.IntervalCount * 2;
         column < LINE_FIRST_INTERVAL_COLUMN + LINE_INTERVAL_CAPACITY * 2;
         ++column)
    {
        CHECK(DecodeLineFixed(pixels, column, row) == 0);
    }
    const float64 visibleLengthWorld = DecodeLineFixed(pixels, 1, row) * LINE_SEGMENT_LENGTH_WORLD;
    CHECK(math::Abs(visibleLengthWorld - visibleParameter * LINE_SEGMENT_LENGTH_WORLD) <=
          LINE_PARAMETER_TOLERANCE * LINE_SEGMENT_LENGTH_WORLD * static_cast<float64>(expected.IntervalCount * 2));
    return largestError;
}

/// The line_visibility package, initialized once and updated per scene.
struct LineVisibilityGraph final
{
    core::UniquePtr<renderer::RenderGraphPackage> Package;
    renderer::RenderGraphPlan Plan;
    renderer::RenderGraphPrograms Programs;
    renderer::RenderGraph Graph;
    uint32 Output = 0;

    LineVisibilityGraph(const GraphGpuFixture& fixture, const PackageLocation& location)
        : Package(LoadPackage(location)), Plan(Package->GetPlan()),
          Programs(Package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()))
    {
        for (uint32 resource = 0; resource < Plan.Resources.GetSize(); ++resource)
        {
            Output = Plan.Resources[resource].IsExported == 1 ? resource : Output;
        }
        wait(Graph.Initialize(fixture.Device, Plan, Programs, nullptr, WIDTH, HEIGHT));
    }
    LineVisibilityGraph(const LineVisibilityGraph&) = delete;
    LineVisibilityGraph& operator=(const LineVisibilityGraph&) = delete;

    void Render(const GraphGpuFixture& fixture, const LineScene& scene, uint64 revision, Array<uint8>& outPixels)
    {
        renderer::RenderGraphReport report;
        wait(Graph.Update({scene.Meshes.GetData(), scene.Meshes.GetSize()},
                          {scene.Items.GetData(), scene.Items.GetSize()},
                          {scene.Cameras.GetData(), scene.Cameras.GetSize()},
                          revision,
                          renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                          report,
                          &scene.Inputs,
                          revision));
        RenderExport(Graph, fixture, Output, {}, outPixels);
    }

private:
    void wait(rhi::RasterStatus initial)
    {
        rhi::RasterStatus status = initial;
        const uint64 beginning = time::NowTicks();
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < VEACH_GUIBAS_TIMEOUT)
        {
            status = Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    }
};

/// The analytic line visibility cases, each one World and the plain row's expected intervals.
enum class LineCase : uint8
{
    Unoccluded,
    OneOccluder,
    OverlappingOccluders,
    AbuttingQuad,
    CoplanarOccluder,
    FullyOccluded,
    FullCapacity,
    Overflow,
};
constexpr usize LINE_CASE_COUNT = 8;

/// Build one case. Returns false for the overflowing case, whose plain row must fail.
bool BuildLineCase(LineCase lineCase, LineScene& inoutScene, LineExpectation& outExpected)
{
    outExpected = {};
    bool isSuccessful = true;
    switch (lineCase)
    {
        case LineCase::Unoccluded:
            break;
        case LineCase::OneOccluder:
            inoutScene.AddOccluder(1, -.25F, .25F);
            outExpected.Status = LINE_QUERY_SURFACE;
            outExpected.IntervalCount = 2;
            outExpected.Bounds = {{0, .25, .75, 1}};
            break;
        case LineCase::OverlappingOccluders:
            // [.25, .75] and [.6, .9] merge; [.05, .15] stays disjoint.
            inoutScene.AddOccluder(1, -.25F, .25F);
            inoutScene.AddOccluder(1.5F, .15F, .6F);
            inoutScene.AddOccluder(1, -.45F, -.35F);
            outExpected.Status = LINE_QUERY_SURFACE;
            outExpected.IntervalCount = 3;
            outExpected.Bounds = {{0, .05, .15, .25, .9, 1}};
            break;
        case LineCase::AbuttingQuad:
            // Two triangles sharing a diagonal that crosses the shadow plane at a non-dyadic point: a
            // crack along it would split the occluded interval in two.
            inoutScene.AddTriangle({-.25F, 1, -.7F}, {.25F, 1, -.7F}, {.25F, 1, 1.3F});
            inoutScene.AddTriangle({-.25F, 1, -.7F}, {.25F, 1, 1.3F}, {-.25F, 1, 1.3F});
            outExpected.Status = LINE_QUERY_SURFACE;
            outExpected.IntervalCount = 2;
            outExpected.Bounds = {{0, .25, .75, 1}};
            break;
        case LineCase::CoplanarOccluder:
            inoutScene.AddTriangle({-.5F, 1, 0}, {.5F, 1, 0}, {0, 1.5F, 0});
            break;
        case LineCase::FullyOccluded:
            inoutScene.AddOccluder(1, -1, 2);
            outExpected.Status = LINE_QUERY_SURFACE;
            outExpected.IntervalCount = 0;
            break;
        case LineCase::FullCapacity:
            // CAPACITY - 1 occluded intervals leave exactly CAPACITY visible ones.
            AddSlats(inoutScene, LINE_INTERVAL_CAPACITY - 1, outExpected);
            break;
        case LineCase::Overflow:
            AddSlats(inoutScene, LINE_INTERVAL_CAPACITY, outExpected);
            isSuccessful = false;
            break;
    }
    inoutScene.Finish();
    return isSuccessful;
}

/// Render every case with one package and check each row; outImages keeps each case's pixels.
void CheckLineCases(const GraphGpuFixture& fixture,
                    const PackageLocation& location,
                    StaticArray<Array<uint8>, LINE_CASE_COUNT>& outImages)
{
    LineVisibilityGraph graph(fixture, location);
    float64 largestError = 0;
    for (usize index = 0; index < LINE_CASE_COUNT; ++index)
    {
        const LineCase lineCase = static_cast<LineCase>(index);
        INFO("case " << index);
        LineScene scene;
        LineExpectation expected;
        const bool isSuccessful = BuildLineCase(lineCase, scene, expected);
        graph.Render(fixture, scene, index + 1, outImages[index]);
        const Array<uint8>& pixels = outImages[index];
        if (isSuccessful == true)
        {
            largestError = math::Max(largestError, CheckLineRow(pixels, LINE_ROW_PLAIN, expected));
        }
        else
        {
            // Capacity overflow latches failure and shows the error color.
            CHECK(IsFailurePixel(pixels, 0, LINE_ROW_PLAIN) == true);
        }
        if (lineCase == LineCase::OneOccluder)
        {
            // Naming the occluder as the receiver's or the segment's carrying triangle excludes it.
            const LineExpectation unoccluded{};
            largestError = math::Max(largestError, CheckLineRow(pixels, LINE_ROW_RECEIVER_EXCLUDED, unoccluded));
            largestError = math::Max(largestError, CheckLineRow(pixels, LINE_ROW_SEGMENT_EXCLUDED, unoccluded));
        }
        // A zero-length segment, a second query beyond the budget of one and a receiver in the
        // segment's line fail in every World.
        for (usize row = LINE_FIRST_FAILING_ROW; row <= LINE_LAST_FAILING_ROW; ++row)
        {
            INFO("failing row " << row);
            CHECK(IsFailurePixel(pixels, 0, row) == true);
            CHECK(IsFailurePixel(pixels, WIDTH - 1, row) == true);
        }
    }
    INFO("largest interval error " << largestError);
    CHECK(largestError <= LINE_PARAMETER_TOLERANCE);
}
} // namespace

TEST_CASE("Line visibility queries match analytic intervals through a triangle-loop scene compute pass",
          "[render-graph][gpu][line-visibility]")
{
    GraphGpuFixture fixture(false, false);
    const PackageLocation location{LUDUS_RG3_LINE_VISIBILITY_LOOP_DIRECTORY, "rg2_line_visibility_loop.lurgrt"};
    {
        const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(location);
        // A triangle-loop pass that calls the line query stays a compute ray-tracing package (PL06).
        CHECK(renderer::RequiresHardwareRayTracing(package->GetPlan()) == false);
        CHECK(renderer::UsesRayTracing(package->GetPlan()) == true);
        CheckSameStage(package->GetPrograms({}, {}).RayCompute,
                       packages::rg2_line_visibility_loop::MakePrograms({}, {}).RayCompute);
    }
    StaticArray<Array<uint8>, LINE_CASE_COUNT> images;
    CheckLineCases(fixture, location, images);
}

TEST_CASE("Line visibility in a NativeQuery pass uses the same software traversal as the triangle loop",
          "[render-graph][gpu][line-visibility]")
{
    // Native traversal serves rays only; the line query scans the packed triangles in every pass, so
    // both strategies give byte-identical results (PL06).
    GraphGpuFixture fixture;
    const PackageLocation native{LUDUS_RG3_LINE_VISIBILITY_NATIVE_DIRECTORY, "rg2_line_visibility_native.lurgrt"};
    REQUIRE(renderer::RequiresHardwareRayTracing(LoadPackage(native)->GetPlan()) == true);
    StaticArray<Array<uint8>, LINE_CASE_COUNT> nativeImages;
    CheckLineCases(fixture, native, nativeImages);
    StaticArray<Array<uint8>, LINE_CASE_COUNT> loopImages;
    CheckLineCases(fixture, {LUDUS_RG3_LINE_VISIBILITY_LOOP_DIRECTORY, "rg2_line_visibility_loop.lurgrt"}, loopImages);
    for (usize index = 0; index < LINE_CASE_COUNT; ++index)
    {
        INFO("case " << index);
        CHECK(CountDifferingPixels(nativeImages[index], loopImages[index], 0) == 0);
    }
}

namespace
{
// Line-sampling template tests. Thanks to Billen and Dutre (2016), above, for the surface-area line
// estimator, and to Katherine Salesin and Wojciech Jarosz, "Combining Point and Line Samples for
// Direct Illumination", Computer Graphics Forum 38(4) (EGSR 2019), pp. 159-169,
// https://doi.org/10.1111/cgf.13779 , for treating a line sample as a point sample with a joint
// density: the template therefore estimates the same integral as ordinary light sampling.
constexpr usize LINE_SAMPLING_OUTPUT_COUNT = 2;
constexpr usize LINE_SAMPLING_COLOR = 0;
constexpr usize LINE_SAMPLING_VISIBLE_FRACTION = 1;
/// Index of the first shade pass; pass 0 records the primary hits.
constexpr usize LINE_SAMPLING_FIRST_SHADE_PASS = 1;

StaticArray<uint32, LINE_SAMPLING_OUTPUT_COUNT> GetLineSamplingExports(const renderer::RenderGraphPlan& plan)
{
    REQUIRE(plan.Passes.GetSize() == LINE_SAMPLING_FIRST_SHADE_PASS + LINE_SAMPLING_OUTPUT_COUNT);
    StaticArray<uint32, LINE_SAMPLING_OUTPUT_COUNT> exports{};
    for (usize output = 0; output < LINE_SAMPLING_OUTPUT_COUNT; ++output)
    {
        exports[output] = plan.Passes[LINE_SAMPLING_FIRST_SHADE_PASS + output].Color;
        REQUIRE(exports[output] < plan.Resources.GetSize());
        REQUIRE(plan.Resources[exports[output]].IsExported == 1);
    }
    REQUIRE(exports[LINE_SAMPLING_COLOR] != exports[LINE_SAMPLING_VISIBLE_FRACTION]);
    return exports;
}

/// Mean decoded value over a square of pixels centered on (column, row).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a pixel column and row, then the square's radius
float32 ComputeRegionMean(const Array<uint8>& pixels, usize column, usize row, usize radius)
{
    float32 sum = 0;
    usize count = 0;
    for (usize y = row - radius; y <= row + radius; ++y)
    {
        for (usize x = column - radius; x <= column + radius; ++x)
        {
            sum += DecodeToneMapped(pixels[GetPixel(x, y)]);
            ++count;
        }
    }
    return sum / static_cast<float32>(count);
}
} // namespace

TEST_CASE("The line-sampling template converges to light sampling and shows its visible fraction",
          "[render-graph][gpu][line-sampling][template]")
{
    GraphGpuFixture fixture(false, false);
    CornellBox room;
    const PackageLocation location{LUDUS_RG3_LINE_SAMPLING_TEMPLATE_DIRECTORY, "rg2_line_sampling_template.lurgrt"};
    PreparedGraph lines(fixture, location, room, 1);
    CheckSameStage(lines.Programs.RayCompute,
                   packages::rg2_line_sampling_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                      shaders::renderer_composite::Fragment())
                       .RayCompute);
    REQUIRE(lines.Programs.HasPersistentRayBuffers == true);
    // Every pass is a triangle loop: a compute ray-tracing package with no native ray requirement.
    REQUIRE(renderer::RequiresHardwareRayTracing(lines.Plan) == false);
    REQUIRE(renderer::UsesRayTracing(lines.Plan) == true);
    for (usize pass = 0; pass < lines.Plan.Passes.GetSize(); ++pass)
    {
        INFO("pass " << pass);
        CHECK(lines.Plan.Passes[pass].Program == format::Technique::RayTracing);
        CHECK((lines.Plan.Passes[pass].AccumulationSamples > 0) == (pass >= LINE_SAMPLING_FIRST_SHADE_PASS));
    }
    const StaticArray<uint32, LINE_SAMPLING_OUTPUT_COUNT> exports = GetLineSamplingExports(lines.Plan);
    constexpr usize FRAME_COUNT = 256;
    for (usize frame = 0; frame < FRAME_COUNT; ++frame)
    {
        RenderFrame(lines.Graph, fixture);
    }
    StaticArray<Array<uint8>, LINE_SAMPLING_OUTPUT_COUNT> pixels;
    for (usize output = 0; output < LINE_SAMPLING_OUTPUT_COUNT; ++output)
    {
        ReadExport(lines.Graph, fixture, exports[output], {}, pixels[output]);
        // Invalid hit records, exhausted budgets and failed line queries would show magenta.
        REQUIRE(CountFailurePixels(pixels[output]) == 0);
    }

    // The same room through the Veach and Guibas template's light-sampling output, with every pass
    // on the triangle loop. Both read the area point that seeds the chord from dimensions 2 to 5.
    PreparedGraph veach(
        fixture,
        {LUDUS_RG3_VEACH_GUIBAS_MIS_TRIANGLE_LOOP_DIRECTORY, "rg2_veach_guibas_mis_triangle_loop.lurgrt"},
        room,
        1);
    REQUIRE(renderer::RequiresHardwareRayTracing(veach.Plan) == false);
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> veachPixels;
    RenderVeachGuibasOutputs(veach, fixture, FRAME_COUNT, veachPixels);
    const math::Vector3 lineMean = ComputeMeanRadiance(pixels[LINE_SAMPLING_COLOR]);
    const math::Vector3 lightMean = ComputeMeanRadiance(veachPixels[VEACH_GUIBAS_LIGHT_SAMPLING]);
    INFO("line sampling mean=" << lineMean.X << ',' << lineMean.Y << ',' << lineMean.Z
                               << ", light sampling mean=" << lightMean.X << ',' << lightMean.Y << ',' << lightMean.Z);
    constexpr float32 MINIMUM_MEAN_RADIANCE = .01F;
    REQUIRE(lightMean.X > MINIMUM_MEAN_RADIANCE);
    // Estimates of one integral, compared after decoding the tone-mapped running means, at 256
    // samples per pixel as for the MIS template's same-integral check.
    constexpr float32 RELATIVE_MEAN_TOLERANCE = .02F;
    CheckMeansAgree(lineMean, lightMean, RELATIVE_MEAN_TOLERANCE);

    // The visible fraction is exact per sample: one where the light is unoccluded, zero in the umbra
    // of the block top. The floor point (0.6, -1, 0.3) sees the whole light past the block; every
    // segment from a floor point with x and z in [-0.58, -0.19] to the light crosses the block top.
    // With this camera, pixel (61, 54) and its neighbors are lit, and the umbra covers pixel row 50
    // from column 38 to 43, whose pixel footprints lie wholly inside it; rows 49 and 51 reach penumbra.
    constexpr usize LIT_RADIUS = 1;
    constexpr usize LIT_COLUMN = 61;
    constexpr usize LIT_ROW = 54;
    constexpr usize UMBRA_FIRST_COLUMN = 39;
    constexpr usize UMBRA_LAST_COLUMN = 42;
    constexpr usize UMBRA_ROW = 50;
    // ToneMap quantizes one to a byte of 188 of 255; decoding recovers it within this tolerance.
    constexpr float32 FRACTION_TOLERANCE = .02F;
    const float32 litFraction =
        ComputeRegionMean(pixels[LINE_SAMPLING_VISIBLE_FRACTION], LIT_COLUMN, LIT_ROW, LIT_RADIUS);
    float32 umbraFraction = 0;
    for (usize column = UMBRA_FIRST_COLUMN; column <= UMBRA_LAST_COLUMN; ++column)
    {
        umbraFraction =
            math::Max(umbraFraction,
                      DecodeToneMapped(pixels[LINE_SAMPLING_VISIBLE_FRACTION][GetPixel(column, UMBRA_ROW)]));
    }
    INFO("visible fraction: lit=" << litFraction << ", largest in umbra=" << umbraFraction);
    CHECK(math::Abs(litFraction - 1) < FRACTION_TOLERANCE);
    CHECK(umbraFraction == 0);
}

namespace
{
// Point-and-line MIS template tests (point-line spec PL2). Thanks to Katherine Salesin and Wojciech
// Jarosz, "Combining Point and Line Samples for Direct Illumination", Computer Graphics Forum 38(4)
// (EGSR 2019), pp. 159-169, https://doi.org/10.1111/cgf.13779 : a line sample is a point sample with
// a joint density, so BSDF samples and line samples in two directions combine by MIS (their section
// 4.2), and line samples lower penumbra noise. Thanks to Veach and Guibas (1995), above, for the
// multi-sample estimator that stays unbiased whenever the weights sum to one.
constexpr usize POINT_LINE_OUTPUT_COUNT = 3;
/// Index of the first shade pass; pass 0 records the primary hits.
constexpr usize POINT_LINE_FIRST_SHADE_PASS = 1;
constexpr usize POINT_LINE_MIS = 0;
constexpr usize POINT_LINE_LINE_SAMPLING = 1;
constexpr usize POINT_LINE_POINT_SAMPLING = 2;
/// The point_line_densities kernel's rows and its error scale (point_line_densities.slang).
constexpr usize POINT_LINE_DENSITY_COUNT = 8;
constexpr usize POINT_LINE_WEIGHT_ROW_COUNT = 8;
constexpr usize POINT_LINE_SINGLE_WEIGHT_ROW = 8;
constexpr usize POINT_LINE_DENSITY_FIRST_ROW = 16;
constexpr float64 POINT_LINE_ERROR_SCALE = 1000;
constexpr float64 POINT_LINE_SKIPPED = 1;

StaticArray<uint32, POINT_LINE_OUTPUT_COUNT> GetPointLineExports(const renderer::RenderGraphPlan& plan)
{
    REQUIRE(plan.Passes.GetSize() == POINT_LINE_FIRST_SHADE_PASS + POINT_LINE_OUTPUT_COUNT);
    StaticArray<uint32, POINT_LINE_OUTPUT_COUNT> exports{};
    for (usize output = 0; output < POINT_LINE_OUTPUT_COUNT; ++output)
    {
        exports[output] = plan.Passes[POINT_LINE_FIRST_SHADE_PASS + output].Color;
        REQUIRE(exports[output] < plan.Resources.GetSize());
        REQUIRE(plan.Resources[exports[output]].IsExported == 1);
    }
    REQUIRE(exports[POINT_LINE_MIS] != exports[POINT_LINE_LINE_SAMPLING]);
    REQUIRE(exports[POINT_LINE_MIS] != exports[POINT_LINE_POINT_SAMPLING]);
    REQUIRE(exports[POINT_LINE_LINE_SAMPLING] != exports[POINT_LINE_POINT_SAMPLING]);
    return exports;
}

/// Mean GPU time per frame of each timed physical pass, in the order the device reports them.
struct PassTimings final
{
    StaticArray<rhi::GpuScopeDescription, rhi::MAX_GPU_TIMED_PASSES> Scopes{};
    StaticArray<float64, rhi::MAX_GPU_TIMED_PASSES> Nanoseconds{};
    usize Count = 0;
    usize FrameCount = 0;
};

/// Render frameCount frames and read each output's running mean after the last one. With
/// timingsOrNull, arm GPU pass timing for every frame and add each available pass duration to it.
void RenderPointLineOutputs(PreparedGraph& inoutGraph,
                            const GraphGpuFixture& fixture,
                            usize frameCount,
                            StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT>& outPixels,
                            PassTimings* timingsOrNull = nullptr)
{
    const StaticArray<uint32, POINT_LINE_OUTPUT_COUNT> exports = GetPointLineExports(inoutGraph.Plan);
    for (usize frame = 0; frame < frameCount; ++frame)
    {
        if (timingsOrNull != nullptr)
        {
            REQUIRE(rhi::ArmGpuPassTimings(fixture.Device) == rhi::RasterStatus::Ready);
        }
        RenderFrame(inoutGraph.Graph, fixture);
        rhi::GpuPassTimings timings;
        if (timingsOrNull == nullptr || rhi::GetGpuPassTimings(fixture.Device, timings) != rhi::RasterStatus::Ready)
        {
            continue;
        }
        timingsOrNull->Count = math::Max(timingsOrNull->Count, timings.Count);
        timingsOrNull->FrameCount += 1;
        for (usize pass = 0; pass < timings.Count; ++pass)
        {
            timingsOrNull->Scopes[pass] = timings.Passes[pass].Scope;
            if (timings.Passes[pass].Timing.State == rhi::GpuTimingState::Available)
            {
                timingsOrNull->Nanoseconds[pass] +=
                    static_cast<float64>(timings.Passes[pass].Timing.DurationNanoseconds);
            }
        }
    }
    for (usize output = 0; output < POINT_LINE_OUTPUT_COUNT; ++output)
    {
        ReadExport(inoutGraph.Graph, fixture, exports[output], {}, outPixels[output]);
        // Invalid hit records, exhausted budgets and failed line queries would show magenta.
        REQUIRE(CountFailurePixels(outPixels[output]) == 0);
    }
}

/// Move the Cornell block's top far behind the camera, leaving no occluder between any room surface
/// and the light.
void RemoveCornellBlock(CornellBox& inoutRoom)
{
    constexpr float32 BEHIND_CAMERA_WORLD = 6;
    for (usize corner = CORNELL_VERTEX_COUNT - CORNELL_QUAD_CORNERS; corner < CORNELL_VERTEX_COUNT; ++corner)
    {
        inoutRoom.Positions[corner].Z += BEHIND_CAMERA_WORLD;
    }
}

/// Mean squared error of decoded linear radiance against a reference, over the masked pixels only.
float64
ComputeMaskedMeanSquaredError(const Array<uint8>& pixels, const Array<uint8>& reference, const Array<bool>& mask)
{
    REQUIRE(pixels.GetSize() == reference.GetSize());
    REQUIRE(mask.GetSize() * 4 == pixels.GetSize());
    float64 sum = 0;
    usize count = 0;
    for (usize pixel = 0; pixel < mask.GetSize(); ++pixel)
    {
        if (mask[pixel] == false)
        {
            continue;
        }
        for (usize channel = 0; channel < 3; ++channel)
        {
            const float64 difference = static_cast<float64>(DecodeToneMapped(pixels[pixel * 4 + channel])) -
                                       static_cast<float64>(DecodeToneMapped(reference[pixel * 4 + channel]));
            sum += difference * difference;
            ++count;
        }
    }
    REQUIRE(count > 0);
    return sum / static_cast<float64>(count);
}

/// The three outputs and the Veach and Guibas light-sampling image estimate one integral.
void CheckPointLineOutputsAgree(const GraphGpuFixture& fixture,
                                const PackageLocation& location,
                                const CornellBox& room,
                                StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT>& outPixels)
{
    PreparedGraph graph(fixture, location, room, 1);
    REQUIRE(renderer::RequiresHardwareRayTracing(graph.Plan) == false);
    constexpr usize FRAME_COUNT = 256;
    RenderPointLineOutputs(graph, fixture, FRAME_COUNT, outPixels);
    PreparedGraph veach(
        fixture,
        {LUDUS_RG3_VEACH_GUIBAS_MIS_TRIANGLE_LOOP_DIRECTORY, "rg2_veach_guibas_mis_triangle_loop.lurgrt"},
        room,
        1);
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> veachPixels;
    RenderVeachGuibasOutputs(veach, fixture, FRAME_COUNT, veachPixels);
    const math::Vector3 mis = ComputeMeanRadiance(outPixels[POINT_LINE_MIS]);
    const math::Vector3 lines = ComputeMeanRadiance(outPixels[POINT_LINE_LINE_SAMPLING]);
    const math::Vector3 points = ComputeMeanRadiance(outPixels[POINT_LINE_POINT_SAMPLING]);
    const math::Vector3 lightSampling = ComputeMeanRadiance(veachPixels[VEACH_GUIBAS_LIGHT_SAMPLING]);
    // MIS and point sampling share the BSDF samples and the area point that seeds the U chord, so the
    // images differ only through the line strategies. A difference in many pixels shows they contribute.
    constexpr uint8 CHANNEL_TOLERANCE = 2;
    const usize differingPixels =
        CountDifferingPixels(outPixels[POINT_LINE_MIS], outPixels[POINT_LINE_POINT_SAMPLING], CHANNEL_TOLERANCE);
    INFO("MIS mean=" << mis.X << ',' << mis.Y << ',' << mis.Z << ", line sampling mean=" << lines.X << ',' << lines.Y
                     << ',' << lines.Z << ", point sampling mean=" << points.X << ',' << points.Y << ',' << points.Z
                     << ", Veach and Guibas light sampling mean=" << lightSampling.X << ',' << lightSampling.Y << ','
                     << lightSampling.Z << ", differing pixels=" << differingPixels);
    constexpr usize DIFFERING_PIXEL_DIVISOR = 10;
    CHECK(differingPixels > static_cast<usize>(WIDTH) * HEIGHT / DIFFERING_PIXEL_DIVISOR);
    constexpr float32 MINIMUM_MEAN_RADIANCE = .01F;
    REQUIRE(points.X > MINIMUM_MEAN_RADIANCE);
    // Estimates of one integral, compared after decoding the tone-mapped running means, at 256
    // samples per pixel as for the MIS template's same-integral check.
    constexpr float32 RELATIVE_MEAN_TOLERANCE = .02F;
    CheckMeansAgree(mis, points, RELATIVE_MEAN_TOLERANCE);
    CheckMeansAgree(lines, points, RELATIVE_MEAN_TOLERANCE);
    CheckMeansAgree(points, lightSampling, RELATIVE_MEAN_TOLERANCE);
}
} // namespace

TEST_CASE("Point-and-line MIS weights sum to one and an unoccluded line density equals the area density",
          "[render-graph][gpu][point-line][mis]")
{
    GraphGpuFixture fixture(false, false);
    PackageLocation location{LUDUS_RG3_POINT_LINE_DENSITIES_BALANCE_DIRECTORY,
                             "rg2_point_line_densities_balance.lurgrt"};
    SECTION("balance") {}
    SECTION("power")
    {
        location = {LUDUS_RG3_POINT_LINE_DENSITIES_POWER_DIRECTORY, "rg2_point_line_densities_power.lurgrt"};
    }
    CornellBox room;
    RemoveCornellBlock(room);
    PreparedGraph graph(fixture, location, room, 1);
    REQUIRE(renderer::RequiresHardwareRayTracing(graph.Plan) == false);
    Array<uint8> pixels;
    RenderExport(graph.Graph, fixture, graph.GetOnlyExport(), {}, pixels);
    REQUIRE(CountFailurePixels(pixels) == 0);

    // Over every triple of densities from zero through 1e-20 to 1e12, including equal ones, the three
    // strategies' weights sum to one, or to zero when no strategy has a positive density.
    // Float rounding of three quotients (and of pow under the power heuristic) stays below this.
    constexpr float64 WEIGHT_SUM_TOLERANCE = 2e-6;
    float64 largestSumError = 0;
    for (usize row = 0; row < POINT_LINE_WEIGHT_ROW_COUNT; ++row)
    {
        for (usize column = 0; column < POINT_LINE_DENSITY_COUNT * POINT_LINE_DENSITY_COUNT; ++column)
        {
            largestSumError = math::Max(largestSumError, DecodeLineFixed(pixels, column, row) / POINT_LINE_ERROR_SCALE);
        }
    }
    // Delta light (both line strategies propose it with one probability, BSDF sampling never), the
    // only strategy, a strategy that did not propose the sample, and three equal densities.
    constexpr usize SINGLE_WEIGHT_COUNT = 4;
    constexpr StaticArray<float64, SINGLE_WEIGHT_COUNT> SINGLE_WEIGHTS{{.5, 1, 0, 1.0 / 3.0}};
    constexpr float64 WEIGHT_TOLERANCE = 1e-6;
    float64 largestWeightError = 0;
    for (usize column = 0; column < SINGLE_WEIGHTS.GetSize(); ++column)
    {
        largestWeightError = math::Max(
            largestWeightError,
            math::Abs(DecodeLineFixed(pixels, column, POINT_LINE_SINGLE_WEIGHT_ROW) - SINGLE_WEIGHTS[column]));
    }

    // With no occluder, the chord through any light point is wholly visible, so both line strategies'
    // densities at that point equal EvaluateLightPdfSolidAngle (design D6 step 6).
    constexpr float64 DENSITY_RELATIVE_TOLERANCE = 1e-5;
    constexpr usize LINE_DIRECTION_COUNT = 2;
    StaticArray<usize, LINE_DIRECTION_COUNT> evaluatedPixels{};
    StaticArray<float64, LINE_DIRECTION_COUNT> largestDensityError{};
    for (usize row = POINT_LINE_DENSITY_FIRST_ROW; row < HEIGHT; ++row)
    {
        for (usize column = 0; column < WIDTH; ++column)
        {
            const float64 value = DecodeLineFixed(pixels, column, row);
            if (value == POINT_LINE_SKIPPED)
            {
                continue;
            }
            const usize direction = column % LINE_DIRECTION_COUNT;
            evaluatedPixels[direction] += 1;
            largestDensityError[direction] = math::Max(largestDensityError[direction], value / POINT_LINE_ERROR_SCALE);
        }
    }
    INFO("largest weight-sum error=" << largestSumError << ", largest single-weight error=" << largestWeightError
                                     << ", U density pixels=" << evaluatedPixels[0] << " largest relative error="
                                     << largestDensityError[0] << ", V density pixels=" << evaluatedPixels[1]
                                     << " largest relative error=" << largestDensityError[1]);
    CHECK(largestSumError <= WEIGHT_SUM_TOLERANCE);
    CHECK(largestWeightError <= WEIGHT_TOLERANCE);
    // Most of the image is room surface; the light and the open front are skipped.
    const usize minimumEvaluated = static_cast<usize>(WIDTH) * (HEIGHT - POINT_LINE_DENSITY_FIRST_ROW) / 4;
    CHECK(evaluatedPixels[0] > minimumEvaluated);
    CHECK(evaluatedPixels[1] > minimumEvaluated);
    CHECK(largestDensityError[0] <= DENSITY_RELATIVE_TOLERANCE);
    CHECK(largestDensityError[1] <= DENSITY_RELATIVE_TOLERANCE);
}

TEST_CASE("The point-and-line MIS template's three outputs estimate one integral under both heuristics",
          "[render-graph][gpu][point-line][template]")
{
    GraphGpuFixture fixture(false, false);
    PackageLocation location{LUDUS_RG3_SALESIN_JAROSZ_LINES_TEMPLATE_DIRECTORY,
                             "rg2_salesin_jarosz_lines_template.lurgrt"};
    // The Cornell room: the block top casts a penumbra from the two-triangle quad light.
    CornellBox room;
    SECTION("balance (default)")
    {
        PreparedGraph graph(fixture, location, room, 1);
        CheckSameStage(
            graph.Programs.RayCompute,
            packages::rg2_salesin_jarosz_lines_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                      shaders::renderer_composite::Fragment())
                .RayCompute);
        REQUIRE(graph.Programs.HasPersistentRayBuffers == true);
        // Every pass is a triangle loop: a compute ray-tracing package with no native ray requirement.
        REQUIRE(renderer::RequiresHardwareRayTracing(graph.Plan) == false);
        for (usize pass = 0; pass < graph.Plan.Passes.GetSize(); ++pass)
        {
            INFO("pass " << pass);
            CHECK(graph.Plan.Passes[pass].Program == format::Technique::RayTracing);
            CHECK((graph.Plan.Passes[pass].AccumulationSamples > 0) == (pass >= POINT_LINE_FIRST_SHADE_PASS));
        }
    }
    SECTION("power")
    {
        location = {LUDUS_RG3_SALESIN_JAROSZ_LINES_POWER_DIRECTORY, "rg2_salesin_jarosz_lines_power.lurgrt"};
    }
    StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT> pixels;
    CheckPointLineOutputsAgree(fixture, location, room, pixels);
}

TEST_CASE("An analytic point light and a constant environment take the point fallback in every point-and-line output",
          "[render-graph][gpu][point-line][template]")
{
    // An analytic point light and a constant environment through the open front, with the ceiling
    // light's emission removed: each line strategy proposes them with the point proposal (design D6).
    GraphGpuFixture fixture(false, false);
    PackageLocation location{LUDUS_RG3_SALESIN_JAROSZ_LINES_TEMPLATE_DIRECTORY,
                             "rg2_salesin_jarosz_lines_template.lurgrt"};
    SECTION("balance (default)") {}
    SECTION("power")
    {
        location = {LUDUS_RG3_SALESIN_JAROSZ_LINES_POWER_DIRECTORY, "rg2_salesin_jarosz_lines_power.lurgrt"};
    }
    CornellBox room;
    room.Materials[CORNELL_LIGHT].Emission = {0, 0, 0};
    StaticArray<renderer::DirectLight, 1> lights{};
    lights[0].SourceId = 1;
    lights[0].Kind = renderer::LightKind::Point;
    lights[0].Position = {0, .6F, .2F};
    constexpr float32 POINT_INTENSITY = 2;
    lights[0].Intensity = POINT_INTENSITY;
    room.Inputs.Lights = {lights.GetData(), lights.GetSize()};
    constexpr float32 SKY_RADIANCE = .5F;
    room.Inputs.Environment = {SKY_RADIANCE, SKY_RADIANCE, SKY_RADIANCE};
    StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT> pixels;
    CheckPointLineOutputsAgree(fixture, location, room, pixels);
}

TEST_CASE("After 16 samples per pixel point-and-line MIS has less penumbra error than point sampling",
          "[render-graph][gpu][point-line][template][mis]")
{
    GraphGpuFixture fixture(true, false);
    CornellBox room;
    // The penumbra: pixels whose mean visible chord fraction, from the line-sampling template, lies
    // strictly between umbra (0) and full light (1).
    PreparedGraph fraction(fixture,
                           {LUDUS_RG3_LINE_SAMPLING_TEMPLATE_DIRECTORY, "rg2_line_sampling_template.lurgrt"},
                           room,
                           1);
    constexpr usize FRACTION_FRAME_COUNT = 256;
    for (usize frame = 0; frame < FRACTION_FRAME_COUNT; ++frame)
    {
        RenderFrame(fraction.Graph, fixture);
    }
    const StaticArray<uint32, LINE_SAMPLING_OUTPUT_COUNT> fractionExports = GetLineSamplingExports(fraction.Plan);
    Array<uint8> fractionPixels;
    ReadExport(fraction.Graph, fixture, fractionExports[LINE_SAMPLING_VISIBLE_FRACTION], {}, fractionPixels);
    Array<bool> penumbra;
    REQUIRE(penumbra.TryResize(fractionPixels.GetSize() / 4) == true);
    constexpr float32 PENUMBRA_LOWEST_FRACTION = .1F;
    constexpr float32 PENUMBRA_HIGHEST_FRACTION = .9F;
    usize penumbraPixels = 0;
    for (usize pixel = 0; pixel < penumbra.GetSize(); ++pixel)
    {
        const float32 visible = DecodeToneMapped(fractionPixels[pixel * 4]);
        penumbra[pixel] = visible > PENUMBRA_LOWEST_FRACTION && visible < PENUMBRA_HIGHEST_FRACTION;
        penumbraPixels += penumbra[pixel] == true ? 1 : 0;
    }
    constexpr usize MINIMUM_PENUMBRA_PIXELS = 32;
    REQUIRE(penumbraPixels >= MINIMUM_PENUMBRA_PIXELS);

    const PackageLocation location{LUDUS_RG3_SALESIN_JAROSZ_LINES_TEMPLATE_DIRECTORY,
                                   "rg2_salesin_jarosz_lines_template.lurgrt"};
    PreparedGraph graph(fixture, location, room, 1);
    constexpr usize LOW_FRAME_COUNT = 16;
    StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT> pixels;
    PassTimings timings;
    RenderPointLineOutputs(graph, fixture, LOW_FRAME_COUNT, pixels, &timings);
    // An independently seeded reference: another graph instance whose scene revision reseeds sampling.
    constexpr uint64 REFERENCE_REVISION = 2;
    PreparedGraph referenceGraph(fixture, location, room, REFERENCE_REVISION);
    constexpr usize REFERENCE_FRAME_COUNT = 1024;
    StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT> reference;
    RenderPointLineOutputs(referenceGraph, fixture, REFERENCE_FRAME_COUNT, reference);
    const float64 misError = ComputeMaskedMeanSquaredError(pixels[POINT_LINE_MIS], reference[POINT_LINE_MIS], penumbra);
    const float64 lineError =
        ComputeMaskedMeanSquaredError(pixels[POINT_LINE_LINE_SAMPLING], reference[POINT_LINE_MIS], penumbra);
    const float64 pointError =
        ComputeMaskedMeanSquaredError(pixels[POINT_LINE_POINT_SAMPLING], reference[POINT_LINE_MIS], penumbra);
    // Equal samples, not equal time: line samples cost line queries, so the GPU time of each pass is
    // reported beside the errors and supports no speed claim.
    constexpr float64 NANOSECONDS_PER_MILLISECOND = 1'000'000;
    REQUIRE(timings.FrameCount > 0);
    StaticArray<float64, POINT_LINE_FIRST_SHADE_PASS + POINT_LINE_OUTPUT_COUNT> passMilliseconds{};
    for (usize pass = 0; pass < passMilliseconds.GetSize() && pass < timings.Count; ++pass)
    {
        passMilliseconds[pass] =
            timings.Nanoseconds[pass] / static_cast<float64>(timings.FrameCount) / NANOSECONDS_PER_MILLISECOND;
    }
    WARN("point-and-line penumbra (" << penumbraPixels << " pixels) mean squared error at " << LOW_FRAME_COUNT
                                     << " samples: MIS=" << misError << ", line sampling=" << lineError
                                     << ", point sampling=" << pointError << "; mean GPU ms per frame over "
                                     << timings.FrameCount << " frames, " << timings.Count << " timed passes: "
                                     << timings.Scopes[0].Name.GetData() << '=' << passMilliseconds[0] << ", "
                                     << timings.Scopes[1].Name.GetData() << '=' << passMilliseconds[1] << ", "
                                     << timings.Scopes[2].Name.GetData() << '=' << passMilliseconds[2] << ", "
                                     << timings.Scopes[3].Name.GetData() << '=' << passMilliseconds[3]);
    CHECK(misError < pointError);
}

TEST_CASE("ToneMap applies exposure after linear accumulation and can be bypassed",
          "[render-graph][gpu][accumulation][tone-mapping]")
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
    float32 exposure = 1;
    SECTION("unit exposure") {}
    SECTION("bypass")
    {
        exposure = 0;
    }
    SECTION("double exposure")
    {
        exposure = 2;
    }
    passes[plan.Passes.GetSize() - 1].AccumulationSamples = SAMPLE_LIMIT;
    passes[plan.Passes.GetSize() - 1].ToneMappingExposure = exposure;
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
    const uint8 firstExpected = exposure == 0 ? 255 : exposure == 1 ? 188 : 213;
    const uint8 meanExpected = exposure == 0 ? 255 : exposure == 1 ? 203 : 225;
    CHECK(fixture.Pixels[center] == firstExpected); // Display conversion of linear sample 1.
    CHECK(fixture.Graph.HasPendingSamples() == true);
    ReadGraphFrame(fixture, output);
    CHECK(fixture.Pixels[center] == meanExpected); // Exposure scales mean(1, 2) = 1.5, before encoding.
    CHECK(fixture.Pixels[center + 1] == 0);
    CHECK(fixture.Graph.HasPendingSamples() == false);
    ReadGraphFrame(fixture, output);
    CHECK(fixture.Pixels[center] == meanExpected);
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

TEST_CASE("OIDN presents stable accumulation after sampling stops", "[render-graph][gpu][accumulation][denoising]")
{
    if (renderer::internal::IsGpuDenoisingAvailable() == false)
    {
        SKIP("This build has no native OIDN adapter");
    }
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
    float32 exposure = 1;
    SECTION("unit exposure") {}
    SECTION("bypass")
    {
        exposure = 0;
    }
    SECTION("double exposure")
    {
        exposure = 2;
    }
    passes[plan.Passes.GetSize() - 1].AccumulationSamples = SAMPLE_LIMIT;
    passes[plan.Passes.GetSize() - 1].ToneMappingExposure = exposure;
    passes[plan.Passes.GetSize() - 1].DenoisingQuality = static_cast<uint32>(format::ProcessingQuality::Balanced);
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
    const uint8 firstFiltered = fixture.Pixels[center];
    CHECK(firstFiltered > 0);
    CHECK(fixture.Pixels[center + 1] <= 2);
    CHECK(fixture.Pixels[center + 2] <= 2);
    CHECK(fixture.Graph.HasPendingSamples() == true);
    ReadGraphFrame(fixture, output);
    const uint8 meanFiltered = fixture.Pixels[center];
    CHECK(meanFiltered > 0);
    CHECK(fixture.Pixels[center + 1] <= 2);
    CHECK(fixture.Graph.HasPendingSamples() == false);
    ReadGraphFrame(fixture, output);
    // The filter may change radiance. Repeated presentation must still start from the raw mean,
    // rather than feed its previous output back into accumulation or denoising.
    CHECK(fixture.Pixels[center] == meanFiltered);
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

TEST_CASE("Builds without OIDN reject denoising at setup, not at scene preparation", "[render-graph][gpu][denoising]")
{
    if (renderer::IsDenoisingAvailable() == true)
    {
        SKIP("This build has a native OIDN adapter");
    }
    GraphGpuFixture fixture;
    const renderer::RenderGraphPrograms programs =
        packages::rg2_scene_ray::MakePrograms(shaders::renderer_composite::Vertex(),
                                              shaders::renderer_composite::Fragment());
    renderer::RenderGraphPlan plan = packages::rg2_scene_ray::MakePlan();
    format::Pass operation = plan.Passes[0];
    plan.Passes = {&operation, 1};
    CHECK(renderer::RequiresDenoising(plan) == false);
    operation.DenoisingQuality = static_cast<uint32>(format::ProcessingQuality::Balanced);
    CHECK(renderer::RequiresDenoising(plan) == true);
    CHECK(fixture.Graph.Initialize(fixture.Device, plan, programs) == rhi::RasterStatus::Unsupported);
    // The rejected setup leaves the graph unstarted, so the same plan without denoising still starts.
    operation.DenoisingQuality = 0;
    rhi::RasterStatus status = fixture.Graph.Initialize(fixture.Device, plan, programs);
    const uint64 start = time::NowTicks();
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - start < 30 * time::NANOSECONDS_PER_SECOND)
    {
        status = fixture.Graph.Poll();
    }
    CHECK(status == rhi::RasterStatus::Ready);
}

TEST_CASE("OIDN preserves scene material metadata and accepts material replacements", "[render-graph][gpu][denoising]")
{
    if (renderer::internal::IsGpuDenoisingAvailable() == false)
    {
        SKIP("This build has no native OIDN adapter");
    }
    GraphGpuFixture fixture;
    const renderer::RenderGraphPrograms programs =
        packages::rg2_scene_ray::MakePrograms(shaders::renderer_composite::Vertex(),
                                              shaders::renderer_composite::Fragment());
    renderer::RenderGraphPlan plan = packages::rg2_scene_ray::MakePlan();
    format::Pass operation = plan.Passes[0];
    operation.DenoisingQuality = static_cast<uint32>(format::ProcessingQuality::Balanced);
    operation.ToneMappingExposure = 1;
    operation.AccumulationSamples = 2;
    plan.Passes = {&operation, 1};
    const auto finish = [&fixture](rhi::RasterStatus status) {
        const uint64 start = time::NowTicks();
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - start < 30 * time::NANOSECONDS_PER_SECOND)
        {
            status = fixture.Graph.Poll();
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
    };
    finish(fixture.Graph.Initialize(fixture.Device, plan, programs));
    fixture.Cameras[0].OrthographicCamera = true;
    uint32 binding = 0;
    fixture.Meshes[0].MaterialBindings = {&binding, 1};
    renderer::SceneShaderMaterial material;
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    for (uint64 revision = 1; revision <= 2; ++revision)
    {
        material.BaseColor = revision == 1 ? math::Vector3{1, 0, 0} : math::Vector3{0, 1, 0};
        renderer::RenderGraphReport report;
        finish(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                    {fixture.Items.GetData(), fixture.Items.GetSize()},
                                    {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                    revision,
                                    renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                    report,
                                    &inputs,
                                    revision));
        ReadGraphFrame(fixture, operation.Color);
        const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
        const usize selectedChannel = revision == 1 ? 0 : 1;
        // OIDN may alter intensity; material replacement must preserve the selected color,
        // and must never turn a valid hit into the magenta invalid-scene diagnostic.
        CHECK(fixture.Pixels[center + selectedChannel] > 0);
        CHECK(fixture.Pixels[center + 1 - selectedChannel] <= 2);
        CHECK(fixture.Pixels[center + 2] <= 2);
    }
}
