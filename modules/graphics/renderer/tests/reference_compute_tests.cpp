#include "l6_scene.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_reference.h"
#include "renderer_reference_reduce.h"
#include "renderer_shadow.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>

#include <catch2/catch_test_macros.hpp>

// CPU oracle / GPU-compute parity for the RT1 reference integral. Both paths read
// the same addressed Philox dimensions, so matching samples follow matching paths;
// differences come from float32 traversal/arithmetic on the device.
namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
using S = rhi::RasterStatus;
constexpr usize CPU_BUDGET = usize{64} * 1024 * 1024;
constexpr usize GPU_BUDGET = usize{64} * 1024 * 1024;
constexpr uint32 CPU_TILE_PIXELS = 256;
constexpr uint64 TIMEOUT = 60 * time::NANOSECONDS_PER_SECOND;
constexpr uint64 NANOSECONDS_PER_MILLISECOND = time::NANOSECONDS_PER_SECOND / 1000;

struct Session final
{
    rr::Renderer Renderer;
    rr::MaterialProgram Program;
    rr::Snapshot Snapshot;
};
S Prepare(rhi::DeviceHandle device, rr::ReferenceTraversal traversal, Session& outSession)
{
    rr::Renderer& renderer = outSession.Renderer;
    REQUIRE(renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) ==
            S::Ready);
    const rr::ReferenceRenderingOptions options{traversal, CPU_BUDGET, 0, CPU_TILE_PIXELS, GPU_BUDGET};
    const S enabled = traversal == rr::ReferenceTraversal::GpuCompute
                          ? renderer.EnableReferenceRendering(shaders::renderer_reference::Compute(),
                                                              shaders::renderer_reference_reduce::Compute(),
                                                              options)
                          : renderer.EnableReferenceRendering({}, options);
    if (enabled != S::Ready)
    {
        return enabled;
    }
    REQUIRE(renderer.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) ==
            S::Ready);
    REQUIRE(renderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                        shaders::renderer_output::Fragment(),
                                        shaders::renderer_shadow::Fragment()) == S::Ready);
    S status = renderer.PrewarmLitProgram(shaders::renderer_lit::Vertex(),
                                          shaders::renderer_lit::Fragment(),
                                          shaders::renderer_lit_shadow::Fragment(),
                                          outSession.Program);
    const uint64 start = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - start < TIMEOUT)
    {
        status = renderer.GetStatus(outSession.Program);
    }
    REQUIRE(status == S::Ready);
    return ludus::qa::BuildL6Snapshot(renderer, outSession.Program, outSession.Snapshot, -.7F);
}
// A tessellated glossy metal sphere in front of a diffuse wall and a small emitter:
// thousands of triangles exercise deep BVH traversal and visible-GGX sampling.
S BuildSphereSnapshot(rhi::DeviceHandle device,
                      rr::Renderer& inoutRenderer,
                      rr::MaterialProgram program,
                      rr::Snapshot& outSnapshot)
{
    constexpr uint32 LONGITUDES = 96;
    constexpr uint32 LATITUDES = 48;
    constexpr float32 RADIUS = .45F;
    Array<math::Vector3> positions;
    Array<math::Vector3> normals;
    Array<uint32> indices;
    for (uint32 row = 0; row <= LATITUDES; ++row)
    {
        const float32 polar = math::kPiF * float32(row) / float32(LATITUDES);
        for (uint32 column = 0; column <= LONGITUDES; ++column)
        {
            const float32 azimuth = math::kTauF * float32(column) / float32(LONGITUDES);
            const math::Vector3 normal{math::Sin(polar) * math::Cos(azimuth),
                                       math::Cos(polar),
                                       math::Sin(polar) * math::Sin(azimuth)};
            REQUIRE(positions.TryAdd(normal * RADIUS) == true);
            REQUIRE(normals.TryAdd(normal) == true);
        }
    }
    for (uint32 row = 1; row + 1 < LATITUDES; ++row)
    {
        for (uint32 column = 0; column < LONGITUDES; ++column)
        {
            const uint32 a = row * (LONGITUDES + 1) + column;
            const uint32 b = a + LONGITUDES + 1;
            const uint32 quad[]{a, a + 1, b, a + 1, b + 1, b};
            for (const uint32 index : quad)
            {
                REQUIRE(indices.TryAdd(index) == true);
            }
        }
    }
    rr::MeshDescription sphereDescription{positions.GetData(),
                                          positions.GetSize(),
                                          indices.GetData(),
                                          indices.GetSize()};
    sphereDescription.Normals = normals.GetData();
    const math::Vector3 quadPositions[]{{-2, -2, 0}, {2, -2, 0}, {2, 2, 0}, {-2, 2, 0}};
    const math::Vector3 quadNormals[]{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}};
    const uint32 quadIndices[]{0, 2, 1, 0, 3, 2};
    const math::Vector2 quadUvs[]{{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    rr::MeshDescription quadDescription{quadPositions, 4, quadIndices, 6};
    quadDescription.Normals = quadNormals;
    quadDescription.Texcoords = quadUvs;
    rr::Mesh meshes[2];
    const rr::MeshDescription* descriptions[]{&sphereDescription, &quadDescription};
    for (usize i = 0; i < 2; ++i)
    {
        S status = inoutRenderer.CreateMesh(*descriptions[i], meshes[i]);
        const uint64 start = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - start < TIMEOUT)
        {
            status = inoutRenderer.GetStatus(meshes[i]);
        }
        REQUIRE(status == S::Ready);
    }
    // sRGB checker whose alpha cuts every other cell: masked, linearly filtered and repeated.
    constexpr usize CHECKER_EXTENT = 4;
    constexpr usize CHECKER_TEXEL_BYTES = 4;
    uint8 checker[CHECKER_EXTENT * CHECKER_EXTENT * CHECKER_TEXEL_BYTES]{};
    for (usize y = 0; y < CHECKER_EXTENT; ++y)
    {
        for (usize x = 0; x < CHECKER_EXTENT; ++x)
        {
            uint8* texel = checker + (y * CHECKER_EXTENT + x) * CHECKER_TEXEL_BYTES;
            const bool isSolid = (x + y) % 2 == 0;
            texel[0] = isSolid == true ? 230 : 40;
            texel[1] = 120;
            texel[2] = isSolid == true ? 60 : 200;
            texel[3] = isSolid == true ? 255 : 30;
        }
    }
    rhi::TextureHandle texture;
    rhi::TextureDescription textureDescription{static_cast<uint32>(CHECKER_EXTENT),
                                               static_cast<uint32>(CHECKER_EXTENT),
                                               rhi::RasterFormat::Rgba8Srgb};
    textureDescription.RetainCpuContent = true;
    REQUIRE(rhi::CreateTexture(device,
                               textureDescription,
                               {{checker, sizeof(checker)}, CHECKER_EXTENT * CHECKER_TEXEL_BYTES},
                               texture) == S::Ready);
    constexpr usize MATERIAL_COUNT = 4;
    rr::MaterialVersion materials[MATERIAL_COUNT];
    for (usize i = 0; i < MATERIAL_COUNT; ++i)
    {
        rr::LitMaterialDescription material;
        material.Surface.Tint = i == 0 ? math::Vector4{.9F, .7F, .3F, 1} : math::Vector4{.6F, .6F, .6F, 1};
        material.Metallic = i == 0 ? 1.F : 0.F;
        material.Roughness = i == 0 ? .3F : 1.F;
        if (i == 2)
        {
            material.Emission = {6, 5, 4};
        }
        if (i == 3)
        {
            material.Surface.Texture = texture;
            material.Surface.Alpha = rr::MaterialAlpha::Masked;
            material.Surface.UvTransform = {3, 3, .1F, 0};
            material.Surface.Sampler.Filter = rhi::RasterFilter::Linear;
            material.Surface.Sampler.U = rhi::RasterAddress::Repeat;
            material.Surface.Sampler.V = rhi::RasterAddress::Repeat;
        }
        rr::LightSelection selection;
        S status = inoutRenderer.CreateLitMaterial(program, material, materials[i], selection);
        const uint64 start = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - start < TIMEOUT)
        {
            status = inoutRenderer.GetStatus(materials[i]);
        }
        REQUIRE(status == S::Ready);
    }
    rr::SceneItem items[MATERIAL_COUNT];
    items[0].Geometry = meshes[0];
    items[0].Surface = materials[0];
    items[0].SourceId = 1;
    items[1].Geometry = meshes[1];
    items[1].Surface = materials[1];
    items[1].SourceId = 2;
    items[1].Transform.Translation.Z = .6F;
    items[2].Geometry = meshes[1];
    items[2].Surface = materials[2];
    items[2].SourceId = 3;
    items[2].Transform.Columns[0].X = .15F;
    items[2].Transform.Columns[1].Y = .15F;
    items[2].Transform.Translation = {-.7F, .7F, -.3F};
    items[3].Geometry = meshes[1];
    items[3].Surface = materials[3];
    items[3].SourceId = 4;
    items[3].Transform.Columns[0].X = .2F;
    items[3].Transform.Columns[1].Y = .2F;
    items[3].Transform.Translation = {.25F, -.2F, -.7F};
    const S created = inoutRenderer.CreateSnapshot(items, MATERIAL_COUNT, outSnapshot);
    static_cast<void>(rhi::Destroy(device, texture));
    for (rr::MaterialVersion& material : materials)
    {
        static_cast<void>(inoutRenderer.Release(material));
    }
    for (rr::Mesh& mesh : meshes)
    {
        static_cast<void>(inoutRenderer.Release(mesh));
    }
    return created;
}
S Run(rr::Renderer& inoutRenderer, rr::ReferenceCapture capture, rhi::SurfaceHandle surface)
{
    S status = S::Pending;
    const uint64 start = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - start < TIMEOUT)
    {
        status = inoutRenderer.AdvanceReferenceCapture(capture, surface);
    }
    return status;
}
void Capture(Session& inoutSession,
             const rr::ReferenceCaptureDescription& description,
             rhi::SurfaceHandle surface,
             Array<rr::ReferencePixel>& outPixels,
             rr::ReferenceCaptureInfo& outInfo)
{
    rr::ReferenceCapture capture;
    const S begun = inoutSession.Renderer.BeginReferenceCapture(inoutSession.Snapshot, description, capture);
    REQUIRE((begun == S::Ready || begun == S::Pending));
    REQUIRE(Run(inoutSession.Renderer, capture, surface) == S::Ready);
    REQUIRE(inoutSession.Renderer.GetStatus(capture) == S::Ready);
    REQUIRE(outPixels.TryResize(usize(description.Width) * description.Height) == true);
    REQUIRE(inoutSession.Renderer.CopyReferencePixels(capture, outPixels.GetData(), outPixels.GetSize()) == S::Ready);
    REQUIRE(inoutSession.Renderer.GetReferenceCaptureInfo(capture, outInfo) == S::Ready);
    REQUIRE(inoutSession.Renderer.Release(capture) == S::Ready);
}
struct Comparison final
{
    float32 MaximumRadiance = 0;
    float32 MaximumEmission = 0;
    float32 MaximumDirect = 0;
    float32 MaximumIndirect = 0;
    float32 SquaredRadiance = 0;
    math::Vector3 CpuMean{};
    math::Vector3 GpuMean{};
};
Comparison Compare(const Array<rr::ReferencePixel>& cpu, const Array<rr::ReferencePixel>& gpu, uint32 samples)
{
    REQUIRE(cpu.GetSize() == gpu.GetSize());
    Comparison result;
    for (usize i = 0; i < cpu.GetSize(); ++i)
    {
        CAPTURE(i);
        REQUIRE(cpu[i].Samples == samples);
        REQUIRE(gpu[i].Samples == samples);
        const float32 radiance = math::Distance(cpu[i].Radiance, gpu[i].Radiance);
        result.MaximumRadiance = math::Max(result.MaximumRadiance, radiance);
        result.MaximumEmission = math::Max(result.MaximumEmission, math::Distance(cpu[i].Emission, gpu[i].Emission));
        result.MaximumDirect = math::Max(result.MaximumDirect, math::Distance(cpu[i].Direct, gpu[i].Direct));
        result.MaximumIndirect = math::Max(result.MaximumIndirect, math::Distance(cpu[i].Indirect, gpu[i].Indirect));
        result.SquaredRadiance += radiance * radiance;
        result.CpuMean = result.CpuMean + cpu[i].Radiance;
        result.GpuMean = result.GpuMean + gpu[i].Radiance;
    }
    result.SquaredRadiance /= float32(cpu.GetSize());
    return result;
}
} // namespace

