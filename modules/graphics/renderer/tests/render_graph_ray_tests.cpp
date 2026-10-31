#include "../shaders/renderer_gpu_preview_layout.h"
#include "../src/internal/gpu_denoiser.h"
#include "../src/internal/ltc_table.h"
#include "../src/internal/render_graph_ray_scene.h"

#include "ltc_lookup_cases.h"
#include "renderer_composite.h"
#include "rg2_compute_mixed.h"
#include "rg2_compute_ray_tracing_template.h"
#include "rg2_compute_split.h"
#include "rg2_direct_lighting_reference.h"
#include "rg2_line_sampling_template.h"
#include "rg2_line_visibility_loop.h"
#include "rg2_ltc_table.h"
#include "rg2_ordered_ray.h"
#include "rg2_polygonal_light_sampling_template.h"
#include "rg2_raytracing.h"
#include "rg2_restir_di.h"
#include "rg2_ris_projected_solid_angle_template.h"
#include "rg2_salesin_jarosz_lines_template.h"
#include "rg2_scene_compute.h"
#include "rg2_scene_ray.h"
#include "rg2_shared_ray_computebvh.h"
#include "ris_projected_solid_angle_benchmark_variants.h"
#if defined(__APPLE__)
#    include "rg2_mixed_invocation.h"
#    include "rg2_native_ray.h"
#    include "rg2_native_visibility.h"
#endif
#include "rg2_shared_ray_default.h"
#include "rg2_simple_compute_ray_tracing_template.h"
#include "rg2_slice_sampling_luminaire_template.h"
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
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/render_graph_capture.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>
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
    /// @param resourceBytes Device-wide logical RHI resource budget; zero keeps rhi::StorageLimits' default.
    explicit GraphGpuFixture(bool shouldEnableGpuTiming = false,
                             bool shouldRequireNativeRays = true,
                             uint64 resourceBytes = 0)
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Storage.ResourcesPerGraph = renderer::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
        if (resourceBytes != 0)
        {
            description.Storage.ResourceBytes = resourceBytes;
        }
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
/// Read a cooked package file whole.
Array<uint8> ReadPackageBytes(const PackageLocation& location)
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
    return bytes;
}
core::UniquePtr<renderer::RenderGraphPackage> LoadPackage(const PackageLocation& location)
{
    const Array<uint8> bytes = ReadPackageBytes(location);
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

TEST_CASE("Shared alpha masks agree between CPU queries and GPU graph traversal", "[render-graph][gpu][scene-mask]")
{
    bool shouldUseNativeQueries = false;
    SECTION("compute BVH") {}
    SECTION("native queries")
    {
        shouldUseNativeQueries = true;
    }
    GraphGpuFixture fixture(false, shouldUseNativeQueries);
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(
        shouldUseNativeQueries == true
            ? PackageLocation{LUDUS_RG3_SCENE_RAY_DIRECTORY, "rg2_scene_ray.lurgrt"}
            : PackageLocation{LUDUS_RG3_SHARED_RAY_COMPUTEBVH_DIRECTORY, "rg2_shared_ray_computebvh.lurgrt"});
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const auto settle = [&fixture](rhi::RasterStatus status) {
        const uint64 start = time::NowTicks();
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - start < 10 * time::NANOSECONDS_PER_SECOND)
        {
            status = fixture.Graph.Poll();
        }
        return status;
    };
    REQUIRE(settle(fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs)) == rhi::RasterStatus::Ready);
    fixture.Cameras[0].OrthographicCamera = true;
    constexpr usize TEXEL_BYTES = 4;
    constexpr usize CPU_SCENE_BYTES = 1024 * 1024;
    StaticArray<uint8, TEXEL_BYTES> texel{{255, 255, 255, 0}};
    StaticArray<math::Vector4, VERTEX_COUNT> colors{{{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}}};
    fixture.Meshes[0].Geometry.Colors = {colors.GetData(), colors.GetSize()};
    const uint32 binding = 0;
    fixture.Meshes[0].MaterialBindings = {&binding, 1};
    renderer::SceneShaderMaterial material;
    material.BaseColor = {1, 0, 0};
    material.IsAlphaMasked = true;
    material.AlphaCutoff = .5F;
    material.ColorTexture = {texel.GetData(), texel.GetSize(), texel.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Srgb};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    inputs.Environment = {0, 0, 1};
    uint64 revision = 0;
    for (const uint8 alpha : {uint8{0}, uint8{64}, uint8{128}, uint8{255}})
    {
        for (const float32 vertexAlpha : {1.0F, .25F})
        {
            texel[3] = alpha;
            for (math::Vector4& color : colors)
            {
                color.W = vertexAlpha;
            }
            const bool hasCoverage = static_cast<float32>(alpha) / 255 * vertexAlpha >= material.AlphaCutoff;
            renderer::RenderGraphSceneDescription scene;
            scene.Meshes = {fixture.Meshes.GetData(), fixture.Meshes.GetSize()};
            scene.Items = {fixture.Items.GetData(), fixture.Items.GetSize()};
            scene.Inputs = inputs;
            scene.HasMaterialInterface = true;
            renderer::ReferenceCaptureDescription camera;
            camera.OrthographicCamera = true;
            core::UniquePtr<renderer::internal::ReferenceJob> oracle;
            REQUIRE(renderer::internal::BuildMaterialGraphRayScene(scene, camera, CPU_SCENE_BYTES, oracle) ==
                    rhi::RasterStatus::Ready);
            rhi::RayInput ray;
            ray.Origin[2] = 1;
            ray.MaxDistance = 1;
            for (const uint32 visibility : {uint32{0}, uint32{1}})
            {
                ray.VisibilityOnly = visibility;
                rhi::RayResult hit;
                REQUIRE(renderer::internal::TraceReferenceOracle(oracle->Scene, ray, hit) == rhi::RasterStatus::Ready);
                CHECK((hit.Hit != 0) == hasCoverage);
            }
            renderer::RenderGraphReport report;
            REQUIRE(settle(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                                {fixture.Items.GetData(), fixture.Items.GetSize()},
                                                {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                                ++revision,
                                                renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                report,
                                                &inputs)) == rhi::RasterStatus::Ready);
            ReadGraphFrame(fixture);
            const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
            CAPTURE(alpha, vertexAlpha, shouldUseNativeQueries);
            CHECK(fixture.Pixels[center] == (hasCoverage == true ? 188 : 0));
            CHECK(fixture.Pixels[center + 1] == 0);
            CHECK(fixture.Pixels[center + 2] == (hasCoverage == true ? 0 : 188));
        }
    }
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
    // Version 25 appends the scene resource word between the storage records and the pass names.
    const usize nameBytes = package.GetPlan().PassNames[0].GetSize();
    const usize paddedNameBytes = ((nameBytes + WORD_BYTES - 1) / WORD_BYTES) * WORD_BYTES;
    const usize sceneResourceOffset = bytes.GetSize() - paddedNameBytes - 2 * WORD_BYTES;
    bytes.RemoveRange(sceneResourceOffset, sceneResourceOffset + WORD_BYTES);
    REQUIRE(TryWriteLittleEndian(format::STORAGE_PACKAGE_VERSION, {bytes.GetData() + sizeof(uint32), sizeof(uint32)}) ==
            true);
    renderer::RenderGraphPackage storageGeneration;
    REQUIRE(storageGeneration.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
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

TEST_CASE("Compute BVH hits on the edge opposite a triangle's first corner reload as valid surface points",
          "[render-graph][gpu][ray-traversal]")
{
    GraphGpuFixture fixture(false, false);
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_BVH_EDGE_BARYCENTRICS_DIRECTORY, "rg2_bvh_edge_barycentrics.lurgrt"});
    REQUIRE(package->GetPlan().Passes[0].Traversal == format::RayTraversal::ComputeBvh);
    // Both triangles name the diagonal (1, 0, 0)-(0, 1, 0) as their second and third corners, so its hits have a zero
    // first barycentric and the other two must sum to one. Irregular first corners keep the watertight test's
    // determinant off powers of two; correctly rounded float32 quotients then exceed one for about 0.7% of the rays.
    constexpr usize EDGE_QUAD_CORNER_COUNT = 4;
    constexpr usize EDGE_QUAD_INDEX_COUNT = 6;
    constexpr usize EDGE_QUAD_TRIANGLE_COUNT = 2;
    StaticArray<math::Vector3, EDGE_QUAD_CORNER_COUNT> positions{
        {{-.15F, -.4F, 0}, {1, 0, 0}, {1.3F, .9F, 0}, {0, 1, 0}}};
    StaticArray<uint32, EDGE_QUAD_INDEX_COUNT> indices{{0, 1, 3, 2, 3, 1}};
    CornellBox room;
    room.Meshes[0].Geometry = {positions.GetData(), positions.GetSize(), indices.GetData(), indices.GetSize()};
    room.Meshes[0].MaterialSlots = {room.Slots.GetData(), EDGE_QUAD_TRIANGLE_COUNT};
    const uint32 output = PrepareRoomGraph(fixture.Graph, fixture, *package, room, MakeTraversalItems(room), 1);
    Array<uint8> pixels;
    RenderExport(fixture.Graph, fixture, output, {}, pixels);
    REQUIRE(CountFailurePixels(pixels) == 0);
    usize misses = 0;
    usize rejectedHits = 0;
    for (usize address = 0; address + 3 < pixels.GetSize(); address += 4)
    {
        misses += pixels[address] == 0 ? 1 : 0;
        rejectedHits += pixels[address] != 0 && pixels[address + 1] == 0 ? 1 : 0;
    }
    CHECK(misses == 0);
    CHECK(rejectedHits == 0);
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
    ExportExtent Extent;

    /// @param room A scene fixture exposing Meshes, Items, Cameras and Inputs, such as CornellBox.
    /// @param revision Scene and camera revision; a different revision reseeds sampling.
    /// @param extent The render size of the graph's view-relative resources; the surface keeps the fixture's size.
    template <typename Room>
    PreparedGraph(const GraphGpuFixture& fixture,
                  const PackageLocation& location,
                  const Room& room,
                  uint64 revision,
                  ExportExtent extent = {})
        : Package(LoadPackage(location)), Plan(Package->GetPlan()),
          Programs(
              Package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment())),
          Extent(extent)
    {
        wait(Graph.Initialize(fixture.Device, Plan, Programs, nullptr, Extent.Width, Extent.Height));
        renderer::RenderGraphReport report;
        const rhi::RasterStatus updated = Graph.Update({room.Meshes.GetData(), room.Meshes.GetSize()},
                                                       {room.Items.GetData(), room.Items.GetSize()},
                                                       {room.Cameras.GetData(), room.Cameras.GetSize()},
                                                       revision,
                                                       renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                       report,
                                                       &room.Inputs,
                                                       revision);
        // A rejected scene names the byte ceiling it exceeded (renderer::SceneMemoryBudget).
        INFO("scene budget " << static_cast<uint32>(report.SceneDiagnostics.ExceededBudget) << " required "
                             << report.SceneDiagnostics.RequiredBytes << " available "
                             << report.SceneDiagnostics.AvailableBytes);
        wait(updated);
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
        ReadExport(inoutTemplate.Graph, fixture, exports[output], inoutTemplate.Extent, outPixels[output]);
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

/// Mean squared error of the tone-mapped RGB values in [0, 1] against a converged image. Unlike
/// ComputeMeanSquaredError it stays bounded where a pixel sees a bright emitter directly: there, inverting the tone map
/// turns one byte of antialiasing difference into a large radiance error that would dominate any comparison.
float64 ComputeDisplayMeanSquaredError(const Array<uint8>& pixels, const Array<uint8>& converged)
{
    REQUIRE(pixels.GetSize() == converged.GetSize());
    constexpr float64 MAXIMUM_CHANNEL = 255;
    float64 sum = 0;
    for (usize address = 0; address + 3 < pixels.GetSize(); address += 4)
    {
        for (usize channel = 0; channel < 3; ++channel)
        {
            const float64 difference =
                (static_cast<float64>(pixels[address + channel]) - static_cast<float64>(converged[address + channel])) /
                MAXIMUM_CHANNEL;
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
// steinberg-yan-plt template tests. Thanks to Shlomi Steinberg and Ling-Qi Yan, "A Generic Framework for Physical
// Light Transport", ACM Transactions on Graphics 40(4), article 139, SIGGRAPH 2021,
// https://doi.org/10.1145/3450626.3459791 : the template renders section 8.1's diffraction of natural light
// (Definition 5.1) by a surface. These tests check that a small source's light is dispersed into a spectrum that
// geometric optics does not show, that a larger and so less coherent source blurs it (as in the paper's Fig. 8),
// that a flat surface looks the same in wave and geometric optics, and the coherence radius of sunlight
// (section 5.1).

/// Shade passes of the template in output order: wave optics (the color output), geometric optics, coherence.
constexpr usize STEINBERG_YAN_OUTPUT_COUNT = 3;
/// Index of the first shade pass; pass 0 records the primary hits.
constexpr usize STEINBERG_YAN_FIRST_SHADE_PASS = 1;
constexpr usize STEINBERG_YAN_WAVE = 0;
constexpr usize STEINBERG_YAN_GEOMETRIC = 1;
constexpr usize STEINBERG_YAN_COHERENCE = 2;
constexpr usize GRATING_ROOM_MATERIAL_COUNT = CORNELL_MATERIAL_COUNT + 1;
/// White metal, whose Fresnel reflectance is one at every angle, at the smallest admitted roughness.
constexpr uint32 GRATING_ROOM_GRATING = CORNELL_MATERIAL_COUNT;
constexpr float32 GRATING_ROOM_ROUGHNESS = .045F;
/// The back wall's quad in CORNELL_CORNERS_WORLD. It faces the camera at z = -1.
constexpr usize GRATING_ROOM_BACK_WALL_QUAD = 2;
/// PLT_POINT_SOURCE_RADIUS_MILLIMETERS of the template's small_source (also flat) and large_source variants. At 96 x 64
/// pixels the default 5 mm source's mirror image covers about one percent of a pixel, which few jittered samples hit.
constexpr float32 GRATING_ROOM_SMALL_SOURCE_MILLIMETERS = 20;
constexpr float32 GRATING_ROOM_LARGE_SOURCE_MILLIMETERS = 200;
/// Point intensity for the 20 mm source, in candela. An order sees a source over a wavelength band proportional to
/// its radius, at a peak radiance proportional to intensity over radius squared, so a pixel's spectrum scales as
/// intensity over radius: the large source's intensity is scaled by the radius ratio to keep it equally bright.
constexpr float32 GRATING_ROOM_SMALL_SOURCE_INTENSITY = 2;
/// Columns around x = 0 on the back wall, where the point light's spectrum lies: the light and the camera are
/// both at x = 0.
constexpr usize GRATING_ROOM_CENTER_FIRST_COLUMN = WIDTH / 2 - 2;
constexpr usize GRATING_ROOM_CENTER_LAST_COLUMN = WIDTH / 2 + 1;
/// Back-wall rows of the first order's spectrum, below the mirror highlight (row 20) and above the second order.
/// With the default 740 nm period, violet reaches the camera from y = 0.2 (row 29) and red from y = -0.55 (row 41).
constexpr usize GRATING_ROOM_SPECTRUM_FIRST_ROW = 24;
constexpr usize GRATING_ROOM_SPECTRUM_LAST_ROW = 43;
/// Smallest color byte of a pixel included in saturation measurements.
constexpr uint8 GRATING_ROOM_BRIGHT_CHANNEL = 64;

/// The Cornell room with its back wall replaced by the grating and at most one analytic light. The mesh has no
/// tangents, so the grooves follow world +X and the grating vector is +Y: the spectrum spreads down the back wall.
struct GratingRoom final
{
    CornellBox Room;
    StaticArray<uint32, GRATING_ROOM_MATERIAL_COUNT> Bindings{
        {CORNELL_WHITE, CORNELL_RED, CORNELL_GREEN, CORNELL_LIGHT, GRATING_ROOM_GRATING}};
    StaticArray<renderer::SceneShaderMaterial, GRATING_ROOM_MATERIAL_COUNT> Materials{};
    StaticArray<renderer::DirectLight, 1> Lights{};

    /// @param ceilingEmission Emission of the ceiling light; zero removes it from the light set.
    explicit GratingRoom(float32 ceilingEmission)
    {
        for (usize material = 0; material < CORNELL_MATERIAL_COUNT; ++material)
        {
            Materials[material] = Room.Materials[material];
        }
        Materials[CORNELL_LIGHT].Emission = {ceilingEmission, ceilingEmission, ceilingEmission};
        Materials[GRATING_ROOM_GRATING].BaseColor = {1, 1, 1};
        Materials[GRATING_ROOM_GRATING].Metallic = 1;
        Materials[GRATING_ROOM_GRATING].Roughness = GRATING_ROOM_ROUGHNESS;
        Room.Slots[GRATING_ROOM_BACK_WALL_QUAD * 2] = GRATING_ROOM_GRATING;
        Room.Slots[GRATING_ROOM_BACK_WALL_QUAD * 2 + 1] = GRATING_ROOM_GRATING;
        Room.Meshes[0].MaterialBindings = {Bindings.GetData(), Bindings.GetSize()};
        Room.Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        Lights[0].SourceId = 1;
    }
    GratingRoom(const GratingRoom&) = delete;
    GratingRoom& operator=(const GratingRoom&) = delete;

    /// A point light below the ceiling's centre, above the back wall's spectrum.
    void AddPointLight(float32 intensity)
    {
        Lights[0].Kind = renderer::LightKind::Point;
        Lights[0].Position = {0, .9F, 0};
        Lights[0].Intensity = intensity;
        Room.Inputs.Lights = {Lights.GetData(), Lights.GetSize()};
    }

    /// A directional light shining into the room through its open front, onto the back wall.
    void AddFrontDirectionalLight()
    {
        Lights[0].Kind = renderer::LightKind::Directional;
        Lights[0].Direction = {0, 0, -1};
        Lights[0].Intensity = 1;
        Room.Inputs.Lights = {Lights.GetData(), Lights.GetSize()};
    }
};

/// Render frameCount frames of the template and read each output's running mean after the last one.
void RenderSteinbergYanOutputs(PreparedGraph& inoutTemplate,
                               const GraphGpuFixture& fixture,
                               usize frameCount,
                               StaticArray<Array<uint8>, STEINBERG_YAN_OUTPUT_COUNT>& outPixels)
{
    REQUIRE(inoutTemplate.Plan.Passes.GetSize() == STEINBERG_YAN_FIRST_SHADE_PASS + STEINBERG_YAN_OUTPUT_COUNT);
    for (usize frame = 0; frame < frameCount; ++frame)
    {
        RenderFrame(inoutTemplate.Graph, fixture);
    }
    for (usize output = 0; output < STEINBERG_YAN_OUTPUT_COUNT; ++output)
    {
        const uint32 resource = inoutTemplate.Plan.Passes[STEINBERG_YAN_FIRST_SHADE_PASS + output].Color;
        REQUIRE(resource < inoutTemplate.Plan.Resources.GetSize());
        REQUIRE(inoutTemplate.Plan.Resources[resource].IsExported == 1);
        ReadExport(inoutTemplate.Graph, fixture, resource, inoutTemplate.Extent, outPixels[output]);
        // Invalid hit records, exhausted budgets, failed queries and negative samples would show magenta.
        REQUIRE(CountFailurePixels(outPixels[output]) == 0);
    }
}

/// Colors of the grating's first-order spectrum: its most saturated bright pixel and the rows where blue and red
/// dominate most.
struct GratingSpectrum final
{
    float32 MaximumSaturation = 0;
    float32 MeanSaturation = 0;
    uint8 BrightestChannel = 0;
    usize BluestRow = 0;
    usize ReddestRow = 0;
};

GratingSpectrum MeasureGratingSpectrum(const Array<uint8>& pixels)
{
    GratingSpectrum result;
    float32 bluest = 0;
    float32 reddest = 0;
    float32 saturationSum = 0;
    usize brightPixels = 0;
    for (usize row = GRATING_ROOM_SPECTRUM_FIRST_ROW; row <= GRATING_ROOM_SPECTRUM_LAST_ROW; ++row)
    {
        for (usize column = GRATING_ROOM_CENTER_FIRST_COLUMN; column <= GRATING_ROOM_CENTER_LAST_COLUMN; ++column)
        {
            const usize address = GetPixel(column, row);
            const float32 red = pixels[address];
            const float32 green = pixels[address + 1];
            const float32 blue = pixels[address + 2];
            const uint8 brightest = math::Max(pixels[address], math::Max(pixels[address + 1], pixels[address + 2]));
            result.BrightestChannel = math::Max(result.BrightestChannel, brightest);
            if (brightest < GRATING_ROOM_BRIGHT_CHANNEL)
            {
                continue;
            }
            const float32 maximum = brightest;
            const float32 minimum = math::Min(red, math::Min(green, blue));
            result.MaximumSaturation = math::Max(result.MaximumSaturation, (maximum - minimum) / maximum);
            saturationSum += (maximum - minimum) / maximum;
            brightPixels += 1;
            if ((blue - red) / maximum > bluest)
            {
                bluest = (blue - red) / maximum;
                result.BluestRow = row;
            }
            if ((red - blue) / maximum > reddest)
            {
                reddest = (red - blue) / maximum;
                result.ReddestRow = row;
            }
        }
    }
    result.MeanSaturation = brightPixels > 0 ? saturationSum / static_cast<float32>(brightPixels) : 0;
    return result;
}
} // namespace

TEST_CASE("The Steinberg and Yan template disperses a small natural source into a spectrum that geometric optics "
          "does not show",
          "[render-graph][gpu][steinberg-yan][template]")
{
    GraphGpuFixture fixture;
    GratingRoom room(0);
    room.AddPointLight(GRATING_ROOM_SMALL_SOURCE_INTENSITY);
    PreparedGraph graph(
        fixture,
        {LUDUS_RG3_STEINBERG_YAN_PLT_SMALL_SOURCE_DIRECTORY, "rg2_steinberg_yan_plt_small_source.lurgrt"},
        room.Room,
        1);
    REQUIRE(renderer::RequiresHardwareRayTracing(graph.Plan) == true);
    for (usize pass = 0; pass < graph.Plan.Passes.GetSize(); ++pass)
    {
        INFO("pass " << pass);
        CHECK(graph.Plan.Passes[pass].Program == format::Technique::RayTracing);
        CHECK((graph.Plan.Passes[pass].AccumulationSamples > 0) == (pass >= STEINBERG_YAN_FIRST_SHADE_PASS));
        // The coherence radius is written linearly; the two radiance images are tone mapped.
        const bool isToneMapped =
            pass >= STEINBERG_YAN_FIRST_SHADE_PASS && pass < STEINBERG_YAN_FIRST_SHADE_PASS + STEINBERG_YAN_COHERENCE;
        CHECK((graph.Plan.Passes[pass].ToneMappingExposure == 1) == isToneMapped);
    }
    constexpr usize FRAME_COUNT = 256;
    StaticArray<Array<uint8>, STEINBERG_YAN_OUTPUT_COUNT> pixels;
    RenderSteinbergYanOutputs(graph, fixture, FRAME_COUNT, pixels);
    const GratingSpectrum wave = MeasureGratingSpectrum(pixels[STEINBERG_YAN_WAVE]);
    const GratingSpectrum geometric = MeasureGratingSpectrum(pixels[STEINBERG_YAN_GEOMETRIC]);
    INFO("wave: saturation " << wave.MaximumSaturation << ", brightest " << static_cast<uint32>(wave.BrightestChannel)
                             << ", bluest row " << wave.BluestRow << ", reddest row " << wave.ReddestRow
                             << "; geometric: saturation " << geometric.MaximumSaturation << ", brightest "
                             << static_cast<uint32>(geometric.BrightestChannel));
    // Both images reflect the source on the back wall's centre column: geometric optics as a streak of tilted facets.
    constexpr uint8 LIT_CHANNEL = 128;
    REQUIRE(wave.BrightestChannel >= LIT_CHANNEL);
    REQUIRE(geometric.BrightestChannel >= LIT_CHANNEL);
    // The first diffraction order is a spectrum: violet at the top, where the grating equation needs the shortest
    // wavelength, and red below. Geometric optics reflects every wavelength alike, so its streak stays white.
    constexpr float32 SPECTRAL_SATURATION = .6F;
    constexpr float32 NEUTRAL_SATURATION = .2F;
    CHECK(wave.MaximumSaturation > SPECTRAL_SATURATION);
    CHECK(wave.BluestRow < wave.ReddestRow);
    CHECK(geometric.MaximumSaturation < NEUTRAL_SATURATION);
}

TEST_CASE("A larger and so less coherent natural source blurs the Steinberg and Yan template's spectrum",
          "[render-graph][gpu][steinberg-yan][template]")
{
    // The paper's Fig. 8: interference colors are most visible under the most coherent light, from the smallest
    // source. The two variants differ only in PLT_POINT_SOURCE_RADIUS_MILLIMETERS, 20 and 200 mm. The larger source's
    // light is spatially coherent over a 10 times smaller radius, so each pixel mixes many more wavelengths.
    GraphGpuFixture fixture;
    StaticArray<GratingSpectrum, 2> spectra{};
    const StaticArray<PackageLocation, 2> locations{{
        {LUDUS_RG3_STEINBERG_YAN_PLT_SMALL_SOURCE_DIRECTORY, "rg2_steinberg_yan_plt_small_source.lurgrt"},
        {LUDUS_RG3_STEINBERG_YAN_PLT_LARGE_SOURCE_DIRECTORY, "rg2_steinberg_yan_plt_large_source.lurgrt"},
    }};
    const StaticArray<float32, 2> radii{{GRATING_ROOM_SMALL_SOURCE_MILLIMETERS, GRATING_ROOM_LARGE_SOURCE_MILLIMETERS}};
    for (usize source = 0; source < spectra.GetSize(); ++source)
    {
        GratingRoom room(0);
        room.AddPointLight(GRATING_ROOM_SMALL_SOURCE_INTENSITY * radii[source] / GRATING_ROOM_SMALL_SOURCE_MILLIMETERS);
        PreparedGraph graph(fixture, locations[source], room.Room, 1);
        constexpr usize FRAME_COUNT = 256;
        StaticArray<Array<uint8>, STEINBERG_YAN_OUTPUT_COUNT> pixels;
        RenderSteinbergYanOutputs(graph, fixture, FRAME_COUNT, pixels);
        spectra[source] = MeasureGratingSpectrum(pixels[STEINBERG_YAN_WAVE]);
    }
    INFO("20 mm: mean saturation " << spectra[0].MeanSaturation << ", brightest "
                                   << static_cast<uint32>(spectra[0].BrightestChannel) << "; 200 mm: mean saturation "
                                   << spectra[1].MeanSaturation << ", brightest "
                                   << static_cast<uint32>(spectra[1].BrightestChannel));
    constexpr uint8 LIT_CHANNEL = 128;
    REQUIRE(spectra[0].BrightestChannel >= LIT_CHANNEL);
    REQUIRE(spectra[1].BrightestChannel >= LIT_CHANNEL);
    constexpr float32 SATURATION_LOSS = .15F;
    CHECK(spectra[1].MeanSaturation < spectra[0].MeanSaturation - SATURATION_LOSS);
}

TEST_CASE("A flat grating looks the same in the Steinberg and Yan template's wave and geometric optics",
          "[render-graph][gpu][steinberg-yan][template]")
{
    // With zero amplitude only the mirror order exists (J_0(0) = 1) and every facet is the mean surface, so both
    // passes reflect the same direction at the same wavelengths: only rounding of that direction differs. The flat
    // variant also uses the 20 mm source.
    GraphGpuFixture fixture;
    constexpr float32 CEILING_EMISSION = 12;
    bool hasAreaLight = false;
    GratingRoom room(CEILING_EMISSION);
    SECTION("ceiling area light")
    {
        hasAreaLight = true;
    }
    SECTION("natural point source")
    {
        room.Materials[CORNELL_LIGHT].Emission = {0, 0, 0};
        room.AddPointLight(GRATING_ROOM_SMALL_SOURCE_INTENSITY);
    }
    PreparedGraph graph(fixture,
                        {LUDUS_RG3_STEINBERG_YAN_PLT_FLAT_DIRECTORY, "rg2_steinberg_yan_plt_flat.lurgrt"},
                        room.Room,
                        1);
    constexpr usize FRAME_COUNT = 128;
    StaticArray<Array<uint8>, STEINBERG_YAN_OUTPUT_COUNT> pixels;
    RenderSteinbergYanOutputs(graph, fixture, FRAME_COUNT, pixels);
    const math::Vector3 wave = ComputeMeanRadiance(pixels[STEINBERG_YAN_WAVE]);
    const math::Vector3 geometric = ComputeMeanRadiance(pixels[STEINBERG_YAN_GEOMETRIC]);
    constexpr uint8 CHANNEL_TOLERANCE = 2;
    const usize differingPixels =
        CountDifferingPixels(pixels[STEINBERG_YAN_WAVE], pixels[STEINBERG_YAN_GEOMETRIC], CHANNEL_TOLERANCE);
    INFO("wave mean=" << wave.X << ',' << wave.Y << ',' << wave.Z << ", geometric mean=" << geometric.X << ','
                      << geometric.Y << ',' << geometric.Z << ", differing pixels=" << differingPixels);
    constexpr usize DIFFERING_PIXEL_DIVISOR = 1000;
    CHECK(differingPixels <= static_cast<usize>(WIDTH) * HEIGHT / DIFFERING_PIXEL_DIVISOR);
    if (hasAreaLight == true)
    {
        // The back wall mirrors the white ceiling light at rows 18 to 20. Wavelengths are drawn for the mirror, but
        // the uplift turns white into a flat spectrum that the sensor reads back as white.
        constexpr usize MIRRORED_LIGHT_COLUMN = WIDTH / 2;
        constexpr usize MIRRORED_LIGHT_ROW = 19;
        const usize address = GetPixel(MIRRORED_LIGHT_COLUMN, MIRRORED_LIGHT_ROW);
        const uint8 red = pixels[STEINBERG_YAN_WAVE][address];
        const uint8 green = pixels[STEINBERG_YAN_WAVE][address + 1];
        const uint8 blue = pixels[STEINBERG_YAN_WAVE][address + 2];
        INFO("mirrored light " << static_cast<uint32>(red) << ',' << static_cast<uint32>(green) << ','
                               << static_cast<uint32>(blue));
        constexpr uint8 LIT_CHANNEL = 128;
        constexpr uint8 NEUTRAL_TOLERANCE = 3;
        CHECK(green >= LIT_CHANNEL);
        CHECK(math::Max(red, green) - math::Min(red, green) <= NEUTRAL_TOLERANCE);
        CHECK(math::Max(blue, green) - math::Min(blue, green) <= NEUTRAL_TOLERANCE);
    }
}

TEST_CASE("The Steinberg and Yan template's coherence output reproduces the coherence radius of sunlight",
          "[render-graph][gpu][steinberg-yan][template]")
{
    // Section 5.1: a natural source of angular radius q0 gives light whose degree of coherence first vanishes at
    // r_c = 4.4934 / (k sin q0). The template's directional light is the sun (q0 = 0.004653 rad); the paper finds
    // about 77 um at 500 nm, and the coherence output shows r_c at 550 nm per 100 um.
    GraphGpuFixture fixture;
    GratingRoom room(0);
    room.AddFrontDirectionalLight();
    PreparedGraph graph(fixture,
                        {LUDUS_RG3_STEINBERG_YAN_PLT_TEMPLATE_DIRECTORY, "rg2_steinberg_yan_plt_template.lurgrt"},
                        room.Room,
                        1);
    constexpr usize FRAME_COUNT = 4;
    StaticArray<Array<uint8>, STEINBERG_YAN_OUTPUT_COUNT> pixels;
    RenderSteinbergYanOutputs(graph, fixture, FRAME_COUNT, pixels);
    constexpr float64 COHERENCE_ZERO = 4.49340945790906;
    constexpr float64 SUN_ANGULAR_RADIUS = .004653;
    constexpr float64 WAVELENGTH_METERS = 550e-9;
    constexpr float64 DISPLAY_MICROMETERS = 100;
    constexpr float64 MICROMETERS_PER_METER = 1e6;
    constexpr float64 MAXIMUM_CHANNEL = 255;
    const float64 wavenumber = math::kTau / WAVELENGTH_METERS;
    const float64 radiusMicrometers =
        COHERENCE_ZERO / (wavenumber * math::Sin(SUN_ANGULAR_RADIUS)) * MICROMETERS_PER_METER;
    const float64 expected = radiusMicrometers / DISPLAY_MICROMETERS * MAXIMUM_CHANNEL;
    // The back wall faces the light; the floor is parallel to it and receives none.
    const usize wall = GetPixel(WIDTH / 2, HEIGHT / 2);
    constexpr usize FLOOR_ROW = 55;
    const usize floor = GetPixel(WIDTH / 2, FLOOR_ROW);
    const Array<uint8>& coherence = pixels[STEINBERG_YAN_COHERENCE];
    INFO("coherence radius " << radiusMicrometers << " um, expected byte " << expected << ", wall "
                             << static_cast<uint32>(coherence[wall]) << ", floor "
                             << static_cast<uint32>(coherence[floor]));
    constexpr float64 BYTE_TOLERANCE = 1;
    CHECK(math::Abs(static_cast<float64>(coherence[wall]) - expected) <= BYTE_TOLERANCE);
    CHECK(coherence[wall + 1] == coherence[wall]);
    CHECK(coherence[floor] == 0);
}

namespace
{
// ris-projected-solid-angle template tests. Thanks to Ishaan Shah, Aakash KT and P. J. Narayanan, "Combining Resampled
// Importance and Projected Solid Angle Samplings for Many Area Light Rendering", SIGGRAPH Asia 2023 Technical
// Communications, https://doi.org/10.1145/3610543.3626165 : the template resamples emissive triangles by their
// unshadowed LTC shading and samples the selected one with Christoph Peters' projected-solid-angle and LTC techniques
// ("BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021,
// https://doi.org/10.1145/3450626.3459672). Its outputs follow the veach-guibas-mis order (combined MIS, light
// strategy, BSDF strategy), so the Veach and Guibas helpers above read them. Acceptance is
// docs/architecture/many-area-light-sampling-reference-review.md, "Acceptance", item 3.
constexpr usize MANY_EMITTER_GRID = 4;
constexpr usize MANY_EMITTER_COUNT = MANY_EMITTER_GRID * MANY_EMITTER_GRID;
/// Floor, ceiling, back, left and right walls and the block top of the Cornell room, without its ceiling light.
constexpr usize MANY_EMITTER_ROOM_QUAD_COUNT = 6;
constexpr usize MANY_EMITTER_QUAD_COUNT = MANY_EMITTER_ROOM_QUAD_COUNT + MANY_EMITTER_COUNT;
constexpr usize MANY_EMITTER_QUAD_CORNERS = 4;
constexpr usize MANY_EMITTER_QUAD_INDICES = 6;
constexpr usize MANY_EMITTER_VERTEX_COUNT = MANY_EMITTER_QUAD_COUNT * MANY_EMITTER_QUAD_CORNERS;
constexpr usize MANY_EMITTER_INDEX_COUNT = MANY_EMITTER_QUAD_COUNT * MANY_EMITTER_QUAD_INDICES;
constexpr usize MANY_EMITTER_TRIANGLE_COUNT = MANY_EMITTER_QUAD_COUNT * 2;
constexpr uint32 MANY_EMITTER_WHITE = 0;
constexpr uint32 MANY_EMITTER_RED = 1;
constexpr uint32 MANY_EMITTER_GREEN = 2;
constexpr uint32 MANY_EMITTER_FIRST_LIGHT = 3;
constexpr usize MANY_EMITTER_MATERIAL_COUNT = MANY_EMITTER_FIRST_LIGHT + MANY_EMITTER_COUNT;
/// The Cornell room quads kept by the many-emitter room, as first corners in CORNELL_CORNERS_WORLD.
constexpr StaticArray<usize, MANY_EMITTER_ROOM_QUAD_COUNT> MANY_EMITTER_ROOM_QUAD_FIRST_CORNERS{{0, 4, 8, 12, 16, 24}};
constexpr StaticArray<uint32, MANY_EMITTER_ROOM_QUAD_COUNT> MANY_EMITTER_ROOM_QUAD_MATERIALS{{MANY_EMITTER_WHITE,
                                                                                              MANY_EMITTER_WHITE,
                                                                                              MANY_EMITTER_WHITE,
                                                                                              MANY_EMITTER_RED,
                                                                                              MANY_EMITTER_GREEN,
                                                                                              MANY_EMITTER_WHITE}};

/// Write the two triangles of quad `quad`, whose corners are positions 4 quad to 4 quad + 3 in winding order, into
/// indices and give both the material slot `material`. indices and slots hold every quad's entries.
void WriteQuad(usize quad, uint32 material, uint32* indices, uint32* slots)
{
    const uint32 first = static_cast<uint32>(quad * MANY_EMITTER_QUAD_CORNERS);
    const StaticArray<uint32, MANY_EMITTER_QUAD_INDICES> corners{{0, 1, 2, 0, 2, 3}};
    for (usize corner = 0; corner < MANY_EMITTER_QUAD_INDICES; ++corner)
    {
        indices[quad * MANY_EMITTER_QUAD_INDICES + corner] = first + corners[corner];
    }
    slots[quad * 2] = material;
    slots[quad * 2 + 1] = material;
}

/// The Cornell room with its ceiling light replaced by a 4 x 4 grid of small one-sided emitters, each its own material,
/// whose power spans a factor of 64. White surfaces are a rough dielectric, so both of Peters' techniques matter.
/// Veach and Guibas light sampling picks emitters uniformly and points by area; RIS adapts to power and to the
/// receiver. The mesh borrows this object's arrays, so it is neither copied nor moved.
struct ManyEmitterRoom final
{
    StaticArray<math::Vector3, MANY_EMITTER_VERTEX_COUNT> Positions{};
    StaticArray<uint32, MANY_EMITTER_INDEX_COUNT> Indices{};
    StaticArray<uint32, MANY_EMITTER_TRIANGLE_COUNT> Slots{};
    StaticArray<uint32, MANY_EMITTER_MATERIAL_COUNT> Bindings{};
    StaticArray<renderer::SceneShaderMaterial, MANY_EMITTER_MATERIAL_COUNT> Materials{};
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::RenderGraphItem, 1> Items{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    renderer::SceneShaderInputs Inputs;
    ManyEmitterRoom()
    {
        for (usize quad = 0; quad < MANY_EMITTER_ROOM_QUAD_COUNT; ++quad)
        {
            for (usize corner = 0; corner < MANY_EMITTER_QUAD_CORNERS; ++corner)
            {
                Positions[quad * MANY_EMITTER_QUAD_CORNERS + corner] =
                    CORNELL_CORNERS_WORLD[MANY_EMITTER_ROOM_QUAD_FIRST_CORNERS[quad] + corner];
            }
            WriteQuad(quad, MANY_EMITTER_ROOM_QUAD_MATERIALS[quad], Indices.GetData(), Slots.GetData());
        }
        // Tiles face down like the Cornell light, with its corner order; centers 0.4 apart, sides 0.16 long.
        constexpr float32 TILE_HEIGHT_WORLD = .98F;
        constexpr float32 TILE_FIRST_CENTER_WORLD = -.6F;
        constexpr float32 TILE_SPACING_WORLD = .4F;
        constexpr float32 TILE_HALF_SIDE_WORLD = .08F;
        constexpr usize EMISSION_LEVEL_COUNT = 4;
        constexpr StaticArray<float32, EMISSION_LEVEL_COUNT> EMISSION_LEVELS{{.5F, 2, 8, 32}};
        constexpr usize TINT_COUNT = 3;
        constexpr StaticArray<math::Vector3, TINT_COUNT> TINTS{{{1, 1, 1}, {1, .6F, .3F}, {.3F, .6F, 1}}};
        for (usize tile = 0; tile < MANY_EMITTER_COUNT; ++tile)
        {
            const float32 centerX =
                TILE_FIRST_CENTER_WORLD + TILE_SPACING_WORLD * static_cast<float32>(tile % MANY_EMITTER_GRID);
            const float32 centerZ =
                TILE_FIRST_CENTER_WORLD + TILE_SPACING_WORLD * static_cast<float32>(tile / MANY_EMITTER_GRID);
            const usize quad = MANY_EMITTER_ROOM_QUAD_COUNT + tile;
            const usize first = quad * MANY_EMITTER_QUAD_CORNERS;
            Positions[first] = {centerX - TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ - TILE_HALF_SIDE_WORLD};
            Positions[first + 1] = {centerX + TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ - TILE_HALF_SIDE_WORLD};
            Positions[first + 2] = {centerX + TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ + TILE_HALF_SIDE_WORLD};
            Positions[first + 3] = {centerX - TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ + TILE_HALF_SIDE_WORLD};
            const uint32 material = MANY_EMITTER_FIRST_LIGHT + static_cast<uint32>(tile);
            WriteQuad(quad, material, Indices.GetData(), Slots.GetData());
            // Levels and tints cycle with different periods, so neighbors differ in both.
            Materials[material].BaseColor = {.8F, .8F, .8F};
            Materials[material].Emission =
                TINTS[tile % TINT_COUNT] * EMISSION_LEVELS[(tile * 3) % EMISSION_LEVEL_COUNT];
        }
        constexpr float32 DIELECTRIC_ROUGHNESS = .4F;
        Materials[MANY_EMITTER_WHITE].BaseColor = {.8F, .8F, .8F};
        Materials[MANY_EMITTER_WHITE].Roughness = DIELECTRIC_ROUGHNESS;
        Materials[MANY_EMITTER_RED].BaseColor = {.8F, .1F, .1F};
        Materials[MANY_EMITTER_GREEN].BaseColor = {.1F, .8F, .1F};
        for (uint32 material = 0; material < MANY_EMITTER_MATERIAL_COUNT; ++material)
        {
            Bindings[material] = material;
        }
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Meshes[0].MaterialSlots = {Slots.GetData(), Slots.GetSize()};
        Meshes[0].MaterialBindings = {Bindings.GetData(), Bindings.GetSize()};
        Items[0].SourceId = 1;
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        // The Cornell room's camera.
        const CornellBox cornell;
        Cameras[0] = cornell.Cameras[0];
    }
    ManyEmitterRoom(const ManyEmitterRoom&) = delete;
    ManyEmitterRoom& operator=(const ManyEmitterRoom&) = delete;
};

constexpr uint32 EMISSION_MAP_SIDE = 4;
constexpr usize EMISSION_MAP_TEXEL_BYTES = 4;
constexpr usize EMISSION_MAP_BYTES =
    static_cast<usize>(EMISSION_MAP_SIDE) * EMISSION_MAP_SIDE * EMISSION_MAP_TEXEL_BYTES;
/// First corner of the Cornell light in CORNELL_CORNERS_WORLD.
constexpr usize CORNELL_LIGHT_FIRST_CORNER = 20;

/// A 4 x 4 linear emission map for the Cornell light, white, warm, cool and black texels in diagonal stripes, and the
/// room's UVs, which span the map over the light. The light's emission varies by a factor of four per channel and is
/// zero on a quarter of its area, while the emitter power CDF and the RIS target see its constant Emission: a light
/// sample that ignored the map at the sampled point, or a density that assumed uniform emission, would bias the image.
/// The room borrows this object's arrays, so it is neither copied nor moved.
struct EmissionMap final
{
    StaticArray<math::Vector2, CORNELL_VERTEX_COUNT> Texcoords{};
    StaticArray<uint8, EMISSION_MAP_BYTES> Texels{};
    EmissionMap()
    {
        constexpr usize COLOR_COUNT = 4;
        constexpr StaticArray<StaticArray<uint8, EMISSION_MAP_TEXEL_BYTES>, COLOR_COUNT> COLORS{
            {{{255, 255, 255, 255}}, {{255, 128, 64, 255}}, {{64, 128, 255, 255}}, {{0, 0, 0, 255}}}};
        for (uint32 y = 0; y < EMISSION_MAP_SIDE; ++y)
        {
            for (uint32 x = 0; x < EMISSION_MAP_SIDE; ++x)
            {
                const usize texel = (static_cast<usize>(y) * EMISSION_MAP_SIDE + x) * EMISSION_MAP_TEXEL_BYTES;
                const StaticArray<uint8, EMISSION_MAP_TEXEL_BYTES>& color = COLORS[(x + y) % COLOR_COUNT];
                for (usize channel = 0; channel < EMISSION_MAP_TEXEL_BYTES; ++channel)
                {
                    Texels[texel + channel] = color[channel];
                }
            }
        }
        // The light's corners in their winding order; every other corner keeps UV zero, where no map is bound.
        Texcoords[CORNELL_LIGHT_FIRST_CORNER] = {0, 0};
        Texcoords[CORNELL_LIGHT_FIRST_CORNER + 1] = {1, 0};
        Texcoords[CORNELL_LIGHT_FIRST_CORNER + 2] = {1, 1};
        Texcoords[CORNELL_LIGHT_FIRST_CORNER + 3] = {0, 1};
    }
    EmissionMap(const EmissionMap&) = delete;
    EmissionMap& operator=(const EmissionMap&) = delete;
};

/// Bind map to the Cornell light of inoutRoom; both must outlive every Update that reads the room.
void AttachEmissionMap(const EmissionMap& map, CornellBox& inoutRoom)
{
    inoutRoom.Meshes[0].Geometry.Texcoords = map.Texcoords.GetData();
    inoutRoom.Materials[CORNELL_LIGHT].EmissionTexture = {map.Texels.GetData(),
                                                          map.Texels.GetSize(),
                                                          static_cast<usize>(EMISSION_MAP_SIDE) *
                                                              EMISSION_MAP_TEXEL_BYTES,
                                                          EMISSION_MAP_SIDE,
                                                          EMISSION_MAP_SIDE,
                                                          rhi::RasterFormat::Rgba8Unorm};
}

const PackageLocation RIS_PROJECTED_SOLID_ANGLE_TEMPLATE{LUDUS_RG3_RIS_PROJECTED_SOLID_ANGLE_TEMPLATE_DIRECTORY,
                                                         "rg2_ris_projected_solid_angle_template.lurgrt"};
const PackageLocation VEACH_GUIBAS_TRIANGLE_LOOP{LUDUS_RG3_VEACH_GUIBAS_MIS_TRIANGLE_LOOP_DIRECTORY,
                                                 "rg2_veach_guibas_mis_triangle_loop.lurgrt"};

/// Render both templates on one room at 256 samples per pixel and require that the three RIS outputs and the Veach and
/// Guibas output veachOutput (VEACH_GUIBAS_LIGHT_SAMPLING or VEACH_GUIBAS_MIS) estimate the same integral.
template <typename Room>
void CheckRisOutputsAgreeWithVeachGuibas(const GraphGpuFixture& fixture, const Room& room, usize veachOutput)
{
    PreparedGraph ris(fixture, RIS_PROJECTED_SOLID_ANGLE_TEMPLATE, room, 1);
    REQUIRE(ris.Programs.HasPersistentRayBuffers == true);
    // Every pass walks the compute BVH: a compute ray-tracing package with no native ray requirement.
    REQUIRE(renderer::RequiresHardwareRayTracing(ris.Plan) == false);
    REQUIRE(renderer::UsesRayTracing(ris.Plan) == true);
    for (usize pass = 0; pass < ris.Plan.Passes.GetSize(); ++pass)
    {
        INFO("pass " << pass);
        CHECK(ris.Plan.Passes[pass].Program == format::Technique::RayTracing);
        CHECK(ris.Plan.Passes[pass].Traversal == format::RayTraversal::ComputeBvh);
        CHECK((ris.Plan.Passes[pass].AccumulationSamples > 0) == (pass >= VEACH_GUIBAS_FIRST_SHADE_PASS));
    }
    constexpr usize FRAME_COUNT = 256;
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> pixels;
    RenderVeachGuibasOutputs(ris, fixture, FRAME_COUNT, pixels);
    PreparedGraph veach(fixture, VEACH_GUIBAS_TRIANGLE_LOOP, room, 1);
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> veachPixels;
    RenderVeachGuibasOutputs(veach, fixture, FRAME_COUNT, veachPixels);
    const math::Vector3 veachMean = ComputeMeanRadiance(veachPixels[veachOutput]);
    constexpr float32 MINIMUM_MEAN_RADIANCE = .01F;
    REQUIRE(veachMean.X > MINIMUM_MEAN_RADIANCE);
    // Estimates of one integral, compared after decoding the tone-mapped running means, at 256 samples per pixel as
    // for the Veach and Guibas template's same-integral check.
    constexpr float32 RELATIVE_MEAN_TOLERANCE = .02F;
    for (usize output = 0; output < VEACH_GUIBAS_OUTPUT_COUNT; ++output)
    {
        const math::Vector3 mean = ComputeMeanRadiance(pixels[output]);
        INFO("RIS output " << output << " mean=" << mean.X << ',' << mean.Y << ',' << mean.Z
                           << ", Veach and Guibas output " << veachOutput << " mean=" << veachMean.X << ','
                           << veachMean.Y << ',' << veachMean.Z);
        CheckMeansAgree(mean, veachMean, RELATIVE_MEAN_TOLERANCE);
    }
}
} // namespace

TEST_CASE("The RIS projected-solid-angle template's outputs agree with the Veach and Guibas template",
          "[render-graph][gpu][ris-projected-solid-angle][template]")
{
    GraphGpuFixture fixture(false, false);
    {
        PreparedGraph ris(fixture, RIS_PROJECTED_SOLID_ANGLE_TEMPLATE, CornellBox{}, 1);
        CheckSameStage(
            ris.Programs.RayCompute,
            packages::rg2_ris_projected_solid_angle_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                           shaders::renderer_composite::Fragment())
                .RayCompute);
    }
    SECTION("glossy Cornell room with a sky")
    {
        CornellBox room;
        MakeRoomGlossy(room);
        CheckRisOutputsAgreeWithVeachGuibas(fixture, room, VEACH_GUIBAS_LIGHT_SAMPLING);
    }
    SECTION("many emitters of different power")
    {
        const ManyEmitterRoom room;
        CheckRisOutputsAgreeWithVeachGuibas(fixture, room, VEACH_GUIBAS_LIGHT_SAMPLING);
    }
    SECTION("a textured emitter over rough dielectric surfaces")
    {
        const EmissionMap map;
        CornellBox room;
        AttachEmissionMap(map, room);
        // A rough dielectric, as in the many-emitter room, so the diffuse and LTC techniques both carry weight.
        constexpr float32 DIELECTRIC_ROUGHNESS = .4F;
        room.Materials[CORNELL_WHITE].Roughness = DIELECTRIC_ROUGHNESS;
        CheckRisOutputsAgreeWithVeachGuibas(fixture, room, VEACH_GUIBAS_LIGHT_SAMPLING);
    }
    SECTION("a two-sided emitter over minimum-roughness metal")
    {
        // The LTC table's smallest roughness, where the specular technique carries almost all the weight.
        constexpr float32 MINIMUM_ROUGHNESS = .045F;
        CornellBox room;
        room.Materials[CORNELL_WHITE].Metallic = 1;
        room.Materials[CORNELL_WHITE].Roughness = MINIMUM_ROUGHNESS;
        room.Materials[CORNELL_LIGHT].IsEmissionTwoSided = true;
        // The upper face lights the ceiling 2 cm away, and the floor mirrors the light. There, Veach and Guibas
        // area-uniform light sampling has unbounded variance: at 256 samples per pixel its image mean is about 4%
        // low, while its MIS and BSDF outputs agree with every RIS output. Compare with its MIS output instead.
        CheckRisOutputsAgreeWithVeachGuibas(fixture, room, VEACH_GUIBAS_MIS);
    }
}

TEST_CASE("The RIS projected-solid-angle MIS output has less error than Veach and Guibas MIS with many emitters",
          "[render-graph][gpu][ris-projected-solid-angle][template][mis]")
{
    // Shah et al.'s motivation: with many emitters of unequal power, uniform emitter selection and area sampling waste
    // most light samples, while RIS over emitted power and the receiver's LTC shading concentrates them. Compare both
    // templates' MIS images at equal samples per pixel with an independent converged image: the Veach and Guibas MIS
    // output at 1024 samples per pixel, whose scene revision reseeds sampling. The error is measured on the tone-mapped
    // image (ComputeDisplayMeanSquaredError); the decoded linear error is reported beside it.
    GraphGpuFixture fixture(false, false);
    const ManyEmitterRoom room;
    // One graph at a time: the device admits a bounded number of ray graph jobs.
    constexpr usize LOW_FRAME_COUNT = 16;
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> risPixels;
    {
        PreparedGraph ris(fixture, RIS_PROJECTED_SOLID_ANGLE_TEMPLATE, room, 1);
        RenderVeachGuibasOutputs(ris, fixture, LOW_FRAME_COUNT, risPixels);
    }
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> veachPixels;
    {
        PreparedGraph veach(fixture, VEACH_GUIBAS_TRIANGLE_LOOP, room, 1);
        RenderVeachGuibasOutputs(veach, fixture, LOW_FRAME_COUNT, veachPixels);
    }
    constexpr uint64 CONVERGED_REVISION = 2;
    constexpr usize CONVERGED_FRAME_COUNT = 1024;
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> converged;
    {
        PreparedGraph convergedGraph(fixture, VEACH_GUIBAS_TRIANGLE_LOOP, room, CONVERGED_REVISION);
        RenderVeachGuibasOutputs(convergedGraph, fixture, CONVERGED_FRAME_COUNT, converged);
    }
    const float64 risError = ComputeDisplayMeanSquaredError(risPixels[VEACH_GUIBAS_MIS], converged[VEACH_GUIBAS_MIS]);
    const float64 veachError =
        ComputeDisplayMeanSquaredError(veachPixels[VEACH_GUIBAS_MIS], converged[VEACH_GUIBAS_MIS]);
    const float64 risLinearError = ComputeMeanSquaredError(risPixels[VEACH_GUIBAS_MIS], converged[VEACH_GUIBAS_MIS]);
    const float64 veachLinearError =
        ComputeMeanSquaredError(veachPixels[VEACH_GUIBAS_MIS], converged[VEACH_GUIBAS_MIS]);
    INFO("MIS mean squared error at " << LOW_FRAME_COUNT << " samples, tone-mapped: RIS projected solid angle="
                                      << risError << ", Veach and Guibas=" << veachError
                                      << "; decoded linear: RIS projected solid angle=" << risLinearError
                                      << ", Veach and Guibas=" << veachLinearError);
    CHECK(risError < veachError);
}

namespace
{
// polygonal-light-sampling template tests. Thanks to Christoph Peters, "BRDF Importance Sampling for Polygonal Lights",
// ACM Transactions on Graphics 40(4), Article 140, 2021, https://doi.org/10.1145/3450626.3459672 : the template shades
// every emissive triangle with his projected solid angle (section 3) and LTC (section 4.1) techniques and exports
// clamped optimal MIS (section 4.3), the balance heuristic, and each technique alone. Every pass is a shade pass; the
// first also records the primary hit. The CPU oracle and its parity with the template's estimators are in
// polygonal_light_sampling_oracle_tests.cpp and polygonal_light_sampling_parity_tests.cpp.
constexpr usize POLYGONAL_LIGHT_OUTPUT_COUNT = 4;
constexpr usize POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS = 0;
constexpr usize POLYGONAL_LIGHT_BALANCE_HEURISTIC = 1;
constexpr usize POLYGONAL_LIGHT_PROJECTED_SOLID_ANGLE = 2;
constexpr usize POLYGONAL_LIGHT_LTC = 3;
const PackageLocation POLYGONAL_LIGHT_SAMPLING_TEMPLATE{LUDUS_RG3_POLYGONAL_LIGHT_SAMPLING_TEMPLATE_DIRECTORY,
                                                        "rg2_polygonal_light_sampling_template.lurgrt"};

/// Render frameCount frames of the template and read each output's running mean after the last one. The template's
/// four passes write its four outputs in output order.
void RenderPolygonalLightOutputs(PreparedGraph& inoutTemplate,
                                 const GraphGpuFixture& fixture,
                                 usize frameCount,
                                 StaticArray<Array<uint8>, POLYGONAL_LIGHT_OUTPUT_COUNT>& outPixels)
{
    REQUIRE(inoutTemplate.Plan.Passes.GetSize() == POLYGONAL_LIGHT_OUTPUT_COUNT);
    StaticArray<uint32, POLYGONAL_LIGHT_OUTPUT_COUNT> exports{};
    for (usize output = 0; output < POLYGONAL_LIGHT_OUTPUT_COUNT; ++output)
    {
        exports[output] = inoutTemplate.Plan.Passes[output].Color;
        REQUIRE(exports[output] < inoutTemplate.Plan.Resources.GetSize());
        REQUIRE(inoutTemplate.Plan.Resources[exports[output]].IsExported == 1);
    }
    for (usize frame = 0; frame < frameCount; ++frame)
    {
        RenderFrame(inoutTemplate.Graph, fixture);
    }
    for (usize output = 0; output < POLYGONAL_LIGHT_OUTPUT_COUNT; ++output)
    {
        ReadExport(inoutTemplate.Graph, fixture, exports[output], {}, outPixels[output]);
    }
}

/// The Cornell room with glossy metal on its white surfaces and no sky: the template lights surfaces only from
/// polygons, so the comparison with the Veach and Guibas template must not include environment lighting.
void MakeRoomGlossyWithoutSky(CornellBox& inoutRoom)
{
    constexpr float32 GLOSSY_ROUGHNESS = .3F;
    inoutRoom.Materials[CORNELL_WHITE].Metallic = 1;
    inoutRoom.Materials[CORNELL_WHITE].Roughness = GLOSSY_ROUGHNESS;
}

/// Render the template and the Veach and Guibas triangle-loop template on one room at 256 samples per pixel. The
/// three unbiased outputs must agree with Veach and Guibas MIS; the LTC output estimates only the part of the integral
/// inside the LTC's support, so it may be lower but not higher.
void CheckPolygonalLightOutputsAgreeWithVeachGuibas(const GraphGpuFixture& fixture, const CornellBox& room)
{
    constexpr usize FRAME_COUNT = 256;
    StaticArray<Array<uint8>, POLYGONAL_LIGHT_OUTPUT_COUNT> pixels;
    {
        PreparedGraph polygonal(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, 1);
        REQUIRE(polygonal.Programs.HasPersistentRayBuffers == true);
        // Every pass walks the compute BVH and accumulates its own output.
        REQUIRE(renderer::RequiresHardwareRayTracing(polygonal.Plan) == false);
        REQUIRE(renderer::UsesRayTracing(polygonal.Plan) == true);
        for (usize pass = 0; pass < polygonal.Plan.Passes.GetSize(); ++pass)
        {
            INFO("pass " << pass);
            CHECK(polygonal.Plan.Passes[pass].Program == format::Technique::RayTracing);
            CHECK(polygonal.Plan.Passes[pass].Traversal == format::RayTraversal::ComputeBvh);
            CHECK(polygonal.Plan.Passes[pass].AccumulationSamples > 0);
        }
        RenderPolygonalLightOutputs(polygonal, fixture, FRAME_COUNT, pixels);
    }
    StaticArray<Array<uint8>, VEACH_GUIBAS_OUTPUT_COUNT> veachPixels;
    {
        PreparedGraph veach(fixture, VEACH_GUIBAS_TRIANGLE_LOOP, room, 1);
        RenderVeachGuibasOutputs(veach, fixture, FRAME_COUNT, veachPixels);
    }
    const math::Vector3 veachMean = ComputeMeanRadiance(veachPixels[VEACH_GUIBAS_MIS]);
    constexpr float32 MINIMUM_MEAN_RADIANCE = .01F;
    REQUIRE(veachMean.X > MINIMUM_MEAN_RADIANCE);
    // Estimates of one integral, compared after decoding the tone-mapped running means, as for the other templates.
    constexpr float32 RELATIVE_MEAN_TOLERANCE = .02F;
    for (usize output = 0; output < POLYGONAL_LIGHT_OUTPUT_COUNT; ++output)
    {
        // Invalid hit records, exhausted budgets and failed queries would show magenta.
        REQUIRE(CountFailurePixels(pixels[output]) == 0);
        const math::Vector3 mean = ComputeMeanRadiance(pixels[output]);
        INFO("polygonal-light output " << output << " mean=" << mean.X << ',' << mean.Y << ',' << mean.Z
                                       << ", Veach and Guibas MIS mean=" << veachMean.X << ',' << veachMean.Y << ','
                                       << veachMean.Z);
        if (output == POLYGONAL_LIGHT_LTC)
        {
            CHECK(mean.X > MINIMUM_MEAN_RADIANCE);
            CHECK(mean.X < (1 + RELATIVE_MEAN_TOLERANCE) * veachMean.X);
            CHECK(mean.Y < (1 + RELATIVE_MEAN_TOLERANCE) * veachMean.Y);
            CHECK(mean.Z < (1 + RELATIVE_MEAN_TOLERANCE) * veachMean.Z);
            continue;
        }
        CheckMeansAgree(mean, veachMean, RELATIVE_MEAN_TOLERANCE);
    }
}
} // namespace

TEST_CASE("The polygonal-light-sampling template's unbiased outputs agree with Veach and Guibas MIS",
          "[render-graph][gpu][polygonal-light-sampling][template]")
{
    GraphGpuFixture fixture(false, false);
    {
        PreparedGraph polygonal(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, CornellBox{}, 1);
        CheckSameStage(
            polygonal.Programs.RayCompute,
            packages::rg2_polygonal_light_sampling_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                          shaders::renderer_composite::Fragment())
                .RayCompute);
    }
    SECTION("diffuse Cornell room")
    {
        const CornellBox room;
        CheckPolygonalLightOutputsAgreeWithVeachGuibas(fixture, room);
    }
    SECTION("glossy Cornell room")
    {
        CornellBox room;
        MakeRoomGlossyWithoutSky(room);
        CheckPolygonalLightOutputsAgreeWithVeachGuibas(fixture, room);
    }
}

TEST_CASE("Polygonal-light clamped optimal MIS has less error than projected solid angle sampling on glossy metal",
          "[render-graph][gpu][polygonal-light-sampling][template][mis]")
{
    // Peters 2021, sections 4.2 and 6.3 and Figs. 10 and 14: projected solid angle sampling suits diffuse shading but
    // not specular highlights, which the LTC technique samples; MIS combines both. Compare low-sample images of each
    // output with an independent converged clamped optimal MIS image, whose scene revision reseeds sampling. The
    // error is measured on the tone-mapped image (ComputeDisplayMeanSquaredError); the decoded linear error and the
    // balance heuristic's are reported beside it.
    GraphGpuFixture fixture(false, false);
    CornellBox room;
    MakeRoomGlossyWithoutSky(room);
    constexpr usize LOW_FRAME_COUNT = 16;
    StaticArray<Array<uint8>, POLYGONAL_LIGHT_OUTPUT_COUNT> pixels;
    {
        PreparedGraph polygonal(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, 1);
        RenderPolygonalLightOutputs(polygonal, fixture, LOW_FRAME_COUNT, pixels);
    }
    constexpr uint64 CONVERGED_REVISION = 2;
    constexpr usize CONVERGED_FRAME_COUNT = 1024;
    StaticArray<Array<uint8>, POLYGONAL_LIGHT_OUTPUT_COUNT> converged;
    {
        PreparedGraph convergedGraph(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, CONVERGED_REVISION);
        RenderPolygonalLightOutputs(convergedGraph, fixture, CONVERGED_FRAME_COUNT, converged);
    }
    const Array<uint8>& reference = converged[POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS];
    const float64 clamped = ComputeDisplayMeanSquaredError(pixels[POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS], reference);
    const float64 balance = ComputeDisplayMeanSquaredError(pixels[POLYGONAL_LIGHT_BALANCE_HEURISTIC], reference);
    const float64 projectedSolidAngle =
        ComputeDisplayMeanSquaredError(pixels[POLYGONAL_LIGHT_PROJECTED_SOLID_ANGLE], reference);
    const float64 clampedLinear = ComputeMeanSquaredError(pixels[POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS], reference);
    const float64 balanceLinear = ComputeMeanSquaredError(pixels[POLYGONAL_LIGHT_BALANCE_HEURISTIC], reference);
    const float64 projectedSolidAngleLinear =
        ComputeMeanSquaredError(pixels[POLYGONAL_LIGHT_PROJECTED_SOLID_ANGLE], reference);
    INFO("mean squared error at " << LOW_FRAME_COUNT << " samples, tone-mapped: clamped optimal MIS=" << clamped
                                  << ", balance heuristic=" << balance << ", projected solid angle="
                                  << projectedSolidAngle << "; decoded linear: clamped optimal MIS=" << clampedLinear
                                  << ", balance heuristic=" << balanceLinear
                                  << ", projected solid angle=" << projectedSolidAngleLinear);
    CHECK(clamped < projectedSolidAngle);
}

TEST_CASE("The polygonal-light-sampling template fails explicitly beyond its emissive triangle budget",
          "[render-graph][gpu][polygonal-light-sampling][template]")
{
    // The many-emitter room has 32 emissive triangles. Both MIS passes would need 64 shadow rays against the
    // technique's 16, so every pass reports the failure color where it shades a surface instead of dropping lights.
    GraphGpuFixture fixture(false, false);
    const ManyEmitterRoom room;
    PreparedGraph polygonal(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, 1);
    StaticArray<Array<uint8>, POLYGONAL_LIGHT_OUTPUT_COUNT> pixels;
    RenderPolygonalLightOutputs(polygonal, fixture, 1, pixels);
    for (usize output = 0; output < POLYGONAL_LIGHT_OUTPUT_COUNT; ++output)
    {
        INFO("output " << output);
        // The room fills most of the view; only the open front shows the environment.
        CHECK(CountFailurePixels(pixels[output]) > static_cast<usize>(WIDTH) * HEIGHT / 2);
    }
}

namespace
{
// An occluded penumbra after Peters 2021, Fig. 10 and section 4.3: a blue diffuse plane and a white glossy plane lit
// by a rectangular Lambertian emitter, with a wall between them that casts a large penumbra over the lower half of both
// planes. Outside the specular highlight the specular coefficient c_1 is small, so the weighted balance heuristic
// (v = 1) gives the LTC samples almost no weight in the penumbra; clamped optimal MIS (v = 1/2) keeps half of the
// balance heuristic there. The planes face the camera at z = -1. The wall is a one-sided quad facing the emitter, so
// the camera sees its black back face; shadow rays meet it from either side.
constexpr usize PENUMBRA_QUAD_COUNT = 4;
constexpr usize PENUMBRA_VERTEX_COUNT = PENUMBRA_QUAD_COUNT * MANY_EMITTER_QUAD_CORNERS;
constexpr usize PENUMBRA_INDEX_COUNT = PENUMBRA_QUAD_COUNT * MANY_EMITTER_QUAD_INDICES;
constexpr usize PENUMBRA_TRIANGLE_COUNT = PENUMBRA_QUAD_COUNT * 2;
constexpr uint32 PENUMBRA_DIFFUSE = 0;
constexpr uint32 PENUMBRA_GLOSSY = 1;
constexpr uint32 PENUMBRA_WALL = 2;
constexpr uint32 PENUMBRA_LIGHT = 3;
constexpr usize PENUMBRA_MATERIAL_COUNT = 4;
/// Each quad's corners wind counterclockwise seen from the side its geometric normal faces.
constexpr StaticArray<math::Vector3, PENUMBRA_VERTEX_COUNT> PENUMBRA_CORNERS_WORLD{{
    // The blue diffuse plane (left) and the white glossy plane (right), facing the camera.
    {-1.6F, -1.2F, -1},
    {0, -1.2F, -1},
    {0, 1, -1},
    {-1.6F, 1, -1},
    {0, -1.2F, -1},
    {1.6F, -1.2F, -1},
    {1.6F, 1, -1},
    {0, 1, -1},
    // The wall, horizontal below the emitter and facing it, 0.45 in front of the planes so light passes beneath it.
    {-1.6F, .5F, .6F},
    {1.6F, .5F, .6F},
    {1.6F, .5F, -.55F},
    {-1.6F, .5F, -.55F},
    // The one-sided emitter, facing down.
    {-.8F, 1.1F, -.8F},
    {.8F, 1.1F, -.8F},
    {.8F, 1.1F, .2F},
    {-.8F, 1.1F, .2F},
}};
constexpr StaticArray<uint32, PENUMBRA_QUAD_COUNT> PENUMBRA_QUAD_MATERIALS{
    {PENUMBRA_DIFFUSE, PENUMBRA_GLOSSY, PENUMBRA_WALL, PENUMBRA_LIGHT}};

/// The occluded-penumbra scene with the Cornell room's camera. The mesh borrows this object's arrays, so it is neither
/// copied nor moved.
struct OccludedPenumbraRoom final
{
    StaticArray<math::Vector3, PENUMBRA_VERTEX_COUNT> Positions = PENUMBRA_CORNERS_WORLD;
    StaticArray<uint32, PENUMBRA_INDEX_COUNT> Indices{};
    StaticArray<uint32, PENUMBRA_TRIANGLE_COUNT> Slots{};
    StaticArray<uint32, PENUMBRA_MATERIAL_COUNT> Bindings{
        {PENUMBRA_DIFFUSE, PENUMBRA_GLOSSY, PENUMBRA_WALL, PENUMBRA_LIGHT}};
    StaticArray<renderer::SceneShaderMaterial, PENUMBRA_MATERIAL_COUNT> Materials{};
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::RenderGraphItem, 1> Items{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    renderer::SceneShaderInputs Inputs;
    OccludedPenumbraRoom()
    {
        for (usize quad = 0; quad < PENUMBRA_QUAD_COUNT; ++quad)
        {
            WriteQuad(quad, PENUMBRA_QUAD_MATERIALS[quad], Indices.GetData(), Slots.GetData());
        }
        constexpr float32 DIFFUSE_ROUGHNESS = 1;
        constexpr float32 GLOSSY_ROUGHNESS = .25F;
        Materials[PENUMBRA_DIFFUSE].BaseColor = {.1F, .2F, .8F};
        Materials[PENUMBRA_DIFFUSE].Roughness = DIFFUSE_ROUGHNESS;
        Materials[PENUMBRA_GLOSSY].BaseColor = {.8F, .8F, .8F};
        Materials[PENUMBRA_GLOSSY].Roughness = GLOSSY_ROUGHNESS;
        Materials[PENUMBRA_WALL].BaseColor = {.5F, .5F, .5F};
        Materials[PENUMBRA_LIGHT].BaseColor = {.8F, .8F, .8F};
        Materials[PENUMBRA_LIGHT].Emission = {6, 6, 6};
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Meshes[0].MaterialSlots = {Slots.GetData(), Slots.GetSize()};
        Meshes[0].MaterialBindings = {Bindings.GetData(), Bindings.GetSize()};
        Items[0].SourceId = 1;
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        const CornellBox cornell;
        Cameras[0] = cornell.Cameras[0];
    }
    OccludedPenumbraRoom(const OccludedPenumbraRoom&) = delete;
    OccludedPenumbraRoom& operator=(const OccludedPenumbraRoom&) = delete;
};

/// Pixels [FirstColumn, EndColumn) x [FirstRow, EndRow) of a WIDTH x HEIGHT image.
struct PixelRegion final
{
    uint32 FirstColumn = 0;
    uint32 EndColumn = WIDTH;
    uint32 FirstRow = 0;
    uint32 EndRow = HEIGHT;
};

// The Cornell camera maps a point (x, y, -1) on the planes to column 48 + 16 x and row 32 - 16 y. The penumbra covers
// the planes below y = 0.2, down to their lower edge at y = -1.2; the planes meet at x = 0.
constexpr PixelRegion PENUMBRA_DIFFUSE_REGION{23, 48, 29, 51};
constexpr PixelRegion PENUMBRA_GLOSSY_REGION{48, 73, 29, 51};

/// ComputeDisplayMeanSquaredError over one region of RGBA8 images of WIDTH x HEIGHT pixels.
float64 ComputeDisplayMeanSquaredError(const Array<uint8>& pixels, const Array<uint8>& converged, PixelRegion region)
{
    constexpr usize CHANNEL_COUNT = 3;
    constexpr usize BYTES_PER_PIXEL = 4;
    constexpr float64 MAXIMUM_CHANNEL = 255;
    REQUIRE(pixels.GetSize() == PIXEL_BYTES);
    REQUIRE(converged.GetSize() == pixels.GetSize());
    REQUIRE(region.FirstColumn < region.EndColumn);
    REQUIRE(region.EndColumn <= WIDTH);
    REQUIRE(region.FirstRow < region.EndRow);
    REQUIRE(region.EndRow <= HEIGHT);
    float64 sum = 0;
    for (uint32 row = region.FirstRow; row < region.EndRow; ++row)
    {
        for (uint32 column = region.FirstColumn; column < region.EndColumn; ++column)
        {
            const usize address = (static_cast<usize>(row) * WIDTH + column) * BYTES_PER_PIXEL;
            for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
            {
                const float64 difference = (static_cast<float64>(pixels[address + channel]) -
                                            static_cast<float64>(converged[address + channel])) /
                                           MAXIMUM_CHANNEL;
                sum += difference * difference;
            }
        }
    }
    const usize pixelCount =
        static_cast<usize>(region.EndColumn - region.FirstColumn) * (region.EndRow - region.FirstRow);
    return sum / static_cast<float64>(pixelCount * CHANNEL_COUNT);
}

} // namespace

TEST_CASE("Polygonal-light clamped optimal MIS beats the weighted balance heuristic in an occluded penumbra",
          "[render-graph][gpu][polygonal-light-sampling][template][mis]")
{
    // Peters 2021, section 4.3 and Fig. 10: the weighted balance heuristic is optimal without occlusion but noisy in a
    // diffuse penumbra, which clamped optimal MIS with v = 1/2 fixes. Peters reports the error with overexposed pixels
    // clamped, where clamped optimal MIS wins (RMSE 0.188 against 0.202); the tone-mapped error here is bounded the
    // same way. The two packages share the scene revision, so they draw the same samples, and the converged reference
    // has its own. On Apple M5, over six independent revisions against a 4096-sample reference, the clamped optimal
    // MIS error was 0.63 to 0.67 times the weighted balance heuristic's in the diffuse plane's penumbra, 0.82 to 0.86
    // times in the glossy plane's, about equal in the fully lit strip above the wall, and 0.93 to 0.96 times over the
    // image; the bounds below leave at least four of those revisions' standard deviations.
    GraphGpuFixture fixture(false, false);
    const OccludedPenumbraRoom room;
    constexpr usize LOW_FRAME_COUNT = 16;
    StaticArray<Array<uint8>, POLYGONAL_LIGHT_OUTPUT_COUNT> pixels;
    {
        PreparedGraph polygonal(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, 1);
        RenderPolygonalLightOutputs(polygonal, fixture, LOW_FRAME_COUNT, pixels);
    }
    // The template staged with PROJECTED_SOLID_ANGLE_MIS_VISIBILITY = 1 (CMakeLists.txt): its color output is the
    // weighted balance heuristic, and its other outputs do not depend on v.
    const PackageLocation weightedTemplate{LUDUS_RG3_POLYGONAL_LIGHT_SAMPLING_WEIGHTED_DIRECTORY,
                                           "rg2_polygonal_light_sampling_weighted.lurgrt"};
    StaticArray<Array<uint8>, POLYGONAL_LIGHT_OUTPUT_COUNT> weightedPixels;
    {
        PreparedGraph weighted(fixture, weightedTemplate, room, 1);
        RenderPolygonalLightOutputs(weighted, fixture, LOW_FRAME_COUNT, weightedPixels);
    }
    constexpr uint64 CONVERGED_REVISION = 2;
    constexpr usize CONVERGED_FRAME_COUNT = 1024;
    StaticArray<Array<uint8>, POLYGONAL_LIGHT_OUTPUT_COUNT> converged;
    {
        PreparedGraph convergedGraph(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, CONVERGED_REVISION);
        RenderPolygonalLightOutputs(convergedGraph, fixture, CONVERGED_FRAME_COUNT, converged);
    }
    for (usize output = 0; output < POLYGONAL_LIGHT_OUTPUT_COUNT; ++output)
    {
        INFO("output " << output);
        REQUIRE(CountFailurePixels(pixels[output]) == 0);
        REQUIRE(CountFailurePixels(weightedPixels[output]) == 0);
        // Only the color output depends on v.
        if (output != POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS)
        {
            CHECK(CountDifferingPixels(pixels[output], weightedPixels[output], 0) == 0);
        }
    }
    const Array<uint8>& reference = converged[POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS];
    const float64 clamped = ComputeDisplayMeanSquaredError(pixels[POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS], reference);
    const float64 weighted =
        ComputeDisplayMeanSquaredError(weightedPixels[POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS], reference);
    const float64 balance = ComputeDisplayMeanSquaredError(pixels[POLYGONAL_LIGHT_BALANCE_HEURISTIC], reference);
    const float64 clampedDiffusePenumbra =
        ComputeDisplayMeanSquaredError(pixels[POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS], reference, PENUMBRA_DIFFUSE_REGION);
    const float64 weightedDiffusePenumbra =
        ComputeDisplayMeanSquaredError(weightedPixels[POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS],
                                       reference,
                                       PENUMBRA_DIFFUSE_REGION);
    const float64 clampedGlossyPenumbra =
        ComputeDisplayMeanSquaredError(pixels[POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS], reference, PENUMBRA_GLOSSY_REGION);
    const float64 weightedGlossyPenumbra =
        ComputeDisplayMeanSquaredError(weightedPixels[POLYGONAL_LIGHT_CLAMPED_OPTIMAL_MIS],
                                       reference,
                                       PENUMBRA_GLOSSY_REGION);
    INFO("tone-mapped mean squared error at "
         << LOW_FRAME_COUNT << " samples: clamped optimal MIS=" << clamped
         << ", weighted balance heuristic=" << weighted << ", balance heuristic=" << balance
         << "; diffuse penumbra: clamped optimal MIS=" << clampedDiffusePenumbra << ", weighted balance heuristic="
         << weightedDiffusePenumbra << "; glossy penumbra: clamped optimal MIS=" << clampedGlossyPenumbra
         << ", weighted balance heuristic=" << weightedGlossyPenumbra);
    constexpr float64 DIFFUSE_PENUMBRA_ERROR_RATIO = .8;
    constexpr float64 GLOSSY_PENUMBRA_ERROR_RATIO = .95;
    REQUIRE(weightedDiffusePenumbra > 0);
    CHECK(clampedDiffusePenumbra < DIFFUSE_PENUMBRA_ERROR_RATIO * weightedDiffusePenumbra);
    CHECK(clampedGlossyPenumbra < GLOSSY_PENUMBRA_ERROR_RATIO * weightedGlossyPenumbra);
    CHECK(clamped < weighted);
}

namespace
{
// The opt-in [.ris-projected-solid-angle-benchmark]: Shah et al.'s claim, lower error at equal time with many lights,
// on a procedural room with thousands of emissive triangles, against the Veach and Guibas template. Both packages
// trace with the compute BVH and replace their BSDF-only program by render_graph_query_count.slang (CMakeLists.txt),
// so their MIS outputs are the templates' own, and their third output counts the MIS program's queries. Error is
// measured against a high-sample Veach and Guibas MIS image as MAPE (Shah et al., section 5) and RMSE of decoded linear
// radiance, beside the tone-mapped mean squared error the CTest comparison uses. The diffuse and specular variants
// follow the paper's Figure 3. Method and recorded results:
// docs/architecture/many-area-light-sampling-reference-review.md, "Equal-time benchmark".
constexpr usize EMITTER_FIELD_GRID = 40;
constexpr usize EMITTER_FIELD_TILE_COUNT = EMITTER_FIELD_GRID * EMITTER_FIELD_GRID;
constexpr usize EMITTER_FIELD_QUAD_COUNT = MANY_EMITTER_ROOM_QUAD_COUNT + EMITTER_FIELD_TILE_COUNT;
constexpr usize EMITTER_FIELD_LEVEL_COUNT = 4;
constexpr usize EMITTER_FIELD_TINT_COUNT = 3;
constexpr usize EMITTER_FIELD_MATERIAL_COUNT =
    MANY_EMITTER_FIRST_LIGHT + EMITTER_FIELD_LEVEL_COUNT * EMITTER_FIELD_TINT_COUNT;

/// Reflectance of the room's white surfaces, after the diffuse and glossy scenes of Shah et al.'s Figure 3.
enum class SurfaceFinish : uint8
{
    Diffuse,  ///< Lambertian-like rough dielectric, roughness 1.
    Specular, ///< Glossy metal, roughness 0.2, which mirrors the emitter field.
};

/// The Cornell room with its ceiling light replaced by a 40 x 40 field of small one-sided emitters (3200 emissive
/// triangles), whose power spans a factor of 64 in four levels and three tints. The mesh borrows this object's arrays,
/// so it is neither copied nor moved.
struct EmitterFieldRoom final
{
    Array<math::Vector3> Positions;
    Array<uint32> Indices;
    Array<uint32> Slots;
    StaticArray<uint32, EMITTER_FIELD_MATERIAL_COUNT> Bindings{};
    StaticArray<renderer::SceneShaderMaterial, EMITTER_FIELD_MATERIAL_COUNT> Materials{};
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::RenderGraphItem, 1> Items{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    renderer::SceneShaderInputs Inputs;
    explicit EmitterFieldRoom(SurfaceFinish finish)
    {
        REQUIRE(Positions.TryResize(EMITTER_FIELD_QUAD_COUNT * MANY_EMITTER_QUAD_CORNERS) == true);
        REQUIRE(Indices.TryResize(EMITTER_FIELD_QUAD_COUNT * MANY_EMITTER_QUAD_INDICES) == true);
        REQUIRE(Slots.TryResize(EMITTER_FIELD_QUAD_COUNT * 2) == true);
        for (usize quad = 0; quad < MANY_EMITTER_ROOM_QUAD_COUNT; ++quad)
        {
            for (usize corner = 0; corner < MANY_EMITTER_QUAD_CORNERS; ++corner)
            {
                Positions[quad * MANY_EMITTER_QUAD_CORNERS + corner] =
                    CORNELL_CORNERS_WORLD[MANY_EMITTER_ROOM_QUAD_FIRST_CORNERS[quad] + corner];
            }
            WriteQuad(quad, MANY_EMITTER_ROOM_QUAD_MATERIALS[quad], Indices.GetData(), Slots.GetData());
        }
        // Tiles face down like the Cornell light, with its corner order; centers 0.05 apart, sides 0.03 long.
        constexpr float32 TILE_HEIGHT_WORLD = .98F;
        constexpr float32 TILE_SPACING_WORLD = 2.F / static_cast<float32>(EMITTER_FIELD_GRID);
        constexpr float32 TILE_FIRST_CENTER_WORLD = -1 + TILE_SPACING_WORLD / 2;
        constexpr float32 TILE_HALF_SIDE_WORLD = .015F;
        for (usize tile = 0; tile < EMITTER_FIELD_TILE_COUNT; ++tile)
        {
            const float32 centerX =
                TILE_FIRST_CENTER_WORLD + TILE_SPACING_WORLD * static_cast<float32>(tile % EMITTER_FIELD_GRID);
            const float32 centerZ =
                TILE_FIRST_CENTER_WORLD + TILE_SPACING_WORLD * static_cast<float32>(tile / EMITTER_FIELD_GRID);
            const usize quad = MANY_EMITTER_ROOM_QUAD_COUNT + tile;
            const usize first = quad * MANY_EMITTER_QUAD_CORNERS;
            Positions[first] = {centerX - TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ - TILE_HALF_SIDE_WORLD};
            Positions[first + 1] = {centerX + TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ - TILE_HALF_SIDE_WORLD};
            Positions[first + 2] = {centerX + TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ + TILE_HALF_SIDE_WORLD};
            Positions[first + 3] = {centerX - TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ + TILE_HALF_SIDE_WORLD};
            // Levels and tints cycle with different periods along and across rows, so neighbors differ in both.
            const usize level = (tile * 5 + tile / EMITTER_FIELD_GRID) % EMITTER_FIELD_LEVEL_COUNT;
            const usize tint = tile % EMITTER_FIELD_TINT_COUNT;
            const uint32 material =
                MANY_EMITTER_FIRST_LIGHT + static_cast<uint32>(level * EMITTER_FIELD_TINT_COUNT + tint);
            WriteQuad(quad, material, Indices.GetData(), Slots.GetData());
        }
        constexpr StaticArray<float32, EMITTER_FIELD_LEVEL_COUNT> EMISSION_LEVELS{{.25F, 1, 4, 16}};
        constexpr StaticArray<math::Vector3, EMITTER_FIELD_TINT_COUNT> TINTS{{{1, 1, 1}, {1, .6F, .3F}, {.3F, .6F, 1}}};
        for (usize level = 0; level < EMITTER_FIELD_LEVEL_COUNT; ++level)
        {
            for (usize tint = 0; tint < EMITTER_FIELD_TINT_COUNT; ++tint)
            {
                renderer::SceneShaderMaterial& light =
                    Materials[MANY_EMITTER_FIRST_LIGHT + level * EMITTER_FIELD_TINT_COUNT + tint];
                light.BaseColor = {.8F, .8F, .8F};
                light.Emission = TINTS[tint] * EMISSION_LEVELS[level];
            }
        }
        Materials[MANY_EMITTER_WHITE].BaseColor = {.8F, .8F, .8F};
        if (finish == SurfaceFinish::Specular)
        {
            constexpr float32 GLOSSY_ROUGHNESS = .2F;
            Materials[MANY_EMITTER_WHITE].Metallic = 1;
            Materials[MANY_EMITTER_WHITE].Roughness = GLOSSY_ROUGHNESS;
        }
        Materials[MANY_EMITTER_RED].BaseColor = {.8F, .1F, .1F};
        Materials[MANY_EMITTER_GREEN].BaseColor = {.1F, .8F, .1F};
        for (uint32 material = 0; material < EMITTER_FIELD_MATERIAL_COUNT; ++material)
        {
            Bindings[material] = material;
        }
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Meshes[0].MaterialSlots = {Slots.GetData(), Slots.GetSize()};
        Meshes[0].MaterialBindings = {Bindings.GetData(), Bindings.GetSize()};
        Items[0].SourceId = 1;
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        // The Cornell room's camera.
        const CornellBox cornell;
        Cameras[0] = cornell.Cameras[0];
    }
    EmitterFieldRoom(const EmitterFieldRoom&) = delete;
    EmitterFieldRoom& operator=(const EmitterFieldRoom&) = delete;
};

/// One ris-projected-solid-angle benchmark package and the Graph settings it was cooked with
/// (LUDUS_RIS_PROJECTED_SOLID_ANGLE_BENCHMARK_VARIANTS in CMakeLists.txt).
struct RisBenchmarkVariant final
{
    PackageLocation Location;
    uint32 Target = 0;             ///< RIS_TARGET: 0 exact clipped integrals, 1 approximate form factor.
    uint32 CandidateCount = 0;     ///< RIS_CANDIDATE_COUNT, M.
    uint32 NormalizationCount = 0; ///< RIS_NORMALIZATION_CANDIDATE_COUNT, K.
};
constexpr usize RIS_BENCHMARK_VARIANT_COUNT = LUDUS_RG3_RIS_BENCHMARK_VARIANT_COUNT;
const StaticArray<RisBenchmarkVariant, RIS_BENCHMARK_VARIANT_COUNT> RIS_BENCHMARK_VARIANTS{
    {LUDUS_RG3_RIS_BENCHMARK_VARIANTS}};
const PackageLocation VEACH_GUIBAS_BENCHMARK{LUDUS_RG3_VEACH_GUIBAS_MIS_BENCHMARK_DIRECTORY,
                                             "rg2_veach_guibas_mis_benchmark.lurgrt"};
/// The benchmark packages' third output, which counts the MIS program's queries.
constexpr usize BENCHMARK_QUERY_COUNT_OUTPUT = VEACH_GUIBAS_BSDF_SAMPLING;
/// render_graph_query_count.slang's QUERY_COUNT_SCALE.
constexpr float64 BENCHMARK_QUERY_COUNT_SCALE = .25;
/// The Cornell camera's aspect ratio at nine times the CTest image area: the largest such extent whose RGBA8 readback,
/// with rows padded to 256 bytes, fits one default 256 KiB transfer slice (rhi::StorageLimits::TransferBytes).
constexpr ExportExtent BENCHMARK_EXTENT{WIDTH * 3, HEIGHT * 3};
constexpr usize BENCHMARK_WARMUP_FRAMES = 8;
constexpr usize BENCHMARK_TIMED_FRAMES = 64;
constexpr usize BENCHMARK_SAMPLE_BUDGET_COUNT = 3;
/// Samples per pixel of the RIS image at each budget; Veach and Guibas gets as many at equal samples, and the same
/// kernel time at equal time.
constexpr StaticArray<usize, BENCHMARK_SAMPLE_BUDGET_COUNT> BENCHMARK_SAMPLE_BUDGETS{{4, 16, 64}};
/// Accumulate's sample limit in both templates.
constexpr usize BENCHMARK_MAXIMUM_FRAMES = 4096;
constexpr usize BENCHMARK_REFERENCE_FRAMES = BENCHMARK_MAXIMUM_FRAMES;
/// Reseeds sampling, so the reference is independent of the measured images.
constexpr uint64 BENCHMARK_REFERENCE_REVISION = 2;
/// Linear radiance above which the 8-bit tone-mapped readback no longer resolves a reference channel: at 4 one byte
/// is about 2.5% of the value, and directly visible emitters (up to 16) saturate. Linear metrics skip such pixels,
/// which see emitters, whose primary-hit emission both templates evaluate identically.
constexpr float32 BENCHMARK_MAXIMUM_RESOLVED_RADIANCE = 4;
/// Guards MAPE's relative error against black reference channels.
constexpr float64 BENCHMARK_PERCENTAGE_GUARD = .01;
constexpr float64 BENCHMARK_NANOSECONDS_PER_MILLISECOND = 1'000'000;

/// Median kernel times and query counts per sample of one benchmark package.
struct EstimatorCost final
{
    float64 PrimaryMilliseconds = 0; ///< The primary-hit pass's kernel.
    float64 ShadeMilliseconds = 0;   ///< The MIS shade pass's kernel, without its Accumulate and ToneMap.
    float64 ShadowRaysPerPixel = 0;  ///< Mean visibility queries of the MIS program per pixel and sample.
    float64 BsdfRaysPerPixel = 0;    ///< Mean surface queries of the MIS program per pixel and sample.
};

/// Error of one image against the reference. Pixels that show the failure color in either image are left out of
/// every metric and counted instead.
struct ImageError final
{
    float64 MeanAbsolutePercentage = 0; ///< Mean |x - r| / (r + guard) over resolved RGB channels.
    float64 LinearRootMeanSquare = 0;   ///< RMSE of decoded linear radiance over resolved RGB channels.
    float64 DisplayMeanSquare = 0;      ///< Mean squared error of tone-mapped RGB in [0, 1] over every other pixel.
    usize FailurePixels = 0;            ///< Pixels left out because either image shows the failure color.
};

bool IsFailurePixelAt(const Array<uint8>& pixels, usize address)
{
    return pixels[address] == 255 && pixels[address + 1] == 0 && pixels[address + 2] == 255;
}

bool IsResolvedReferencePixel(const Array<uint8>& reference, usize address)
{
    for (usize channel = 0; channel < 3; ++channel)
    {
        if (DecodeToneMapped(reference[address + channel]) > BENCHMARK_MAXIMUM_RESOLVED_RADIANCE)
        {
            return false;
        }
    }
    return true;
}

usize CountResolvedReferencePixels(const Array<uint8>& reference)
{
    usize count = 0;
    for (usize address = 0; address + 3 < reference.GetSize(); address += 4)
    {
        count += IsResolvedReferencePixel(reference, address) == true ? 1 : 0;
    }
    return count;
}

ImageError ComputeImageError(const Array<uint8>& pixels, const Array<uint8>& reference)
{
    REQUIRE(pixels.GetSize() == reference.GetSize());
    constexpr float64 MAXIMUM_CHANNEL = 255;
    float64 percentageSum = 0;
    float64 squareSum = 0;
    float64 displaySquareSum = 0;
    usize channelCount = 0;
    usize displayChannelCount = 0;
    ImageError error;
    for (usize address = 0; address + 3 < pixels.GetSize(); address += 4)
    {
        if (IsFailurePixelAt(pixels, address) == true || IsFailurePixelAt(reference, address) == true)
        {
            ++error.FailurePixels;
            continue;
        }
        for (usize channel = 0; channel < 3; ++channel)
        {
            const float64 difference =
                (static_cast<float64>(pixels[address + channel]) - static_cast<float64>(reference[address + channel])) /
                MAXIMUM_CHANNEL;
            displaySquareSum += difference * difference;
            ++displayChannelCount;
        }
        if (IsResolvedReferencePixel(reference, address) == false)
        {
            continue;
        }
        for (usize channel = 0; channel < 3; ++channel)
        {
            const float64 value = static_cast<float64>(DecodeToneMapped(pixels[address + channel]));
            const float64 expected = static_cast<float64>(DecodeToneMapped(reference[address + channel]));
            percentageSum += math::Abs(value - expected) / (expected + BENCHMARK_PERCENTAGE_GUARD);
            squareSum += (value - expected) * (value - expected);
            ++channelCount;
        }
    }
    REQUIRE(channelCount > 0);
    error.MeanAbsolutePercentage = percentageSum / static_cast<float64>(channelCount);
    error.LinearRootMeanSquare = math::Sqrt(squareSum / static_cast<float64>(channelCount));
    error.DisplayMeanSquare = displaySquareSum / static_cast<float64>(displayChannelCount);
    return error;
}

float64 ComputeMedian(Array<float64>& inoutValues)
{
    REQUIRE(inoutValues.GetSize() > 0);
    Sort(inoutValues.GetData(), inoutValues.GetSize(), [](float64 left, float64 right) noexcept {
        return left < right;
    });
    const usize middle = inoutValues.GetSize() / 2;
    return inoutValues.GetSize() % 2 == 1 ? inoutValues[middle] : (inoutValues[middle - 1] + inoutValues[middle]) / 2;
}

/// Render warmup and timed frames of one benchmark package, with per-pass GPU timing, and read its query counts. On
/// Metal the ray passes share one GPU scope, whose compute operations are the passes' kernels in pass order, with
/// each shade pass's Accumulate and ToneMap after its kernel.
EstimatorCost MeasureEstimatorCost(const GraphGpuFixture& fixture,
                                   const PackageLocation& location,
                                   const EmitterFieldRoom& room,
                                   std::string_view name)
{
    PreparedGraph graph(fixture, location, room, 1, BENCHMARK_EXTENT);
    constexpr usize PRIMARY_KERNEL = 0;
    constexpr usize SHADE_KERNEL = VEACH_GUIBAS_FIRST_SHADE_PASS + VEACH_GUIBAS_MIS;
    Array<float64> primary;
    Array<float64> shade;
    REQUIRE(primary.TryEnsureCapacity(BENCHMARK_TIMED_FRAMES) == true);
    REQUIRE(shade.TryEnsureCapacity(BENCHMARK_TIMED_FRAMES) == true);
    for (usize frame = 0; frame < BENCHMARK_WARMUP_FRAMES + BENCHMARK_TIMED_FRAMES; ++frame)
    {
        REQUIRE(rhi::ArmGpuPassTimings(fixture.Device) == rhi::RasterStatus::Ready);
        RenderFrame(graph.Graph, fixture);
        rhi::GpuPassTimings timings;
        REQUIRE(rhi::GetGpuPassTimings(fixture.Device, timings) == rhi::RasterStatus::Ready);
        REQUIRE(timings.State == rhi::GpuTimingState::Available);
        float64 primaryNanoseconds = -1;
        float64 shadeNanoseconds = -1;
        // The first timed frame's operations, as scope.operation, kind and milliseconds, show what is timed.
        std::string operations;
        usize kernel = 0;
        for (usize index = 0; index < timings.Count; ++index)
        {
            const rhi::GpuPassTiming& pass = timings.Passes[index];
            operations += ' ' + std::to_string(pass.Scope.Identity) + '.' + std::to_string(pass.ScopePassIndex) +
                          (pass.Kind == rhi::GpuPassKind::Compute ? " compute " : " other ") +
                          std::to_string(static_cast<float64>(pass.Timing.DurationNanoseconds) /
                                         BENCHMARK_NANOSECONDS_PER_MILLISECOND) +
                          ';';
            if (pass.Kind != rhi::GpuPassKind::Compute)
            {
                continue;
            }
            REQUIRE(pass.Timing.State == rhi::GpuTimingState::Available);
            const float64 duration = static_cast<float64>(pass.Timing.DurationNanoseconds);
            primaryNanoseconds = kernel == PRIMARY_KERNEL ? duration : primaryNanoseconds;
            shadeNanoseconds = kernel == SHADE_KERNEL ? duration : shadeNanoseconds;
            ++kernel;
        }
        if (frame == BENCHMARK_WARMUP_FRAMES)
        {
            WARN(name << " GPU operations in one frame:" << operations);
        }
        INFO(name << " GPU operations:" << operations);
        REQUIRE(kernel == graph.Plan.Passes.GetSize());
        REQUIRE(primaryNanoseconds >= 0);
        REQUIRE(shadeNanoseconds >= 0);
        if (frame >= BENCHMARK_WARMUP_FRAMES)
        {
            REQUIRE(primary.TryAdd(primaryNanoseconds / BENCHMARK_NANOSECONDS_PER_MILLISECOND) == true);
            REQUIRE(shade.TryAdd(shadeNanoseconds / BENCHMARK_NANOSECONDS_PER_MILLISECOND) == true);
        }
    }
    const StaticArray<uint32, VEACH_GUIBAS_OUTPUT_COUNT> exports = graph.GetVeachGuibasExports();
    Array<uint8> counts;
    ReadExport(graph.Graph, fixture, exports[BENCHMARK_QUERY_COUNT_OUTPUT], graph.Extent, counts);
    REQUIRE(CountFailurePixels(counts) == 0);
    const math::Vector3 meanCounts = ComputeMeanRadiance(counts);
    EstimatorCost cost;
    cost.PrimaryMilliseconds = ComputeMedian(primary);
    cost.ShadeMilliseconds = ComputeMedian(shade);
    cost.ShadowRaysPerPixel = static_cast<float64>(meanCounts.X) / BENCHMARK_QUERY_COUNT_SCALE;
    cost.BsdfRaysPerPixel = static_cast<float64>(meanCounts.Y) / BENCHMARK_QUERY_COUNT_SCALE;
    return cost;
}

/// Veach and Guibas is read at each budget's equal samples and at each RIS variant's equal time.
constexpr usize BENCHMARK_MAXIMUM_CHECKPOINTS = BENCHMARK_SAMPLE_BUDGET_COUNT * (1 + RIS_BENCHMARK_VARIANT_COUNT);

/// Sample counts at which one package's MIS image is read, and the images read there.
struct MisCheckpoints final
{
    StaticArray<usize, BENCHMARK_MAXIMUM_CHECKPOINTS> FrameCounts{};
    usize Count = 0;
    StaticArray<Array<uint8>, BENCHMARK_MAXIMUM_CHECKPOINTS> Images;
};

/// Render one package's MIS output and read it after each checkpoint's number of samples.
void RenderMisCheckpoints(const GraphGpuFixture& fixture,
                          const PackageLocation& location,
                          const EmitterFieldRoom& room,
                          MisCheckpoints& inoutCheckpoints)
{
    PreparedGraph graph(fixture, location, room, 1, BENCHMARK_EXTENT);
    const uint32 output = graph.GetVeachGuibasExports()[VEACH_GUIBAS_MIS];
    usize lastFrame = 0;
    for (usize checkpoint = 0; checkpoint < inoutCheckpoints.Count; ++checkpoint)
    {
        REQUIRE(inoutCheckpoints.FrameCounts[checkpoint] >= 1);
        REQUIRE(inoutCheckpoints.FrameCounts[checkpoint] <= BENCHMARK_MAXIMUM_FRAMES);
        lastFrame = math::Max(lastFrame, inoutCheckpoints.FrameCounts[checkpoint]);
    }
    for (usize frame = 1; frame <= lastFrame; ++frame)
    {
        RenderFrame(graph.Graph, fixture);
        for (usize checkpoint = 0; checkpoint < inoutCheckpoints.Count; ++checkpoint)
        {
            if (inoutCheckpoints.FrameCounts[checkpoint] == frame)
            {
                ReadExport(graph.Graph, fixture, output, graph.Extent, inoutCheckpoints.Images[checkpoint]);
            }
        }
    }
}
} // namespace

TEST_CASE("Measure RIS projected-solid-angle and Veach and Guibas MIS error at equal samples and equal time",
          "[.ris-projected-solid-angle-benchmark]")
{
    SurfaceFinish finish = SurfaceFinish::Diffuse;
    std::string_view finishName = "diffuse";
    SECTION("diffuse surfaces") {}
    SECTION("specular surfaces")
    {
        finish = SurfaceFinish::Specular;
        finishName = "specular";
    }
    GraphGpuFixture fixture(true, false);
    rhi::MeasurementAdapter adapter;
    REQUIRE(rhi::GetMeasurementAdapter(fixture.Device, adapter) == rhi::RasterStatus::Ready);
    const EmitterFieldRoom room(finish);
    // Estimators 0 to RIS_BENCHMARK_VARIANT_COUNT - 1 are the RIS variants; the last is Veach and Guibas. Timing rounds
    // run every RIS variant, Veach and Guibas twice, then the RIS variants in reverse, so drift in machine load or
    // clocks affects all packages alike; each package's cost is the mean of its two rounds' medians. One graph at a
    // time: the device admits a bounded number of ray graph jobs.
    constexpr usize VEACH = RIS_BENCHMARK_VARIANT_COUNT;
    constexpr usize ESTIMATOR_COUNT = RIS_BENCHMARK_VARIANT_COUNT + 1;
    StaticArray<PackageLocation, ESTIMATOR_COUNT> locations{};
    StaticArray<std::string, ESTIMATOR_COUNT> names{};
    for (usize variant = 0; variant < RIS_BENCHMARK_VARIANT_COUNT; ++variant)
    {
        const RisBenchmarkVariant& settings = RIS_BENCHMARK_VARIANTS[variant];
        locations[variant] = settings.Location;
        names[variant] = std::string("ris-projected-solid-angle (") +
                         (settings.Target == 0 ? "exact target" : "form-factor target") +
                         ", M=" + std::to_string(settings.CandidateCount) +
                         ", K=" + std::to_string(settings.NormalizationCount) + ')';
    }
    locations[VEACH] = VEACH_GUIBAS_BENCHMARK;
    names[VEACH] = "veach-guibas-mis";
    StaticArray<EstimatorCost, ESTIMATOR_COUNT> costs{};
    constexpr usize ROUND_COUNT = 2 * ESTIMATOR_COUNT;
    StaticArray<usize, ROUND_COUNT> roundOrder{};
    for (usize variant = 0; variant < RIS_BENCHMARK_VARIANT_COUNT; ++variant)
    {
        roundOrder[variant] = variant;
        roundOrder[ROUND_COUNT - 1 - variant] = variant;
    }
    roundOrder[RIS_BENCHMARK_VARIANT_COUNT] = VEACH;
    roundOrder[RIS_BENCHMARK_VARIANT_COUNT + 1] = VEACH;
    for (const usize estimator : roundOrder)
    {
        const EstimatorCost measured = MeasureEstimatorCost(fixture, locations[estimator], room, names[estimator]);
        costs[estimator].PrimaryMilliseconds += measured.PrimaryMilliseconds / 2;
        costs[estimator].ShadeMilliseconds += measured.ShadeMilliseconds / 2;
        costs[estimator].ShadowRaysPerPixel = measured.ShadowRaysPerPixel;
        costs[estimator].BsdfRaysPerPixel = measured.BsdfRaysPerPixel;
    }
    StaticArray<float64, ESTIMATOR_COUNT> sampleMilliseconds{};
    for (usize estimator = 0; estimator < ESTIMATOR_COUNT; ++estimator)
    {
        sampleMilliseconds[estimator] = costs[estimator].PrimaryMilliseconds + costs[estimator].ShadeMilliseconds;
        REQUIRE(sampleMilliseconds[estimator] > 0);
        WARN(finishName << " room, " << EMITTER_FIELD_TILE_COUNT * 2 << " emissive triangles, "
                        << BENCHMARK_EXTENT.Width << 'x' << BENCHMARK_EXTENT.Height << ", " << adapter.Name << ": "
                        << names[estimator] << " primary kernel " << costs[estimator].PrimaryMilliseconds
                        << " ms, MIS shade kernel " << costs[estimator].ShadeMilliseconds
                        << " ms per sample; shadow rays per pixel " << costs[estimator].ShadowRaysPerPixel
                        << ", BSDF rays per pixel " << costs[estimator].BsdfRaysPerPixel);
    }
    // Only the MIS output is read. Failure pixels would be left out of every metric; none are expected since ComputeBvh
    // barycentrics stay within the bound GetSurfaceHit checks.
    Array<uint8> reference;
    {
        PreparedGraph referenceGraph(fixture,
                                     VEACH_GUIBAS_BENCHMARK,
                                     room,
                                     BENCHMARK_REFERENCE_REVISION,
                                     BENCHMARK_EXTENT);
        const uint32 output = referenceGraph.GetVeachGuibasExports()[VEACH_GUIBAS_MIS];
        for (usize frame = 0; frame < BENCHMARK_REFERENCE_FRAMES; ++frame)
        {
            RenderFrame(referenceGraph.Graph, fixture);
        }
        ReadExport(referenceGraph.Graph, fixture, output, referenceGraph.Extent, reference);
    }
    const usize pixelCount = static_cast<usize>(BENCHMARK_EXTENT.Width) * BENCHMARK_EXTENT.Height;
    const usize resolvedPixels = CountResolvedReferencePixels(reference);
    WARN(finishName << " room reference: veach-guibas-mis MIS at " << BENCHMARK_REFERENCE_FRAMES
                    << " samples per pixel, independent seed; " << CountFailurePixels(reference)
                    << " failure pixels; linear metrics cover " << resolvedPixels << " of " << pixelCount << " pixels");
    CHECK(CountFailurePixels(reference) == 0);
    // Veach and Guibas at each budget's samples, then at each RIS variant's primary and MIS kernel time.
    MisCheckpoints veach;
    for (usize budget = 0; budget < BENCHMARK_SAMPLE_BUDGET_COUNT; ++budget)
    {
        const usize samples = BENCHMARK_SAMPLE_BUDGETS[budget];
        veach.FrameCounts[budget] = samples;
        for (usize variant = 0; variant < RIS_BENCHMARK_VARIANT_COUNT; ++variant)
        {
            const float64 equalTimeSamples =
                static_cast<float64>(samples) * sampleMilliseconds[variant] / sampleMilliseconds[VEACH];
            veach.FrameCounts[BENCHMARK_SAMPLE_BUDGET_COUNT * (1 + variant) + budget] =
                math::Min(math::Max(static_cast<usize>(equalTimeSamples + .5), usize{1}), BENCHMARK_MAXIMUM_FRAMES);
        }
    }
    veach.Count = BENCHMARK_SAMPLE_BUDGET_COUNT * (1 + RIS_BENCHMARK_VARIANT_COUNT);
    RenderMisCheckpoints(fixture, VEACH_GUIBAS_BENCHMARK, room, veach);
    for (usize budget = 0; budget < BENCHMARK_SAMPLE_BUDGET_COUNT; ++budget)
    {
        const ImageError equalSamples = ComputeImageError(veach.Images[budget], reference);
        WARN(finishName << " room, veach-guibas-mis at " << veach.FrameCounts[budget] << " samples: MAPE "
                        << equalSamples.MeanAbsolutePercentage << ", linear RMSE " << equalSamples.LinearRootMeanSquare
                        << ", tone-mapped MSE " << equalSamples.DisplayMeanSquare
                        << "; failure pixels left out: " << equalSamples.FailurePixels);
        CHECK(equalSamples.FailurePixels == 0);
    }
    for (usize variant = 0; variant < RIS_BENCHMARK_VARIANT_COUNT; ++variant)
    {
        MisCheckpoints ris;
        for (usize budget = 0; budget < BENCHMARK_SAMPLE_BUDGET_COUNT; ++budget)
        {
            ris.FrameCounts[budget] = BENCHMARK_SAMPLE_BUDGETS[budget];
        }
        ris.Count = BENCHMARK_SAMPLE_BUDGET_COUNT;
        RenderMisCheckpoints(fixture, locations[variant], room, ris);
        for (usize budget = 0; budget < BENCHMARK_SAMPLE_BUDGET_COUNT; ++budget)
        {
            const ImageError risError = ComputeImageError(ris.Images[budget], reference);
            const usize equalTimeCheckpoint = BENCHMARK_SAMPLE_BUDGET_COUNT * (1 + variant) + budget;
            const ImageError equalTime = ComputeImageError(veach.Images[equalTimeCheckpoint], reference);
            WARN(finishName << " room, " << names[variant] << " at " << ris.FrameCounts[budget] << " samples ("
                            << static_cast<float64>(ris.FrameCounts[budget]) * sampleMilliseconds[variant]
                            << " ms of kernel time): MAPE " << risError.MeanAbsolutePercentage << ", linear RMSE "
                            << risError.LinearRootMeanSquare << ", tone-mapped MSE " << risError.DisplayMeanSquare
                            << "; veach-guibas-mis at equal time (" << veach.FrameCounts[equalTimeCheckpoint]
                            << " samples): MAPE " << equalTime.MeanAbsolutePercentage << ", linear RMSE "
                            << equalTime.LinearRootMeanSquare << ", tone-mapped MSE " << equalTime.DisplayMeanSquare
                            << "; failure pixels left out: " << risError.FailurePixels << ", "
                            << equalTime.FailurePixels);
            CHECK(risError.FailurePixels == 0);
            CHECK(equalTime.FailurePixels == 0);
        }
    }
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

TEST_CASE("Line visibility through the packed BVH equals the linear scan in triangle-loop and native passes",
          "[render-graph][gpu][line-visibility][line-bvh]")
{
    // The default packages walk the packed BVH; the line-scan packages are the same fixture cooked with
    // scene_interface.line_traversal "TriangleLoop" (point-line PL3, design D5). Each set first passes
    // the analytic checks, then every readout byte must agree. The 31- and 32-slat cases pack 62 and
    // 64 occluder triangles into a multi-level tree, and the overflow case must fail under both.
    GraphGpuFixture fixture;
    const StaticArray<PackageLocation, 2> walks{{
        {LUDUS_RG3_LINE_VISIBILITY_LOOP_DIRECTORY, "rg2_line_visibility_loop.lurgrt"},
        {LUDUS_RG3_LINE_VISIBILITY_NATIVE_DIRECTORY, "rg2_line_visibility_native.lurgrt"},
    }};
    const StaticArray<PackageLocation, 2> scans{{
        {LUDUS_RG3_LINE_VISIBILITY_LOOP_LINE_SCAN_DIRECTORY, "rg2_line_visibility_loop_line_scan.lurgrt"},
        {LUDUS_RG3_LINE_VISIBILITY_NATIVE_LINE_SCAN_DIRECTORY, "rg2_line_visibility_native_line_scan.lurgrt"},
    }};
    for (usize strategy = 0; strategy < walks.GetSize(); ++strategy)
    {
        INFO("ray strategy " << (strategy == 0 ? "TriangleLoop" : "NativeQuery"));
        // The line traversal never changes whether a package needs hardware ray tracing (PL06).
        CHECK(renderer::RequiresHardwareRayTracing(LoadPackage(walks[strategy])->GetPlan()) ==
              renderer::RequiresHardwareRayTracing(LoadPackage(scans[strategy])->GetPlan()));
        StaticArray<Array<uint8>, LINE_CASE_COUNT> walkImages;
        CheckLineCases(fixture, walks[strategy], walkImages);
        StaticArray<Array<uint8>, LINE_CASE_COUNT> scanImages;
        CheckLineCases(fixture, scans[strategy], scanImages);
        for (usize index = 0; index < LINE_CASE_COUNT; ++index)
        {
            INFO("case " << index);
            CHECK(CountDifferingPixels(walkImages[index], scanImages[index], 0) == 0);
        }
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

TEST_CASE("The line-sampling template accumulates byte-identical images with the BVH and the line scan",
          "[render-graph][gpu][line-sampling][template][line-bvh]")
{
    // The shipped template walks the packed BVH; the line-scan package is the same template cooked with
    // scene_interface.line_traversal "TriangleLoop" (point-line PL3). Rays are unchanged, and the BVH
    // changes only which triangles the line query visits, so every running mean is byte-identical
    // after one sample and after many. Graphs are prepared one at a time: the device holds two.
    GraphGpuFixture fixture(false, false);
    CornellBox room;
    const StaticArray<PackageLocation, 2> locations{{
        {LUDUS_RG3_LINE_SAMPLING_TEMPLATE_DIRECTORY, "rg2_line_sampling_template.lurgrt"},
        {LUDUS_RG3_LINE_SAMPLING_LINE_SCAN_DIRECTORY, "rg2_line_sampling_line_scan.lurgrt"},
    }};
    constexpr usize FRAME_COUNT = 64;
    StaticArray<StaticArray<Array<uint8>, LINE_SAMPLING_OUTPUT_COUNT>, 2> first;
    StaticArray<StaticArray<Array<uint8>, LINE_SAMPLING_OUTPUT_COUNT>, 2> last;
    for (usize traversal = 0; traversal < locations.GetSize(); ++traversal)
    {
        PreparedGraph graph(fixture, locations[traversal], room, 1);
        REQUIRE(renderer::RequiresHardwareRayTracing(graph.Plan) == false);
        const StaticArray<uint32, LINE_SAMPLING_OUTPUT_COUNT> exports = GetLineSamplingExports(graph.Plan);
        for (usize frame = 0; frame < FRAME_COUNT; ++frame)
        {
            RenderFrame(graph.Graph, fixture);
            for (usize output = 0; output < LINE_SAMPLING_OUTPUT_COUNT && (frame == 0 || frame + 1 == FRAME_COUNT);
                 ++output)
            {
                Array<uint8>& pixels = frame == 0 ? first[traversal][output] : last[traversal][output];
                ReadExport(graph.Graph, fixture, exports[output], {}, pixels);
                REQUIRE(CountFailurePixels(pixels) == 0);
            }
        }
    }
    for (usize output = 0; output < LINE_SAMPLING_OUTPUT_COUNT; ++output)
    {
        INFO("output " << output);
        CHECK(CountDifferingPixels(first[0][output], first[1][output], 0) == 0);
        CHECK(CountDifferingPixels(last[0][output], last[1][output], 0) == 0);
    }
}

namespace
{
/// A large scene for the hidden line traversal measurement, read from the file that
/// LUDUS_LINE_TRAVERSAL_SCENE names: a little-endian uint32 vertex count and index count, then float32
/// x, y, z World positions and uint32 triangle indices, as export_line_traversal_scene.py writes them
/// from a glTF file. A square emitter facing down, lightHalfWidthWorld from its center to each side, is
/// added above the scene's horizontal center, and the camera looks along -X from inside its bounds. A
/// generated floor and ceiling stand in for the file where a test needs a large scene without the
/// reference assets.
struct LargeLineScene final
{
    static constexpr usize MATERIAL_COUNT = 2;
    static constexpr uint32 SURFACE = 0;
    static constexpr uint32 LIGHT = 1;
    static constexpr usize LIGHT_CORNERS = 4;
    static constexpr usize LIGHT_INDEX_COUNT = 6;
    /// The 2 m square light of the line traversal measurement.
    static constexpr float32 DEFAULT_LIGHT_HALF_WIDTH_WORLD = 1;
    Array<math::Vector3> Positions;
    Array<uint32> Indices;
    Array<uint32> Slots;
    StaticArray<uint32, MATERIAL_COUNT> Bindings{{SURFACE, LIGHT}};
    StaticArray<renderer::SceneShaderMaterial, MATERIAL_COUNT> Materials{};
    StaticArray<renderer::RenderGraphMesh, 1> Meshes{};
    StaticArray<renderer::RenderGraphItem, 1> Items{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    renderer::SceneShaderInputs Inputs;

    explicit LargeLineScene(std::string_view path, float32 lightHalfWidthWorld = DEFAULT_LIGHT_HALF_WIDTH_WORLD)
    {
        const usize separator = path.rfind('/');
        REQUIRE(separator != std::string_view::npos);
        filesystem::Directory directory;
        REQUIRE(directory.Open(path.substr(0, separator)).Succeeded() == true);
        filesystem::File file;
        REQUIRE(directory.OpenRead(path.substr(separator + 1), file).Succeeded() == true);
        Array<uint8> bytes;
        REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
        const filesystem::ReadResult read = file.ReadAt(0, bytes.AsSpan());
        REQUIRE(read.Outcome.Succeeded() == true);
        REQUIRE(read.BytesRead == bytes.GetSize());
        constexpr usize WORD_BYTES = 4;
        usize offset = 0;
        const auto readWord = [&bytes, &offset]() {
            uint32 word = 0;
            REQUIRE(TryReadLittleEndian({bytes.GetData() + offset, bytes.GetSize() - offset}, word) == true);
            offset += WORD_BYTES;
            return word;
        };
        const usize vertexCount = readWord();
        const usize indexCount = readWord();
        REQUIRE(indexCount % 3 == 0);
        REQUIRE(bytes.GetSize() == (2 + vertexCount * 3 + indexCount) * WORD_BYTES);
        reserve(vertexCount, indexCount);
        for (usize vertex = 0; vertex < vertexCount; ++vertex)
        {
            StaticArray<float32, 3> coordinates{};
            for (usize axis = 0; axis < 3; ++axis)
            {
                const uint32 bits = readWord();
                std::memcpy(&coordinates[axis], &bits, sizeof(bits));
            }
            Positions[vertex] = {coordinates[0], coordinates[1], coordinates[2]};
        }
        for (usize index = 0; index < indexCount; ++index)
        {
            Indices[index] = readWord();
            REQUIRE(Indices[index] < vertexCount);
        }
        finish(vertexCount, indexCount, lightHalfWidthWorld);
    }
    /// A floor and a ceiling, each a square grid of gridQuads x gridQuads quads: 4 * gridQuads^2 triangles.
    explicit LargeLineScene(uint32 gridQuads)
    {
        constexpr usize GRID_COUNT = 2;
        constexpr float32 GRID_WIDTH_WORLD = 20;
        constexpr float32 CEILING_HEIGHT_WORLD = 4;
        constexpr usize QUAD_INDEX_COUNT = 6;
        const usize rowVertices = usize{gridQuads} + 1;
        const usize gridVertices = rowVertices * rowVertices;
        const usize vertexCount = GRID_COUNT * gridVertices;
        const usize indexCount = GRID_COUNT * usize{gridQuads} * gridQuads * QUAD_INDEX_COUNT;
        reserve(vertexCount, indexCount);
        const float32 spacingWorld = GRID_WIDTH_WORLD / static_cast<float32>(gridQuads);
        usize index = 0;
        for (usize grid = 0; grid < GRID_COUNT; ++grid)
        {
            const bool isCeiling = grid == 1;
            const float32 heightWorld = isCeiling == true ? CEILING_HEIGHT_WORLD : 0;
            const usize first = grid * gridVertices;
            for (usize row = 0; row < rowVertices; ++row)
            {
                for (usize column = 0; column < rowVertices; ++column)
                {
                    Positions[first + row * rowVertices + column] = {static_cast<float32>(column) * spacingWorld,
                                                                     heightWorld,
                                                                     static_cast<float32>(row) * spacingWorld};
                }
            }
            for (usize row = 0; row < gridQuads; ++row)
            {
                for (usize column = 0; column < gridQuads; ++column)
                {
                    const uint32 corner = static_cast<uint32>(first + row * rowVertices + column);
                    const uint32 nextRow = corner + static_cast<uint32>(rowVertices);
                    // The floor faces +Y and the ceiling -Y, both toward the room.
                    const StaticArray<uint32, QUAD_INDEX_COUNT> quad =
                        isCeiling == true ? StaticArray<uint32, QUAD_INDEX_COUNT>{{corner,
                                                                                   corner + 1,
                                                                                   nextRow,
                                                                                   corner + 1,
                                                                                   nextRow + 1,
                                                                                   nextRow}}
                                          : StaticArray<uint32, QUAD_INDEX_COUNT>{
                                                {corner, nextRow, corner + 1, corner + 1, nextRow, nextRow + 1}};
                    for (uint32 vertex : quad)
                    {
                        Indices[index++] = vertex;
                    }
                }
            }
        }
        finish(vertexCount, indexCount, DEFAULT_LIGHT_HALF_WIDTH_WORLD);
    }
    LargeLineScene(const LargeLineScene&) = delete;
    LargeLineScene& operator=(const LargeLineScene&) = delete;

private:
    /// Size the arrays for the scene's vertices and indices plus the added emitter.
    void reserve(usize vertexCount, usize indexCount)
    {
        REQUIRE(Positions.TryResize(vertexCount + LIGHT_CORNERS) == true);
        REQUIRE(Indices.TryResize(indexCount + LIGHT_INDEX_COUNT) == true);
        REQUIRE(Slots.TryResize((indexCount + LIGHT_INDEX_COUNT) / 3) == true);
    }
    /// Add the emitter, materials and camera around the first vertexCount positions.
    void finish(usize vertexCount, usize indexCount, float32 lightHalfWidthWorld)
    {
        math::Vector3 minimumWorld{1e30F, 1e30F, 1e30F};
        math::Vector3 maximumWorld{-1e30F, -1e30F, -1e30F};
        for (usize vertex = 0; vertex < vertexCount; ++vertex)
        {
            const math::Vector3 positionWorld = Positions[vertex];
            minimumWorld = {math::Min(minimumWorld.X, positionWorld.X),
                            math::Min(minimumWorld.Y, positionWorld.Y),
                            math::Min(minimumWorld.Z, positionWorld.Z)};
            maximumWorld = {math::Max(maximumWorld.X, positionWorld.X),
                            math::Max(maximumWorld.Y, positionWorld.Y),
                            math::Max(maximumWorld.Z, positionWorld.Z)};
        }
        for (usize triangle = 0; triangle < indexCount / 3; ++triangle)
        {
            Slots[triangle] = SURFACE;
        }
        // A square light, facing down, at 60% of the scene's height above its horizontal center.
        constexpr float32 LIGHT_HEIGHT_FRACTION = .6F;
        const float32 centerX = (minimumWorld.X + maximumWorld.X) * .5F;
        const float32 centerZ = (minimumWorld.Z + maximumWorld.Z) * .5F;
        const float32 lightY = minimumWorld.Y + (maximumWorld.Y - minimumWorld.Y) * LIGHT_HEIGHT_FRACTION;
        const uint32 firstCorner = static_cast<uint32>(vertexCount);
        Positions[vertexCount] = {centerX - lightHalfWidthWorld, lightY, centerZ - lightHalfWidthWorld};
        // Counterclockwise seen from below, so the one-sided emitter faces -Y.
        Positions[vertexCount + 1] = {centerX + lightHalfWidthWorld, lightY, centerZ - lightHalfWidthWorld};
        Positions[vertexCount + 2] = {centerX + lightHalfWidthWorld, lightY, centerZ + lightHalfWidthWorld};
        Positions[vertexCount + 3] = {centerX - lightHalfWidthWorld, lightY, centerZ + lightHalfWidthWorld};
        const StaticArray<uint32, LIGHT_INDEX_COUNT> corners{{0, 1, 2, 0, 2, 3}};
        for (usize corner = 0; corner < LIGHT_INDEX_COUNT; ++corner)
        {
            Indices[indexCount + corner] = firstCorner + corners[corner];
        }
        Slots[indexCount / 3] = LIGHT;
        Slots[indexCount / 3 + 1] = LIGHT;
        Materials[SURFACE].BaseColor = {.8F, .8F, .8F};
        Materials[LIGHT].BaseColor = {.8F, .8F, .8F};
        Materials[LIGHT].Emission = {50, 50, 50};
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Meshes[0].MaterialSlots = {Slots.GetData(), Slots.GetSize()};
        Meshes[0].MaterialBindings = {Bindings.GetData(), Bindings.GetSize()};
        Items[0].SourceId = 1;
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        // Eye at 15% of the scene's height, a quarter of the X extent in from the +X end, looking along -X.
        constexpr float32 EYE_HEIGHT_FRACTION = .15F;
        constexpr float32 EYE_INSET_FRACTION = .25F;
        const math::Vector3 eyeWorld{maximumWorld.X - (maximumWorld.X - minimumWorld.X) * EYE_INSET_FRACTION,
                                     minimumWorld.Y + (maximumWorld.Y - minimumWorld.Y) * EYE_HEIGHT_FRACTION,
                                     centerZ};
        // View axes: right -Z, up +Y, forward -X; clip depth 0 at the near plane and 1 at the far plane.
        constexpr float32 FOCAL_LENGTH = 1.2F;
        constexpr float32 NEAR_WORLD = .1F;
        constexpr float32 FAR_WORLD = 100;
        constexpr float32 DEPTH_SCALE = FAR_WORLD / (NEAR_WORLD - FAR_WORLD);
        const float32 aspect = static_cast<float32>(WIDTH) / static_cast<float32>(HEIGHT);
        math::Matrix4& worldToClip = Cameras[0].WorldToClip;
        // Rows of the view: x = -(z - eye.z), y = y - eye.y, view z = -forward = +(x - eye.x).
        worldToClip.Columns[0] = {0, 0, DEPTH_SCALE, -1};
        worldToClip.Columns[1] = {0, FOCAL_LENGTH, 0, 0};
        worldToClip.Columns[2] = {-FOCAL_LENGTH / aspect, 0, 0, 0};
        worldToClip.Columns[3] = {FOCAL_LENGTH / aspect * eyeWorld.Z,
                                  -FOCAL_LENGTH * eyeWorld.Y,
                                  DEPTH_SCALE * (NEAR_WORLD - eyeWorld.X),
                                  eyeWorld.X};
        Cameras[0].CameraPosition = eyeWorld;
        WARN("large line scene: " << vertexCount << " vertices, " << indexCount / 3 << " triangles, "
                                  << 2 * lightHalfWidthWorld << " m square light at y=" << lightY << ", eye "
                                  << eyeWorld.X << ',' << eyeWorld.Y << ',' << eyeWorld.Z);
    }
};
} // namespace

TEST_CASE("Measure BVH line queries against the line scan on a large reference scene", "[.line-traversal-performance]")
{
    // A measurement for point-line PL3, not an acceptance threshold. The line-sampling template runs
    // with every ray pass on ComputeBvh, once with the default BVH line walk and once with the line
    // scan, so the two runs differ only in the line traversal: their images must be byte-identical,
    // and their shade passes' GPU times compare one line query per pixel (plus its shading).
    const char* pathOrNull = std::getenv("LUDUS_LINE_TRAVERSAL_SCENE");
    if (pathOrNull == nullptr)
    {
        SKIP("Set LUDUS_LINE_TRAVERSAL_SCENE to an exported scene, for example Intel Sponza.");
    }
    const LargeLineScene scene(pathOrNull);
    // The Editor viewport's device budget (MakeViewportDeviceDescription), so the measurement admits
    // what the Editor admits rather than rhi::StorageLimits' 512 MiB default.
    constexpr uint64 EDITOR_RESOURCE_BYTES = uint64{2} * 1024 * 1024 * 1024;
    GraphGpuFixture fixture(true, false, EDITOR_RESOURCE_BYTES);
    const StaticArray<PackageLocation, 2> locations{{
        {LUDUS_RG3_LINE_SAMPLING_COMPUTE_BVH_DIRECTORY, "rg2_line_sampling_compute_bvh.lurgrt"},
        {LUDUS_RG3_LINE_SAMPLING_COMPUTE_BVH_LINE_SCAN_DIRECTORY, "rg2_line_sampling_compute_bvh_line_scan.lurgrt"},
    }};
    const StaticArray<const char*, 2> names{{"BVH", "scan"}};
    constexpr usize FRAME_COUNT = 8;
    constexpr usize PASS_COUNT = LINE_SAMPLING_FIRST_SHADE_PASS + LINE_SAMPLING_OUTPUT_COUNT;
    StaticArray<StaticArray<Array<uint8>, LINE_SAMPLING_OUTPUT_COUNT>, 2> pixels;
    StaticArray<StaticArray<float64, PASS_COUNT>, 2> passNanoseconds{};
    for (usize traversal = 0; traversal < locations.GetSize(); ++traversal)
    {
        PreparedGraph graph(fixture, locations[traversal], scene, 1);
        REQUIRE(renderer::RequiresHardwareRayTracing(graph.Plan) == false);
        usize timedFrames = 0;
        for (usize frame = 0; frame < FRAME_COUNT; ++frame)
        {
            REQUIRE(rhi::ArmGpuPassTimings(fixture.Device) == rhi::RasterStatus::Ready);
            RenderFrame(graph.Graph, fixture);
            rhi::GpuPassTimings timings;
            if (rhi::GetGpuPassTimings(fixture.Device, timings) != rhi::RasterStatus::Ready)
            {
                continue;
            }
            timedFrames += 1;
            // The ordered sequence's Compute operations are its passes in plan order.
            usize computePass = 0;
            for (usize pass = 0; pass < timings.Count; ++pass)
            {
                if (timings.Passes[pass].Kind != rhi::GpuPassKind::Compute)
                {
                    continue;
                }
                REQUIRE(computePass < PASS_COUNT);
                if (timings.Passes[pass].Timing.State == rhi::GpuTimingState::Available)
                {
                    passNanoseconds[traversal][computePass] +=
                        static_cast<float64>(timings.Passes[pass].Timing.DurationNanoseconds);
                }
                computePass += 1;
            }
            REQUIRE(computePass == PASS_COUNT);
        }
        REQUIRE(timedFrames > 0);
        const StaticArray<uint32, LINE_SAMPLING_OUTPUT_COUNT> exports = GetLineSamplingExports(graph.Plan);
        for (usize output = 0; output < LINE_SAMPLING_OUTPUT_COUNT; ++output)
        {
            ReadExport(graph.Graph, fixture, exports[output], {}, pixels[traversal][output]);
            // Parity over a dark image would say little, so report how much of it is lit.
            usize litPixels = 0;
            for (usize address = 0; address + 3 < pixels[traversal][output].GetSize(); address += 4)
            {
                litPixels += pixels[traversal][output][address] != 0 ? 1 : 0;
            }
            const math::Vector3 mean = ComputeMeanRadiance(pixels[traversal][output]);
            WARN(names[traversal] << " output " << output << ": " << CountFailurePixels(pixels[traversal][output])
                                  << " failure pixels, " << litPixels << " of " << WIDTH * HEIGHT
                                  << " pixels with nonzero red, mean " << mean.X << ',' << mean.Y << ',' << mean.Z);
        }
        constexpr float64 NANOSECONDS_PER_MILLISECOND = 1'000'000;
        for (usize pass = 0; pass < PASS_COUNT; ++pass)
        {
            passNanoseconds[traversal][pass] /= static_cast<float64>(timedFrames);
            WARN(names[traversal] << " pass " << pass << " mean GPU ms per frame over " << timedFrames << " frames at "
                                  << WIDTH << 'x' << HEIGHT << ": "
                                  << passNanoseconds[traversal][pass] / NANOSECONDS_PER_MILLISECOND);
        }
    }
    for (usize output = 0; output < LINE_SAMPLING_OUTPUT_COUNT; ++output)
    {
        INFO("output " << output);
        CHECK(CountFailurePixels(pixels[0][output]) == 0);
        CHECK(CountDifferingPixels(pixels[0][output], pixels[1][output], 0) == 0);
    }
}

TEST_CASE("Ray passes that read the same scene data upload and charge one packed copy of the scene",
          "[render-graph][gpu][line-sampling][scene-capacity]")
{
    // The ComputeBvh line-sampling template has three ray passes over the same scene data. Each pass
    // used to pack and upload its own copy of the triangles and BVH, so every scene byte budget held
    // a third of the scene it should: Intel Sponza's first million triangles needed three copies of
    // 321,936,656 bytes against rhi::StorageLimits' 512 MiB default and failed CapacityExceeded. The
    // passes now share the first pass's copy, so the graph's logical RHI bytes grow by one copy.
    constexpr uint32 GRID_QUADS = 160; // 102,400 triangles
    const LargeLineScene scene(GRID_QUADS);
    GraphGpuFixture fixture(false, false);
    rhi::StorageUsage before;
    REQUIRE(rhi::GetStorageUsage(fixture.Device, before) == rhi::RasterStatus::Ready);
    PreparedGraph graph(fixture,
                        {LUDUS_RG3_LINE_SAMPLING_COMPUTE_BVH_DIRECTORY, "rg2_line_sampling_compute_bvh.lurgrt"},
                        scene,
                        1);
    REQUIRE(renderer::RequiresHardwareRayTracing(graph.Plan) == false);
    const StaticArray<uint32, LINE_SAMPLING_OUTPUT_COUNT> exports = GetLineSamplingExports(graph.Plan);
    rhi::StorageUsage after;
    REQUIRE(rhi::GetStorageUsage(fixture.Device, after) == rhi::RasterStatus::Ready);
    // Each packed triangle takes LUDUS_GPU_PREVIEW_TRIANGLE_WORDS float4 words before its BVH nodes,
    // order word and the scene's surfaces, so this is a lower bound on one copy. The graph's targets,
    // accumulation and LTC table take a few MiB, well under one copy of this scene.
    const uint64 copyLowerBound =
        uint64{scene.Indices.GetSize() / 3} * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS * sizeof(math::Vector4);
    const uint64 graphBytes = after.ResourceBytes - before.ResourceBytes;
    INFO("graph RHI bytes " << graphBytes << ", one packed copy is at least " << copyLowerBound);
    CHECK(graphBytes >= copyLowerBound);
    CHECK(graphBytes < 2 * copyLowerBound);
    // Every pass reads the shared copy: each output renders without failure pixels.
    RenderFrame(graph.Graph, fixture);
    for (usize output = 0; output < LINE_SAMPLING_OUTPUT_COUNT; ++output)
    {
        INFO("output " << output);
        Array<uint8> pixels;
        ReadExport(graph.Graph, fixture, exports[output], {}, pixels);
        CHECK(CountFailurePixels(pixels) == 0);
    }
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
constexpr usize POINT_LINE_VERTEX_CHORD_ROW = 9;
constexpr usize POINT_LINE_VERTEX_CHORD_CASES = 6;
constexpr float64 POINT_LINE_QUERYABLE_CHORD = .25;
constexpr float64 POINT_LINE_DEGENERATE_CHORD = .5;
constexpr float64 POINT_LINE_POSITIVE_DENSITY = .75;
/// The vertex chord case along the first edge through vertex 2, whose chord always has zero length.
constexpr usize POINT_LINE_U_VERTEX_2_CHORD = 4;
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
    StaticArray<rhi::GpuPassTiming, rhi::MAX_GPU_TIMED_PASSES> Passes{};
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
            timingsOrNull->Passes[pass] = timings.Passes[pass];
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
    WARN("MIS mean=" << mis.X << ',' << mis.Y << ',' << mis.Z << ", line sampling mean=" << lines.X << ',' << lines.Y
                     << ',' << lines.Z << ", point sampling mean=" << points.X << ',' << points.Y << ',' << points.Z
                     << ", Veach and Guibas light sampling mean=" << lightSampling.X << ',' << lightSampling.Y << ','
                     << lightSampling.Z);
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
    WARN("largest weight-sum error=" << largestSumError << ", largest single-weight error=" << largestWeightError
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

    // Chords through a light triangle's vertices (design D6, section 5.1). IsEmitterChordQueryable rejects
    // the zero-length chord along the first edge through vertex 2, and any other vertex chord that
    // rounding leaves shorter than SCENE_UNIT_TOLERANCE. Such a chord latched a shader failure,
    // which the failure-pixel check above would now catch. On Sponza, the area point at vertex 0
    // did this across an acute angle. A rejected chord's density is zero.
    constexpr float64 CHORD_FLAG_TOLERANCE = 1e-6;
    usize degenerateChords = 0;
    for (usize chordCase = 0; chordCase < POINT_LINE_VERTEX_CHORD_CASES; ++chordCase)
    {
        const float64 flag = DecodeLineFixed(pixels, chordCase * 2, POINT_LINE_VERTEX_CHORD_ROW);
        const float64 density = DecodeLineFixed(pixels, chordCase * 2 + 1, POINT_LINE_VERTEX_CHORD_ROW);
        const bool isDegenerate = math::Abs(flag - POINT_LINE_DEGENERATE_CHORD) <= CHORD_FLAG_TOLERANCE;
        INFO("vertex chord case " << chordCase << ": flag " << flag << ", density flag " << density);
        CHECK((isDegenerate == true || math::Abs(flag - POINT_LINE_QUERYABLE_CHORD) <= CHORD_FLAG_TOLERANCE));
        CHECK((density == 0 || math::Abs(density - POINT_LINE_POSITIVE_DENSITY) <= CHORD_FLAG_TOLERANCE));
        if (isDegenerate == true)
        {
            CHECK(density == 0);
        }
        CHECK((chordCase != POINT_LINE_U_VERTEX_2_CHORD || isDegenerate == true));
        degenerateChords += isDegenerate == true ? 1 : 0;
    }
    WARN(degenerateChords << " of " << POINT_LINE_VERTEX_CHORD_CASES << " vertex chords rejected");
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
    // The penumbra: pixels below eye height (the floor and lower walls, where the block's shadow falls)
    // whose mean visible chord fraction, from the line-sampling template, lies strictly between umbra
    // (0) and full light (1). The light and ceiling project above eye height: pixels at the light's edge
    // mix emission with the dark ceiling, and one tone-mapped byte there decodes to a large radiance.
    // The graph is released before the template's two graphs are prepared: the device holds two at once.
    Array<uint8> fractionPixels;
    {
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
        ReadExport(fraction.Graph, fixture, fractionExports[LINE_SAMPLING_VISIBLE_FRACTION], {}, fractionPixels);
    }
    Array<bool> penumbra;
    REQUIRE(penumbra.TryResize(fractionPixels.GetSize() / 4) == true);
    constexpr float32 PENUMBRA_LOWEST_FRACTION = .1F;
    constexpr float32 PENUMBRA_HIGHEST_FRACTION = .9F;
    usize penumbraPixels = 0;
    for (usize pixel = 0; pixel < penumbra.GetSize(); ++pixel)
    {
        const float32 visible = DecodeToneMapped(fractionPixels[pixel * 4]);
        penumbra[pixel] =
            pixel / WIDTH >= HEIGHT / 2 && visible > PENUMBRA_LOWEST_FRACTION && visible < PENUMBRA_HIGHEST_FRACTION;
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
    WARN("point-and-line penumbra (" << penumbraPixels << " pixels) mean squared error at " << LOW_FRAME_COUNT
                                     << " samples: MIS=" << misError << ", line sampling=" << lineError
                                     << ", point sampling=" << pointError);
    // Equal samples, not equal time: line samples cost line queries, so the mean GPU time of every timed
    // operation is reported beside the errors and supports no speed claim.
    constexpr float64 NANOSECONDS_PER_MILLISECOND = 1'000'000;
    REQUIRE(timings.FrameCount > 0);
    for (usize pass = 0; pass < timings.Count; ++pass)
    {
        const rhi::GpuPassTiming& timing = timings.Passes[pass];
        WARN("mean GPU ms per frame over "
             << timings.FrameCount << " frames: " << timing.Scope.Name.GetData() << " operation "
             << timing.ScopePassIndex << " (kind " << static_cast<uint32>(timing.Kind) << ")="
             << timings.Nanoseconds[pass] / static_cast<float64>(timings.FrameCount) / NANOSECONDS_PER_MILLISECOND);
    }
    CHECK(misError < pointError);
}

TEST_CASE("The point-and-line MIS template accumulates byte-identical images with the BVH and the line scan",
          "[render-graph][gpu][point-line][template][line-bvh]")
{
    // As for the line-sampling template: the line-scan package differs only in
    // scene_interface.line_traversal "TriangleLoop" (point-line PL3), so all three outputs, including
    // the BSDF and point samples that share their passes with line queries, must match byte for byte.
    GraphGpuFixture fixture(false, false);
    CornellBox room;
    const StaticArray<PackageLocation, 2> locations{{
        {LUDUS_RG3_SALESIN_JAROSZ_LINES_TEMPLATE_DIRECTORY, "rg2_salesin_jarosz_lines_template.lurgrt"},
        {LUDUS_RG3_SALESIN_JAROSZ_LINES_LINE_SCAN_DIRECTORY, "rg2_salesin_jarosz_lines_line_scan.lurgrt"},
    }};
    constexpr usize SINGLE_FRAME_COUNT = 1;
    constexpr usize FRAME_COUNT = 64;
    StaticArray<StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT>, 2> first;
    StaticArray<StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT>, 2> last;
    for (usize traversal = 0; traversal < locations.GetSize(); ++traversal)
    {
        PreparedGraph graph(fixture, locations[traversal], room, 1);
        REQUIRE(renderer::RequiresHardwareRayTracing(graph.Plan) == false);
        RenderPointLineOutputs(graph, fixture, SINGLE_FRAME_COUNT, first[traversal]);
        RenderPointLineOutputs(graph, fixture, FRAME_COUNT - SINGLE_FRAME_COUNT, last[traversal]);
    }
    for (usize output = 0; output < POINT_LINE_OUTPUT_COUNT; ++output)
    {
        INFO("output " << output);
        CHECK(CountDifferingPixels(first[0][output], first[1][output], 0) == 0);
        CHECK(CountDifferingPixels(last[0][output], last[1][output], 0) == 0);
    }
}

TEST_CASE("Measure point-and-line MIS against point sampling in the penumbra at equal GPU time",
          "[.point-line-performance]")
{
    // A measurement for point-line PL3, not an acceptance threshold: GPU time varies with machine load.
    // Each estimator alone needs the primary pass and its own shading pass per sample, so point
    // sampling receives round(colorFrames * (primary + shade) / (primary + shadePoints)) samples, and
    // both are compared with the independently seeded 1024-sample reference over the PL2 penumbra.
    GraphGpuFixture fixture(true, false);
    CornellBox room;
    Array<uint8> fractionPixels;
    {
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
        ReadExport(fraction.Graph, fixture, fractionExports[LINE_SAMPLING_VISIBLE_FRACTION], {}, fractionPixels);
    }
    Array<bool> penumbra;
    REQUIRE(penumbra.TryResize(fractionPixels.GetSize() / 4) == true);
    constexpr float32 PENUMBRA_LOWEST_FRACTION = .1F;
    constexpr float32 PENUMBRA_HIGHEST_FRACTION = .9F;
    usize penumbraPixels = 0;
    for (usize pixel = 0; pixel < penumbra.GetSize(); ++pixel)
    {
        const float32 visible = DecodeToneMapped(fractionPixels[pixel * 4]);
        penumbra[pixel] =
            pixel / WIDTH >= HEIGHT / 2 && visible > PENUMBRA_LOWEST_FRACTION && visible < PENUMBRA_HIGHEST_FRACTION;
        penumbraPixels += penumbra[pixel] == true ? 1 : 0;
    }
    constexpr usize MINIMUM_PENUMBRA_PIXELS = 32;
    REQUIRE(penumbraPixels >= MINIMUM_PENUMBRA_PIXELS);

    const PackageLocation location{LUDUS_RG3_SALESIN_JAROSZ_LINES_TEMPLATE_DIRECTORY,
                                   "rg2_salesin_jarosz_lines_template.lurgrt"};
    // Mean GPU time per frame of each pass. The ordered sequence is one timing scope whose Compute
    // operations are its passes in plan order; its Raster operations write the color targets.
    constexpr usize TIMING_FRAME_COUNT = 64;
    constexpr usize PASS_COUNT = POINT_LINE_FIRST_SHADE_PASS + POINT_LINE_OUTPUT_COUNT;
    const StaticArray<PackageLocation, 2> timedLocations{{
        location,
        {LUDUS_RG3_SALESIN_JAROSZ_LINES_LINE_SCAN_DIRECTORY, "rg2_salesin_jarosz_lines_line_scan.lurgrt"},
    }};
    StaticArray<StaticArray<float64, PASS_COUNT>, 2> traversalNanoseconds{};
    for (usize traversal = 0; traversal < timedLocations.GetSize(); ++traversal)
    {
        PreparedGraph timed(fixture, timedLocations[traversal], room, 1);
        StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT> ignored;
        PassTimings timings;
        RenderPointLineOutputs(timed, fixture, TIMING_FRAME_COUNT, ignored, &timings);
        REQUIRE(timings.FrameCount > 0);
        usize computePass = 0;
        for (usize pass = 0; pass < timings.Count; ++pass)
        {
            if (timings.Passes[pass].Kind != rhi::GpuPassKind::Compute)
            {
                continue;
            }
            REQUIRE(computePass < PASS_COUNT);
            traversalNanoseconds[traversal][computePass] =
                timings.Nanoseconds[pass] / static_cast<float64>(timings.FrameCount);
            WARN((traversal == 0 ? "BVH" : "scan")
                 << " pass " << computePass << " mean GPU ns per frame over " << timings.FrameCount
                 << " frames: " << traversalNanoseconds[traversal][computePass]);
            computePass += 1;
        }
        REQUIRE(computePass == PASS_COUNT);
    }
    const StaticArray<float64, PASS_COUNT>& passNanoseconds = traversalNanoseconds[0];
    const float64 primary = passNanoseconds[0];
    const float64 colorCost = primary + passNanoseconds[POINT_LINE_FIRST_SHADE_PASS + POINT_LINE_MIS];
    const float64 pointCost = primary + passNanoseconds[POINT_LINE_FIRST_SHADE_PASS + POINT_LINE_POINT_SAMPLING];
    REQUIRE(colorCost > 0);
    REQUIRE(pointCost > 0);
    constexpr usize COLOR_FRAME_COUNT = 16;
    constexpr float64 MAXIMUM_POINT_FRAMES = 1024;
    const usize pointFrameCount = static_cast<usize>(
        math::Min(MAXIMUM_POINT_FRAMES,
                  math::Max(1.0, static_cast<float64>(COLOR_FRAME_COUNT) * colorCost / pointCost + .5)));
    // One run: the running means after the fewer frames, then after the more.
    const usize fewerFrameCount = math::Min(COLOR_FRAME_COUNT, pointFrameCount);
    const usize moreFrameCount = math::Max(COLOR_FRAME_COUNT, pointFrameCount);
    StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT> atFewerFrames;
    StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT> atMoreFrames;
    {
        PreparedGraph graph(fixture, location, room, 1);
        RenderPointLineOutputs(graph, fixture, fewerFrameCount, atFewerFrames);
        RenderPointLineOutputs(graph, fixture, moreFrameCount - fewerFrameCount, atMoreFrames);
    }
    const StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT>& atColorFrames =
        COLOR_FRAME_COUNT == fewerFrameCount ? atFewerFrames : atMoreFrames;
    const StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT>& atPointFrames =
        pointFrameCount == moreFrameCount ? atMoreFrames : atFewerFrames;
    StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT> reference;
    {
        constexpr uint64 REFERENCE_REVISION = 2;
        PreparedGraph referenceGraph(fixture, location, room, REFERENCE_REVISION);
        constexpr usize REFERENCE_FRAME_COUNT = 1024;
        RenderPointLineOutputs(referenceGraph, fixture, REFERENCE_FRAME_COUNT, reference);
    }
    const float64 misError =
        ComputeMaskedMeanSquaredError(atColorFrames[POINT_LINE_MIS], reference[POINT_LINE_MIS], penumbra);
    const float64 pointError =
        ComputeMaskedMeanSquaredError(atPointFrames[POINT_LINE_POINT_SAMPLING], reference[POINT_LINE_MIS], penumbra);
    const float64 equalSamplePointError =
        ComputeMaskedMeanSquaredError(atColorFrames[POINT_LINE_POINT_SAMPLING], reference[POINT_LINE_MIS], penumbra);
    WARN("equal GPU time over " << penumbraPixels << " penumbra pixels: primary " << primary << " ns, MIS sample "
                                << colorCost << " ns, point sample " << pointCost << " ns; MIS at " << COLOR_FRAME_COUNT
                                << " samples MSE=" << misError << ", point sampling at " << pointFrameCount
                                << " samples MSE=" << pointError << " (at " << COLOR_FRAME_COUNT << " samples "
                                << equalSamplePointError << ")");
}

TEST_CASE("Measure point-and-line MIS against point sampling in a large scene's penumbra at equal GPU time",
          "[.point-line-large-scene-performance]")
{
    // A point-line measurement after PL3, not an acceptance threshold: GPU time varies with machine load
    // and clock state. PL3's equal-time comparison ([.point-line-performance]) on the scene of the line
    // traversal measurement, which LUDUS_LINE_TRAVERSAL_SCENE names, lit by a 6 m square light so that
    // its occluders cast wide penumbrae. Both templates run with every ray pass on ComputeBvh. The
    // penumbra is PL2's: pixels below eye height whose mean visible chord fraction, from the
    // line-sampling template, lies strictly between 0.1 and 0.9. The rest of the lower half is reported
    // separately. For each color budget, point sampling receives
    // round(colorFrames * (primary + shade) / (primary + shadePoints)) samples.
    const char* pathOrNull = std::getenv("LUDUS_LINE_TRAVERSAL_SCENE");
    if (pathOrNull == nullptr)
    {
        SKIP("Set LUDUS_LINE_TRAVERSAL_SCENE to an exported scene, for example Intel Sponza.");
    }
    constexpr float32 LIGHT_HALF_WIDTH_WORLD = 3;
    const LargeLineScene scene(pathOrNull, LIGHT_HALF_WIDTH_WORLD);
    // The Editor viewport's device budget, as in the line traversal measurement.
    constexpr uint64 EDITOR_RESOURCE_BYTES = uint64{2} * 1024 * 1024 * 1024;
    GraphGpuFixture fixture(true, false, EDITOR_RESOURCE_BYTES);
    // The graph is released before the template's graphs are prepared: the device holds two at once.
    Array<uint8> fractionPixels;
    {
        PreparedGraph fraction(fixture,
                               {LUDUS_RG3_LINE_SAMPLING_COMPUTE_BVH_DIRECTORY, "rg2_line_sampling_compute_bvh.lurgrt"},
                               scene,
                               1);
        REQUIRE(renderer::RequiresHardwareRayTracing(fraction.Plan) == false);
        constexpr usize FRACTION_FRAME_COUNT = 256;
        for (usize frame = 0; frame < FRACTION_FRAME_COUNT; ++frame)
        {
            RenderFrame(fraction.Graph, fixture);
        }
        const StaticArray<uint32, LINE_SAMPLING_OUTPUT_COUNT> fractionExports = GetLineSamplingExports(fraction.Plan);
        ReadExport(fraction.Graph, fixture, fractionExports[LINE_SAMPLING_VISIBLE_FRACTION], {}, fractionPixels);
    }
    constexpr usize MASK_COUNT = 2;
    constexpr usize PENUMBRA = 0;
    constexpr usize OUTSIDE_PENUMBRA = 1;
    const StaticArray<const char*, MASK_COUNT> maskNames{{"penumbra", "outside the penumbra"}};
    StaticArray<Array<bool>, MASK_COUNT> masks;
    StaticArray<usize, MASK_COUNT> maskPixels{};
    for (usize mask = 0; mask < MASK_COUNT; ++mask)
    {
        REQUIRE(masks[mask].TryResize(fractionPixels.GetSize() / 4) == true);
    }
    constexpr float32 PENUMBRA_LOWEST_FRACTION = .1F;
    constexpr float32 PENUMBRA_HIGHEST_FRACTION = .9F;
    for (usize pixel = 0; pixel < masks[PENUMBRA].GetSize(); ++pixel)
    {
        const float32 visible = DecodeToneMapped(fractionPixels[pixel * 4]);
        const bool isBelowEye = pixel / WIDTH >= HEIGHT / 2;
        const bool isPenumbra = visible > PENUMBRA_LOWEST_FRACTION && visible < PENUMBRA_HIGHEST_FRACTION;
        // A visible fraction showing the error color says nothing about the penumbra.
        const bool isFailure = fractionPixels[pixel * 4] == 255 && fractionPixels[pixel * 4 + 1] == 0 &&
                               fractionPixels[pixel * 4 + 2] == 255;
        masks[PENUMBRA][pixel] = isBelowEye == true && isFailure == false && isPenumbra == true;
        masks[OUTSIDE_PENUMBRA][pixel] = isBelowEye == true && isFailure == false && isPenumbra == false;
        for (usize mask = 0; mask < MASK_COUNT; ++mask)
        {
            maskPixels[mask] += masks[mask][pixel] == true ? 1 : 0;
        }
    }
    constexpr usize MINIMUM_PENUMBRA_PIXELS = 32;
    REQUIRE(maskPixels[PENUMBRA] >= MINIMUM_PENUMBRA_PIXELS);
    REQUIRE(maskPixels[OUTSIDE_PENUMBRA] > 0);
    WARN(maskPixels[PENUMBRA] << " penumbra pixels and " << maskPixels[OUTSIDE_PENUMBRA]
                              << " other pixels below eye height, of " << WIDTH * HEIGHT << "; "
                              << CountFailurePixels(fractionPixels) << " visible fraction failure pixels");

    const PackageLocation location{LUDUS_RG3_SALESIN_JAROSZ_LINES_COMPUTE_BVH_DIRECTORY,
                                   "rg2_salesin_jarosz_lines_compute_bvh.lurgrt"};
    // Mean GPU time per frame of each pass: the ordered sequence's Compute operations in plan order.
    constexpr usize TIMING_FRAME_COUNT = 64;
    constexpr usize PASS_COUNT = POINT_LINE_FIRST_SHADE_PASS + POINT_LINE_OUTPUT_COUNT;
    StaticArray<float64, PASS_COUNT> passNanoseconds{};
    {
        PreparedGraph timed(fixture, location, scene, 1);
        REQUIRE(renderer::RequiresHardwareRayTracing(timed.Plan) == false);
        StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT> ignored;
        PassTimings timings;
        RenderPointLineOutputs(timed, fixture, TIMING_FRAME_COUNT, ignored, &timings);
        REQUIRE(timings.FrameCount > 0);
        usize computePass = 0;
        for (usize pass = 0; pass < timings.Count; ++pass)
        {
            if (timings.Passes[pass].Kind != rhi::GpuPassKind::Compute)
            {
                continue;
            }
            REQUIRE(computePass < PASS_COUNT);
            passNanoseconds[computePass] = timings.Nanoseconds[pass] / static_cast<float64>(timings.FrameCount);
            WARN("pass " << computePass << " mean GPU ns per frame over " << timings.FrameCount
                         << " frames: " << passNanoseconds[computePass]);
            computePass += 1;
        }
        REQUIRE(computePass == PASS_COUNT);
    }
    const float64 primary = passNanoseconds[0];
    const float64 colorCost = primary + passNanoseconds[POINT_LINE_FIRST_SHADE_PASS + POINT_LINE_MIS];
    const float64 pointCost = primary + passNanoseconds[POINT_LINE_FIRST_SHADE_PASS + POINT_LINE_POINT_SAMPLING];
    REQUIRE(colorCost > 0);
    REQUIRE(pointCost > 0);
    WARN("GPU ns per sample: primary " << primary << ", color (primary + shade) " << colorCost
                                       << ", pointSampling (primary + shadePoints) " << pointCost << ", ratio "
                                       << colorCost / pointCost);

    // A failed sample shows the error color in its output. At this scale a few pixels can fail over
    // thousands of samples, so failures are counted per output and their pixels leave both masks
    // instead of ending the measurement.
    const auto renderOutputs = [&fixture](PreparedGraph& inoutGraph,
                                          usize frameCount,
                                          StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT>& outPixels) {
        const StaticArray<uint32, POINT_LINE_OUTPUT_COUNT> exports = GetPointLineExports(inoutGraph.Plan);
        for (usize frame = 0; frame < frameCount; ++frame)
        {
            RenderFrame(inoutGraph.Graph, fixture);
        }
        for (usize output = 0; output < POINT_LINE_OUTPUT_COUNT; ++output)
        {
            ReadExport(inoutGraph.Graph, fixture, exports[output], {}, outPixels[output]);
        }
    };

    // Two color budgets, each with its equal-time point budget, read from one run in increasing order.
    constexpr usize BUDGET_COUNT = 2;
    const StaticArray<usize, BUDGET_COUNT> colorFrameCounts{{16, 64}};
    StaticArray<usize, BUDGET_COUNT> pointFrameCounts{};
    constexpr float64 MAXIMUM_POINT_FRAMES = 4096;
    constexpr usize CHECKPOINT_COUNT = 2 * BUDGET_COUNT;
    StaticArray<usize, CHECKPOINT_COUNT> checkpoints{};
    for (usize budget = 0; budget < BUDGET_COUNT; ++budget)
    {
        pointFrameCounts[budget] = static_cast<usize>(
            math::Min(MAXIMUM_POINT_FRAMES,
                      math::Max(1.0, static_cast<float64>(colorFrameCounts[budget]) * colorCost / pointCost + .5)));
        checkpoints[budget * 2] = colorFrameCounts[budget];
        checkpoints[budget * 2 + 1] = pointFrameCounts[budget];
    }
    Sort(checkpoints.GetData(), checkpoints.GetSize(), [](usize left, usize right) noexcept { return left < right; });
    StaticArray<StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT>, CHECKPOINT_COUNT> atCheckpoints;
    {
        PreparedGraph graph(fixture, location, scene, 1);
        usize renderedFrames = 0;
        for (usize checkpoint = 0; checkpoint < CHECKPOINT_COUNT; ++checkpoint)
        {
            // A repeated frame count renders no frame and reads the same running means again.
            renderOutputs(graph, checkpoints[checkpoint] - renderedFrames, atCheckpoints[checkpoint]);
            renderedFrames = checkpoints[checkpoint];
        }
    }
    const auto getOutputsAt = [&checkpoints, &atCheckpoints](usize frameCount) {
        usize found = 0;
        while (checkpoints[found] != frameCount)
        {
            ++found;
            REQUIRE(found < CHECKPOINT_COUNT);
        }
        return &atCheckpoints[found];
    };

    // The reference: the mean decoded color of independently seeded graph instances, whose scene
    // revisions reseed sampling. A ray pass stops accumulating at MAX_ACCUMULATION_SAMPLES, so each
    // instance contributes that many samples. The spread between instances estimates the noise that
    // the reference adds to every error below: the sample variance over REFERENCE_COUNT.
    constexpr usize REFERENCE_COUNT = 8;
    constexpr uint64 FIRST_REFERENCE_REVISION = 2;
    constexpr usize REFERENCE_FRAME_COUNT = format::MAX_ACCUMULATION_SAMPLES;
    const usize valueCount = masks[PENUMBRA].GetSize() * 3;
    Array<float64> referenceSum;
    Array<float64> referenceSquareSum;
    REQUIRE(referenceSum.TryResize(valueCount) == true);
    REQUIRE(referenceSquareSum.TryResize(valueCount) == true);
    for (usize value = 0; value < valueCount; ++value)
    {
        referenceSum[value] = 0;
        referenceSquareSum[value] = 0;
    }
    const StaticArray<const char*, POINT_LINE_OUTPUT_COUNT> outputNames{{"color", "lineSampling", "pointSampling"}};
    StaticArray<usize, POINT_LINE_OUTPUT_COUNT> referenceFailurePixels{};
    const auto removeFailurePixels = [&masks](const Array<uint8>& pixels) {
        for (usize pixel = 0; pixel < masks[PENUMBRA].GetSize(); ++pixel)
        {
            const bool isFailure =
                pixels[pixel * 4] == 255 && pixels[pixel * 4 + 1] == 0 && pixels[pixel * 4 + 2] == 255;
            for (usize mask = 0; mask < MASK_COUNT; ++mask)
            {
                masks[mask][pixel] = masks[mask][pixel] == true && isFailure == false;
            }
        }
    };
    for (usize reference = 0; reference < REFERENCE_COUNT; ++reference)
    {
        StaticArray<Array<uint8>, POINT_LINE_OUTPUT_COUNT> referencePixels;
        {
            PreparedGraph referenceGraph(fixture, location, scene, FIRST_REFERENCE_REVISION + reference);
            renderOutputs(referenceGraph, REFERENCE_FRAME_COUNT, referencePixels);
        }
        for (usize output = 0; output < POINT_LINE_OUTPUT_COUNT; ++output)
        {
            referenceFailurePixels[output] += CountFailurePixels(referencePixels[output]);
        }
        removeFailurePixels(referencePixels[POINT_LINE_MIS]);
        for (usize pixel = 0; pixel < masks[PENUMBRA].GetSize(); ++pixel)
        {
            for (usize channel = 0; channel < 3; ++channel)
            {
                const float64 radiance =
                    static_cast<float64>(DecodeToneMapped(referencePixels[POINT_LINE_MIS][pixel * 4 + channel]));
                referenceSum[pixel * 3 + channel] += radiance;
                referenceSquareSum[pixel * 3 + channel] += radiance * radiance;
            }
        }
    }
    for (usize output = 0; output < POINT_LINE_OUTPUT_COUNT; ++output)
    {
        WARN(outputNames[output] << " failure pixels after " << checkpoints[CHECKPOINT_COUNT - 1]
                                 << " samples: " << CountFailurePixels(atCheckpoints[CHECKPOINT_COUNT - 1][output])
                                 << "; summed over " << REFERENCE_COUNT << " references of " << REFERENCE_FRAME_COUNT
                                 << " samples: " << referenceFailurePixels[output]);
    }
    for (usize checkpoint = 0; checkpoint < CHECKPOINT_COUNT; ++checkpoint)
    {
        removeFailurePixels(atCheckpoints[checkpoint][POINT_LINE_MIS]);
        removeFailurePixels(atCheckpoints[checkpoint][POINT_LINE_POINT_SAMPLING]);
    }
    const auto computeError = [&referenceSum](const Array<uint8>& pixels, const Array<bool>& mask) {
        float64 sum = 0;
        usize count = 0;
        for (usize pixel = 0; pixel < mask.GetSize(); ++pixel)
        {
            for (usize channel = 0; mask[pixel] == true && channel < 3; ++channel)
            {
                const float64 difference = static_cast<float64>(DecodeToneMapped(pixels[pixel * 4 + channel])) -
                                           referenceSum[pixel * 3 + channel] / static_cast<float64>(REFERENCE_COUNT);
                sum += difference * difference;
                ++count;
            }
        }
        REQUIRE(count > 0);
        return sum / static_cast<float64>(count);
    };

    math::Vector3 referenceMean{};
    for (usize pixel = 0; pixel < masks[PENUMBRA].GetSize(); ++pixel)
    {
        referenceMean = referenceMean + math::Vector3{static_cast<float32>(referenceSum[pixel * 3]),
                                                      static_cast<float32>(referenceSum[pixel * 3 + 1]),
                                                      static_cast<float32>(referenceSum[pixel * 3 + 2])} /
                                            static_cast<float32>(REFERENCE_COUNT * masks[PENUMBRA].GetSize());
    }
    WARN("reference color mean " << referenceMean.X << ',' << referenceMean.Y << ',' << referenceMean.Z << " from "
                                 << REFERENCE_COUNT * REFERENCE_FRAME_COUNT << " samples");
    for (usize mask = 0; mask < MASK_COUNT; ++mask)
    {
        usize keptPixels = 0;
        float64 referenceNoise = 0;
        for (usize pixel = 0; pixel < masks[mask].GetSize(); ++pixel)
        {
            if (masks[mask][pixel] == false)
            {
                continue;
            }
            keptPixels += 1;
            for (usize channel = 0; channel < 3; ++channel)
            {
                const float64 mean = referenceSum[pixel * 3 + channel] / static_cast<float64>(REFERENCE_COUNT);
                const float64 variance =
                    (referenceSquareSum[pixel * 3 + channel] - mean * referenceSum[pixel * 3 + channel]) /
                    static_cast<float64>(REFERENCE_COUNT - 1);
                referenceNoise += variance / static_cast<float64>(REFERENCE_COUNT);
            }
        }
        REQUIRE(keptPixels > 0);
        referenceNoise /= static_cast<float64>(keptPixels * 3);
        WARN(maskNames[mask] << " (" << keptPixels << " of " << maskPixels[mask]
                             << " pixels without failures): estimated reference noise " << referenceNoise);
        for (usize budget = 0; budget < BUDGET_COUNT; ++budget)
        {
            const float64 colorError =
                computeError((*getOutputsAt(colorFrameCounts[budget]))[POINT_LINE_MIS], masks[mask]);
            const float64 pointError =
                computeError((*getOutputsAt(pointFrameCounts[budget]))[POINT_LINE_POINT_SAMPLING], masks[mask]);
            const float64 equalSamplePointError =
                computeError((*getOutputsAt(colorFrameCounts[budget]))[POINT_LINE_POINT_SAMPLING], masks[mask]);
            WARN(maskNames[mask] << " at equal GPU time: color at " << colorFrameCounts[budget]
                                 << " samples MSE=" << colorError << ", pointSampling at " << pointFrameCounts[budget]
                                 << " samples MSE=" << pointError << ", point/color MSE ratio "
                                 << pointError / colorError << " (pointSampling at " << colorFrameCounts[budget]
                                 << " samples " << equalSamplePointError << ")");
        }
    }
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

TEST_CASE("A render graph kernel reads the renderer-bound LTC table like the CPU lookup",
          "[render-graph][gpu][compute-ray-tracing][scene-shader][ltc]")
{
    // ltc_lookup.slang writes, per target column, unit-range functions of LookupSpecularLtc for the
    // cases of ltc_lookup_cases.h. They come from the table buffer the renderer binds at slot 13.
    static_assert(LTC_LOOKUP_CASE_COUNT < WIDTH);
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_LTC_TABLE_DIRECTORY, "rg2_ltc_table.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPrograms compiled =
        packages::rg2_ltc_table::MakePrograms(shaders::renderer_composite::Vertex(),
                                              shaders::renderer_composite::Fragment());
    REQUIRE(decoded.HasLtcTable == true);
    REQUIRE(compiled.HasLtcTable == true);
    // The package rebuilds the reflected binding list of the generated header: the table slot, then the failure
    // summary last.
    CheckSameStage(decoded.RayCompute, compiled.RayCompute);
    const usize bindingCount = decoded.RayCompute.Bindings.size();
    REQUIRE(bindingCount >= 2);
    CHECK(decoded.RayCompute.Bindings[bindingCount - 2].Binding == LUDUS_GRAPH_LTC_TABLE_BINDING);
    CHECK(decoded.RayCompute.Bindings.back().Binding == LUDUS_GRAPH_FAILURE_BINDING);
    renderer::RenderGraphPlan plan = package->GetPlan();
    constexpr usize PASS_COUNT = 1;
    REQUIRE(plan.Passes.GetSize() == PASS_COUNT);
    StaticArray<format::Pass, PASS_COUNT> passes{{plan.Passes[0]}};
    passes[0].ToneMappingExposure = 0; // Bypass ToneMap: linear values read back as 255 * value.
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
                                report));
    uint32 output = 0;
    for (uint32 resource = 0; resource < plan.Resources.GetSize(); ++resource)
    {
        output = plan.Resources[resource].IsExported == 1 ? resource : output;
    }
    ReadGraphFrame(fixture, output);
    // RGBA8 quantization and rounding leave at most a step or two; a missing or misindexed table differs by far more.
    constexpr float32 TOLERANCE_STEPS = 2.0F;
    constexpr float32 UNORM_MAXIMUM = 255.0F;
    const usize row = HEIGHT / 2;
    for (uint32 column = 0; column < LTC_LOOKUP_CASE_COUNT; ++column)
    {
        const uint32 roughnessIndex = column % LTC_LOOKUP_ROUGHNESS_COUNT;
        const uint32 viewIndex = (column / LTC_LOOKUP_ROUGHNESS_COUNT) % LTC_LOOKUP_VIEW_COUNT;
        const uint32 fresnelIndex = column / (LTC_LOOKUP_ROUGHNESS_COUNT * LTC_LOOKUP_VIEW_COUNT);
        const float32 roughness = (static_cast<float32>(roughnessIndex) + 0.5F) / LTC_LOOKUP_ROUGHNESS_COUNT;
        const float32 viewCosine = (static_cast<float32>(viewIndex) + 0.5F) / LTC_LOOKUP_VIEW_COUNT;
        const float32 f0 = static_cast<float32>(fresnelIndex) / (LTC_LOOKUP_FRESNEL_COUNT - 1);
        const renderer::internal::SpecularLtc ltc =
            renderer::internal::LookupSpecularLtc(roughness,
                                                  {math::Sqrt(1 - viewCosine * viewCosine), 0, viewCosine},
                                                  {f0, f0, f0});
        const math::Matrix3& inverse = ltc.InverseTransform;
        const float32 spread = math::Abs(inverse.Columns[0].Z) + math::Abs(inverse.Columns[1].Y) +
                               math::Abs(inverse.Columns[2].X) + math::Abs(inverse.Columns[2].Z);
        const StaticArray<float32, 3> expected{{ltc.Norm, ltc.FresnelNorm / ltc.Norm, 1 / (1 + spread)}};
        for (usize channel = 0; channel < expected.GetSize(); ++channel)
        {
            CAPTURE(column, channel, expected[channel]);
            const float32 actual = fixture.Pixels[GetPixel(column, row) + channel];
            CHECK(math::Abs(actual - expected[channel] * UNORM_MAXIMUM) <= TOLERANCE_STEPS);
        }
    }
    CHECK(fixture.Pixels[GetPixel(LTC_LOOKUP_CASE_COUNT, row)] == 0);
}

namespace
{
/// Time allowed for one capture to finish, including its private graph's preparation.
constexpr uint64 CAPTURE_TIMEOUT = 60 * time::NANOSECONDS_PER_SECOND;
/// A small target keeps the GPU captures quick; it still exceeds the reduced authored limit below.
constexpr uint32 CAPTURE_TARGET_SAMPLES = 6;
/// An authored Accumulate limit below CAPTURE_TARGET_SAMPLES, so reaching the target needs the override.
constexpr uint32 REDUCED_AUTHORED_LIMIT = 2;

/// The polygonal-light template's clamped optimal MIS output: the first pass's colour.
uint32 GetClampedMisOutput(const renderer::RenderGraphPlan& plan)
{
    REQUIRE(plan.Passes.IsEmpty() == false);
    return plan.Passes[0].Color;
}

/// Service a capture until it ends, submitting the room when it waits for a scene.
// The scene revision and the frame count that triggers Stop are distinct quantities.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
template <typename Room>
renderer::RenderGraphCapturePhase
FinishCapture(renderer::RenderGraphCapture& inoutCapture, const Room& room, uint64 revision, uint32 stopAfterOrZero = 0)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const uint64 beginning = time::NowTicks();
    renderer::RenderGraphCapturePhase phase = inoutCapture.GetPhase();
    while (time::NowTicks() - beginning < CAPTURE_TIMEOUT)
    {
        phase = inoutCapture.Service();
        if (phase == renderer::RenderGraphCapturePhase::AwaitingScene)
        {
            const rhi::RasterStatus submitted =
                inoutCapture.SubmitScene({room.Meshes.GetData(), room.Meshes.GetSize()},
                                         {room.Items.GetData(), room.Items.GetSize()},
                                         {room.Cameras.GetData(), room.Cameras.GetSize()},
                                         revision,
                                         &room.Inputs);
            REQUIRE((submitted == rhi::RasterStatus::Ready || submitted == rhi::RasterStatus::Pending));
        }
        if (stopAfterOrZero != 0 && inoutCapture.GetInfo().SubmittedSamples >= stopAfterOrZero)
        {
            inoutCapture.Stop();
        }
        if (phase == renderer::RenderGraphCapturePhase::Complete ||
            phase == renderer::RenderGraphCapturePhase::Stopped || phase == renderer::RenderGraphCapturePhase::Failed)
        {
            return phase;
        }
    }
    FAIL("The capture did not finish");
    return phase;
}

/// Read a ray pass's accumulation records after the graph's last producer completed.
void ReadAccumulation(const renderer::RenderGraph& graph,
                      const GraphGpuFixture& fixture,
                      uint32 pass,
                      Array<math::Vector4>& outRecords)
{
    rhi::BufferHandle buffer;
    REQUIRE(graph.GetAccumulation(pass, buffer) == rhi::RasterStatus::Ready);
    REQUIRE(outRecords.TryResize(usize{WIDTH} * HEIGHT * renderer::RENDER_GRAPH_ACCUMULATION_WORDS) == true);
    rhi::LifetimeCapabilities capabilities;
    REQUIRE(rhi::GetLifetimeCapabilities(fixture.Device, capabilities) == rhi::RasterStatus::Ready);
    const usize totalBytes = outRecords.GetSize() * sizeof(math::Vector4);
    for (usize offset = 0; offset < totalBytes; offset += capabilities.TransferBytes)
    {
        const usize bytes = math::Min(capabilities.TransferBytes, totalBytes - offset);
        rhi::ReadbackTicket ticket;
        rhi::RasterStatus status = rhi::RequestBufferReadback(fixture.Device, buffer, offset, bytes, ticket);
        const uint64 beginning = time::NowTicks();
        while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TRAVERSAL_TIMEOUT)
        {
            status = rhi::GetStatus(fixture.Device, ticket);
        }
        REQUIRE(status == rhi::RasterStatus::Ready);
        REQUIRE(
            rhi::CopyReadback(fixture.Device, ticket, reinterpret_cast<uint8*>(outRecords.GetData()) + offset, bytes) ==
            rhi::RasterStatus::Ready);
        REQUIRE(rhi::Release(fixture.Device, ticket) == rhi::RasterStatus::Ready);
    }
}

uint32 ReadFileUint32(const Array<uint8>& bytes, usize offset)
{
    uint32 value = 0;
    REQUIRE(TryReadLittleEndian(std::span<const uint8>{bytes.GetData() + offset, sizeof(uint32)}, value) == true);
    return value;
}
} // namespace

TEST_CASE("A render graph capture reaches its target past the authored limit and saves the graph's own mean",
          "[render-graph][gpu][render-graph-capture]")
{
    GraphGpuFixture fixture(false, false);
    CornellBox room;
    MakeRoomGlossyWithoutSky(room);
    constexpr uint64 REVISION = 3;
    PreparedGraph live(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, REVISION);
    // Reduce the authored limit of every pass, so the capture's target is above it.
    StaticArray<format::Pass, format::MAX_PASSES> passes{};
    for (usize pass = 0; pass < live.Plan.Passes.GetSize(); ++pass)
    {
        passes[pass] = live.Plan.Passes[pass];
        passes[pass].AccumulationSamples = REDUCED_AUTHORED_LIMIT;
    }
    renderer::RenderGraphPlan plan = live.Plan;
    plan.Passes = {passes.GetData(), live.Plan.Passes.GetSize()};
    renderer::RenderGraphCaptureSettings settings;
    settings.Output = GetClampedMisOutput(plan);
    settings.TargetSamples = CAPTURE_TARGET_SAMPLES;
    settings.PackageIdentity[0] = 1;
    StaticArray<Array<uint8>, 2> files;
    for (Array<uint8>& file : files)
    {
        renderer::RenderGraphCapture capture;
        const rhi::RasterStatus started =
            capture.Start(fixture.Device, fixture.Surface, plan, live.Programs, WIDTH, HEIGHT, settings);
        REQUIRE((started == rhi::RasterStatus::Ready || started == rhi::RasterStatus::Pending));
        REQUIRE(FinishCapture(capture, room, REVISION) == renderer::RenderGraphCapturePhase::Complete);
        const renderer::RenderGraphCaptureInfo info = capture.GetInfo();
        CHECK(info.SubmittedSamples == CAPTURE_TARGET_SAMPLES);
        CHECK(info.MinimumSamples == CAPTURE_TARGET_SAMPLES);
        CHECK(info.AuthoredLimit == REDUCED_AUTHORED_LIMIT);
        CHECK(info.FailedPixels == 0);
        CHECK((info.Width == WIDTH && info.Height == HEIGHT));
        REQUIRE(capture.Encode(file) == rhi::RasterStatus::Ready);
        // The live graph with the authored package averages the same estimator from the same revision: its first
        // pass after CAPTURE_TARGET_SAMPLES frames is the captured mean.
        if (&file == &files[0])
        {
            for (uint32 frame = 0; frame < CAPTURE_TARGET_SAMPLES; ++frame)
            {
                RenderFrame(live.Graph, fixture);
            }
            Array<math::Vector4> records;
            ReadAccumulation(live.Graph, fixture, 0, records);
            const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
            REQUIRE(pixels.GetSize() == usize{WIDTH} * HEIGHT);
            usize differing = 0;
            for (usize pixel = 0; pixel < pixels.GetSize(); ++pixel)
            {
                const math::Vector4& sum = records[pixel * renderer::RENDER_GRAPH_ACCUMULATION_WORDS];
                const float32 scale = 1.0F / static_cast<float32>(CAPTURE_TARGET_SAMPLES);
                const math::Vector3 expected{sum.X * scale, sum.Y * scale, sum.Z * scale};
                differing += expected == pixels[pixel].Mean ? 0 : 1;
            }
            CHECK(differing == 0);
        }
    }
    // Equal inputs reproduce the file bit for bit.
    CHECK(files[0] == files[1]);
    const Array<uint8>& file = files[0];
    REQUIRE(file.GetSize() == format::CAPTURE_HEADER_BYTES + usize{WIDTH} * HEIGHT * format::CAPTURE_PIXEL_BYTES);
    CHECK(std::memcmp(file.GetData(), format::CAPTURE_MAGIC, format::CAPTURE_MAGIC_BYTES) == 0);
    CHECK(ReadFileUint32(file, format::CAPTURE_TARGET_SAMPLES_OFFSET) == CAPTURE_TARGET_SAMPLES);
    CHECK(ReadFileUint32(file, format::CAPTURE_SUBMITTED_SAMPLES_OFFSET) == CAPTURE_TARGET_SAMPLES);
    CHECK(ReadFileUint32(file, format::CAPTURE_AUTHORED_LIMIT_OFFSET) == REDUCED_AUTHORED_LIMIT);
    CHECK(ReadFileUint32(file, format::CAPTURE_OUTPUT_RESOURCE_OFFSET) == settings.Output);
    CHECK(ReadFileUint32(file, format::CAPTURE_FLAGS_OFFSET) ==
          static_cast<uint32>(format::CaptureFlag::LimitOverride));
    CHECK(file[format::CAPTURE_PACKAGE_IDENTITY_OFFSET] == 1);
}

TEST_CASE("A stopped render graph capture keeps exact counts and leaves the live graph unchanged",
          "[render-graph][gpu][render-graph-capture]")
{
    GraphGpuFixture fixture(false, false);
    CornellBox room;
    MakeRoomGlossyWithoutSky(room);
    constexpr uint64 REVISION = 5;
    PreparedGraph live(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, REVISION);
    constexpr uint32 LIVE_FRAMES = 2;
    for (uint32 frame = 0; frame < LIVE_FRAMES; ++frame)
    {
        RenderFrame(live.Graph, fixture);
    }
    Array<math::Vector4> before;
    ReadAccumulation(live.Graph, fixture, 0, before);
    const bool wasPending = live.Graph.HasPendingSamples();
    const uint64 drawableRevision = live.Graph.GetDrawableSceneRevision();

    renderer::RenderGraphCaptureSettings settings;
    settings.Output = GetClampedMisOutput(live.Plan);
    settings.TargetSamples = CAPTURE_TARGET_SAMPLES * 2;
    constexpr uint32 STOP_AFTER = 3;
    renderer::RenderGraphCapture capture;
    const rhi::RasterStatus started =
        capture.Start(fixture.Device, fixture.Surface, live.Plan, live.Programs, WIDTH, HEIGHT, settings);
    REQUIRE((started == rhi::RasterStatus::Ready || started == rhi::RasterStatus::Pending));
    // A different revision reseeds the capture; the live graph must not notice it either way.
    REQUIRE(FinishCapture(capture, room, REVISION + 1, STOP_AFTER) == renderer::RenderGraphCapturePhase::Stopped);
    const renderer::RenderGraphCaptureInfo info = capture.GetInfo();
    CHECK(info.SubmittedSamples == STOP_AFTER);
    CHECK(info.MinimumSamples == STOP_AFTER);
    Array<uint8> file;
    REQUIRE(capture.Encode(file) == rhi::RasterStatus::Ready);
    CHECK(
        ReadFileUint32(file, format::CAPTURE_FLAGS_OFFSET) ==
        (static_cast<uint32>(format::CaptureFlag::Partial) | static_cast<uint32>(format::CaptureFlag::LimitOverride)));
    const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
    REQUIRE(pixels.GetSize() == usize{WIDTH} * HEIGHT);
    for (usize pixel = 0; pixel < pixels.GetSize(); ++pixel)
    {
        REQUIRE(pixels[pixel].Samples == STOP_AFTER);
    }
    capture.Release();

    Array<math::Vector4> after;
    ReadAccumulation(live.Graph, fixture, 0, after);
    CHECK(after == before);
    CHECK(live.Graph.HasPendingSamples() == wasPending);
    CHECK(live.Graph.GetDrawableSceneRevision() == drawableRevision);
}

TEST_CASE("A render graph capture fails with the latched pixel count beyond the template's emitter budget",
          "[render-graph][gpu][render-graph-capture][polygonal-light-sampling]")
{
    GraphGpuFixture fixture(false, false);
    const ManyEmitterRoom room;
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(POLYGONAL_LIGHT_SAMPLING_TEMPLATE);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    renderer::RenderGraphCaptureSettings settings;
    settings.Output = GetClampedMisOutput(plan);
    settings.TargetSamples = 1;
    renderer::RenderGraphCapture capture;
    const rhi::RasterStatus started =
        capture.Start(fixture.Device, fixture.Surface, plan, programs, WIDTH, HEIGHT, settings);
    REQUIRE((started == rhi::RasterStatus::Ready || started == rhi::RasterStatus::Pending));
    REQUIRE(FinishCapture(capture, room, 1) == renderer::RenderGraphCapturePhase::Failed);
    CHECK(capture.GetProblem() == renderer::RenderGraphCaptureProblem::LatchedPixels);
    // As in the template's own budget test, the room fills most of the view.
    CHECK(capture.GetInfo().FailedPixels > WIDTH * HEIGHT / 2);
    Array<uint8> file;
    REQUIRE(capture.Encode(file) == rhi::RasterStatus::Ready);
    CHECK((ReadFileUint32(file, format::CAPTURE_FLAGS_OFFSET) & static_cast<uint32>(format::CaptureFlag::Failed)) != 0);
    CHECK(ReadFileUint32(file, format::CAPTURE_FAILED_PIXELS_OFFSET) == capture.GetInfo().FailedPixels);
}

TEST_CASE("A render graph capture rejects routes and settings it cannot capture without changing anything",
          "[render-graph][gpu][render-graph-capture]")
{
    GraphGpuFixture fixture(false, false);
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(POLYGONAL_LIGHT_SAMPLING_TEMPLATE);
    const renderer::RenderGraphPlan authored = package->GetPlan();
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    StaticArray<format::Pass, format::MAX_PASSES> passes{};
    for (usize pass = 0; pass < authored.Passes.GetSize(); ++pass)
    {
        passes[pass] = authored.Passes[pass];
    }
    StaticArray<format::Resource, format::MAX_RESOURCES> resources{};
    for (usize resource = 0; resource < authored.Resources.GetSize(); ++resource)
    {
        resources[resource] = authored.Resources[resource];
    }
    renderer::RenderGraphPlan plan = authored;
    plan.Passes = {passes.GetData(), authored.Passes.GetSize()};
    plan.Resources = {resources.GetData(), authored.Resources.GetSize()};
    renderer::RenderGraphCaptureSettings settings;
    settings.Output = GetClampedMisOutput(authored);
    renderer::RenderGraphCaptureProblem expected = renderer::RenderGraphCaptureProblem::None;
    SECTION("a surface-backed output, which would present the capture's frames")
    {
        resources[settings.Output].IsSurface = 1;
        expected = renderer::RenderGraphCaptureProblem::NoOutput;
    }
    SECTION("several exports and no chosen output")
    {
        settings.Output = format::ABSENT;
        expected = renderer::RenderGraphCaptureProblem::NoOutput;
    }
    SECTION("a ray pass without an Accumulate")
    {
        passes[0].AccumulationSamples = 0;
        expected = renderer::RenderGraphCaptureProblem::NoAccumulation;
    }
    SECTION("a raster writer")
    {
        passes[0].Program = format::Technique::ColorAndDepth;
        expected = renderer::RenderGraphCaptureProblem::RasterRoute;
    }
    SECTION("a target above the capture maximum")
    {
        settings.TargetSamples = renderer::MAX_RENDER_GRAPH_CAPTURE_SAMPLES + 1;
        expected = renderer::RenderGraphCaptureProblem::InvalidSettings;
    }
    // The device-free route check reports the same route problems before anything is created; settings are
    // judged by Start alone.
    CHECK(renderer::FindRenderGraphCaptureProblem(plan, settings.Output) ==
          (expected == renderer::RenderGraphCaptureProblem::InvalidSettings ? renderer::RenderGraphCaptureProblem::None
                                                                            : expected));
    renderer::RenderGraphCapture capture;
    CHECK(capture.Start(fixture.Device, fixture.Surface, plan, programs, WIDTH, HEIGHT, settings) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(capture.GetPhase() == renderer::RenderGraphCapturePhase::Failed);
    CHECK(capture.GetProblem() == expected);
    CHECK(capture.GetPixels().IsEmpty() == true);
    Array<uint8> file;
    CHECK(capture.Encode(file) == rhi::RasterStatus::NotReady);
    CHECK(file.IsEmpty() == true);
}

TEST_CASE("Render graph capture progress read-backs show exact partial counts and then resume",
          "[render-graph][gpu][render-graph-capture]")
{
    GraphGpuFixture fixture(false, false);
    CornellBox room;
    MakeRoomGlossyWithoutSky(room);
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(POLYGONAL_LIGHT_SAMPLING_TEMPLATE);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    renderer::RenderGraphCaptureSettings settings;
    settings.Output = GetClampedMisOutput(plan);
    settings.TargetSamples = CAPTURE_TARGET_SAMPLES;
    constexpr uint32 PREVIEW_INTERVAL = 2;
    settings.PreviewInterval = PREVIEW_INTERVAL;
    renderer::RenderGraphCapture capture;
    const rhi::RasterStatus started =
        capture.Start(fixture.Device, fixture.Surface, plan, programs, WIDTH, HEIGHT, settings);
    REQUIRE((started == rhi::RasterStatus::Ready || started == rhi::RasterStatus::Pending));
    // Progress read-backs follow the first sample and then every PREVIEW_INTERVAL samples.
    Array<uint32> previews;
    uint32 lastReadback = 0;
    const uint64 beginning = time::NowTicks();
    renderer::RenderGraphCapturePhase phase = capture.GetPhase();
    while (phase != renderer::RenderGraphCapturePhase::Complete && phase != renderer::RenderGraphCapturePhase::Failed &&
           time::NowTicks() - beginning < CAPTURE_TIMEOUT)
    {
        phase = capture.Service();
        if (phase == renderer::RenderGraphCapturePhase::AwaitingScene)
        {
            const rhi::RasterStatus submitted = capture.SubmitScene({room.Meshes.GetData(), room.Meshes.GetSize()},
                                                                    {room.Items.GetData(), room.Items.GetSize()},
                                                                    {room.Cameras.GetData(), room.Cameras.GetSize()},
                                                                    1,
                                                                    &room.Inputs);
            REQUIRE((submitted == rhi::RasterStatus::Ready || submitted == rhi::RasterStatus::Pending));
        }
        const renderer::RenderGraphCaptureInfo info = capture.GetInfo();
        if (phase == renderer::RenderGraphCapturePhase::Rendering && info.ReadbackSamples != lastReadback)
        {
            lastReadback = info.ReadbackSamples;
            REQUIRE(previews.TryAdd(lastReadback) == true);
            const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
            REQUIRE(pixels.GetSize() == usize{WIDTH} * HEIGHT);
            CHECK(pixels[0].Samples == lastReadback);
            CHECK(info.MinimumSamples == lastReadback);
        }
    }
    REQUIRE(phase == renderer::RenderGraphCapturePhase::Complete);
    const Array<uint32> expected{1, 2, 4};
    CHECK(previews == expected);
    CHECK(capture.GetInfo().MinimumSamples == CAPTURE_TARGET_SAMPLES);
    CHECK(capture.GetInfo().ReadbackSamples == CAPTURE_TARGET_SAMPLES);
}

TEST_CASE("Measure the GPU cost of polygonal-light shadow queries per emissive triangle",
          "[.capture-budget-measurement]")
{
    // Phase 3 evidence for docs/architecture/render-graph-frame-capture.md: GPU time of the polygonal-light
    // template's four shade passes as the number of emissive triangles grows. The many-emitter room keeps its
    // geometry; only the first tiles emit, so the emitter loop is the variable. Clamped MIS and balance cost
    // two shadow queries per triangle, projected solid angle and LTC one.
    GraphGpuFixture fixture(true, false);
    constexpr usize MEASURED_TILE_COUNTS = 4;
    constexpr usize SHADE_KERNELS = 4;
    const usize pixels = usize{BENCHMARK_EXTENT.Width} * BENCHMARK_EXTENT.Height;
    std::string report = "pixels " + std::to_string(pixels) + "; per triangle count, median ms of the four passes:";
    for (usize tiles = 1; tiles <= MEASURED_TILE_COUNTS; ++tiles)
    {
        ManyEmitterRoom room;
        for (usize tile = tiles; tile < MANY_EMITTER_COUNT; ++tile)
        {
            room.Materials[MANY_EMITTER_FIRST_LIGHT + tile].Emission = {};
        }
        PreparedGraph graph(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, 1, BENCHMARK_EXTENT);
        StaticArray<Array<float64>, SHADE_KERNELS> durations;
        for (Array<float64>& values : durations)
        {
            REQUIRE(values.TryEnsureCapacity(BENCHMARK_TIMED_FRAMES) == true);
        }
        for (usize frame = 0; frame < BENCHMARK_WARMUP_FRAMES + BENCHMARK_TIMED_FRAMES; ++frame)
        {
            const rhi::RasterStatus armed = rhi::ArmGpuPassTimings(fixture.Device);
            if (armed == rhi::RasterStatus::Unsupported)
            {
                SKIP("Per-pass GPU timestamps are required for this measurement");
            }
            REQUIRE(armed == rhi::RasterStatus::Ready);
            RenderFrame(graph.Graph, fixture);
            rhi::GpuPassTimings timings;
            REQUIRE(rhi::GetGpuPassTimings(fixture.Device, timings) == rhi::RasterStatus::Ready);
            REQUIRE(timings.State == rhi::GpuTimingState::Available);
            if (frame < BENCHMARK_WARMUP_FRAMES)
            {
                continue;
            }
            // The k-th compute operation of the ordered ray sequence is its k-th pass.
            usize kernel = 0;
            for (usize index = 0; index < timings.Count && kernel < SHADE_KERNELS; ++index)
            {
                const rhi::GpuPassTiming& pass = timings.Passes[index];
                if (pass.Kind != rhi::GpuPassKind::Compute)
                {
                    continue;
                }
                REQUIRE(pass.Timing.State == rhi::GpuTimingState::Available);
                REQUIRE(durations[kernel].TryAdd(static_cast<float64>(pass.Timing.DurationNanoseconds) /
                                                 BENCHMARK_NANOSECONDS_PER_MILLISECOND) == true);
                ++kernel;
            }
            REQUIRE(kernel == SHADE_KERNELS);
        }
        report += "\n  " + std::to_string(tiles * 2) + " triangles:";
        for (Array<float64>& values : durations)
        {
            report += ' ' + std::to_string(ComputeMedian(values));
        }
    }
    WARN(report);
}

TEST_CASE("A capture profile lets a frame capture shade the 32-emitter room that the interactive budget rejects",
          "[render-graph][gpu][render-graph-capture][polygonal-light-sampling]")
{
    // The many-emitter room has 32 emissive triangles: 64 shadow queries per MIS invocation. The interactive
    // package's budget of 16 fails explicitly; the capture profile links the template's
    // capture_visibility_queries (256) and shades every triangle, as Peters' protocol requires.
    GraphGpuFixture fixture(false, false);
    const ManyEmitterRoom room;
    const Array<uint8> bytes = ReadPackageBytes(
        {LUDUS_RG3_POLYGONAL_LIGHT_SAMPLING_CAPTURE_DIRECTORY, "rg2_polygonal_light_sampling_capture.lurgcp"});
    renderer::RenderGraphPackage refused;
    // Runtime decoding refuses a capture profile, so its budget never reaches an interactive graph.
    CHECK(refused.Decode({bytes.GetData(), bytes.GetSize()}) != renderer::RenderGraphPackageStatus::Ready);
    CHECK(refused.IsLoaded() == false);
    renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    const core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
    REQUIRE(package.Get() != nullptr);
    REQUIRE(package->DecodeCaptureProfile({bytes.GetData(), bytes.GetSize()}) ==
            renderer::RenderGraphPackageStatus::Ready);
    CHECK(package->IsCaptureProfile() == true);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    renderer::RenderGraphCaptureSettings settings;
    settings.Output = GetClampedMisOutput(plan);
    settings.TargetSamples = 2;
    renderer::RenderGraphCapture capture;
    const rhi::RasterStatus started =
        capture.Start(fixture.Device, fixture.Surface, plan, programs, WIDTH, HEIGHT, settings);
    REQUIRE((started == rhi::RasterStatus::Ready || started == rhi::RasterStatus::Pending));
    REQUIRE(FinishCapture(capture, room, 1) == renderer::RenderGraphCapturePhase::Complete);
    const renderer::RenderGraphCaptureInfo info = capture.GetInfo();
    CHECK(info.FailedPixels == 0);
    CHECK(info.MinimumSamples == settings.TargetSamples);
    // The tiles light the room. After two samples, surfaces the shadow rays found occluded and the open front's
    // environment stay black; about half of the image receives radiance on Metal.
    const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
    usize lit = 0;
    for (usize pixel = 0; pixel < pixels.GetSize(); ++pixel)
    {
        lit += pixels[pixel].Mean.X + pixels[pixel].Mean.Y + pixels[pixel].Mean.Z > 0 ? 1 : 0;
    }
    constexpr usize MINIMUM_LIT_DIVISOR = 4;
    CHECK(lit > pixels.GetSize() / MINIMUM_LIT_DIVISOR);
}

namespace
{
/// Render one frame of a graph, retrying while it prepares row bands, and wait for its producer.
void RenderPreparedFrame(renderer::RenderGraph& inoutGraph, const GraphGpuFixture& fixture)
{
    constexpr uint64 TIMEOUT = 10 * time::NANOSECONDS_PER_SECOND;
    const uint64 beginning = time::NowTicks();
    rhi::SubmissionToken completion;
    rhi::RasterStatus rendered = inoutGraph.Render(fixture.Surface, WIDTH, HEIGHT, completion);
    while (rendered == rhi::RasterStatus::NotReady && time::NowTicks() - beginning < TIMEOUT)
    {
        rendered = inoutGraph.Render(fixture.Surface, WIDTH, HEIGHT, completion);
    }
    REQUIRE(rendered == rhi::RasterStatus::Ready);
    rhi::RasterStatus result = rhi::GetStatus(fixture.Device, completion);
    while (result == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
    {
        result = rhi::GetStatus(fixture.Device, completion);
    }
    REQUIRE(result == rhi::RasterStatus::Ready);
}
} // namespace

TEST_CASE("A tiled render graph capture accumulates exactly the pixels of an untiled one",
          "[render-graph][gpu][render-graph-capture][render-graph-tiles]")
{
    // Bands change only the submission boundaries: pixel indices, sample streams and accumulation stay those of a
    // whole-image dispatch, so the means must match bit for bit. The polygonal-light template's four passes share
    // a hit record indexed by the full extent, so every pass must dispatch the same band. Five bands split the 64
    // rows unevenly; MAX_RENDER_GRAPH_RAY_TILES bands outnumber the rows and leave empty bands.
    GraphGpuFixture fixture(false, false);
    CornellBox room;
    MakeRoomGlossyWithoutSky(room);
    constexpr uint64 REVISION = 7;
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(POLYGONAL_LIGHT_SAMPLING_TEMPLATE);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(programs.HasRayTiles == true);
    constexpr uint32 UNEVEN_TILES = 5;
    const StaticArray<uint32, 3> tileCounts{{1, UNEVEN_TILES, renderer::MAX_RENDER_GRAPH_RAY_TILES}};
    StaticArray<Array<renderer::RenderGraphCapturePixel>, 3> captured;
    for (usize run = 0; run < tileCounts.GetSize(); ++run)
    {
        renderer::RenderGraphCaptureSettings settings;
        settings.Output = GetClampedMisOutput(plan);
        settings.TargetSamples = CAPTURE_TARGET_SAMPLES;
        settings.TileCount = tileCounts[run];
        renderer::RenderGraphCapture capture;
        const rhi::RasterStatus started =
            capture.Start(fixture.Device, fixture.Surface, plan, programs, WIDTH, HEIGHT, settings);
        REQUIRE((started == rhi::RasterStatus::Ready || started == rhi::RasterStatus::Pending));
        const renderer::RenderGraphCapturePhase phase = FinishCapture(capture, room, REVISION);
        CAPTURE(tileCounts[run], capture.GetProblem(), capture.GetStatus(), capture.GetInfo().SubmittedSamples);
        REQUIRE(phase == renderer::RenderGraphCapturePhase::Complete);
        const renderer::RenderGraphCaptureInfo info = capture.GetInfo();
        CHECK(info.TileCount == tileCounts[run]);
        CHECK(info.SubmittedSamples == CAPTURE_TARGET_SAMPLES);
        CHECK(info.MinimumSamples == CAPTURE_TARGET_SAMPLES);
        CHECK(info.FailedPixels == 0);
        const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
        REQUIRE(pixels.GetSize() == usize{WIDTH} * HEIGHT);
        REQUIRE(captured[run].TryResize(pixels.GetSize()) == true);
        for (usize pixel = 0; pixel < pixels.GetSize(); ++pixel)
        {
            captured[run][pixel] = pixels[pixel];
        }
    }
    for (usize run = 1; run < captured.GetSize(); ++run)
    {
        CAPTURE(tileCounts[run]);
        usize differing = 0;
        for (usize pixel = 0; pixel < captured[0].GetSize(); ++pixel)
        {
            differing += captured[run][pixel].Mean == captured[0][pixel].Mean &&
                                 captured[run][pixel].Samples == captured[0][pixel].Samples
                             ? 0
                             : 1;
        }
        CHECK(differing == 0);
    }
}

TEST_CASE("A tiled render graph counts a sample once every band was submitted",
          "[render-graph][gpu][render-graph-tiles]")
{
    GraphGpuFixture fixture(false, false);
    CornellBox room;
    MakeRoomGlossyWithoutSky(room);
    PreparedGraph live(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, 1);
    CHECK(live.Graph.SetRayTileCount(0) == rhi::RasterStatus::InvalidDescription);
    CHECK(live.Graph.SetRayTileCount(renderer::MAX_RENDER_GRAPH_RAY_TILES + 1) ==
          rhi::RasterStatus::InvalidDescription);
    renderer::RenderGraphRayWorkload workload;
    REQUIRE(live.Graph.GetRayWorkload(workload) == rhi::RasterStatus::Ready);
    // Every pass renders at the drawable extent; the room's ceiling light is emissive geometry.
    CHECK(workload.Pixels == uint64{WIDTH} * HEIGHT * live.Plan.Passes.GetSize());
    CHECK(workload.Lights > 0);
    constexpr uint32 TILES = 3;
    REQUIRE(live.Graph.SetRayTileCount(TILES) == rhi::RasterStatus::Ready);
    for (uint32 band = 0; band + 1 < TILES; ++band)
    {
        RenderPreparedFrame(live.Graph, fixture);
        CHECK(live.Graph.GetSubmittedRaySamples() == 0);
        CHECK(live.Graph.HasPendingSamples() == true);
        // The band layout changes only at a sample boundary.
        CHECK(live.Graph.SetRayTileCount(1) == rhi::RasterStatus::InvalidState);
    }
    RenderPreparedFrame(live.Graph, fixture);
    CHECK(live.Graph.GetSubmittedRaySamples() == 1);
    // Every pixel committed exactly one sample, in whichever band it was dispatched.
    Array<math::Vector4> records;
    ReadAccumulation(live.Graph, fixture, 0, records);
    usize wrong = 0;
    for (usize pixel = 0; pixel < usize{WIDTH} * HEIGHT; ++pixel)
    {
        uint32 count = 0;
        std::memcpy(&count, &records[pixel * renderer::RENDER_GRAPH_ACCUMULATION_WORDS].W, sizeof(count));
        wrong += count == 1 ? 0 : 1;
    }
    CHECK(wrong == 0);
    CHECK(live.Graph.SetRayTileCount(1) == rhi::RasterStatus::Ready);
    RenderPreparedFrame(live.Graph, fixture);
    CHECK(live.Graph.GetSubmittedRaySamples() == 2);
}

TEST_CASE("A render graph capture refuses bands that its package's ray kernels cannot dispatch",
          "[render-graph][gpu][render-graph-capture][render-graph-tiles]")
{
    GraphGpuFixture fixture(false, false);
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(POLYGONAL_LIGHT_SAMPLING_TEMPLATE);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    // Kernels cooked before package version 28 cover the whole image on every dispatch.
    programs.HasRayTiles = false;
    renderer::RenderGraphCaptureSettings settings;
    settings.Output = GetClampedMisOutput(plan);
    settings.TileCount = 2;
    renderer::RenderGraphCapture capture;
    CHECK(capture.Start(fixture.Device, fixture.Surface, plan, programs, WIDTH, HEIGHT, settings) ==
          rhi::RasterStatus::Unsupported);
    CHECK(capture.GetPhase() == renderer::RenderGraphCapturePhase::Failed);
    CHECK(capture.GetProblem() == renderer::RenderGraphCaptureProblem::UntiledPackage);
    settings.TileCount = renderer::MAX_RENDER_GRAPH_RAY_TILES + 1;
    CHECK(capture.Start(fixture.Device, fixture.Surface, plan, programs, WIDTH, HEIGHT, settings) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(capture.GetProblem() == renderer::RenderGraphCaptureProblem::InvalidSettings);
}

namespace
{
/// Poll a graph's failure summary of one ray pass until its read-back completes.
renderer::RenderGraphRayFailures ReadRayFailures(renderer::RenderGraph& inoutGraph, uint32 pass)
{
    renderer::RenderGraphRayFailures failures;
    const uint64 beginning = time::NowTicks();
    rhi::RasterStatus status = inoutGraph.PollRayFailures(pass, failures);
    while (status == rhi::RasterStatus::Pending && time::NowTicks() - beginning < TRAVERSAL_TIMEOUT)
    {
        status = inoutGraph.PollRayFailures(pass, failures);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    return failures;
}

/// Pixels of a ray pass whose accumulation latched the failure colour (an odd history tag).
usize CountLatchedPixels(const renderer::RenderGraph& graph, const GraphGpuFixture& fixture, uint32 pass)
{
    Array<math::Vector4> records;
    ReadAccumulation(graph, fixture, pass, records);
    usize latched = 0;
    for (usize pixel = 0; pixel < usize{WIDTH} * HEIGHT; ++pixel)
    {
        latched +=
            static_cast<uint32>(records[pixel * renderer::RENDER_GRAPH_ACCUMULATION_WORDS + 1].W) % 2 == 1 ? 1 : 0;
    }
    return latched;
}
} // namespace

TEST_CASE("Latched graph pixels name the check that failed them",
          "[render-graph][gpu][render-graph-failures][polygonal-light-sampling]")
{
    GraphGpuFixture fixture(false, false);
    SECTION("the polygonal-light template beyond its visibility budget")
    {
        // 32 emissive triangles need 64 shadow queries per MIS invocation; the interactive budget is 16.
        const ManyEmitterRoom room;
        PreparedGraph live(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, 1);
        RenderFrame(live.Graph, fixture);
        const renderer::RenderGraphRayFailures failures = ReadRayFailures(live.Graph, 0);
        const usize latched = CountLatchedPixels(live.Graph, fixture, 0);
        constexpr uint32 BUDGET = static_cast<uint32>(renderer::RenderGraphFailureReason::VisibilityBudget);
        REQUIRE(latched > 0);
        // Every latched pixel is counted once, under the reason its shader recorded.
        CHECK(failures.TotalPixels == latched);
        CHECK(failures.Pixels[BUDGET] == latched);
        CHECK(failures.MainReason == BUDGET);
        // A latched pixel never samples again: a later frame counts only the pixels that latch in it.
        RenderFrame(live.Graph, fixture);
        const renderer::RenderGraphRayFailures later = ReadRayFailures(live.Graph, 0);
        CHECK(later.TotalPixels == CountLatchedPixels(live.Graph, fixture, 0));
        CHECK(later.TotalPixels >= latched);
    }
    SECTION("a lit room within every budget")
    {
        CornellBox room;
        MakeRoomGlossyWithoutSky(room);
        PreparedGraph live(fixture, POLYGONAL_LIGHT_SAMPLING_TEMPLATE, room, 1);
        constexpr uint32 FRAMES = 2;
        for (uint32 frame = 0; frame < FRAMES; ++frame)
        {
            RenderFrame(live.Graph, fixture);
        }
        CHECK(CountLatchedPixels(live.Graph, fixture, 0) == 0);
        CHECK(ReadRayFailures(live.Graph, 0).TotalPixels == 0);
    }
}

namespace
{
// slice-sampling-luminaire template tests. Thanks to Shlomi Steinberg, "Slice Sampling: Importance sampling with an
// auxiliary map" (draft, University of Waterloo), section 6: the template lights the room with an analytic directional
// LED panel and exports a defensive slice-sampling mixture (Eq. 15), slice sampling alone and uniform panel-area
// sampling. Every pass is a shade pass; the first also records the primary hit. The proposals' unbiasedness on raw
// linear irradiance is checked in slice_sampling_luminaire_estimator_tests.cpp.
constexpr usize SLICE_SAMPLING_OUTPUT_COUNT = 3;
constexpr usize SLICE_SAMPLING_DEFENSIVE = 0;
constexpr usize SLICE_SAMPLING_ALONE = 1;
constexpr usize SLICE_SAMPLING_UNIFORM_AREA = 2;
const PackageLocation SLICE_SAMPLING_LUMINAIRE_TEMPLATE{LUDUS_RG3_SLICE_SAMPLING_LUMINAIRE_TEMPLATE_DIRECTORY,
                                                        "rg2_slice_sampling_luminaire_template.lurgrt"};

/// Render frameCount frames of the template and read each output's running mean after the last one. The template's
/// three passes write its three outputs in output order.
void RenderSliceSamplingOutputs(PreparedGraph& inoutTemplate,
                                const GraphGpuFixture& fixture,
                                usize frameCount,
                                StaticArray<Array<uint8>, SLICE_SAMPLING_OUTPUT_COUNT>& outPixels)
{
    REQUIRE(inoutTemplate.Plan.Passes.GetSize() == SLICE_SAMPLING_OUTPUT_COUNT);
    StaticArray<uint32, SLICE_SAMPLING_OUTPUT_COUNT> exports{};
    for (usize output = 0; output < SLICE_SAMPLING_OUTPUT_COUNT; ++output)
    {
        exports[output] = inoutTemplate.Plan.Passes[output].Color;
        REQUIRE(exports[output] < inoutTemplate.Plan.Resources.GetSize());
        REQUIRE(inoutTemplate.Plan.Resources[exports[output]].IsExported == 1);
    }
    for (usize frame = 0; frame < frameCount; ++frame)
    {
        RenderFrame(inoutTemplate.Graph, fixture);
    }
    for (usize output = 0; output < SLICE_SAMPLING_OUTPUT_COUNT; ++output)
    {
        ReadExport(inoutTemplate.Graph, fixture, exports[output], {}, outPixels[output]);
        // Invalid hit records, exhausted budgets and failed queries would show magenta.
        REQUIRE(CountFailurePixels(outPixels[output]) == 0);
    }
}
} // namespace

TEST_CASE("The slice-sampling-luminaire template lights the room with its panel through three compute-BVH passes",
          "[render-graph][gpu][slice-sampling-luminaire][template]")
{
    GraphGpuFixture fixture(false, false);
    const CornellBox room;
    PreparedGraph luminaire(fixture, SLICE_SAMPLING_LUMINAIRE_TEMPLATE, room, 1);
    CheckSameStage(
        luminaire.Programs.RayCompute,
        packages::rg2_slice_sampling_luminaire_template::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                      shaders::renderer_composite::Fragment())
            .RayCompute);
    REQUIRE(luminaire.Programs.HasPersistentRayBuffers == true);
    // Every pass walks the compute BVH and accumulates its own output.
    REQUIRE(renderer::RequiresHardwareRayTracing(luminaire.Plan) == false);
    REQUIRE(renderer::UsesRayTracing(luminaire.Plan) == true);
    for (usize pass = 0; pass < luminaire.Plan.Passes.GetSize(); ++pass)
    {
        INFO("pass " << pass);
        CHECK(luminaire.Plan.Passes[pass].Program == format::Technique::RayTracing);
        CHECK(luminaire.Plan.Passes[pass].Traversal == format::RayTraversal::ComputeBvh);
        CHECK(luminaire.Plan.Passes[pass].AccumulationSamples > 0);
    }
    constexpr usize FRAME_COUNT = 256;
    StaticArray<Array<uint8>, SLICE_SAMPLING_OUTPUT_COUNT> pixels;
    RenderSliceSamplingOutputs(luminaire, fixture, FRAME_COUNT, pixels);
    const math::Vector3 defensive = ComputeMeanRadiance(pixels[SLICE_SAMPLING_DEFENSIVE]);
    const math::Vector3 slice = ComputeMeanRadiance(pixels[SLICE_SAMPLING_ALONE]);
    const math::Vector3 area = ComputeMeanRadiance(pixels[SLICE_SAMPLING_UNIFORM_AREA]);
    INFO("defensive mean=" << defensive.X << ',' << defensive.Y << ',' << defensive.Z << ", slice mean=" << slice.X
                           << ',' << slice.Y << ',' << slice.Z << ", uniform area mean=" << area.X << ',' << area.Y
                           << ',' << area.Z);
    // The panel's footprints light the floor and the block; the ceiling light is only seen directly.
    constexpr float32 MINIMUM_MEAN_RADIANCE = .01F;
    REQUIRE(defensive.X > MINIMUM_MEAN_RADIANCE);
    // Both low-variance proposals estimate the same image after 256 samples per pixel. The mixture differs from slice
    // sampling alone only through the far tails that uniform seeds miss, which are below a display step here.
    constexpr float32 RELATIVE_MEAN_TOLERANCE = .02F;
    CheckMeansAgree(slice, defensive, RELATIVE_MEAN_TOLERANCE);
    // Uniform area sampling is unbiased too, but at 256 samples most pixels' running means are still noisy, and the
    // tone mapping of a noisy mean is biased; the raw estimators' agreement is checked in linear float elsewhere.
    constexpr float32 NOISY_RELATIVE_MEAN_TOLERANCE = .15F;
    CheckMeansAgree(area, defensive, NOISY_RELATIVE_MEAN_TOLERANCE);
}

TEST_CASE("Slice sampling the template's luminaire has far less error than uniform panel-area sampling",
          "[render-graph][gpu][slice-sampling-luminaire][template]")
{
    // Steinberg's section 6.4: a narrow, position-dependent beam profile lights each receiver from small panel regions
    // that uniform area sampling rarely hits, while slice sampling proposes them directly. Compare low-sample images
    // of each output with an independent converged defensive image, whose scene revision reseeds sampling.
    GraphGpuFixture fixture(false, false);
    const CornellBox room;
    constexpr usize LOW_FRAME_COUNT = 16;
    StaticArray<Array<uint8>, SLICE_SAMPLING_OUTPUT_COUNT> pixels;
    {
        PreparedGraph luminaire(fixture, SLICE_SAMPLING_LUMINAIRE_TEMPLATE, room, 1);
        RenderSliceSamplingOutputs(luminaire, fixture, LOW_FRAME_COUNT, pixels);
    }
    constexpr uint64 CONVERGED_REVISION = 2;
    constexpr usize CONVERGED_FRAME_COUNT = 1024;
    StaticArray<Array<uint8>, SLICE_SAMPLING_OUTPUT_COUNT> converged;
    {
        PreparedGraph convergedGraph(fixture, SLICE_SAMPLING_LUMINAIRE_TEMPLATE, room, CONVERGED_REVISION);
        RenderSliceSamplingOutputs(convergedGraph, fixture, CONVERGED_FRAME_COUNT, converged);
    }
    const Array<uint8>& reference = converged[SLICE_SAMPLING_DEFENSIVE];
    const float64 defensive = ComputeDisplayMeanSquaredError(pixels[SLICE_SAMPLING_DEFENSIVE], reference);
    const float64 slice = ComputeDisplayMeanSquaredError(pixels[SLICE_SAMPLING_ALONE], reference);
    const float64 area = ComputeDisplayMeanSquaredError(pixels[SLICE_SAMPLING_UNIFORM_AREA], reference);
    INFO("mean squared error at " << LOW_FRAME_COUNT << " samples, tone-mapped: defensive slice sampling=" << defensive
                                  << ", slice sampling=" << slice << ", uniform area=" << area);
    // Pixel jitter and shadow edges add noise that every output shares, so the image error ratio is far smaller than
    // the per-sample variance ratio of the raw estimators.
    constexpr float64 ERROR_REDUCTION = 4;
    CHECK(defensive * ERROR_REDUCTION < area);
    CHECK(slice * ERROR_REDUCTION < area);
}