TEST_CASE("GPU-compute reference matches the CPU oracle's raw AOVs", "[renderer][gpu][reference-compute]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Required.Compute = true;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    Session gpu;
    {
        rr::Renderer rejected;
        REQUIRE(rejected.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) ==
                S::Ready);
        const rr::ReferenceRenderingOptions options{rr::ReferenceTraversal::GpuCompute,
                                                    CPU_BUDGET,
                                                    0,
                                                    CPU_TILE_PIXELS,
                                                    GPU_BUDGET};
        // The trace kernel must bind the whole ABI; another traversal needs its own overload.
        CHECK(rejected.EnableReferenceRendering(shaders::renderer_reference_reduce::Compute(),
                                                shaders::renderer_reference_reduce::Compute(),
                                                options) == S::InvalidDescription);
        CHECK(rejected.EnableReferenceRendering({}, options) == S::InvalidDescription);
        rr::ReferenceRenderingOptions oracle = options;
        oracle.Traversal = rr::ReferenceTraversal::CpuOracle;
        CHECK(rejected.EnableReferenceRendering(shaders::renderer_reference::Compute(),
                                                shaders::renderer_reference_reduce::Compute(),
                                                oracle) == S::InvalidDescription);
        rr::ReferenceRenderingOptions unbudgeted = options;
        unbudgeted.GpuByteBudget = 0;
        CHECK(rejected.EnableReferenceRendering(shaders::renderer_reference::Compute(),
                                                shaders::renderer_reference_reduce::Compute(),
                                                unbudgeted) == S::InvalidDescription);
    }
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);

    rr::DirectLight lights[4];
    lights[0].SourceId = 41;
    lights[0].Direction = math::Vector3{.2F, -.3F, 1} / math::Length(math::Vector3{.2F, -.3F, 1});
    lights[0].Intensity = .2F;
    lights[1].SourceId = 42;
    lights[1].Kind = rr::LightKind::Point;
    lights[1].Position = {.5F, -.5F, -.4F};
    lights[1].Range = 4;
    lights[1].Intensity = .3F;
    lights[1].Color = {1, .8F, .6F};
    lights[2].SourceId = 43;
    lights[2].Kind = rr::LightKind::Spot;
    lights[2].Position = {-.5F, .5F, -.5F};
    lights[2].Direction = {0, 0, 1};
    lights[2].Range = 3;
    lights[2].InnerCos = .95F;
    lights[2].OuterCos = .7F;
    lights[2].Intensity = .5F;
    lights[3].SourceId = 44;
    lights[3].Kind = rr::LightKind::AreaTwoSided;
    lights[3].Position = {0, -.8F, -.3F};
    lights[3].Direction = {0, 1, 0};
    lights[3].Range = 4;
    lights[3].Intensity = .2F;

    struct Case final
    {
        const char* Name = "";
        bool IsPerspective = false;
        uint32 MaxDepth = 4;
        uint32 RouletteDepth = 3;
        uint64 Seed = 871;
        usize LightCount = 1;
    };
    const Case cases[]{
        {"orthographic emitter and environment", false, 4, 3, 871, 1},
        {"perspective analytic lights and roulette", true, 8, 2, 1729, 4},
        {"single bounce", true, 1, 1, 5, 4},
    };
    for (const Case& test : cases)
    {
        SECTION(test.Name)
        {
            rr::ReferenceCaptureDescription capture;
            capture.Width = 16;
            capture.Height = 12;
            capture.TargetSamples = 64;
            capture.MaxDepth = test.MaxDepth;
            capture.RouletteDepth = test.RouletteDepth;
            capture.Seed = test.Seed;
            capture.Environment = {.3F, .4F, .5F};
            capture.Lights = lights;
            capture.LightCount = test.LightCount;
            if (test.IsPerspective == true)
            {
                math::Matrix4 view;
                math::Matrix4 projection;
                capture.CameraPosition = {.1F, -.2F, -2.5F};
                REQUIRE(math::TryLookAt(capture.CameraPosition, {0, 0, 0}, {0, 1, 0}, view) ==
                        math::MathStatus::Success);
                REQUIRE(math::TryPerspectiveReverseZ(1,
                                                     float32(capture.Width) / float32(capture.Height),
                                                     .1F,
                                                     20,
                                                     projection) == math::MathStatus::Success);
                capture.WorldToClip = projection * view;
                capture.ReverseZ = true;
            }
            else
            {
                capture.OrthographicCamera = true;
                capture.WorldToClip.Columns[3].Z = .7F;
                capture.CameraPosition.Z -= .7F;
            }
            Array<rr::ReferencePixel> oracle;
            Array<rr::ReferencePixel> compute;
            rr::ReferenceCaptureInfo oracleInfo;
            rr::ReferenceCaptureInfo computeInfo;
            Capture(cpu, capture, surface, oracle, oracleInfo);
            Capture(gpu, capture, surface, compute, computeInfo);
            CHECK(computeInfo.Traversal == rr::ReferenceTraversal::GpuCompute);
            CHECK(computeInfo.InvalidContributions == 0);
            CHECK(computeInfo.Complete == true);
            CHECK(computeInfo.MinimumSamples == capture.TargetSamples);
            CHECK(computeInfo.MaximumSamples == capture.TargetSamples);
            CHECK(computeInfo.CommittedSamples == oracleInfo.CommittedSamples);
            CHECK(computeInfo.SceneFingerprintLow == oracleInfo.SceneFingerprintLow);
            CHECK(computeInfo.SceneFingerprintHigh == oracleInfo.SceneFingerprintHigh);
            const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
            CAPTURE(comparison.MaximumRadiance,
                    comparison.MaximumEmission,
                    comparison.MaximumDirect,
                    comparison.MaximumIndirect,
                    math::Sqrt(comparison.SquaredRadiance),
                    oracleInfo.ClosestQueries,
                    computeInfo.ClosestQueries,
                    oracleInfo.VisibilityQueries,
                    computeInfo.VisibilityQueries,
                    oracleInfo.NullBsdfSamples,
                    computeInfo.NullBsdfSamples,
                    oracleInfo.RouletteTerminations,
                    computeInfo.RouletteTerminations);
            // Matching addresses: float32 device arithmetic keeps each mean within a
            // small absolute RGB distance of the float64-intersection oracle.
            CHECK(comparison.MaximumRadiance <= 2e-3F);
            CHECK(comparison.MaximumEmission <= 2e-3F);
            CHECK(comparison.MaximumDirect <= 2e-3F);
            CHECK(comparison.MaximumIndirect <= 2e-3F);
            CHECK(math::Sqrt(comparison.SquaredRadiance) <= 2e-4F);
            CHECK(math::Distance(comparison.CpuMean, comparison.GpuMean) <= 1e-3F * float32(oracle.GetSize()));
            // Path events agree; a shadow ray whose proposal rounds to zero contribution in one
            // precision but not the other may be skipped, so visibility counts stay within 0.1%.
            constexpr uint64 VISIBILITY_COUNT_DIVISOR = 1000;
            CHECK(computeInfo.ClosestQueries == oracleInfo.ClosestQueries);
            CHECK(computeInfo.NullBsdfSamples == oracleInfo.NullBsdfSamples);
            CHECK(computeInfo.RouletteTerminations == oracleInfo.RouletteTerminations);
            const uint64 visibilityDifference = computeInfo.VisibilityQueries > oracleInfo.VisibilityQueries
                                                    ? computeInfo.VisibilityQueries - oracleInfo.VisibilityQueries
                                                    : oracleInfo.VisibilityQueries - computeInfo.VisibilityQueries;
            CHECK(visibilityDifference <= oracleInfo.VisibilityQueries / VISIBILITY_COUNT_DIVISOR + 1);
        }
    }
}

TEST_CASE("GPU-compute reference matches the CPU oracle on a dense glossy sphere", "[renderer][gpu][reference-compute]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Required.Compute = true;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    Session gpu;
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);
    rr::Snapshot spheres[2];
    REQUIRE(BuildSphereSnapshot(device, cpu.Renderer, cpu.Program, spheres[0]) == S::Ready);
    REQUIRE(BuildSphereSnapshot(device, gpu.Renderer, gpu.Program, spheres[1]) == S::Ready);
    REQUIRE(cpu.Renderer.Release(cpu.Snapshot) == S::Ready);
    REQUIRE(gpu.Renderer.Release(gpu.Snapshot) == S::Ready);
    cpu.Snapshot = spheres[0];
    gpu.Snapshot = spheres[1];
    rr::DirectLight sun;
    sun.SourceId = 7;
    sun.Direction = math::Vector3{-.3F, -.4F, 1} / math::Length(math::Vector3{-.3F, -.4F, 1});
    sun.Intensity = .8F;
    rr::ReferenceCaptureDescription capture;
    capture.Width = 32;
    capture.Height = 24;
    capture.TargetSamples = 16;
    capture.MaxDepth = 6;
    capture.Seed = 99;
    capture.Environment = {.2F, .25F, .3F};
    capture.Lights = &sun;
    capture.LightCount = 1;
    math::Matrix4 view;
    math::Matrix4 projection;
    capture.CameraPosition = {.3F, .2F, -2};
    REQUIRE(math::TryLookAt(capture.CameraPosition, {0, 0, 0}, {0, 1, 0}, view) == math::MathStatus::Success);
    REQUIRE(math::TryPerspectiveReverseZ(.9F, float32(capture.Width) / float32(capture.Height), .1F, 20, projection) ==
            math::MathStatus::Success);
    capture.WorldToClip = projection * view;
    capture.ReverseZ = true;
    Array<rr::ReferencePixel> oracle;
    Array<rr::ReferencePixel> compute;
    rr::ReferenceCaptureInfo oracleInfo;
    rr::ReferenceCaptureInfo computeInfo;
    const uint64 cpuStart = time::NowTicks();
    Capture(cpu, capture, surface, oracle, oracleInfo);
    const uint64 gpuStart = time::NowTicks();
    Capture(gpu, capture, surface, compute, computeInfo);
    const uint64 gpuEnd = time::NowTicks();
    const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
    CAPTURE(comparison.MaximumRadiance,
            math::Sqrt(comparison.SquaredRadiance),
            oracleInfo.ClosestQueries,
            computeInfo.ClosestQueries,
            oracleInfo.VisibilityQueries,
            computeInfo.VisibilityQueries,
            (gpuStart - cpuStart) / NANOSECONDS_PER_MILLISECOND,
            (gpuEnd - gpuStart) / NANOSECONDS_PER_MILLISECOND);
    CHECK(computeInfo.InvalidContributions == 0);
    CHECK(computeInfo.CommittedSamples == oracleInfo.CommittedSamples);
    // A rare near-edge hit can send one path elsewhere; the image mean stays matched.
    CHECK(math::Sqrt(comparison.SquaredRadiance) <= 1e-3F);
    CHECK(math::Distance(comparison.CpuMean, comparison.GpuMean) <= 1e-3F * float32(oracle.GetSize()));
}

TEST_CASE("GPU-compute reference continues targets and exports its traversal", "[renderer][gpu][reference-compute]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Required.Compute = true;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    Session gpu;
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    rr::ReferenceCaptureDescription capture;
    capture.OrthographicCamera = true;
    capture.WorldToClip.Columns[3].Z = .7F;
    capture.CameraPosition.Z -= .7F;
    capture.Width = 9;
    capture.Height = 7;
    capture.TargetSamples = 5;
    capture.Seed = 3;
    capture.Environment = {.3F, .4F, .5F};
    rr::ReferenceCapture handle;
    const S begun = gpu.Renderer.BeginReferenceCapture(gpu.Snapshot, capture, handle);
    REQUIRE((begun == S::Ready || begun == S::Pending));
    // Setup polls without a surface; a dispatch needs one and the capture stays retryable.
    S status = S::Pending;
    const uint64 start = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - start < TIMEOUT)
    {
        status = gpu.Renderer.AdvanceReferenceCapture(handle);
    }
    CHECK(status == S::InvalidDescription);
    CHECK(gpu.Renderer.GetStatus(handle) == S::Pending);
    REQUIRE(Run(gpu.Renderer, handle, surface) == S::Ready);
    Array<rr::ReferencePixel> first;
    REQUIRE(first.TryResize(usize(capture.Width) * capture.Height) == true);
    REQUIRE(gpu.Renderer.CopyReferencePixels(handle, first.GetData(), first.GetSize()) == S::Ready);
    for (const rr::ReferencePixel& pixel : first)
    {
        CHECK(pixel.Samples == 5);
    }
    // Raising only the target continues the same accumulation.
    rr::ReferenceCaptureDescription extended = capture;
    extended.TargetSamples = 9;
    CHECK(gpu.Renderer.UpdateReferenceCapture(handle, gpu.Snapshot, extended) == S::Pending);
    REQUIRE(Run(gpu.Renderer, handle, surface) == S::Ready);
    rr::ReferenceCaptureInfo info;
    REQUIRE(gpu.Renderer.GetReferenceCaptureInfo(handle, info) == S::Ready);
    CHECK(info.Revision == 1);
    CHECK(info.MinimumSamples == 9);
    CHECK(info.CommittedSamples == uint64{9} * first.GetSize());
    // A fresh capture of nine samples equals the continued one exactly.
    rr::ReferenceCapture fresh;
    const S freshBegun = gpu.Renderer.BeginReferenceCapture(gpu.Snapshot, extended, fresh);
    REQUIRE((freshBegun == S::Ready || freshBegun == S::Pending));
    REQUIRE(Run(gpu.Renderer, fresh, surface) == S::Ready);
    Array<rr::ReferencePixel> continued;
    Array<rr::ReferencePixel> restarted;
    REQUIRE(continued.TryResize(first.GetSize()) == true);
    REQUIRE(restarted.TryResize(first.GetSize()) == true);
    REQUIRE(gpu.Renderer.CopyReferencePixels(handle, continued.GetData(), continued.GetSize()) == S::Ready);
    REQUIRE(gpu.Renderer.CopyReferencePixels(fresh, restarted.GetData(), restarted.GetSize()) == S::Ready);
    for (usize i = 0; i < continued.GetSize(); ++i)
    {
        CAPTURE(i);
        CHECK(continued[i].Samples == 9);
        CHECK(continued[i].Radiance == restarted[i].Radiance);
    }
    REQUIRE(gpu.Renderer.Release(fresh) == S::Ready);
    // A seed change rebuilds the integral transactionally with a new revision.
    rr::ReferenceCaptureDescription reseeded = extended;
    reseeded.Seed = 4;
    const S replaced = gpu.Renderer.UpdateReferenceCapture(handle, gpu.Snapshot, reseeded);
    REQUIRE((replaced == S::Ready || replaced == S::Pending));
    REQUIRE(Run(gpu.Renderer, handle, surface) == S::Ready);
    REQUIRE(gpu.Renderer.GetReferenceCaptureInfo(handle, info) == S::Ready);
    CHECK(info.Revision == 2);
    CHECK(info.Seed == 4);
    constexpr usize HEADER_BYTES = 256;
    constexpr usize PIXEL_BYTES = 16;
    constexpr usize FLAGS_OFFSET = 44;
    constexpr uint32 GPU_COMPUTE_FLAG = 64;
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(HEADER_BYTES + first.GetSize() * PIXEL_BYTES) == true);
    usize written = 0;
    REQUIRE(gpu.Renderer.EncodeReferenceHdr(handle, bytes.GetData(), bytes.GetSize(), written) == S::Ready);
    CHECK(written == bytes.GetSize());
    const uint32 flags = uint32(bytes[FLAGS_OFFSET]) | (uint32(bytes[FLAGS_OFFSET + 1]) << 8);
    CHECK((flags & GPU_COMPUTE_FLAG) != 0);
    CHECK((flags & 1) == 0);
    REQUIRE(gpu.Renderer.Release(handle) == S::Ready);
}
