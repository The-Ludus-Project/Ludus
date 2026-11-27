#include "../shaders/wave_connection_layout.h"
#include "../src/internal/spectral_sensor.h"
#include "l6_scene.h"
#include "material_program_fixtures.h"
#include "microfacet_random_walk.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_reference.h"
#include "renderer_reference_reduce.h"
#include "renderer_shadow.h"
#include <catch2/generators/catch_generators.hpp>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/graphics/renderer/environment.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

// CPU oracle / GPU-compute parity for the RT1 reference integral. Both paths read
// the same addressed Philox dimensions, so matching samples follow matching paths;
// differences come from float32 traversal/arithmetic on the device.
namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
namespace threading = ludus::foundation::threading;
using S = rhi::RasterStatus;
constexpr usize CPU_BUDGET = usize{64} * 1024 * 1024;
constexpr usize GPU_BUDGET = usize{64} * 1024 * 1024;
constexpr uint32 CPU_TILE_PIXELS = 256;
constexpr uint64 TIMEOUT = 60 * time::NANOSECONDS_PER_SECOND;
constexpr uint64 COLD_CAPTURE_TIMEOUT_NANOSECONDS =
    LUDUS_RENDERER_TEST_COLD_GPU_TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND;
constexpr time::Duration COMPLETION_POLL_INTERVAL = time::Milliseconds(1);
constexpr uint64 NANOSECONDS_PER_MILLISECOND = time::NANOSECONDS_PER_SECOND / 1000;

struct Session final
{
    rr::Renderer Renderer;
    rr::MaterialProgram Program;
    rr::Snapshot Snapshot;
};
S Prepare(rhi::DeviceHandle device,
          rr::ReferenceTraversal traversal,
          Session& outSession,
          bool shouldIncludeDegenerateTriangles = false)
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
    return ludus::qa::BuildL6Snapshot(renderer,
                                      outSession.Program,
                                      outSession.Snapshot,
                                      -.7F,
                                      shouldIncludeDegenerateTriangles);
}
/// How BuildSphereSnapshot's authored normals relate to its triangle winding.
enum class NormalLayout : uint8
{
    /// Every corner normal lies on the winding's side of its face.
    Matched,
    /// The sphere's winding is reversed against its outward normals.
    ReversedWinding,
    /// ReversedWinding, an inward band of sphere normals, and quad corner normals on both sides of every face, so the
    /// wall and the emitters straddle as smoothed normals across a crease do.
    Mixed,
};
/// Typed scattering that replaces BuildSphereSnapshot's opaque materials.
enum class SphereSurfaces : uint8
{
    /// The legacy or typed opaque reflectors only.
    Opaque,
    /// Weighted diffuse and rough conductor with matched proposals.
    Mix,
    /// Project-authored three-lobe continuous reflection.
    ProjectBrdf,
    /// Coated diffuse on the sphere, the wall and the masked quad.
    Coated,
    /// Coated diffuse with a black substrate on the sphere and reflective substrates on the wall and quad.
    CoatedBlack,
    /// A smooth glass sphere (index 1.5) and a thin masked pane (index 1.33) in front of the wall.
    Glass,
    /// A closure program of diffuse, conductor and cosine-power lobes whose weights read the textured
    /// base colour and metallic inputs, on every reflector including the checker-textured quad.
    Program,
    /// A rough complex-IOR conductor sphere (alpha 0.6) before a rough legacy metal wall (roughness 0.8), where
    /// scattering between microfacets is significant.
    RoughMetal,
};

/// Every SPHERE_INWARD_ROW_PERIOD-th sphere ring of a Mixed layout points its normals inwards.
constexpr uint32 SPHERE_INWARD_ROW_PERIOD = 8;
// A tessellated glossy metal sphere in front of a diffuse wall and two emitters of
// unequal area: thousands of triangles exercise deep BVH traversal and visible-GGX
// sampling, and the unequal emitters exercise the power-weighted emitter CDF.
S BuildSphereSnapshot(rhi::DeviceHandle device,
                      rr::Renderer& inoutRenderer,
                      rr::MaterialProgram program,
                      rr::Snapshot& outSnapshot,
                      bool shouldUseTypedMaterials = false,
                      SphereSurfaces surfaces = SphereSurfaces::Opaque,
                      NormalLayout normalLayout = NormalLayout::Matched)
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
            const bool isInward =
                normalLayout == NormalLayout::Mixed && row % SPHERE_INWARD_ROW_PERIOD == SPHERE_INWARD_ROW_PERIOD / 2;
            REQUIRE(normals.TryAdd(isInward == true ? -normal : normal) == true);
        }
    }
    for (uint32 row = 1; row + 1 < LATITUDES; ++row)
    {
        for (uint32 column = 0; column < LONGITUDES; ++column)
        {
            const uint32 a = row * (LONGITUDES + 1) + column;
            const uint32 b = a + LONGITUDES + 1;
            const uint32 quad[]{a, a + 1, b, a + 1, b + 1, b};
            const uint32 reversedQuad[]{a, b, a + 1, a + 1, b, b + 1};
            for (const uint32 index : normalLayout == NormalLayout::Matched ? quad : reversedQuad)
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
    const math::Vector3 straddlingQuadNormals[]{{0, 0, -1}, {0, 0, -1}, {0, 0, 1}, {0, 0, 1}};
    const uint32 quadIndices[]{0, 2, 1, 0, 3, 2};
    const math::Vector2 quadUvs[]{{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    rr::MeshDescription quadDescription{quadPositions, 4, quadIndices, 6};
    quadDescription.Normals = normalLayout == NormalLayout::Mixed ? straddlingQuadNormals : quadNormals;
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
        if (shouldUseTypedMaterials == true)
        {
            namespace format = ludus::content::scene_format;
            material.Scattering.Model = i == 0 ? format::MaterialModel::Conductor : format::MaterialModel::Diffuse;
            material.Scattering.AlphaU = 0.1F;
            material.Scattering.AlphaV = 0.1F;
            material.Scattering.Eta[0] = 1.65746F;
            material.Scattering.Eta[1] = 0.880369F;
            material.Scattering.Eta[2] = 0.521229F;
            material.Scattering.K[0] = 9.223869F;
            material.Scattering.K[1] = 6.269523F;
            material.Scattering.K[2] = 4.837001F;
        }
        if (surfaces == SphereSurfaces::RoughMetal && i < 2)
        {
            namespace format = ludus::content::scene_format;
            material.Metallic = 1;
            material.Roughness = .8F;
            if (i == 0)
            {
                material.Scattering.Model = format::MaterialModel::Conductor;
                material.Scattering.AlphaU = 0.6F;
                material.Scattering.AlphaV = 0.6F;
                material.Scattering.Eta = {{0.143F, 0.374F, 1.442F}};
                material.Scattering.K = {{3.983F, 2.385F, 1.603F}};
            }
            else
            {
                material.Surface.Tint = {.9F, .9F, .9F, 1};
            }
        }
        if (surfaces == SphereSurfaces::Mix)
        {
            material.Scattering.Model = ludus::content::scene_format::MaterialModel::DiffuseConductorMix;
            material.Metallic = i == 0 ? 0.25F : 0.65F;
        }
        if (surfaces == SphereSurfaces::ProjectBrdf)
        {
            namespace format = ludus::content::scene_format;
            material.Scattering = {};
            material.Scattering.Model = format::MaterialModel::ProjectBrdf;
            material.Scattering.Eta = {{0.25F, 0.5F, 0.25F}};
            material.Scattering.K = {{0, 4, 12}};
            material.Scattering.CoatingThickness = 1;
            material.Metallic = 0;
            material.Roughness = i == 0 ? 0.35F : 0.7F;
        }
        if (surfaces == SphereSurfaces::Program && i != 2)
        {
            namespace format = ludus::content::scene_format;
            material.Scattering = {};
            material.Scattering.Model = format::MaterialModel::Program;
            material.Program = ludus::renderer_tests::MakeParityProgram();
            material.Metallic = i == 0 ? 0.8F : 0.2F;
            material.Roughness = i == 0 ? 0.35F : 0.7F;
        }
        if (surfaces == SphereSurfaces::Glass && (i == 0 || i == 3))
        {
            namespace format = ludus::content::scene_format;
            material.Scattering = {};
            material.Scattering.Model =
                i == 0 ? format::MaterialModel::Dielectric : format::MaterialModel::ThinDielectric;
            const float32 eta = i == 0 ? format::DEFAULT_DIELECTRIC_ETA : 1.33F;
            material.Scattering.Eta = {{eta, eta, eta}};
            material.Scattering.AlphaU = format::MIN_MATERIAL_ALPHA;
            material.Scattering.AlphaV = format::MIN_MATERIAL_ALPHA;
        }
        if ((surfaces == SphereSurfaces::Coated || surfaces == SphereSurfaces::CoatedBlack) && i != 2)
        {
            namespace format = ludus::content::scene_format;
            material.Scattering.Model = format::MaterialModel::CoatedDiffuse;
            material.Scattering.AlphaU = i == 1 ? 0.01F : 0.1F;
            material.Scattering.AlphaV = material.Scattering.AlphaU;
            if (surfaces == SphereSurfaces::CoatedBlack && i == 0)
            {
                material.Surface.Tint = {0, 0, 0, 1};
            }
            if (i == 3)
            {
                material.Scattering.CoatingEta = 1.33F;
                material.Scattering.CoatingThickness = 0.03F;
            }
        }
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
    constexpr usize ITEM_COUNT = MATERIAL_COUNT + 1;
    rr::SceneItem items[ITEM_COUNT];
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
    items[4].Geometry = meshes[1];
    items[4].Surface = materials[2];
    items[4].SourceId = 5;
    items[4].Transform.Columns[0].X = .04F;
    items[4].Transform.Columns[1].Y = .04F;
    items[4].Transform.Translation = {.7F, .6F, -.4F};
    const S created = inoutRenderer.CreateSnapshot(items, ITEM_COUNT, outSnapshot);
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
    uint64 windowStartedTicks = time::NowTicks();
    uint64 timeoutNanoseconds = COLD_CAPTURE_TIMEOUT_NANOSECONDS;
    bool hasCompletedColdWork = false;
    rr::ReferenceCaptureInfo progress;
    while (status == S::Pending && time::NowTicks() - windowStartedTicks < timeoutNanoseconds)
    {
        threading::SleepFor(COMPLETION_POLL_INTERVAL);
        status = inoutRenderer.AdvanceReferenceCapture(capture, surface);
        REQUIRE(inoutRenderer.GetReferenceCaptureInfo(capture, progress) == S::Ready);
        if (hasCompletedColdWork == false && progress.CommittedSamples > 0)
        {
            hasCompletedColdWork = true;
            windowStartedTicks = time::NowTicks();
            timeoutNanoseconds = TIMEOUT;
        }
    }
    return status;
}
void Capture(Session& inoutSession,
             const rr::ReferenceCaptureDescription& description,
             rhi::SurfaceHandle surface,
             Array<rr::ReferencePixel>& outPixels,
             rr::ReferenceCaptureInfo& outInfo)
{
    const uint64 startedTicks = time::NowTicks();
    rr::ReferenceCapture capture;
    const S begun = inoutSession.Renderer.BeginReferenceCapture(inoutSession.Snapshot, description, capture);
    REQUIRE((begun == S::Ready || begun == S::Pending));
    const S completed = Run(inoutSession.Renderer, capture, surface);
    REQUIRE(inoutSession.Renderer.GetReferenceCaptureInfo(capture, outInfo) == S::Ready);
    const uint64 elapsedNanoseconds = time::NowTicks() - startedTicks;
    CAPTURE(outInfo.MinimumSamples,
            outInfo.MaximumSamples,
            outInfo.TargetSamples,
            outInfo.CommittedSamples,
            elapsedNanoseconds,
            COLD_CAPTURE_TIMEOUT_NANOSECONDS,
            outInfo.FailurePixel,
            outInfo.FailureDepth,
            outInfo.FailureSample,
            static_cast<uint32>(outInfo.Failure),
            outInfo.InvalidContributions);
    REQUIRE(completed == S::Ready);
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

namespace
{
struct ReferenceRawAovCase final
{
    const char* Name = "";
    bool IsPerspective = false;
    uint32 MaxDepth = 4;
    uint32 RouletteDepth = 3;
    uint64 Seed = 871;
    usize LightCount = 1;
    bool HasEnvironmentMap = false;
    float32 OpticalWavelengthWorld = 0;
    rr::ReferenceOpticalConnection OpticalConnection = rr::ReferenceOpticalConnection::AngularSpectrum;
    rr::ReferenceOpticalSpectrum OpticalSpectrum = rr::ReferenceOpticalSpectrum::Monochromatic;
    bool IsFarConventional = false;
    rr::ReferenceLightSampling LightSampling = rr::ReferenceLightSampling::UniformArea;
    bool HasSecondDirectionalLight = false;
    /// Replace the environment map with a World Atmosphere's sky and attenuate the first light as its Sun,
    /// as the editor's reference capture does.
    bool HasAtmosphereSky = false;
};

void CheckReferenceRawAovs(const ReferenceRawAovCase& test)
{
    constexpr uint32 CAPTURE_WIDTH_PIXELS = 16;
    constexpr uint32 CAPTURE_HEIGHT_PIXELS = 12;
    constexpr uint32 CAPTURE_SAMPLE_COUNT = 64;
    CAPTURE(test.Name);

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
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu, true);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu, true) == S::Ready);

    constexpr usize ENVIRONMENT_PIXEL_COUNT = 4;
    // Reconstruct a factored import's radiance before either reference implementation
    // sees it. A nonuniform map exercises its raw lookup and solid-angle CDF.
    StaticArray<math::Vector4, ENVIRONMENT_PIXEL_COUNT> environmentPixels{
        {{.125F, .25F, .0625F, 0}, {.0625F, .125F, .25F, 0}, {.25F, .0625F, .125F, 0}, {.03125F, .0625F, .125F, 0}}};
    for (math::Vector4& pixel : environmentPixels)
    {
        pixel = pixel * 16;
    }
    // A clear sky above an anchor below the scene, with a Sun forty degrees up.
    constexpr uint32 ATMOSPHERE_WIDTH = 32;
    constexpr uint32 ATMOSPHERE_HEIGHT = ATMOSPHERE_WIDTH / 2;
    constexpr float32 ATMOSPHERE_ANCHOR_DEPTH = 5;
    constexpr float32 SOLAR_ILLUMINANCE = 6;
    StaticArray<math::Vector4, static_cast<usize>(ATMOSPHERE_WIDTH) * ATMOSPHERE_HEIGHT> atmospherePixels{};
    rr::AtmosphereDescription atmosphere;
    atmosphere.IsEnabled = true;
    atmosphere.ShouldLightScene = true;
    atmosphere.SurfaceAnchorWorld = {0, -ATMOSPHERE_ANCHOR_DEPTH, 0};
    atmosphere.DirectionToSunWorld = math::Vector3{.4F, .64F, .3F} / math::Length(math::Vector3{.4F, .64F, .3F});
    atmosphere.SolarIlluminance = {SOLAR_ILLUMINANCE, SOLAR_ILLUMINANCE, SOLAR_ILLUMINANCE};
    math::Vector3 sunIlluminance{};
    rr::EnvironmentMap environment;
    rr::EnvironmentDescription environmentDescription;
    environmentDescription.PixelsOrNull = environmentPixels.GetData();
    environmentDescription.Width = 2;
    environmentDescription.Height = 2;
    if (test.HasAtmosphereSky == true)
    {
        REQUIRE(rr::ResolveAtmosphereEnvironment(atmosphere,
                                                 {0, 0, 0},
                                                 ATMOSPHERE_WIDTH,
                                                 ATMOSPHERE_HEIGHT,
                                                 {atmospherePixels.GetData(), atmospherePixels.GetSize()},
                                                 sunIlluminance) == rr::AtmosphereEnvironmentStatus::Ready);
        environmentDescription.PixelsOrNull = atmospherePixels.GetData();
        environmentDescription.Width = ATMOSPHERE_WIDTH;
        environmentDescription.Height = ATMOSPHERE_HEIGHT;
    }
    environmentDescription.FilterWidth = 16;
    environmentDescription.FilterSamples = 16;
    environmentDescription.CpuByteBudget = CPU_BUDGET;
    S environmentStatus = environment.Initialize(device, environmentDescription);
    const uint64 environmentStarted = time::NowTicks();
    while (environmentStatus == S::Pending && time::NowTicks() - environmentStarted < TIMEOUT)
    {
        environmentStatus = environment.GetStatus();
    }
    REQUIRE(environmentStatus == S::Ready);

    constexpr usize DIRECT_LIGHT_COUNT = 4;
    StaticArray<rr::DirectLight, DIRECT_LIGHT_COUNT> lights;
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
    if (test.HasSecondDirectionalLight == true)
    {
        lights[3].Kind = rr::LightKind::Directional;
        lights[3].Direction = {0, 0, 1};
    }
    if (test.HasAtmosphereSky == true)
    {
        lights[0].Direction = -atmosphere.DirectionToSunWorld;
        lights[0].Intensity = math::Max(sunIlluminance.X, math::Max(sunIlluminance.Y, sunIlluminance.Z));
        REQUIRE(lights[0].Intensity > 0);
        lights[0].Color = sunIlluminance / lights[0].Intensity;
    }

    rr::ReferenceCaptureDescription capture;
    capture.Width = CAPTURE_WIDTH_PIXELS;
    capture.Height = CAPTURE_HEIGHT_PIXELS;
    capture.TargetSamples = CAPTURE_SAMPLE_COUNT;
    capture.MaxDepth = test.MaxDepth;
    capture.OpticalWavelengthWorld = test.OpticalWavelengthWorld;
    capture.OpticalConnection = test.OpticalConnection;
    capture.OpticalSpectrum = test.OpticalSpectrum;
    capture.RouletteDepth = test.RouletteDepth;
    capture.Seed = test.Seed;
    capture.LightSampling = test.LightSampling;
    capture.Environment = test.HasAtmosphereSky == true ? math::Vector3{} : math::Vector3{.3F, .4F, .5F};
    capture.EnvironmentTextureOrNull =
        test.HasEnvironmentMap == true || test.HasAtmosphereSky == true ? &environment : nullptr;
    capture.Lights = lights.GetData();
    capture.LightCount = test.LightCount;
    if (test.IsPerspective == true)
    {
        math::Matrix4 view;
        math::Matrix4 projection;
        capture.CameraPosition =
            test.IsFarConventional == true ? math::Vector3{300, -.2F, -2.5F} : math::Vector3{.1F, -.2F, -2.5F};
        REQUIRE(math::TryLookAt(capture.CameraPosition, {0, 0, 0}, {0, 1, 0}, view) == math::MathStatus::Success);
        REQUIRE(
            math::TryPerspectiveReverseZ(1, float32(capture.Width) / float32(capture.Height), .1F, 20, projection) ==
            math::MathStatus::Success);
        capture.WorldToClip = projection * view;
        if (test.IsFarConventional == true)
        {
            for (math::Vector4& column : capture.WorldToClip.Columns)
            {
                column.Z = column.W - column.Z;
            }
        }
        capture.ReverseZ = test.IsFarConventional == false;
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
    CHECK(computeInfo.LightSampling == test.LightSampling);
    CHECK(computeInfo.SamplerVersion == oracleInfo.SamplerVersion);
    CHECK(computeInfo.InvalidContributions == 0);
    CHECK(computeInfo.SkippedTriangles > 0);
    CHECK(computeInfo.SkippedTriangles == oracleInfo.SkippedTriangles);
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
} // namespace

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: orthographic emitter and environment",
          "[renderer][gpu][reference-compute]")
{
    CheckReferenceRawAovs(
    {
        .Name = "orthographic emitter and environment",
        .IsPerspective = false,
        .MaxDepth = 4,
        .RouletteDepth = 3,
        .Seed = 871,
        .LightCount = 1,
        .HasEnvironmentMap = false,
        .OpticalWavelengthWorld = 0,
        .IsFarConventional = false,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: perspective analytic lights and roulette",
          "[renderer][gpu][reference-compute]")
{
    CheckReferenceRawAovs(
    {
        .Name = "perspective analytic lights and roulette",
        .IsPerspective = true,
        .MaxDepth = 8,
        .RouletteDepth = 2,
        .Seed = 1729,
        .LightCount = 4,
        .HasEnvironmentMap = false,
        .OpticalWavelengthWorld = 0,
        .IsFarConventional = false,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: single bounce", "[renderer][gpu][reference-compute]")
{
    CheckReferenceRawAovs(
    {
        .Name = "single bounce",
        .IsPerspective = true,
        .MaxDepth = 1,
        .RouletteDepth = 1,
        .Seed = 5,
        .LightCount = 4,
        .HasEnvironmentMap = false,
        .OpticalWavelengthWorld = 0,
        .IsFarConventional = false,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: independent directional proposals",
          "[renderer][gpu][reference-compute]")
{
    CheckReferenceRawAovs(
    {
        .Name = "two directional lights, finite lights, emitter and environment",
        .IsPerspective = true,
        .MaxDepth = 4,
        .RouletteDepth = 2,
        .Seed = 31,
        .LightCount = 4,
        .HasEnvironmentMap = true,
        .LightSampling = rr::ReferenceLightSampling::SeparateDirectional,
        .HasSecondDirectionalLight = true,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: World Atmosphere sky and attenuated Sun",
          "[renderer][gpu][reference-compute][atmosphere]")
{
    CheckReferenceRawAovs(
    {
        .Name = "World Atmosphere sky and attenuated Sun",
        .IsPerspective = true,
        .MaxDepth = 4,
        .RouletteDepth = 3,
        .Seed = 2718,
        .LightCount = 1,
        .HasEnvironmentMap = false,
        .LightSampling = rr::ReferenceLightSampling::SeparateDirectional,
        .HasAtmosphereSky = true,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: scalar optical scene connections",
          "[renderer][gpu][reference-compute]")
{
    CheckReferenceRawAovs(
    {
        .Name = "scalar optical scene connections",
        .IsPerspective = true,
        .MaxDepth = 4,
        .RouletteDepth = 3,
        .Seed = 871,
        .LightCount = 4,
        .HasEnvironmentMap = false,
        .OpticalWavelengthWorld = 0.001F,
        .IsFarConventional = false,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: UTD edge diffraction scene connections",
          "[renderer][gpu][reference-compute][wave-utd]")
{
    CheckReferenceRawAovs(
    {
        .Name = "UTD edge diffraction scene connections",
        .IsPerspective = true,
        .MaxDepth = 4,
        .RouletteDepth = 3,
        .Seed = 871,
        .LightCount = 4,
        .HasEnvironmentMap = false,
        .OpticalWavelengthWorld = 0.001F,
        .OpticalConnection = rr::ReferenceOpticalConnection::UtdEdgeDiffraction,
        .IsFarConventional = false,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: Fraunhofer edge diffraction scene connections",
          "[renderer][gpu][reference-compute][wave-fraunhofer]")
{
    CheckReferenceRawAovs(
    {
        .Name = "Fraunhofer edge diffraction scene connections",
        .IsPerspective = true,
        .MaxDepth = 4,
        .RouletteDepth = 3,
        .Seed = 877,
        .LightCount = 4,
        .HasEnvironmentMap = false,
        .OpticalWavelengthWorld = 0.001F,
        .OpticalConnection = rr::ReferenceOpticalConnection::FraunhoferEdgeDiffraction,
        .IsFarConventional = false,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: spectral optical scene connections",
          "[renderer][gpu][reference-compute][wave-spectral]")
{
    // Every path samples its own wavelength; matching addresses give both traversals the same one.
    const rr::ReferenceOpticalConnection connection =
        GENERATE(rr::ReferenceOpticalConnection::AngularSpectrum,
                 rr::ReferenceOpticalConnection::UtdEdgeDiffraction,
                 rr::ReferenceOpticalConnection::FraunhoferEdgeDiffraction);
    CAPTURE(connection);
    CheckReferenceRawAovs(
    {
        .Name = "spectral optical scene connections",
        .IsPerspective = true,
        .MaxDepth = 4,
        .RouletteDepth = 3,
        .Seed = 883,
        .LightCount = 4,
        .HasEnvironmentMap = false,
        .OpticalWavelengthWorld = 0.001F,
        .OpticalConnection = connection,
        .OpticalSpectrum = rr::ReferenceOpticalSpectrum::Visible,
        .IsFarConventional = false,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: perspective textured environment",
          "[renderer][gpu][reference-compute]")
{
    CheckReferenceRawAovs(
    {
        .Name = "perspective textured environment",
        .IsPerspective = true,
        .MaxDepth = 4,
        .RouletteDepth = 3,
        .Seed = 314159,
        .LightCount = 1,
        .HasEnvironmentMap = true,
        .OpticalWavelengthWorld = 0,
        .IsFarConventional = false,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: far conventional optical camera",
          "[renderer][gpu][reference-compute]")
{
    CheckReferenceRawAovs(
    {
        .Name = "far conventional optical camera",
        .IsPerspective = true,
        .MaxDepth = 4,
        .RouletteDepth = 3,
        .Seed = 871,
        .LightCount = 4,
        .HasEnvironmentMap = false,
        .OpticalWavelengthWorld = 0.001F,
        .IsFarConventional = true,
    });
}

TEST_CASE("GPU-compute raw AOVs match the CPU oracle: resampled mixed lights and textured environment",
          "[renderer][gpu][reference-compute][resampled-light]")
{
    for (const rr::ReferenceLightSampling mode :
         {rr::ReferenceLightSampling::ResampledProjectedSolidAngle, rr::ReferenceLightSampling::ResampledFormFactor})
    {
        CAPTURE(static_cast<uint32>(mode));
        CheckReferenceRawAovs(
        {
            .Name = "resampled mixed lights and textured environment",
            .IsPerspective = true,
            .MaxDepth = 4,
            .RouletteDepth = 2,
            .Seed = 314159,
            .LightCount = 4,
            .HasEnvironmentMap = true,
            .LightSampling = mode,
        });
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
    constexpr usize SCENE_FINGERPRINT_OFFSET = 64;
    constexpr uint32 GPU_COMPUTE_FLAG = 64;
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(HEADER_BYTES + first.GetSize() * PIXEL_BYTES) == true);
    usize written = 0;
    REQUIRE(gpu.Renderer.EncodeReferenceHdr(handle, bytes.GetData(), bytes.GetSize(), written) == S::Ready);
    CHECK(written == bytes.GetSize());
    uint64 fingerprintLow = 0;
    uint64 fingerprintHigh = 0;
    const uint8* fingerprint = bytes.GetData() + SCENE_FINGERPRINT_OFFSET;
    REQUIRE(TryReadLittleEndian({fingerprint, sizeof(uint64)}, fingerprintLow) == true);
    REQUIRE(TryReadLittleEndian({fingerprint + sizeof(uint64), sizeof(uint64)}, fingerprintHigh) == true);
    CHECK(fingerprintLow == info.SceneFingerprintLow);
    CHECK(fingerprintHigh == info.SceneFingerprintHigh);
    const uint32 flags = uint32(bytes[FLAGS_OFFSET]) | (uint32(bytes[FLAGS_OFFSET + 1]) << 8);
    CHECK((flags & GPU_COMPUTE_FLAG) != 0);
    CHECK((flags & 1) == 0);
    REQUIRE(gpu.Renderer.Release(handle) == S::Ready);
}

TEST_CASE("RT1 directional shadow rays respect occlusion alongside emissive and sky samples",
          "[renderer][gpu][reference-compute]")
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
    rhi::DeviceDescription deviceDescription;
    deviceDescription.Required.PortableRaster = true;
    deviceDescription.Required.Compute = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 1, .Height = 1 }, deviceDescription, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No offscreen compute adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);

    rr::ReferenceCaptureDescription capture;
    capture.Width = 4;
    capture.Height = 1;
    capture.TargetSamples = 4;
    capture.MaxDepth = 1;
    capture.RouletteDepth = 1;
    capture.CenterRays = true;
    capture.OrthographicCamera = true;
    capture.WorldToClip.Columns[0].X = 0.5F;
    capture.WorldToClip.Columns[3].Z = 0.7F;
    capture.CameraPosition.Z = -0.7F;
    capture.Environment = {0, 0, 0.2F};
    capture.LightSampling = rr::ReferenceLightSampling::SeparateDirectional;
    rr::DirectLight light;
    light.SourceId = 1;
    light.Kind = rr::LightKind::Directional;
    light.Direction = {-0.70710677F, 0, 0.70710677F};
    light.Color = {1, 0, 0};
    capture.Lights = &light;
    capture.LightCount = 1;
    for (const rr::ReferenceTraversal traversal :
         {rr::ReferenceTraversal::CpuOracle, rr::ReferenceTraversal::GpuCompute})
    {
        CAPTURE(static_cast<uint32>(traversal));
        Session session;
        REQUIRE(Prepare(device, traversal, session) == S::Ready);
        Array<rr::ReferencePixel> dark;
        Array<rr::ReferencePixel> lit;
        rr::ReferenceCaptureInfo info;
        light.Intensity = 0;
        Capture(session, capture, surface, dark, info);
        light.Intensity = 1;
        Capture(session, capture, surface, lit, info);
        CHECK(info.SamplerVersion == 4);
        CHECK(info.LightSampling == rr::ReferenceLightSampling::SeparateDirectional);
        // The -1.5 receiver clears the end of the L6 wall; the +0.5 receiver is behind it.
        constexpr usize UNBLOCKED_PIXEL = 0;
        constexpr usize BLOCKED_PIXEL = 2;
        CHECK(lit[UNBLOCKED_PIXEL].Direct.X - dark[UNBLOCKED_PIXEL].Direct.X > 0.1F);
        CHECK(math::Distance(lit[BLOCKED_PIXEL].Direct, dark[BLOCKED_PIXEL].Direct) < 1e-5F);
        CHECK(lit[UNBLOCKED_PIXEL].Direct.Z == dark[UNBLOCKED_PIXEL].Direct.Z);
    }
}

// The directional estimate at each pixel, isolated from the emitter's by subtracting a capture with the light off:
// the directional draws use their own dimensions, so the remaining estimates match sample for sample.
Array<float32> CaptureDirectionalSignal(Session& inoutSession,
                                        rr::ReferenceCaptureDescription capture,
                                        rr::DirectLight light,
                                        rhi::SurfaceHandle surface)
{
    capture.Lights = &light;
    capture.LightCount = 1;
    Array<rr::ReferencePixel> dark;
    Array<rr::ReferencePixel> lit;
    rr::ReferenceCaptureInfo info;
    const float32 intensity = light.Intensity;
    light.Intensity = 0;
    Capture(inoutSession, capture, surface, dark, info);
    light.Intensity = intensity;
    Capture(inoutSession, capture, surface, lit, info);
    CHECK(info.InvalidContributions == 0);
    Array<float32> signal;
    REQUIRE(signal.TryResize(lit.GetSize()) == true);
    for (usize i = 0; i < lit.GetSize(); ++i)
    {
        signal[i] = lit[i].Direct.X - dark[i].Direct.X;
    }
    return signal;
}

TEST_CASE("Reference directional disks soften shadows and keep their illuminance on CPU and GPU",
          "[renderer][gpu][reference-compute]")
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
    rhi::DeviceDescription deviceDescription;
    deviceDescription.Required.PortableRaster = true;
    deviceDescription.Required.Compute = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 1, .Height = 1 }, deviceDescription, device, surface);
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No offscreen compute adapter");
    }
    REQUIRE(started == rhi::DeviceStatus::Ready);
    const rr::ReferenceLightSampling sampling =
        GENERATE(rr::ReferenceLightSampling::UniformArea, rr::ReferenceLightSampling::SeparateDirectional);
    CAPTURE(static_cast<uint32>(sampling));

    // A row of floor receivers from x = -2 to 2 across the end of the L6 wall's shadow. A disk of radius r about the
    // 45 degree light leaves receivers whose edge elevation atan(2.3 / (1.2 - x)) lies within r of 45 degrees in
    // penumbra: x in [-1.61, -0.68] for r = 0.1, about seven pixels. Pixels 0 and 1 see the whole disk.
    constexpr uint32 RECEIVER_COUNT = 32;
    constexpr uint32 SAMPLE_COUNT = 256;
    constexpr float32 DISK_RADIUS_RADIANS = 0.1F;
    rr::ReferenceCaptureDescription capture;
    capture.Width = RECEIVER_COUNT;
    capture.Height = 1;
    capture.TargetSamples = SAMPLE_COUNT;
    capture.MaxDepth = 1;
    capture.RouletteDepth = 1;
    capture.CenterRays = true;
    capture.OrthographicCamera = true;
    capture.WorldToClip.Columns[0].X = 0.5F;
    capture.WorldToClip.Columns[3].Z = 0.7F;
    capture.CameraPosition.Z = -0.7F;
    capture.LightSampling = sampling;
    rr::DirectLight light;
    light.SourceId = 1;
    light.Kind = rr::LightKind::Directional;
    light.Direction = {-0.70710677F, 0, 0.70710677F};
    light.Color = {1, 0, 0};

    StaticArray<Array<float32>, 2> disks;
    usize traversalIndex = 0;
    for (const rr::ReferenceTraversal traversal :
         {rr::ReferenceTraversal::CpuOracle, rr::ReferenceTraversal::GpuCompute})
    {
        CAPTURE(static_cast<uint32>(traversal));
        Session session;
        REQUIRE(Prepare(device, traversal, session) == S::Ready);
        light.AngularRadiusRadians = 0;
        const Array<float32> delta = CaptureDirectionalSignal(session, capture, light, surface);
        light.AngularRadiusRadians = DISK_RADIUS_RADIANS;
        Array<float32> disk = CaptureDirectionalSignal(session, capture, light, surface);
        const float32 unblocked = delta[0];
        REQUIRE(unblocked > 0.1F);
        // UniformArea picks the directional light at a random half of each pixel's vertices, so its per-pixel
        // estimates differ by that selection. The delta and disk captures share the selection draws, so their ratio at
        // a pixel the delta light reaches is the disk's visible fraction without it.
        constexpr float32 HARD_EDGE_FRACTION = 1e-3F;
        constexpr float32 SELECTION_NOISE_FRACTION = 0.5F;
        constexpr float32 PENUMBRA_LOWER_FRACTION = 0.1F;
        constexpr float32 PENUMBRA_UPPER_FRACTION = 0.95F;
        constexpr float32 UMBRA_FRACTION = 0.05F;
        uint32 litPenumbraPixels = 0;
        uint32 blockedPenumbraPixels = 0;
        for (usize i = 0; i < delta.GetSize(); ++i)
        {
            CAPTURE(i, delta[i], disk[i]);
            // A zero radius keeps the delta light's hard edge: every receiver is wholly lit or wholly blocked.
            const bool isBlocked = delta[i] <= HARD_EDGE_FRACTION * unblocked;
            CHECK((isBlocked == true || delta[i] >= SELECTION_NOISE_FRACTION * unblocked));
            if (sampling == rr::ReferenceLightSampling::SeparateDirectional && isBlocked == false)
            {
                CHECK(math::Abs(delta[i] - unblocked) <= HARD_EDGE_FRACTION * unblocked);
            }
            if (isBlocked == true)
            {
                blockedPenumbraPixels += disk[i] > UMBRA_FRACTION * unblocked ? 1U : 0U;
                continue;
            }
            const float32 visible = disk[i] / delta[i];
            litPenumbraPixels += visible > PENUMBRA_LOWER_FRACTION && visible < PENUMBRA_UPPER_FRACTION ? 1U : 0U;
        }
        // The disk spreads the edge over its predicted width on both sides of the delta light's edge: x in
        // [-1.61, -0.68] holds four lit and three blocked pixel centres, and the outermost lit one sees almost all of
        // the disk.
        CAPTURE(litPenumbraPixels, blockedPenumbraPixels);
        constexpr uint32 MINIMUM_LIT_PENUMBRA_PIXELS = 3;
        constexpr uint32 MINIMUM_BLOCKED_PENUMBRA_PIXELS = 2;
        CHECK(litPenumbraPixels >= MINIMUM_LIT_PENUMBRA_PIXELS);
        CHECK(blockedPenumbraPixels >= MINIMUM_BLOCKED_PENUMBRA_PIXELS);
        // Where the whole disk is visible it delivers the delta light's illuminance: the projected solid angle of a
        // cap above the horizon is pi sin(r)^2 times its centre cosine. The residual is Monte Carlo error over the
        // disk and the rough GGX lobe's variation across it.
        constexpr float32 FLUX_TOLERANCE = 0.01F;
        CHECK(math::Abs(disk[0] / delta[0] - 1) <= FLUX_TOLERANCE);
        CHECK(math::Abs(disk[1] / delta[1] - 1) <= FLUX_TOLERANCE);
        disks[traversalIndex++] = Move(disk);
    }
    // Matched samples draw matched disk directions. Float32 traversal near the occluder edge may still flip a
    // sample's visibility, each worth 1 / SAMPLE_COUNT of the unblocked estimate.
    constexpr float32 PARITY_TOLERANCE = 2e-3F;
    constexpr float32 FLIPPED_SAMPLES = 2;
    const float32 unblocked = disks[0][0];
    for (usize i = 0; i < disks[0].GetSize(); ++i)
    {
        CAPTURE(i, disks[0][i], disks[1][i]);
        CHECK(math::Abs(disks[0][i] - disks[1][i]) <=
              (PARITY_TOLERANCE + FLIPPED_SAMPLES / static_cast<float32>(SAMPLE_COUNT)) * unblocked);
    }
}

TEST_CASE("GPU-compute reference preserves bounded white sRGB textures and vertex colors",
          "[renderer][gpu][reference-compute][reference-color]")
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
    rhi::DeviceDescription deviceDescription;
    deviceDescription.Required.PortableRaster = true;
    deviceDescription.Required.Compute = true;
    REQUIRE(rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, deviceDescription, device, surface) ==
            rhi::DeviceStatus::Ready);
    Session cpu;
    Session gpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    constexpr usize TEXEL_BYTES = 4;
    const StaticArray<uint8, TEXEL_BYTES> texel{{255, 255, 255, 255}};
    rhi::TextureDescription textureDescription{1, 1, rhi::RasterFormat::Rgba8Srgb};
    textureDescription.RetainCpuContent = true;
    rhi::TextureHandle texture;
    REQUIRE(
        rhi::CreateTexture(device, textureDescription, {{texel.GetData(), texel.GetSize()}, TEXEL_BYTES}, texture) ==
        S::Ready);
    constexpr usize VERTEX_COUNT = 3;
    const StaticArray<math::Vector3, VERTEX_COUNT> positions{
        {{-1.73F, -2.31F, 0}, {2.17F, -1.89F, 0}, {.13F, 2.27F, 0}}};
    const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}}};
    const StaticArray<math::Vector4, VERTEX_COUNT> colors{{{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}}};
    const StaticArray<uint32, VERTEX_COUNT> indices{{0, 2, 1}};
    constexpr usize SESSION_COUNT = 2;
    const StaticArray<Session*, SESSION_COUNT> sessions{{&cpu, &gpu}};
    for (Session* session : sessions)
    {
        rr::MeshDescription meshDescription{positions.GetData(),
                                            positions.GetSize(),
                                            indices.GetData(),
                                            indices.GetSize()};
        meshDescription.Normals = normals.GetData();
        meshDescription.Colors = {colors.GetData(), colors.GetSize()};
        rr::Mesh mesh;
        S status = session->Renderer.CreateMesh(meshDescription, mesh);
        const uint64 meshStart = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - meshStart < TIMEOUT)
        {
            status = session->Renderer.GetStatus(mesh);
        }
        REQUIRE(status == S::Ready);
        rr::LitMaterialDescription materialDescription;
        materialDescription.Surface.Tint = {1, 1, 1, 1};
        materialDescription.Surface.Texture = texture;
        materialDescription.Roughness = 1;
        rr::MaterialVersion material;
        rr::LightSelection selection;
        status = session->Renderer.CreateLitMaterial(session->Program, materialDescription, material, selection);
        const uint64 materialStart = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - materialStart < TIMEOUT)
        {
            status = session->Renderer.GetStatus(material);
        }
        REQUIRE(status == S::Ready);
        rr::SceneItem item;
        item.Geometry = mesh;
        item.Surface = material;
        item.SourceId = 1;
        rr::Snapshot snapshot;
        REQUIRE(session->Renderer.CreateSnapshot(&item, 1, snapshot) == S::Ready);
        REQUIRE(session->Renderer.Release(session->Snapshot) == S::Ready);
        session->Snapshot = snapshot;
        REQUIRE(session->Renderer.Release(mesh) == S::Ready);
        REQUIRE(session->Renderer.Release(material) == S::Ready);
    }
    REQUIRE(rhi::Destroy(device, texture) == S::Ready);
    rr::ReferenceCaptureDescription capture;
    capture.Width = 64;
    capture.Height = 32;
    capture.TargetSamples = 8;
    capture.MaxDepth = 2;
    capture.Seed = 1;
    capture.OrthographicCamera = true;
    capture.WorldToClip.Columns[3].Z = .7F;
    capture.CameraPosition.Z = -.7F;
    capture.Environment = {.3F, .4F, .5F};
    Array<rr::ReferencePixel> oracle;
    Array<rr::ReferencePixel> compute;
    rr::ReferenceCaptureInfo oracleInfo;
    rr::ReferenceCaptureInfo computeInfo;
    Capture(cpu, capture, surface, oracle, oracleInfo);
    Capture(gpu, capture, surface, compute, computeInfo);
    const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
    CAPTURE(comparison.MaximumRadiance, math::Sqrt(comparison.SquaredRadiance));
    CHECK(computeInfo.InvalidContributions == 0);
    CHECK(computeInfo.Failure == rr::ReferenceFailure::None);
    CHECK(computeInfo.Complete == true);
    CHECK(comparison.MaximumRadiance <= 2e-3F);
    CHECK(comparison.MaximumEmission <= 2e-3F);
    CHECK(comparison.MaximumDirect <= 2e-3F);
    CHECK(comparison.MaximumIndirect <= 2e-3F);
}

// --- ResampledProjectedSolidAngle -------------------------------------------------
// RT1's second light-sampling mode, sampler version 2 (reference_light_sampling.h). Both modes estimate the same
// integral, so agreement with UniformArea is statistical: image means over independent seeds. CPU and GPU-compute
// share addressed randomness, but a reservoir decision can flip between FP32 roundings of the same target, so their
// agreement is also judged on means rather than per pixel.
namespace
{
constexpr usize REFERENCE_SEED_COUNT = 6;
using SeedRadiances = StaticArray<math::Vector3, REFERENCE_SEED_COUNT>;
// Seed-mean differences below this many combined standard errors, plus an absolute floor for AOVs that are zero or
// nearly constant across seeds.
constexpr float32 RESAMPLED_STANDARD_ERRORS = 5;
constexpr float32 RESAMPLED_ABSOLUTE_FLOOR = 2e-4F;

/// One AOV's image mean per seed.
struct SeedMeans final
{
    SeedRadiances Emission{};
    SeedRadiances Direct{};
    SeedRadiances Indirect{};
    SeedRadiances Radiance{};
};
struct ImageMeans final
{
    math::Vector3 Emission{};
    math::Vector3 Direct{};
    math::Vector3 Indirect{};
    math::Vector3 Radiance{};
};
ImageMeans MeasureImage(const Array<rr::ReferencePixel>& pixels)
{
    ImageMeans result;
    for (const rr::ReferencePixel& pixel : pixels)
    {
        result.Emission = result.Emission + pixel.Emission;
        result.Direct = result.Direct + pixel.Direct;
        result.Indirect = result.Indirect + pixel.Indirect;
        result.Radiance = result.Radiance + pixel.Radiance;
    }
    const float32 scale = 1 / float32(pixels.GetSize());
    result.Emission = result.Emission * scale;
    result.Direct = result.Direct * scale;
    result.Indirect = result.Indirect * scale;
    result.Radiance = result.Radiance * scale;
    return result;
}
/// The largest per-channel z-score |mean_a - mean_b| / (combined standard error + floor) of two seed sets; symmetric
/// in its arguments.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
float32 MeasureSeparation(const SeedRadiances& a, const SeedRadiances& b)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    float32 largest = 0;
    for (usize channel = 0; channel < 3; ++channel)
    {
        const auto component = [channel](math::Vector3 value) noexcept {
            return channel == 0 ? value.X : channel == 1 ? value.Y : value.Z;
        };
        float32 meanA = 0;
        float32 meanB = 0;
        for (usize seed = 0; seed < REFERENCE_SEED_COUNT; ++seed)
        {
            meanA += component(a[seed]) / REFERENCE_SEED_COUNT;
            meanB += component(b[seed]) / REFERENCE_SEED_COUNT;
        }
        float32 varianceA = 0;
        float32 varianceB = 0;
        for (usize seed = 0; seed < REFERENCE_SEED_COUNT; ++seed)
        {
            varianceA += (component(a[seed]) - meanA) * (component(a[seed]) - meanA) / (REFERENCE_SEED_COUNT - 1);
            varianceB += (component(b[seed]) - meanB) * (component(b[seed]) - meanB) / (REFERENCE_SEED_COUNT - 1);
        }
        const float32 error = math::Sqrt((varianceA + varianceB) / REFERENCE_SEED_COUNT);
        largest = math::Max(largest, math::Abs(meanA - meanB) / (error + RESAMPLED_ABSOLUTE_FLOOR));
    }
    return largest;
}
void RecordSeed(const Array<rr::ReferencePixel>& pixels, usize seed, SeedMeans& inoutMeans)
{
    const ImageMeans means = MeasureImage(pixels);
    inoutMeans.Emission[seed] = means.Emission;
    inoutMeans.Direct[seed] = means.Direct;
    inoutMeans.Indirect[seed] = means.Indirect;
    inoutMeans.Radiance[seed] = means.Radiance;
}
rr::ReferenceCaptureDescription MakePerspectiveCapture(math::Vector3 cameraPosition, uint32 width, uint32 height)
{
    rr::ReferenceCaptureDescription capture;
    capture.Width = width;
    capture.Height = height;
    math::Matrix4 view;
    math::Matrix4 projection;
    capture.CameraPosition = cameraPosition;
    REQUIRE(math::TryLookAt(capture.CameraPosition, {0, 0, 0}, {0, 1, 0}, view) == math::MathStatus::Success);
    REQUIRE(math::TryPerspectiveReverseZ(1, float32(width) / float32(height), .1F, 20, projection) ==
            math::MathStatus::Success);
    capture.WorldToClip = projection * view;
    capture.ReverseZ = true;
    return capture;
}
bool StartComputeDevice(rhi::DeviceHandle& outDevice, rhi::SurfaceHandle& outSurface)
{
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Required.Compute = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, outDevice, outSurface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        return false;
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    return true;
}

constexpr usize BOX_FACE_COUNT = 6;
constexpr usize QUAD_CORNER_COUNT = 4;
using QuadCorners = StaticArray<math::Vector3, QUAD_CORNER_COUNT>;

/// One quad of a test room: its corners, material and emission sidedness.
struct QuadItem final
{
    QuadCorners CornersWorld;
    math::Vector3 Emission{};
    bool IsEmissionTwoSided = true;
    /// Linear base color.
    math::Vector3 BaseColor{.7F, .6F, .5F};
    /// Typed Diffuse with a black BaseColor reflects nothing, as pbrt scene lights author their surfaces.
    ludus::content::scene_format::MaterialModel Model = ludus::content::scene_format::MaterialModel::MetallicRoughness;
    float32 Metallic = 0;
    float32 Roughness = 1;
    /// Real index of a smooth Dielectric or ThinDielectric model.
    float32 Eta = ludus::content::scene_format::DEFAULT_DIELECTRIC_ETA;
    /// Conductor model only: RGB complex index and isotropic GGX alpha.
    StaticArray<float32, 3> ConductorEta{{1, 1, 1}};
    StaticArray<float32, 3> ConductorK{};
    float32 ConductorAlpha = 1;
    /// Optional base texture multiplying BaseColor, sampled nearest and clamped. Corners c0..c3 map to UVs (0, 1),
    /// (0, 0), (1, 0) and (1, 1), so u runs along c1 to c2.
    rhi::TextureHandle Texture{};
    /// Closure program of a Program model, borrowed until BuildQuadSnapshot returns.
    const ludus::content::scene_format::MaterialProgram* ProgramOrNull = nullptr;
};
/// A snapshot of planar quads, wound so that each quad's geometric normal is cross(c1 - c0, c3 - c0).
S BuildQuadSnapshot(rr::Renderer& inoutRenderer,
                    rr::MaterialProgram program,
                    const QuadItem* quads,
                    usize count,
                    rr::Snapshot& outSnapshot)
{
    REQUIRE(count <= BOX_FACE_COUNT);
    StaticArray<rr::Mesh, BOX_FACE_COUNT> meshes{};
    StaticArray<rr::MaterialVersion, BOX_FACE_COUNT> materials{};
    StaticArray<rr::SceneItem, BOX_FACE_COUNT> items{};
    for (usize i = 0; i < count; ++i)
    {
        const QuadItem& quad = quads[i];
        const math::Vector3 normal =
            math::Cross(quad.CornersWorld[1] - quad.CornersWorld[0], quad.CornersWorld[3] - quad.CornersWorld[0]);
        const math::Vector3 unitNormal = normal * (1 / math::Length(normal));
        const math::Vector3 normals[]{unitNormal, unitNormal, unitNormal, unitNormal};
        // Counterclockwise about the normal: (0, 1, 2) and (0, 2, 3).
        const uint32 indices[]{0, 1, 2, 0, 2, 3};
        rr::MeshDescription mesh{quad.CornersWorld.GetData(), 4, indices, 6};
        mesh.Normals = normals;
        const math::Vector2 texcoords[]{{0, 1}, {0, 0}, {1, 0}, {1, 1}};
        const bool isTextured = quad.Texture != rhi::TextureHandle{};
        if (isTextured == true)
        {
            mesh.Texcoords = texcoords;
        }
        S status = inoutRenderer.CreateMesh(mesh, meshes[i]);
        uint64 start = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - start < TIMEOUT)
        {
            status = inoutRenderer.GetStatus(meshes[i]);
        }
        REQUIRE(status == S::Ready);
        rr::LitMaterialDescription material;
        material.Surface.Tint = {quad.BaseColor.X, quad.BaseColor.Y, quad.BaseColor.Z, 1};
        material.Metallic = quad.Metallic;
        material.Roughness = quad.Roughness;
        material.Scattering.Model = quad.Model;
        if (quad.Model == ludus::content::scene_format::MaterialModel::Dielectric ||
            quad.Model == ludus::content::scene_format::MaterialModel::ThinDielectric)
        {
            material.Scattering.Eta = {{quad.Eta, quad.Eta, quad.Eta}};
            material.Scattering.AlphaU = ludus::content::scene_format::MIN_MATERIAL_ALPHA;
            material.Scattering.AlphaV = ludus::content::scene_format::MIN_MATERIAL_ALPHA;
        }
        if (quad.Model == ludus::content::scene_format::MaterialModel::Conductor)
        {
            material.Scattering.Eta = quad.ConductorEta;
            material.Scattering.K = quad.ConductorK;
            material.Scattering.AlphaU = quad.ConductorAlpha;
            material.Scattering.AlphaV = quad.ConductorAlpha;
        }
        material.Emission = quad.Emission;
        material.IsEmissionTwoSided = quad.IsEmissionTwoSided;
        if (isTextured == true)
        {
            material.Surface.Texture = quad.Texture;
            material.Surface.Sampler.Filter = rhi::RasterFilter::Nearest;
            material.Surface.Sampler.U = rhi::RasterAddress::Clamp;
            material.Surface.Sampler.V = rhi::RasterAddress::Clamp;
        }
        if (quad.ProgramOrNull != nullptr)
        {
            material.Program = *quad.ProgramOrNull;
        }
        rr::LightSelection selection;
        status = inoutRenderer.CreateLitMaterial(program, material, materials[i], selection);
        start = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - start < TIMEOUT)
        {
            status = inoutRenderer.GetStatus(materials[i]);
        }
        REQUIRE(status == S::Ready);
        items[i].Geometry = meshes[i];
        items[i].Surface = materials[i];
        items[i].SourceId = 100 + i;
    }
    const S created = inoutRenderer.CreateSnapshot(items.GetData(), count, outSnapshot);
    for (usize i = 0; i < count; ++i)
    {
        static_cast<void>(inoutRenderer.Release(materials[i]));
        static_cast<void>(inoutRenderer.Release(meshes[i]));
    }
    return created;
}
constexpr usize ROOM_QUAD_COUNT = 4;

/// A glossy floor, a diffuse back wall and two emitters. The bright one-sided ceiling panel faces away from the
/// room, so its power dominates the CDF while every receiver sees it from behind: most vertices then
/// estimate Z-hat = 0, and with `isDimEmitterPresent` false no emitter lights any receiver at all.
usize MakeRoomQuads(bool isDimEmitterPresent, StaticArray<QuadItem, ROOM_QUAD_COUNT>& outQuads)
{
    QuadItem& floor = outQuads[0];
    floor.CornersWorld = {{{-1, -1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, -1, -1}}};
    floor.Metallic = 1;
    floor.Roughness = .25F;
    QuadItem& wall = outQuads[1];
    // Below the panel, so no receiver sees the panel's front.
    wall.CornersWorld = {{{-1, -1, 1}, {-1, .8F, 1}, {1, .8F, 1}, {1, -1, 1}}};
    QuadItem& bright = outQuads[2];
    // Facing +y, away from the room below it.
    bright.CornersWorld = {{{-.6F, .9F, -.6F}, {-.6F, .9F, .6F}, {.6F, .9F, .6F}, {.6F, .9F, -.6F}}};
    bright.Emission = {400, 400, 400};
    bright.IsEmissionTwoSided = false;
    // Reflection is two-sided, so a reflective panel would scatter light reaching its underside, and a receiver on its
    // own plane would weigh it by a side that only rounding decides. Like pbrt scene lights, the panels reflect
    // nothing.
    bright.BaseColor = {};
    bright.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    if (isDimEmitterPresent == false)
    {
        return 3;
    }
    QuadItem& dim = outQuads[3];
    dim.CornersWorld = {{{-.9F, .2F, .1F}, {-.9F, .6F, .1F}, {-.9F, .6F, .5F}, {-.9F, .2F, .5F}}};
    dim.Emission = {3, 2.5F, 2};
    dim.BaseColor = {};
    dim.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    return ROOM_QUAD_COUNT;
}

enum class ResampledLightScene : uint8
{
    L6,
    Sphere,
    Room,
};
constexpr uint32 L6_MULTIPLE_BOUNCE_DEPTH = 4;
constexpr uint32 SPHERE_SHALLOW_DEPTH = 3;
constexpr uint32 SPHERE_DEEP_DEPTH = 6;
constexpr uint32 ROOM_PATH_DEPTH = 4;

// Discover each scene/depth separately so its independent seed sweep has its own CTest deadline.
void CheckResampledLightSampling(ResampledLightScene scene, uint32 maximumDepth)
{
    constexpr uint32 CAPTURE_WIDTH_PIXELS = 16;
    constexpr uint32 CAPTURE_HEIGHT_PIXELS = 12;
    constexpr uint32 TARGET_SAMPLE_COUNT = 64;
    constexpr uint64 FIRST_SEED = 40;
    constexpr uint32 EXPECTED_FORM_FACTOR_SAMPLER_VERSION = 3;
    constexpr math::Vector3 SPHERE_CAMERA_POSITION_WORLD{.3F, .2F, -2};
    constexpr math::Vector3 ROOM_CAMERA_POSITION_WORLD{0, .1F, -2.4F};
    constexpr math::Vector3 L6_CAMERA_POSITION_WORLD{.1F, -.2F, -2.5F};
    constexpr math::Vector3 DEFAULT_ENVIRONMENT_RADIANCE{.05F, .06F, .08F};
    CAPTURE(scene, maximumDepth);
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);
    rr::ReferenceCaptureDescription capture =
        scene == ResampledLightScene::Sphere
            ? MakePerspectiveCapture(SPHERE_CAMERA_POSITION_WORLD, CAPTURE_WIDTH_PIXELS, CAPTURE_HEIGHT_PIXELS)
        : scene == ResampledLightScene::Room
            ? MakePerspectiveCapture(ROOM_CAMERA_POSITION_WORLD, CAPTURE_WIDTH_PIXELS, CAPTURE_HEIGHT_PIXELS)
            : MakePerspectiveCapture(L6_CAMERA_POSITION_WORLD, CAPTURE_WIDTH_PIXELS, CAPTURE_HEIGHT_PIXELS);
    capture.TargetSamples = TARGET_SAMPLE_COUNT;
    capture.MaxDepth = maximumDepth;
    capture.RouletteDepth = 2;
    capture.Environment = scene == ResampledLightScene::Room ? math::Vector3{} : DEFAULT_ENVIRONMENT_RADIANCE;
    if (scene != ResampledLightScene::L6)
    {
        StaticArray<QuadItem, ROOM_QUAD_COUNT> quads{};
        const usize quadCount = MakeRoomQuads(true, quads);
        REQUIRE(cpu.Renderer.Release(cpu.Snapshot) == S::Ready);
        const S built = scene == ResampledLightScene::Sphere
                            ? BuildSphereSnapshot(device, cpu.Renderer, cpu.Program, cpu.Snapshot)
                            : BuildQuadSnapshot(cpu.Renderer, cpu.Program, quads.GetData(), quadCount, cpu.Snapshot);
        REQUIRE(built == S::Ready);
    }
    const rr::Snapshot snapshot = cpu.Snapshot;
    // UniformArea, then both resampled modes, which differ only in their RIS target.
    constexpr usize MODE_COUNT = 3;
    constexpr StaticArray<rr::ReferenceLightSampling, MODE_COUNT> MODES{
        {rr::ReferenceLightSampling::UniformArea,
         rr::ReferenceLightSampling::ResampledProjectedSolidAngle,
         rr::ReferenceLightSampling::ResampledFormFactor}};
    constexpr StaticArray<uint32, MODE_COUNT> SAMPLER_VERSIONS{{1, 2, EXPECTED_FORM_FACTOR_SAMPLER_VERSION}};
    StaticArray<SeedMeans, MODE_COUNT> means{};
    for (usize seed = 0; seed < REFERENCE_SEED_COUNT; ++seed)
    {
        capture.Seed = FIRST_SEED + seed;
        for (usize modeIndex = 0; modeIndex < MODE_COUNT; ++modeIndex)
        {
            const rr::ReferenceLightSampling mode = MODES[modeIndex];
            capture.LightSampling = mode;
            Array<rr::ReferencePixel> pixels;
            rr::ReferenceCaptureInfo info;
            rr::ReferenceCapture handle;
            const S begun = cpu.Renderer.BeginReferenceCapture(snapshot, capture, handle);
            REQUIRE((begun == S::Ready || begun == S::Pending));
            REQUIRE(Run(cpu.Renderer, handle, surface) == S::Ready);
            REQUIRE(cpu.Renderer.GetReferenceCaptureInfo(handle, info) == S::Ready);
            CHECK(info.InvalidContributions == 0);
            CHECK(info.LightSampling == mode);
            CHECK(info.SamplerVersion == SAMPLER_VERSIONS[modeIndex]);
            REQUIRE(pixels.TryResize(usize(capture.Width) * capture.Height) == true);
            REQUIRE(cpu.Renderer.CopyReferencePixels(handle, pixels.GetData(), pixels.GetSize()) == S::Ready);
            REQUIRE(cpu.Renderer.Release(handle) == S::Ready);
            RecordSeed(pixels, seed, means[modeIndex]);
        }
        // Primary rays and primary emission use the unchanged dimensions 0 and 1.
        CHECK(means[0].Emission[seed] == means[1].Emission[seed]);
        CHECK(means[0].Emission[seed] == means[2].Emission[seed]);
    }
    for (usize modeIndex = 1; modeIndex < MODE_COUNT; ++modeIndex)
    {
        const SeedMeans& uniform = means[0];
        const SeedMeans& resampled = means[modeIndex];
        const float32 direct = MeasureSeparation(uniform.Direct, resampled.Direct);
        const float32 indirect = MeasureSeparation(uniform.Indirect, resampled.Indirect);
        const float32 radiance = MeasureSeparation(uniform.Radiance, resampled.Radiance);
        CAPTURE(modeIndex, direct, indirect, radiance, uniform.Radiance[0], resampled.Radiance[0]);
        CHECK(direct <= RESAMPLED_STANDARD_ERRORS);
        CHECK(indirect <= RESAMPLED_STANDARD_ERRORS);
        CHECK(radiance <= RESAMPLED_STANDARD_ERRORS);
    }
}
} // namespace

TEST_CASE("Resampled light sampling agrees with UniformArea: L6 single bounce",
          "[renderer][gpu][reference-compute][resampled-light]")
{
    CheckResampledLightSampling(ResampledLightScene::L6, 1);
}

TEST_CASE("Resampled light sampling agrees with UniformArea: L6 depth 4",
          "[renderer][gpu][reference-compute][resampled-light]")
{
    CheckResampledLightSampling(ResampledLightScene::L6, L6_MULTIPLE_BOUNCE_DEPTH);
}

TEST_CASE("Resampled light sampling agrees with UniformArea: glossy sphere single bounce",
          "[renderer][gpu][reference-compute][resampled-light]")
{
    CheckResampledLightSampling(ResampledLightScene::Sphere, 1);
}

TEST_CASE("Resampled light sampling agrees with UniformArea: glossy sphere depth 3",
          "[renderer][gpu][reference-compute][resampled-light]")
{
    CheckResampledLightSampling(ResampledLightScene::Sphere, SPHERE_SHALLOW_DEPTH);
}

TEST_CASE("Resampled light sampling agrees with UniformArea: glossy sphere depth 6",
          "[renderer][gpu][reference-compute][resampled-light]")
{
    CheckResampledLightSampling(ResampledLightScene::Sphere, SPHERE_DEEP_DEPTH);
}

TEST_CASE("Resampled light sampling agrees with UniformArea: room with mostly zero Z-hat at depth 4",
          "[renderer][gpu][reference-compute][resampled-light]")
{
    CheckResampledLightSampling(ResampledLightScene::Room, ROOM_PATH_DEPTH);
}

TEST_CASE("GPU-compute resampled reference matches the CPU oracle",
          "[renderer][gpu][reference-compute][resampled-light]")
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
    Session gpu;
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);
    rr::DirectLight sun;
    sun.SourceId = 7;
    sun.Direction = math::Vector3{-.3F, -.4F, 1} / math::Length(math::Vector3{-.3F, -.4F, 1});
    sun.Intensity = .4F;
    enum class Scene : uint8
    {
        L6,
        Sphere,
        Room,
    };
    struct Case final
    {
        const char* Name = "";
        Scene Fixture = Scene::L6;
        uint32 MaxDepth = 4;
        uint64 Seed = 1;
    };
    const Case cases[]{
        {"L6 depth 4 with a sun", Scene::L6, 4, 871},
        {"glossy sphere depth 6", Scene::Sphere, 6, 99},
        {"room where most vertices estimate Z-hat = 0", Scene::Room, 4, 5},
    };
    for (const Case& test : cases)
    {
        SECTION(test.Name)
        {
            rr::ReferenceCaptureDescription capture =
                test.Fixture == Scene::Sphere ? MakePerspectiveCapture({.3F, .2F, -2}, 16, 12)
                : test.Fixture == Scene::Room ? MakePerspectiveCapture({0, .1F, -2.4F}, 16, 12)
                                              : MakePerspectiveCapture({.1F, -.2F, -2.5F}, 16, 12);
            capture.TargetSamples = 64;
            capture.MaxDepth = test.MaxDepth;
            capture.RouletteDepth = 2;
            capture.Seed = test.Seed;
            capture.Environment = test.Fixture == Scene::Room ? math::Vector3{} : math::Vector3{.2F, .25F, .3F};
            capture.Lights = &sun;
            capture.LightCount = test.Fixture == Scene::L6 ? 1 : 0;
            // Each session holds one fixture at a time within the renderer's material capacity.
            if (test.Fixture != Scene::L6)
            {
                StaticArray<QuadItem, ROOM_QUAD_COUNT> quads{};
                const usize quadCount = MakeRoomQuads(true, quads);
                for (Session* session : {&cpu, &gpu})
                {
                    REQUIRE(session->Renderer.Release(session->Snapshot) == S::Ready);
                    const S built =
                        test.Fixture == Scene::Sphere
                            ? BuildSphereSnapshot(device, session->Renderer, session->Program, session->Snapshot)
                            : BuildQuadSnapshot(session->Renderer,
                                                session->Program,
                                                quads.GetData(),
                                                quadCount,
                                                session->Snapshot);
                    REQUIRE(built == S::Ready);
                }
            }
            for (const rr::ReferenceLightSampling mode : {rr::ReferenceLightSampling::ResampledProjectedSolidAngle,
                                                          rr::ReferenceLightSampling::ResampledFormFactor})
            {
                capture.LightSampling = mode;
                CAPTURE(static_cast<uint32>(mode));
                Array<rr::ReferencePixel> oracle;
                Array<rr::ReferencePixel> compute;
                rr::ReferenceCaptureInfo oracleInfo;
                rr::ReferenceCaptureInfo computeInfo;
                Capture(cpu, capture, surface, oracle, oracleInfo);
                Capture(gpu, capture, surface, compute, computeInfo);
                const uint32 samplerVersion =
                    capture.LightSampling == rr::ReferenceLightSampling::ResampledFormFactor ? 3 : 2;
                CHECK(oracleInfo.SamplerVersion == samplerVersion);
                CHECK(computeInfo.SamplerVersion == samplerVersion);
                CHECK(computeInfo.LightSampling == capture.LightSampling);
                CHECK(oracleInfo.InvalidContributions == 0);
                CHECK(computeInfo.InvalidContributions == 0);
                CHECK(computeInfo.Complete == true);
                CHECK(computeInfo.CommittedSamples == oracleInfo.CommittedSamples);
                const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
                const float32 radianceMean =
                    math::Distance(MeasureImage(oracle).Radiance, MeasureImage(compute).Radiance);
                CAPTURE(comparison.MaximumRadiance,
                        comparison.MaximumEmission,
                        comparison.MaximumDirect,
                        comparison.MaximumIndirect,
                        math::Sqrt(comparison.SquaredRadiance),
                        radianceMean,
                        oracleInfo.ClosestQueries,
                        computeInfo.ClosestQueries,
                        oracleInfo.VisibilityQueries,
                        computeInfo.VisibilityQueries);
                // The same addressed candidates and tickets select the same emitters in both precisions for almost
                // every sample, so the UniformArea parity tolerance holds per pixel. A sample whose contribution
                // rounds differently can still select another emitter (the visibility count below admits these);
                // it moves its pixel by about one sample's share, the pixel's radiance / TargetSamples. The glossy
                // sphere's worst such pixel differed by 2.0e-3 at radiance 0.26 on an Apple M5 and by 2.01e-3 on the
                // GitHub macOS runner.
                constexpr float32 PIXEL_TOLERANCE = 2e-3F;
                usize radianceOutlierCount = 0;
                usize directOutlierCount = 0;
                for (usize i = 0; i < oracle.GetSize(); ++i)
                {
                    const float32 sampleShare =
                        math::Length(oracle[i].Radiance) / static_cast<float32>(capture.TargetSamples);
                    radianceOutlierCount +=
                        math::Distance(oracle[i].Radiance, compute[i].Radiance) <= PIXEL_TOLERANCE + sampleShare ? 0U
                                                                                                                 : 1U;
                    directOutlierCount +=
                        math::Distance(oracle[i].Direct, compute[i].Direct) <= PIXEL_TOLERANCE + sampleShare ? 0U : 1U;
                }
                CHECK(radianceOutlierCount == 0);
                CHECK(directOutlierCount == 0);
                CHECK(comparison.MaximumEmission <= PIXEL_TOLERANCE);
                CHECK(comparison.MaximumIndirect <= PIXEL_TOLERANCE);
                CHECK(math::Sqrt(comparison.SquaredRadiance) <= 2e-4F);
                CHECK(radianceMean <= 1e-4F);
                CHECK(computeInfo.ClosestQueries == oracleInfo.ClosestQueries);
                CHECK(computeInfo.NullBsdfSamples == oracleInfo.NullBsdfSamples);
                CHECK(computeInfo.RouletteTerminations == oracleInfo.RouletteTerminations);
                // A light sample whose contribution rounds to zero in one precision skips its shadow ray; with two
                // techniques per vertex a few such samples remain where emitters are rarely selected.
                constexpr uint64 VISIBILITY_COUNT_DIVISOR = 100;
                const uint64 visibilityDifference = computeInfo.VisibilityQueries > oracleInfo.VisibilityQueries
                                                        ? computeInfo.VisibilityQueries - oracleInfo.VisibilityQueries
                                                        : oracleInfo.VisibilityQueries - computeInfo.VisibilityQueries;
                CHECK(visibilityDifference <= oracleInfo.VisibilityQueries / VISIBILITY_COUNT_DIVISOR + 3);
            }
        }
    }
}

TEST_CASE("Reference transport orients geometric normals by authored normals as pbrt-v4 does",
          "[renderer][gpu][reference-compute]")
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);
    rr::ReferenceCaptureDescription capture = MakePerspectiveCapture({.3F, .2F, -2}, 32, 24);
    capture.TargetSamples = 16;
    capture.MaxDepth = 6;
    capture.Seed = 99;
    capture.Environment = {.2F, .25F, .3F};
    // Reversing the winding against authored normals leaves the side that reflects and emits unchanged.
    StaticArray<Array<rr::ReferencePixel>, 2> images;
    const NormalLayout layouts[]{NormalLayout::Matched, NormalLayout::ReversedWinding};
    for (usize i = 0; i < images.GetSize(); ++i)
    {
        CAPTURE(i);
        REQUIRE(cpu.Renderer.Release(cpu.Snapshot) == S::Ready);
        REQUIRE(BuildSphereSnapshot(device,
                                    cpu.Renderer,
                                    cpu.Program,
                                    cpu.Snapshot,
                                    false,
                                    SphereSurfaces::Opaque,
                                    layouts[i]) == S::Ready);
        rr::ReferenceCaptureInfo information;
        Capture(cpu, capture, surface, images[i], information);
        CHECK(information.InvalidContributions == 0);
    }
    const Comparison comparison = Compare(images[0], images[1], capture.TargetSamples);
    const float32 radianceMean = math::Distance(MeasureImage(images[0]).Radiance, MeasureImage(images[1]).Radiance);
    CAPTURE(comparison.MaximumRadiance, math::Sqrt(comparison.SquaredRadiance), radianceMean);
    // Swapped corners only reorder barycentric arithmetic; a rare near-edge hit can send one path elsewhere.
    CHECK(math::Sqrt(comparison.SquaredRadiance) <= 1e-3F);
    CHECK(radianceMean <= 1e-4F);
    CHECK(MeasureImage(images[1]).Radiance.X > 0);
}

TEST_CASE("GPU-compute reference matches the CPU oracle where shading normals straddle their faces",
          "[renderer][gpu][reference-compute][resampled-light]")
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
    Session gpu;
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);
    for (Session* session : {&cpu, &gpu})
    {
        REQUIRE(session->Renderer.Release(session->Snapshot) == S::Ready);
        REQUIRE(BuildSphereSnapshot(device,
                                    session->Renderer,
                                    session->Program,
                                    session->Snapshot,
                                    false,
                                    SphereSurfaces::Opaque,
                                    NormalLayout::Mixed) == S::Ready);
    }
    rr::ReferenceCaptureDescription capture = MakePerspectiveCapture({.3F, .2F, -2}, 32, 24);
    capture.TargetSamples = 16;
    capture.MaxDepth = 6;
    capture.Seed = 99;
    capture.Environment = {.2F, .25F, .3F};
    for (const rr::ReferenceLightSampling mode :
         {rr::ReferenceLightSampling::UniformArea, rr::ReferenceLightSampling::ResampledProjectedSolidAngle})
    {
        capture.LightSampling = mode;
        CAPTURE(static_cast<uint32>(mode));
        Array<rr::ReferencePixel> oracle;
        Array<rr::ReferencePixel> compute;
        rr::ReferenceCaptureInfo oracleInfo;
        rr::ReferenceCaptureInfo computeInfo;
        Capture(cpu, capture, surface, oracle, oracleInfo);
        Capture(gpu, capture, surface, compute, computeInfo);
        const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
        const float32 radianceMean = math::Distance(MeasureImage(oracle).Radiance, MeasureImage(compute).Radiance);
        CAPTURE(comparison.MaximumRadiance, math::Sqrt(comparison.SquaredRadiance), radianceMean);
        CHECK(oracleInfo.InvalidContributions == 0);
        CHECK(computeInfo.InvalidContributions == 0);
        CHECK(computeInfo.CommittedSamples == oracleInfo.CommittedSamples);
        CHECK(MeasureImage(oracle).Emission.X > 0);
        // A rare near-edge hit or a near-tangent orientation can send one path elsewhere; the image stays matched.
        CHECK(math::Sqrt(comparison.SquaredRadiance) <= 1e-3F);
        CHECK(radianceMean <= 1e-4F);
    }
}

TEST_CASE("Resampled light sampling ignores emitters behind every receiver in both implementations",
          "[renderer][gpu][reference-compute][resampled-light]")
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
    Session gpu;
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);
    StaticArray<QuadItem, ROOM_QUAD_COUNT> quads{};
    const usize quadCount = MakeRoomQuads(false, quads);
    for (Session* session : {&cpu, &gpu})
    {
        REQUIRE(session->Renderer.Release(session->Snapshot) == S::Ready);
        REQUIRE(BuildQuadSnapshot(session->Renderer, session->Program, quads.GetData(), quadCount, session->Snapshot) ==
                S::Ready);
    }
    // The only emitter faces away from the room and there is no environment or analytic light, so both estimators
    // see zero emission; the resampled one never selects an emitter and traces no shadow ray.
    rr::ReferenceCaptureDescription capture = MakePerspectiveCapture({0, .1F, -2.4F}, 12, 9);
    capture.TargetSamples = 16;
    capture.MaxDepth = 4;
    capture.Seed = 17;
    for (const rr::ReferenceLightSampling mode : {rr::ReferenceLightSampling::UniformArea,
                                                  rr::ReferenceLightSampling::ResampledProjectedSolidAngle,
                                                  rr::ReferenceLightSampling::ResampledFormFactor})
    {
        capture.LightSampling = mode;
        for (Session* session : {&cpu, &gpu})
        {
            Array<rr::ReferencePixel> pixels;
            rr::ReferenceCaptureInfo info;
            Capture(*session, capture, surface, pixels, info);
            CAPTURE(static_cast<uint32>(mode), static_cast<uint32>(info.Traversal));
            CHECK(info.InvalidContributions == 0);
            CHECK(info.VisibilityQueries == 0);
            for (const rr::ReferencePixel& pixel : pixels)
            {
                CHECK(pixel.Radiance == math::Vector3{});
            }
        }
    }
}

TEST_CASE("GPU-compute light-sampling mode is part of the integral and the raw capture",
          "[renderer][gpu][reference-compute][resampled-light]")
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
    Session gpu;
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    rr::ReferenceCaptureDescription capture = MakePerspectiveCapture({.1F, -.2F, -2.5F}, 9, 7);
    capture.TargetSamples = 4;
    capture.Seed = 3;
    capture.Environment = {.3F, .4F, .5F};
    rr::ReferenceCaptureDescription invalid = capture;
    constexpr uint8 INVALID_LIGHT_SAMPLING = 255;
    invalid.LightSampling = static_cast<rr::ReferenceLightSampling>(INVALID_LIGHT_SAMPLING);
    rr::ReferenceCapture rejected;
    CHECK(gpu.Renderer.BeginReferenceCapture(gpu.Snapshot, invalid, rejected) == S::InvalidDescription);
    rr::ReferenceCapture handle;
    const S begun = gpu.Renderer.BeginReferenceCapture(gpu.Snapshot, capture, handle);
    REQUIRE((begun == S::Ready || begun == S::Pending));
    REQUIRE(Run(gpu.Renderer, handle, surface) == S::Ready);
    rr::ReferenceCaptureInfo info;
    REQUIRE(gpu.Renderer.GetReferenceCaptureInfo(handle, info) == S::Ready);
    CHECK(info.Revision == 1);
    CHECK(info.SamplerVersion == 1);
    CHECK(info.LightSampling == rr::ReferenceLightSampling::UniformArea);
    // Changing only the light-sampling mode replaces the integral and restarts accumulation.
    rr::ReferenceCaptureDescription resampled = capture;
    resampled.LightSampling = rr::ReferenceLightSampling::ResampledProjectedSolidAngle;
    const S replaced = gpu.Renderer.UpdateReferenceCapture(handle, gpu.Snapshot, resampled);
    REQUIRE((replaced == S::Ready || replaced == S::Pending));
    REQUIRE(Run(gpu.Renderer, handle, surface) == S::Ready);
    REQUIRE(gpu.Renderer.GetReferenceCaptureInfo(handle, info) == S::Ready);
    CHECK(info.Revision == 2);
    CHECK(info.SamplerVersion == 2);
    CHECK(info.LightSampling == rr::ReferenceLightSampling::ResampledProjectedSolidAngle);
    CHECK(info.CommittedSamples == uint64{4} * capture.Width * capture.Height);
    constexpr usize HEADER_BYTES = 256;
    constexpr usize PIXEL_BYTES = 16;
    constexpr usize SAMPLER_OFFSET = 16;
    constexpr usize FLAGS_OFFSET = 44;
    constexpr uint32 GPU_COMPUTE_FLAG = 64;
    constexpr uint32 RESAMPLED_LIGHT_FLAG = 128;
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(HEADER_BYTES + usize(capture.Width) * capture.Height * PIXEL_BYTES) == true);
    usize written = 0;
    REQUIRE(gpu.Renderer.EncodeReferenceHdr(handle, bytes.GetData(), bytes.GetSize(), written) == S::Ready);
    CHECK(bytes[SAMPLER_OFFSET] == 2);
    const uint32 flags = uint32(bytes[FLAGS_OFFSET]) | (uint32(bytes[FLAGS_OFFSET + 1]) << 8);
    CHECK((flags & GPU_COMPUTE_FLAG) != 0);
    CHECK((flags & RESAMPLED_LIGHT_FLAG) != 0);
    // The approximate target is another sampler version and another integral key.
    resampled.LightSampling = rr::ReferenceLightSampling::ResampledFormFactor;
    const S approximated = gpu.Renderer.UpdateReferenceCapture(handle, gpu.Snapshot, resampled);
    REQUIRE((approximated == S::Ready || approximated == S::Pending));
    REQUIRE(Run(gpu.Renderer, handle, surface) == S::Ready);
    REQUIRE(gpu.Renderer.GetReferenceCaptureInfo(handle, info) == S::Ready);
    CHECK(info.Revision == 3);
    CHECK(info.SamplerVersion == 3);
    CHECK(info.LightSampling == rr::ReferenceLightSampling::ResampledFormFactor);
    CHECK(info.CommittedSamples == uint64{4} * capture.Width * capture.Height);
    REQUIRE(gpu.Renderer.EncodeReferenceHdr(handle, bytes.GetData(), bytes.GetSize(), written) == S::Ready);
    CHECK(bytes[SAMPLER_OFFSET] == 3);
    const uint32 approximatedFlags = uint32(bytes[FLAGS_OFFSET]) | (uint32(bytes[FLAGS_OFFSET + 1]) << 8);
    CHECK((approximatedFlags & RESAMPLED_LIGHT_FLAG) != 0);
    REQUIRE(gpu.Renderer.Release(handle) == S::Ready);
}

TEST_CASE("Typed reflection and transmission match between CPU and GPU reference transport",
          "[renderer][gpu][reference-compute]")
{
    const SphereSurfaces surfaces = GENERATE(SphereSurfaces::Opaque,
                                             SphereSurfaces::Mix,
                                             SphereSurfaces::ProjectBrdf,
                                             SphereSurfaces::Coated,
                                             SphereSurfaces::CoatedBlack,
                                             SphereSurfaces::Glass,
                                             SphereSurfaces::Program);
    CAPTURE(surfaces);
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
    REQUIRE(BuildSphereSnapshot(device, cpu.Renderer, cpu.Program, spheres[0], true, surfaces) == S::Ready);
    REQUIRE(BuildSphereSnapshot(device, gpu.Renderer, gpu.Program, spheres[1], true, surfaces) == S::Ready);
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

TEST_CASE("Optical connections relayed through glass match between CPU and GPU reference transport",
          "[renderer][gpu][reference-compute][wave-relay]")
{
    // The glass spheres (a solid dielectric and a thin one) relay the emissive sphere's connection: camera paths and
    // rough vertices reach it through reflections and refractions, and every operator evaluates the relayed chain.
    const rr::ReferenceOpticalConnection connection =
        GENERATE(rr::ReferenceOpticalConnection::AngularSpectrum,
                 rr::ReferenceOpticalConnection::UtdEdgeDiffraction,
                 rr::ReferenceOpticalConnection::FraunhoferEdgeDiffraction);
    // A spectral capture relays each path at its own wavelength, through glass whose index is evaluated there.
    const rr::ReferenceOpticalSpectrum spectrum =
        GENERATE(rr::ReferenceOpticalSpectrum::Monochromatic, rr::ReferenceOpticalSpectrum::Visible);
    CAPTURE(connection, spectrum);
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
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
    REQUIRE(BuildSphereSnapshot(device, cpu.Renderer, cpu.Program, spheres[0], true, SphereSurfaces::Glass) ==
            S::Ready);
    REQUIRE(BuildSphereSnapshot(device, gpu.Renderer, gpu.Program, spheres[1], true, SphereSurfaces::Glass) ==
            S::Ready);
    REQUIRE(cpu.Renderer.Release(cpu.Snapshot) == S::Ready);
    REQUIRE(gpu.Renderer.Release(gpu.Snapshot) == S::Ready);
    cpu.Snapshot = spheres[0];
    gpu.Snapshot = spheres[1];
    rr::ReferenceCaptureDescription capture;
    capture.Width = 32;
    capture.Height = 24;
    capture.TargetSamples = 16;
    capture.MaxDepth = 6;
    capture.Seed = 99;
    capture.Environment = {.2F, .25F, .3F};
    // The template's visible default. At the raw-AOV cases' millimetre, the envelope would gather more than the
    // region capacity of the tessellated spheres.
    constexpr float32 OPTICAL_WAVELENGTH_WORLD = 550e-9F;
    capture.OpticalWavelengthWorld = OPTICAL_WAVELENGTH_WORLD;
    capture.OpticalConnection = connection;
    capture.OpticalSpectrum = spectrum;
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
    Capture(cpu, capture, surface, oracle, oracleInfo);
    Capture(gpu, capture, surface, compute, computeInfo);
    const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
    CAPTURE(comparison.MaximumRadiance,
            comparison.MaximumEmission,
            math::Sqrt(comparison.SquaredRadiance),
            oracleInfo.ClosestQueries,
            computeInfo.ClosestQueries,
            oracleInfo.VisibilityQueries,
            computeInfo.VisibilityQueries);
    CHECK(oracleInfo.InvalidContributions == 0);
    CHECK(computeInfo.InvalidContributions == 0);
    CHECK(computeInfo.CommittedSamples == oracleInfo.CommittedSamples);
    // The same bounds as the typed glass transport case: a rare near-edge hit can send one path elsewhere.
    CHECK(math::Sqrt(comparison.SquaredRadiance) <= 1e-3F);
    CHECK(math::Distance(comparison.CpuMean, comparison.GpuMean) <= 1e-3F * float32(oracle.GetSize()));
    // Each relayed emitter hit counts one visibility query beyond the geometric profile's next-event estimates, so
    // the capture exercised relays.
    rr::ReferenceCaptureDescription geometric = capture;
    geometric.OpticalWavelengthWorld = 0;
    geometric.OpticalConnection = rr::ReferenceOpticalConnection::AngularSpectrum;
    geometric.OpticalSpectrum = rr::ReferenceOpticalSpectrum::Monochromatic;
    Array<rr::ReferencePixel> classical;
    rr::ReferenceCaptureInfo classicalInfo;
    Capture(cpu, geometric, surface, classical, classicalInfo);
    CHECK(oracleInfo.VisibilityQueries > classicalInfo.VisibilityQueries);
}

namespace
{
/// A complex Fresnel integral.
struct FresnelIntegral final
{
    float64 Real = 0;
    float64 Imaginary = 0;
};
/// Integral of exp(-i pi t^2 / 2) from 0 to v, by composite Simpson quadrature; it shares no code with the operators.
FresnelIntegral IntegrateFresnelFromZero(float64 v)
{
    constexpr usize INTERVALS = 2000;
    const float64 step = v / static_cast<float64>(INTERVALS);
    float64 real = 0;
    float64 imaginary = 0;
    for (usize index = 0; index <= INTERVALS; ++index)
    {
        const float64 t = static_cast<float64>(index) * step;
        const float64 weight = index == 0 || index == INTERVALS ? 1 : (index % 2 == 1 ? 4 : 2);
        const float64 phase = math::kPi * t * t / 2;
        real += weight * math::Cos(phase);
        imaginary -= weight * math::Sin(phase);
    }
    return {real * step / 3, imaginary * step / 3};
}
/// A point source (z < 0), a receiver (z > 0) and the x positions of a slit's edges, which run along y at z = 0, at one
/// wavelength.
struct SlitConnection final
{
    math::Vector3d SourceWorld{};
    math::Vector3d ReceiverWorld{};
    float64 LowerEdgeWorld = 0;
    float64 UpperEdgeWorld = 0;
    float64 WavelengthWorld = 1;
};
/// Fresnel-Kirchhoff power, relative to the unobstructed field, behind an absorbing screen with the connection's
/// straight slit. Each edge's signed Fresnel number is 2 sqrt(excess / lambda), from the excess length of the shortest
/// path that touches it (Keller's law), and the slit field is (1 + i)/2 times the integral of exp(-i pi t^2 / 2)
/// between them.
float64 EvaluateFresnelKirchhoffSlitPower(const SlitConnection& connection)
{
    const math::Vector3d sourceWorld = connection.SourceWorld;
    const math::Vector3d receiverWorld = connection.ReceiverWorld;
    const float64 wavelengthWorld = connection.WavelengthWorld;
    const math::Vector3d deltaWorld = receiverWorld - sourceWorld;
    const float64 distanceWorld = math::Sqrt(math::Dot(deltaWorld, deltaWorld));
    // The straight path crosses the screen at x = crossingWorld.
    const float64 crossingWorld = sourceWorld.X - sourceWorld.Z * deltaWorld.X / deltaWorld.Z;
    const auto computeFresnelNumber = [&](float64 edgeWorld) {
        const float64 sourceEdgeWorld =
            math::Sqrt((edgeWorld - sourceWorld.X) * (edgeWorld - sourceWorld.X) + sourceWorld.Z * sourceWorld.Z);
        const float64 edgeReceiverWorld = math::Sqrt((receiverWorld.X - edgeWorld) * (receiverWorld.X - edgeWorld) +
                                                     receiverWorld.Z * receiverWorld.Z);
        const float64 unfoldedWorld = sourceEdgeWorld + edgeReceiverWorld;
        const float64 heightWorld = receiverWorld.Y - sourceWorld.Y;
        const float64 excessWorld =
            math::Max(math::Sqrt(unfoldedWorld * unfoldedWorld + heightWorld * heightWorld) - distanceWorld, 0.0);
        const float64 magnitude = 2 * math::Sqrt(excessWorld / wavelengthWorld);
        return edgeWorld < crossingWorld ? -magnitude : magnitude;
    };
    const FresnelIntegral lower = IntegrateFresnelFromZero(computeFresnelNumber(connection.LowerEdgeWorld));
    const FresnelIntegral upper = IntegrateFresnelFromZero(computeFresnelNumber(connection.UpperEdgeWorld));
    const float64 real = upper.Real - lower.Real;
    const float64 imaginary = upper.Imaginary - lower.Imaginary;
    // |(1 + i)/2|^2 = 1/2.
    return (real * real + imaginary * imaginary) / 2;
}
} // namespace

TEST_CASE("A white-light slit's colour fringes match the analytic spectral integral",
          "[renderer][gpu][reference-compute][wave-spectral][wave-utd]")
{
    // Thanks to Shlomi Steinberg and Matt Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics", CGF
    // 45(2), e70322 (2026), Section 3: diffraction is wavelength dependent, so a slit under white light separates
    // colours. A point light shines through a 10 cm slit onto a white diffuse wall; an orthographic camera between the
    // screen and the wall sees the fringes. The spectral UTD capture, divided by an unobstructed geometric capture,
    // estimates each pixel's sensor-weighted spectral integral of the Fresnel-Kirchhoff slit power.
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
    Session gpu;
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);
    namespace format = ludus::content::scene_format;
    constexpr float32 HALF_EXTENT_WORLD = 2;
    constexpr float32 SLIT_HALF_WIDTH_WORLD = 0.05F;
    constexpr float32 WALL_DEPTH_WORLD = 1;
    const math::Vector3 sourceWorld{0, 0, -0.5F};
    // Design-scale light: 5 mm at 550 nm keeps the fringes' phases exact in binary32 (render-graph-authoring.md, "UTD
    // edge diffraction connections"); the spectrum scales it from 3.45 mm to 7.09 mm.
    constexpr float32 OPTICAL_WAVELENGTH_WORLD = 0.005F;
    constexpr usize SLIT_QUAD_COUNT = 3;
    StaticArray<QuadItem, SLIT_QUAD_COUNT> quads{};
    quads[0].CornersWorld = {{{-HALF_EXTENT_WORLD, -HALF_EXTENT_WORLD, WALL_DEPTH_WORLD},
                              {-HALF_EXTENT_WORLD, HALF_EXTENT_WORLD, WALL_DEPTH_WORLD},
                              {HALF_EXTENT_WORLD, HALF_EXTENT_WORLD, WALL_DEPTH_WORLD},
                              {HALF_EXTENT_WORLD, -HALF_EXTENT_WORLD, WALL_DEPTH_WORLD}}};
    quads[0].Model = format::MaterialModel::Diffuse;
    quads[0].BaseColor = {0.8F, 0.8F, 0.8F};
    for (usize side = 1; side < SLIT_QUAD_COUNT; ++side)
    {
        const float32 innerWorld = side == 1 ? -SLIT_HALF_WIDTH_WORLD : SLIT_HALF_WIDTH_WORLD;
        const float32 outerWorld = side == 1 ? -HALF_EXTENT_WORLD : HALF_EXTENT_WORLD;
        const float32 leftWorld = math::Min(innerWorld, outerWorld);
        const float32 rightWorld = math::Max(innerWorld, outerWorld);
        quads[side].CornersWorld = {{{leftWorld, -HALF_EXTENT_WORLD, 0},
                                     {leftWorld, HALF_EXTENT_WORLD, 0},
                                     {rightWorld, HALF_EXTENT_WORLD, 0},
                                     {rightWorld, -HALF_EXTENT_WORLD, 0}}};
        quads[side].Model = format::MaterialModel::Diffuse;
        quads[side].BaseColor = {};
    }
    REQUIRE(cpu.Renderer.Release(cpu.Snapshot) == S::Ready);
    REQUIRE(gpu.Renderer.Release(gpu.Snapshot) == S::Ready);
    REQUIRE(BuildQuadSnapshot(cpu.Renderer, cpu.Program, quads.GetData(), SLIT_QUAD_COUNT, cpu.Snapshot) == S::Ready);
    REQUIRE(BuildQuadSnapshot(gpu.Renderer, gpu.Program, quads.GetData(), SLIT_QUAD_COUNT, gpu.Snapshot) == S::Ready);
    rr::DirectLight light;
    light.SourceId = 1;
    light.Kind = rr::LightKind::Point;
    light.Position = sourceWorld;
    light.Range = 20;
    light.Intensity = 9;
    rr::ReferenceCaptureDescription capture;
    constexpr uint32 COLUMNS = 16;
    // Captures progress one sample of every pixel at a time, so rows add samples to a column cheaply on the GPU. The
    // CPU oracle and the GPU match sample for sample on the short capture; the tall GPU capture tightens the analytic
    // comparison.
    constexpr uint32 PARITY_ROWS = 4;
    constexpr uint32 ANALYTIC_ROWS = 64;
    constexpr uint32 SAMPLES = 1024;
    capture.Width = COLUMNS;
    capture.Height = PARITY_ROWS;
    capture.TargetSamples = SAMPLES;
    // One wall vertex: light reaches it only through the slit, and its continuation finds black screens and sky.
    capture.MaxDepth = 1;
    capture.RouletteDepth = 1;
    capture.Seed = 2281;
    capture.CenterRays = true;
    capture.Environment = {};
    capture.Lights = &light;
    capture.LightCount = 1;
    // Clip x and y span +-0.18 m and +-0.0625 m of the wall; rays start at z = 0.3, past the screen, and run along +z.
    // The columns cross the slit's image into the geometric shadow beyond x = 0.15 m. Their connections cross the
    // screen within 0.056 m of the axis, so at 380 nm both edges lie inside the 0.121 m Gaussian envelope that admits
    // UTD diffraction points.
    constexpr float32 CAMERA_DEPTH_WORLD = 0.3F;
    constexpr float32 HALF_WIDTH_WORLD = 0.18F;
    constexpr float32 HALF_HEIGHT_WORLD = 0.0625F;
    capture.OrthographicCamera = true;
    capture.WorldToClip.Columns[0].X = 1 / HALF_WIDTH_WORLD;
    capture.WorldToClip.Columns[1].Y = 1 / HALF_HEIGHT_WORLD;
    capture.WorldToClip.Columns[3].Z = -CAMERA_DEPTH_WORLD;
    capture.CameraPosition = {0, 0, CAMERA_DEPTH_WORLD};
    capture.OpticalWavelengthWorld = OPTICAL_WAVELENGTH_WORLD;
    capture.OpticalConnection = rr::ReferenceOpticalConnection::UtdEdgeDiffraction;
    capture.OpticalSpectrum = rr::ReferenceOpticalSpectrum::Visible;
    rr::ReferenceCaptureDescription tall = capture;
    tall.Height = ANALYTIC_ROWS;
    Array<rr::ReferencePixel> oracle;
    Array<rr::ReferencePixel> compute;
    Array<rr::ReferencePixel> converged;
    rr::ReferenceCaptureInfo oracleInfo;
    rr::ReferenceCaptureInfo computeInfo;
    rr::ReferenceCaptureInfo convergedInfo;
    Capture(cpu, capture, surface, oracle, oracleInfo);
    Capture(gpu, capture, surface, compute, computeInfo);
    Capture(gpu, tall, surface, converged, convergedInfo);
    CHECK(oracleInfo.InvalidContributions == 0);
    CHECK(computeInfo.InvalidContributions == 0);
    CHECK(convergedInfo.InvalidContributions == 0);
    // CPU and GPU follow the same wavelengths and paths; binary32 device arithmetic keeps their means close.
    const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
    CAPTURE(comparison.MaximumRadiance, math::Sqrt(comparison.SquaredRadiance));
    CHECK(comparison.MaximumRadiance <= 2e-3F);
    CHECK(math::Sqrt(comparison.SquaredRadiance) <= 1e-3F);
    // The unobstructed wall under geometric transport: the spectral integral of a white light on a white wall is the
    // same flat RGB, without wavelength noise.
    StaticArray<QuadItem, 1> wall{{quads[0]}};
    REQUIRE(cpu.Renderer.Release(cpu.Snapshot) == S::Ready);
    REQUIRE(BuildQuadSnapshot(cpu.Renderer, cpu.Program, wall.GetData(), 1, cpu.Snapshot) == S::Ready);
    // Analytic reference per column, averaged over its rows: midpoint quadrature over the spectrum of the sensor
    // times the Fresnel-Kirchhoff slit power at the scaled wavelength. The same quadrature gives the deviation of the
    // capture's one-wavelength estimates, so the bound is four standard errors plus the UTD operator's agreement with
    // Fresnel-Kirchhoff diffraction (render-graph-authoring.md, "UTD edge diffraction connections").
    constexpr usize SPECTRUM_STEPS = 200;
    constexpr float64 STANDARD_ERRORS = 4;
    constexpr float64 OPERATOR_TOLERANCE = 0.02;
    constexpr float64 MINIMUM_TINT = 0.1;
    const float64 range = static_cast<float64>(rr::internal::SPECTRUM_WAVELENGTH_RANGE_NANOMETERS);
    const float64 step = range / static_cast<float64>(SPECTRUM_STEPS);
    const auto checkAgainstAnalytic = [&](const rr::ReferenceCaptureDescription& description,
                                          const Array<rr::ReferencePixel>& pixels,
                                          const char* name) {
        const uint32 rows = description.Height;
        CAPTURE(name, rows);
        rr::ReferenceCaptureDescription open = description;
        open.TargetSamples = 1;
        open.OpticalWavelengthWorld = 0;
        open.OpticalConnection = rr::ReferenceOpticalConnection::AngularSpectrum;
        open.OpticalSpectrum = rr::ReferenceOpticalSpectrum::Monochromatic;
        Array<rr::ReferencePixel> unobstructed;
        rr::ReferenceCaptureInfo unobstructedInfo;
        Capture(cpu, open, surface, unobstructed, unobstructedInfo);
        float64 largestTint = 0;
        float64 estimatedTintAtLargest = 0;
        for (uint32 column = 0; column < COLUMNS; ++column)
        {
            math::Vector3d expected{};
            math::Vector3d actual{};
            math::Vector3d meanSquares{};
            for (uint32 row = 0; row < rows; ++row)
            {
                const usize pixel = usize{row} * COLUMNS + column;
                const math::Vector3d receiverWorld{
                    (2 * (static_cast<float64>(column) + 0.5) / COLUMNS - 1) * static_cast<float64>(HALF_WIDTH_WORLD),
                    (1 - 2 * (static_cast<float64>(row) + 0.5) / rows) * static_cast<float64>(HALF_HEIGHT_WORLD),
                    static_cast<float64>(WALL_DEPTH_WORLD)};
                for (usize index = 0; index < SPECTRUM_STEPS; ++index)
                {
                    const float64 nanometers =
                        static_cast<float64>(rr::internal::SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS) +
                        (static_cast<float64>(index) + 0.5) * step;
                    const float64 power = EvaluateFresnelKirchhoffSlitPower(
                    {
                        .SourceWorld = {0, 0, static_cast<float64>(sourceWorld.Z)},
                        .ReceiverWorld = receiverWorld,
                        .LowerEdgeWorld = -static_cast<float64>(SLIT_HALF_WIDTH_WORLD),
                        .UpperEdgeWorld = static_cast<float64>(SLIT_HALF_WIDTH_WORLD),
                        .WavelengthWorld = static_cast<float64>(OPTICAL_WAVELENGTH_WORLD) * nanometers /
                                           LUDUS_WAVE_SPECTRAL_REFERENCE_WAVELENGTH_NANOMETERS,
                    });
                    const math::Vector3 sensitivity =
                        rr::internal::EvaluateSensorSensitivity(static_cast<float32>(nanometers));
                    const math::Vector3d weight{static_cast<float64>(sensitivity.X),
                                                static_cast<float64>(sensitivity.Y),
                                                static_cast<float64>(sensitivity.Z)};
                    expected = expected + weight * (power * step / rows);
                    // The capture's one-sample estimate is range * sensitivity * power at a uniform wavelength.
                    meanSquares =
                        meanSquares + math::Vector3d{weight.X * weight.X, weight.Y * weight.Y, weight.Z * weight.Z} *
                                          (range * power * power * step / rows);
                }
                const math::Vector3 normalization = unobstructed[pixel].Radiance;
                REQUIRE(normalization.X > 0);
                REQUIRE(normalization.Y > 0);
                REQUIRE(normalization.Z > 0);
                REQUIRE(pixels[pixel].Samples == description.TargetSamples);
                actual = actual + math::Vector3d{static_cast<float64>(pixels[pixel].Radiance.X / normalization.X),
                                                 static_cast<float64>(pixels[pixel].Radiance.Y / normalization.Y),
                                                 static_cast<float64>(pixels[pixel].Radiance.Z / normalization.Z)} *
                                      (1.0 / rows);
            }
            // The rows' spread about the column mean only enlarges this deviation, keeping the bound conservative.
            const float64 standardErrorScale =
                STANDARD_ERRORS / math::Sqrt(static_cast<float64>(description.TargetSamples) * rows);
            const math::Vector3d bound =
                math::Vector3d{math::Sqrt(math::Max(meanSquares.X - expected.X * expected.X, 0.0)),
                               math::Sqrt(math::Max(meanSquares.Y - expected.Y * expected.Y, 0.0)),
                               math::Sqrt(math::Max(meanSquares.Z - expected.Z * expected.Z, 0.0))} *
                    standardErrorScale +
                math::Vector3d{OPERATOR_TOLERANCE, OPERATOR_TOLERANCE, OPERATOR_TOLERANCE};
            INFO("column " << column << " analytic " << expected.X << "," << expected.Y << "," << expected.Z
                           << " estimated " << actual.X << "," << actual.Y << "," << actual.Z << " bound " << bound.X
                           << "," << bound.Y << "," << bound.Z);
            CHECK(math::Abs(actual.X - expected.X) <= bound.X);
            CHECK(math::Abs(actual.Y - expected.Y) <= bound.Y);
            CHECK(math::Abs(actual.Z - expected.Z) <= bound.Z);
            if (math::Abs(expected.X - expected.Z) > math::Abs(largestTint))
            {
                largestTint = expected.X - expected.Z;
                estimatedTintAtLargest = actual.X - actual.Z;
            }
        }
        // The fixture separates colours: red and blue differ by more than a tenth of the unobstructed power at some
        // column, and the capture reproduces the sign of that fringe's tint.
        CAPTURE(largestTint, estimatedTintAtLargest);
        CHECK(math::Abs(largestTint) > MINIMUM_TINT);
        CHECK(largestTint * estimatedTintAtLargest > 0);
    };
    checkAgainstAnalytic(capture, oracle, "CPU oracle");
    checkAgainstAnalytic(tall, converged, "GPU compute");
}

TEST_CASE("A grain-masked closure program matches between CPU and GPU reference transport",
          "[renderer][gpu][reference-compute][program]")
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
    Session gpu;
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);

    // A linear grain mask in vertical stripes of zero, one and an interior value, read through the base colour
    // input: the board's white Lambertian lobe takes 1 - m and its glossy gold takes m.
    constexpr usize GRAIN_TEXELS = 6;
    constexpr usize GRAIN_TEXEL_BYTES = 4;
    constexpr uint8 INTERIOR_MASK_BYTE = 89;
    constexpr usize MASK_VALUES = 3;
    constexpr StaticArray<uint8, MASK_VALUES> MASK_BYTES{{0, 255, INTERIOR_MASK_BYTE}};
    StaticArray<uint8, GRAIN_TEXELS * GRAIN_TEXEL_BYTES> grainTexels{};
    for (usize texel = 0; texel < GRAIN_TEXELS; ++texel)
    {
        const uint8 mask = MASK_BYTES[texel % MASK_VALUES];
        grainTexels[texel * GRAIN_TEXEL_BYTES] = mask;
        grainTexels[texel * GRAIN_TEXEL_BYTES + 1] = mask;
        grainTexels[texel * GRAIN_TEXEL_BYTES + 2] = mask;
        grainTexels[texel * GRAIN_TEXEL_BYTES + 3] = 255;
    }
    rhi::TextureDescription grainDescription{static_cast<uint32>(GRAIN_TEXELS), 1, rhi::RasterFormat::Rgba8Unorm};
    grainDescription.RetainCpuContent = true;
    rhi::TextureHandle grain;
    REQUIRE(rhi::CreateTexture(device,
                               grainDescription,
                               {{grainTexels.GetData(), grainTexels.GetSize()}, GRAIN_TEXELS * GRAIN_TEXEL_BYTES},
                               grain) == S::Ready);
    const ludus::content::scene_format::MaterialProgram program = ludus::renderer_tests::MakeGrainMixProgram();
    StaticArray<QuadItem, 2> quads{};
    QuadItem& board = quads[0];
    board.CornersWorld = {{{-1, -1, 0}, {-1, 1, 0}, {1, 1, 0}, {1, -1, 0}}};
    board.BaseColor = {1, 1, 1};
    board.Model = ludus::content::scene_format::MaterialModel::Program;
    board.Roughness = ludus::renderer_tests::GRAIN_MIX_ROUGHNESS;
    board.Texture = grain;
    board.ProgramOrNull = &program;
    // A diffuse floor below the board returns light between the two surfaces.
    QuadItem& floor = quads[1];
    floor.CornersWorld = {{{-2, -1, -2}, {-2, -1, 2}, {2, -1, 2}, {2, -1, -2}}};
    for (Session* session : {&cpu, &gpu})
    {
        REQUIRE(session->Renderer.Release(session->Snapshot) == S::Ready);
        REQUIRE(BuildQuadSnapshot(session->Renderer,
                                  session->Program,
                                  quads.GetData(),
                                  quads.GetSize(),
                                  session->Snapshot) == S::Ready);
    }
    // The sun lights the board from off the camera's mirror direction.
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
            (gpuStart - cpuStart) / NANOSECONDS_PER_MILLISECOND,
            (gpuEnd - gpuStart) / NANOSECONDS_PER_MILLISECOND);
    CHECK(computeInfo.InvalidContributions == 0);
    CHECK(computeInfo.CommittedSamples == oracleInfo.CommittedSamples);
    // The tolerances of the typed-material parity case: matched raw linear radiance, not a converged mean.
    CHECK(math::Sqrt(comparison.SquaredRadiance) <= 1e-3F);
    CHECK(math::Distance(comparison.CpuMean, comparison.GpuMean) <= 1e-3F * float32(oracle.GetSize()));
    // Each stripe reaches the image: the board's centre row crosses all three mask values, so its pixels differ.
    float32 darkest = oracle[0].Radiance.X;
    float32 brightest = darkest;
    const usize centreRow = usize{capture.Height / 2} * capture.Width;
    for (usize column = 0; column < capture.Width; ++column)
    {
        darkest = math::Min(darkest, oracle[centreRow + column].Radiance.X);
        brightest = math::Max(brightest, oracle[centreRow + column].Radiance.X);
    }
    CHECK(brightest > darkest);
}

TEST_CASE("RT1 rough-metal white furnace matches the single-scattering integral on CPU and GPU",
          "[renderer][gpu][reference-compute][materials]")
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
    Session gpu;
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);

    QuadItem metal;
    metal.CornersWorld = {{{-2, -2, 0}, {-2, 2, 0}, {2, 2, 0}, {2, -2, 0}}};
    metal.BaseColor = {1, 1, 1};
    metal.Model = ludus::content::scene_format::MaterialModel::MetallicRoughness;
    metal.Metallic = 1;
    metal.Roughness = 1;
    for (Session* session : {&cpu, &gpu})
    {
        REQUIRE(session->Renderer.Release(session->Snapshot) == S::Ready);
        REQUIRE(BuildQuadSnapshot(session->Renderer, session->Program, &metal, 1, session->Snapshot) == S::Ready);
    }

    // Thanks to Sébastien Lagarde, "Physically-Based Materials: Where Are We?", SIGGRAPH 2017,
    // slides 36–37, https://openproblems.realtimerendering.com/s2017/02-PhysicallyBasedMaterialWhereAreWe.pdf.
    // The furnace isolates missing microfacet bounces. At normal incidence, alpha = F = 1 gives
    // f(z) = 1 / (2*pi*(1+z)); integrating 2*pi*z*f(z) from 0 to 1 gives 1 - ln(2).
    const float64 expectedRadiance = 1 - math::Log(2.0);
    constexpr uint32 IMAGE_EXTENT = 16;
    constexpr uint32 SAMPLES_PER_PIXEL = 64;
    constexpr uint32 DEEP_PATH_LIMIT = 4;
    constexpr uint64 FIRST_SEED = 23;
    constexpr uint64 SECOND_SEED = 59;
    // Both proposals have density 1/(4*pi) for valid directions. The two MIS-weighted terms sum to at most one
    // per trial. At 16,384 trials, Hoeffding's two-sided bound for an error above 0.02 is about 4.1e-6.
    constexpr float64 MEAN_RADIANCE_TOLERANCE = 0.02;
    constexpr float32 MATCHED_PIXEL_TOLERANCE = 1e-3F;
    // Thanks to Eric Heitz, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs",
    // JCGT 3(2), 2014, section 3.4, https://jcgt.org/published/0003/02/03/paper.pdf#page=18.
    // The weak white furnace includes both hemispheres. With alpha = 1 and a normal view, the masking-only
    // reflected density is uniform on the sphere. Half its proposals fall below the plane and stay null.
    constexpr float64 EXPECTED_NULL_FRACTION = 0.5;
    constexpr float64 NULL_FRACTION_TOLERANCE = 0.02;
    constexpr float64 PROPOSALS_PER_CAPTURE = static_cast<float64>(IMAGE_EXTENT * IMAGE_EXTENT * SAMPLES_PER_PIXEL);
    rr::ReferenceCaptureDescription capture;
    capture.Width = IMAGE_EXTENT;
    capture.Height = IMAGE_EXTENT;
    capture.OrthographicCamera = true;
    capture.WorldToClip.Columns[3].Z = 0.5F;
    capture.CenterRays = true;
    capture.Environment = {1, 1, 1};
    capture.TargetSamples = SAMPLES_PER_PIXEL;
    capture.RouletteDepth = DEEP_PATH_LIMIT;
    for (const uint64 seed : {FIRST_SEED, SECOND_SEED})
    {
        capture.Seed = seed;
        for (const uint32 pathLimit : {uint32{1}, DEEP_PATH_LIMIT})
        {
            capture.MaxDepth = pathLimit;
            CAPTURE(seed, pathLimit);
            Array<rr::ReferencePixel> oracle;
            Array<rr::ReferencePixel> compute;
            rr::ReferenceCaptureInfo oracleInfo;
            rr::ReferenceCaptureInfo computeInfo;
            Capture(cpu, capture, surface, oracle, oracleInfo);
            Capture(gpu, capture, surface, compute, computeInfo);
            CHECK(oracleInfo.Traversal == rr::ReferenceTraversal::CpuOracle);
            CHECK(computeInfo.Traversal == rr::ReferenceTraversal::GpuCompute);
            CHECK(oracleInfo.InvalidContributions == 0);
            CHECK(computeInfo.InvalidContributions == 0);
            const float64 cpuNullFraction = static_cast<float64>(oracleInfo.NullBsdfSamples) / PROPOSALS_PER_CAPTURE;
            const float64 gpuNullFraction = static_cast<float64>(computeInfo.NullBsdfSamples) / PROPOSALS_PER_CAPTURE;
            CAPTURE(cpuNullFraction, gpuNullFraction);
            CHECK(math::Abs(cpuNullFraction - EXPECTED_NULL_FRACTION) <= NULL_FRACTION_TOLERANCE);
            CHECK(math::Abs(gpuNullFraction - EXPECTED_NULL_FRACTION) <= NULL_FRACTION_TOLERANCE);
            const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
            CAPTURE(comparison.MaximumRadiance, comparison.MaximumDirect);
            CHECK(comparison.MaximumRadiance <= MATCHED_PIXEL_TOLERANCE);
            CHECK(comparison.MaximumDirect <= MATCHED_PIXEL_TOLERANCE);
            for (const Array<rr::ReferencePixel>* pixels : {&oracle, &compute})
            {
                INFO((pixels == &oracle ? "CPU oracle" : "GPU compute"));
                const ImageMeans means = MeasureImage(*pixels);
                CAPTURE(means.Radiance.X, expectedRadiance);
                CHECK(means.Emission == math::Vector3{});
                CHECK(means.Indirect == math::Vector3{});
                CHECK(means.Radiance == means.Direct);
                CHECK(means.Radiance.X == means.Radiance.Y);
                CHECK(means.Radiance.X == means.Radiance.Z);
                CHECK(math::Abs(static_cast<float64>(means.Radiance.X) - expectedRadiance) <= MEAN_RADIANCE_TOLERANCE);
            }
        }
    }
}

namespace
{
using ludus::renderer_tests::Double3;
using ludus::renderer_tests::MicrofacetRandomWalk;
using ludus::renderer_tests::OracleRandom;
constexpr rr::ReferenceMaterialProfile SINGLE_SCATTERING = rr::ReferenceMaterialProfile::SingleScattering;
constexpr rr::ReferenceMaterialProfile MULTIPLE_SCATTERING = rr::ReferenceMaterialProfile::MultipleScatteringGgx;
/// Gold-like RGB optical constants whose Fresnel reflectance differs strongly by channel.
constexpr StaticArray<float32, 3> GOLD_ETA{{0.143F, 0.374F, 1.442F}};
constexpr StaticArray<float32, 3> GOLD_K{{3.983F, 2.385F, 1.603F}};
/// The achromatic control repeats gold's green channel in every channel.
constexpr StaticArray<float32, 3> ACHROMATIC_ETA{{0.374F, 0.374F, 0.374F}};
constexpr StaticArray<float32, 3> ACHROMATIC_K{{2.385F, 2.385F, 2.385F}};

float64 Widen(float32 value)
{
    return static_cast<float64>(value);
}
Double3 Widen(math::Vector3 value)
{
    return {Widen(value.X), Widen(value.Y), Widen(value.Z)};
}
Double3 Widen(const StaticArray<float32, 3>& value)
{
    return {Widen(value[0]), Widen(value[1]), Widen(value[2])};
}
float64 SumChannels(Double3 value)
{
    return value.X + value.Y + value.Z;
}
/// Direction at a signed angle from the normal in the plane x = 0 of the tangent frame (+z the normal).
math::Vector3 MakeTangentDirection(float64 degrees)
{
    const float64 radians = degrees * math::kPi / 180;
    return {0, static_cast<float32>(std::sin(radians)), static_cast<float32>(std::cos(radians))};
}
/// The z = 0 quad of these tests faces -z: world (x, y, z) is tangent (x, -y, -z).
math::Vector3 ToWorld(math::Vector3 tangent)
{
    return {tangent.X, -tangent.Y, -tangent.Z};
}
/// A 4 x 4 m quad at z = 0 facing -z with one material.
QuadItem MakeTestPlane()
{
    QuadItem plane;
    plane.CornersWorld = {{{-2, -2, 0}, {-2, 2, 0}, {2, 2, 0}, {2, -2, 0}}};
    return plane;
}
QuadItem MakeWhiteMetalPlane(float32 roughness)
{
    QuadItem plane = MakeTestPlane();
    plane.BaseColor = {1, 1, 1};
    plane.Metallic = 1;
    plane.Roughness = roughness;
    return plane;
}
QuadItem
MakeConductorPlane(float32 alpha, const StaticArray<float32, 3>& eta, const StaticArray<float32, 3>& extinction)
{
    QuadItem plane = MakeTestPlane();
    plane.Model = ludus::content::scene_format::MaterialModel::Conductor;
    plane.ConductorAlpha = alpha;
    plane.ConductorEta = eta;
    plane.ConductorK = extinction;
    return plane;
}
rr::ReferenceBsdf MakeConductorBsdf(float32 alpha,
                                    const StaticArray<float32, 3>& eta,
                                    const StaticArray<float32, 3>& extinction,
                                    rr::ReferenceMaterialProfile profile)
{
    rr::ReferenceBsdf bsdf;
    bsdf.Scattering.Model = ludus::content::scene_format::MaterialModel::Conductor;
    bsdf.Scattering.AlphaU = alpha;
    bsdf.Scattering.AlphaV = alpha;
    bsdf.Scattering.Eta = eta;
    bsdf.Scattering.K = extinction;
    bsdf.Profile = profile;
    return bsdf;
}
/// Orthographic view of the test plane at `viewTangent`, a direction in the x = 0 plane of its tangent frame. The clip
/// window spans +-halfExtentWorld across the view and +-2 m along it, centred on the origin.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the window's half extent in metres, then its pixel count.
rr::ReferenceCaptureDescription MakeTiltedCapture(math::Vector3 viewTangent, float32 halfExtentWorld, uint32 extent)
{
    constexpr float32 DEPTH_SCALE = 0.25F;
    // Rays travel along -view in world space; the image y axis is the view rotated a quarter turn about x.
    const math::Vector3 rayWorld = -ToWorld(viewTangent);
    const math::Vector3 upWorld{0, rayWorld.Z, -rayWorld.Y};
    const float32 scale = 1 / halfExtentWorld;
    rr::ReferenceCaptureDescription capture;
    capture.Width = extent;
    capture.Height = extent;
    capture.OrthographicCamera = true;
    capture.CenterRays = true;
    capture.WorldToClip.Columns[0] = {scale, 0, 0, 0};
    capture.WorldToClip.Columns[1] = {0, upWorld.Y * scale, DEPTH_SCALE * rayWorld.Y, 0};
    capture.WorldToClip.Columns[2] = {0, upWorld.Z * scale, DEPTH_SCALE * rayWorld.Z, 0};
    capture.WorldToClip.Columns[3] = {0, 0, 0.5F, 1};
    return capture;
}
/// Paired sessions on one compute device; CpuOracle first, then GpuCompute.
struct PairedSessions final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    Session Cpu;
    Session Gpu;
};
/// Start both traversals; on a device that cannot run the GPU-compute reference, return the skip reason, otherwise
/// null.
const char* StartPairedSessions(PairedSessions& outSessions)
{
    if (StartComputeDevice(outSessions.Device, outSessions.Surface) == false)
    {
        return "No Metal adapter";
    }
    const S enabled = Prepare(outSessions.Device, rr::ReferenceTraversal::GpuCompute, outSessions.Gpu);
    if (enabled == S::Unsupported)
    {
        return "Compute storage is unavailable on this device";
    }
    REQUIRE(enabled == S::Ready);
    REQUIRE(Prepare(outSessions.Device, rr::ReferenceTraversal::CpuOracle, outSessions.Cpu) == S::Ready);
    return nullptr;
}
void UsePlane(PairedSessions& inoutSessions, const QuadItem& plane)
{
    for (Session* session : {&inoutSessions.Cpu, &inoutSessions.Gpu})
    {
        REQUIRE(session->Renderer.Release(session->Snapshot) == S::Ready);
        REQUIRE(BuildQuadSnapshot(session->Renderer, session->Program, &plane, 1, session->Snapshot) == S::Ready);
    }
}
/// The direct radiance of a 1 x 1, one-sample capture of the plane under one unit white directional light, which is
/// f(view, light) cos(light) exactly: the continuation finds a black environment.
struct DirectionalResponse final
{
    math::Vector3 Cpu{};
    math::Vector3 Gpu{};
};
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the view then the light, as EvaluateReferenceBsdf orders them.
DirectionalResponse CaptureDirectionalResponse(PairedSessions& inoutSessions,
                                               math::Vector3 viewTangent,
                                               math::Vector3 lightTangent,
                                               rr::ReferenceMaterialProfile profile)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    rr::ReferenceCaptureDescription capture = MakeTiltedCapture(viewTangent, 0.01F, 1);
    capture.MaxDepth = 1;
    capture.RouletteDepth = 1;
    capture.TargetSamples = 1;
    capture.LightSampling = rr::ReferenceLightSampling::SeparateDirectional;
    capture.MaterialProfile = profile;
    rr::DirectLight light;
    light.SourceId = 1;
    light.Kind = rr::LightKind::Directional;
    light.Direction = -ToWorld(lightTangent);
    light.Color = {1, 1, 1};
    light.Intensity = 1;
    capture.Lights = &light;
    capture.LightCount = 1;
    DirectionalResponse response;
    for (const bool isGpu : {false, true})
    {
        Session& session = isGpu == true ? inoutSessions.Gpu : inoutSessions.Cpu;
        Array<rr::ReferencePixel> pixels;
        rr::ReferenceCaptureInfo info;
        Capture(session, capture, inoutSessions.Surface, pixels, info);
        CHECK(info.MaterialProfile == profile);
        CHECK(info.InvalidContributions == 0);
        REQUIRE(pixels.GetSize() == 1);
        CHECK(pixels[0].Indirect == math::Vector3{});
        (isGpu == true ? response.Gpu : response.Cpu) = pixels[0].Direct;
    }
    return response;
}
/// Peak angle and full width at half maximum of a sampled lobe, with linear interpolation between samples.
struct LobeShape final
{
    float64 PeakDegrees = 0;
    float64 WidthDegrees = 0;
};
template <usize COUNT>
LobeShape MeasureLobe(const StaticArray<float64, COUNT>& degrees, const StaticArray<float64, COUNT>& values)
{
    usize peak = 0;
    for (usize i = 1; i < COUNT; ++i)
    {
        peak = values[i] > values[peak] ? i : peak;
    }
    const float64 half = 0.5 * values[peak];
    float64 left = degrees[0];
    for (usize i = peak; i > 0; --i)
    {
        if (values[i - 1] < half)
        {
            left =
                degrees[i - 1] + (degrees[i] - degrees[i - 1]) * (half - values[i - 1]) / (values[i] - values[i - 1]);
            break;
        }
    }
    float64 right = degrees[COUNT - 1];
    for (usize i = peak; i + 1 < COUNT; ++i)
    {
        if (values[i + 1] < half)
        {
            right = degrees[i] + (degrees[i + 1] - degrees[i]) * (values[i] - half) / (values[i] - values[i + 1]);
            break;
        }
    }
    return {degrees[peak], right - left};
}
} // namespace

TEST_CASE("RT1 smooths the camera-visible atmosphere on CPU and GPU", "[renderer][gpu][reference-compute]")
{
    struct Shutdown final
    {
        ~Shutdown()
        {
            rhi::Shutdown();
        }
    } shutdown;
    PairedSessions sessions;
    if (const char* skipReasonOrNull = StartPairedSessions(sessions); skipReasonOrNull != nullptr)
    {
        SKIP(skipReasonOrNull);
    }
    QuadItem plane = MakeTestPlane();
    for (math::Vector3& cornerWorld : plane.CornersWorld)
    {
        cornerWorld.X += 10;
        cornerWorld.Z += 10;
    }
    UsePlane(sessions, plane);

    // A coarse map with distinct rows and columns makes the interpolation weights observable in linear RGB.
    constexpr uint32 SIDE = 2;
    const StaticArray<math::Vector4, SIDE * SIDE> texels{{{1, 2, 3, 1}, {5, 6, 7, 1}, {3, 4, 5, 1}, {7, 8, 9, 1}}};
    rr::EnvironmentDescription source;
    source.PixelsOrNull = texels.GetData();
    source.Width = SIDE;
    source.Height = SIDE;
    source.FilterWidth = 16;
    source.FilterSamples = 16;
    source.CpuByteBudget = CPU_BUDGET;
    rr::EnvironmentMap environment;
    S status = environment.Initialize(sessions.Device, source);
    const uint64 started = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - started < TIMEOUT)
    {
        threading::YieldThread();
        status = environment.GetStatus();
    }
    REQUIRE(status == S::Ready);

    rr::AtmosphereDescription atmosphere;
    atmosphere.IsEnabled = true;
    atmosphere.SolarIlluminance = {};
    rr::ReferenceCaptureDescription capture = MakeTiltedCapture({0, 0, 1}, 0.01F, 1);
    capture.TargetSamples = 1;
    capture.MaxDepth = 1;
    capture.EnvironmentTextureOrNull = &environment;
    capture.AtmosphereBackgroundOrNull = &atmosphere;

    const auto checkSky = [&](math::Vector3 expected) {
        for (Session* session : {&sessions.Cpu, &sessions.Gpu})
        {
            Array<rr::ReferencePixel> pixels;
            rr::ReferenceCaptureInfo info;
            Capture(*session, capture, sessions.Surface, pixels, info);
            REQUIRE(pixels.GetSize() == 1);
            CHECK(info.InvalidContributions == 0);
            CHECK(pixels[0].Radiance == pixels[0].Emission);
            CHECK(pixels[0].Direct == math::Vector3{});
            CHECK(pixels[0].Indirect == math::Vector3{});
            constexpr float32 RGB_TOLERANCE = 2e-5F;
            CHECK(math::Length(pixels[0].Emission - expected) <= RGB_TOLERANCE);
        }
    };

    {
        INFO("Imported environment, nearest texel at the equator");
        atmosphere.ShouldLightScene = false;
        checkSky({3, 4, 5});
    }
    atmosphere.ShouldLightScene = true;
    {
        INFO("Atmosphere at the equator, halfway between rows");
        checkSky({2, 3, 4});
    }
    {
        INFO("Atmosphere at the longitude seam, halfway between all four texels");
        const math::Vector4 horizontal = capture.WorldToClip.Columns[0];
        capture.WorldToClip.Columns[0] = capture.WorldToClip.Columns[2];
        capture.WorldToClip.Columns[2] = -horizontal;
        checkSky({4, 5, 6});
    }
    {
        INFO("Atmosphere above the northmost texel center");
        capture.WorldToClip = MakeTiltedCapture({0, 0.8F, 0.6F}, 0.01F, 1).WorldToClip;
        checkSky({1, 2, 3});
    }
}

TEST_CASE("RT1 draws celestial disks and planet points on CPU and GPU", "[renderer][gpu][reference-compute]")
{
    struct Shutdown final
    {
        ~Shutdown()
        {
            rhi::Shutdown();
        }
    } shutdown;
    PairedSessions sessions;
    const char* skipReasonOrNull = StartPairedSessions(sessions);
    if (skipReasonOrNull != nullptr)
    {
        SKIP(skipReasonOrNull);
    }
    QuadItem plane = MakeTestPlane();
    for (math::Vector3& cornerWorld : plane.CornersWorld)
    {
        cornerWorld.X += 10;
    }
    UsePlane(sessions, plane);

    const math::Vector3 directionWorld{0, 0, 1};
    rr::ReferenceCaptureDescription capture = MakeTiltedCapture(directionWorld, 0.01F, 1);
    capture.MaxDepth = 1;
    capture.WorldToClip.Columns[3].Z = 0;
    rr::AtmosphereDescription sky;
    sky.IsEnabled = true;
    sky.SurfaceAnchorWorld = capture.CameraPosition;
    sky.SurfaceUpWorld = directionWorld;
    sky.DirectionToSunWorld = directionWorld;
    sky.SolarIlluminance = {0.02F, 0, 0};
    capture.AtmosphereBackgroundOrNull = &sky;

    const auto checkSky = [&](math::Vector3 minimum, math::Vector3 maximum) {
        Array<rr::ReferencePixel> oracle;
        Array<rr::ReferencePixel> compute;
        rr::ReferenceCaptureInfo oracleInfo;
        rr::ReferenceCaptureInfo computeInfo;
        Capture(sessions.Cpu, capture, sessions.Surface, oracle, oracleInfo);
        Capture(sessions.Gpu, capture, sessions.Surface, compute, computeInfo);
        const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
        constexpr float32 PARITY_TOLERANCE = 2e-3F;
        CHECK(comparison.MaximumRadiance <= PARITY_TOLERANCE);
        CHECK(comparison.MaximumEmission <= PARITY_TOLERANCE);
        CHECK(comparison.MaximumDirect == 0);
        CHECK(comparison.MaximumIndirect == 0);
        const rr::ReferencePixel pixel = oracle[0];
        CHECK(pixel.Radiance == pixel.Emission);
        CHECK(pixel.Direct == math::Vector3{});
        CHECK(pixel.Indirect == math::Vector3{});
        CHECK(pixel.Emission.X >= minimum.X);
        CHECK(pixel.Emission.Y >= minimum.Y);
        CHECK(pixel.Emission.Z >= minimum.Z);
        CHECK(pixel.Emission.X <= maximum.X);
        CHECK(pixel.Emission.Y <= maximum.Y);
        CHECK(pixel.Emission.Z <= maximum.Z);
    };
    INFO("Sun disk at zenith");
    checkSky({100, 0, 0}, {400, 0, 0});

    sky.Celestial.IsEnabled = true;
    sky.Celestial.DirectionToMoonWorld = directionWorld;
    sky.Celestial.DirectionToSunAtMoonWorld = -directionWorld;
    sky.Celestial.LunarAngularRadiusRadians = rr::CelestialBackgroundDescription::DEFAULT_LUNAR_RADIUS_RADIANS;
    sky.Celestial.LunarPeakRadiance = {0, 4, 0};
    INFO("Full Moon occludes the Sun");
    checkSky({0, 3, 0}, {0, 4, 0});

    sky.Celestial.DirectionToSunAtMoonWorld = directionWorld;
    INFO("Unlit lunar disk still occludes the Sun");
    checkSky({}, {});

    sky.Celestial.LunarAngularRadiusRadians = 0;
    sky.SolarIlluminance = {};
    sky.Celestial.Planets[0] = { .DirectionWorld = directionWorld, .Illuminance = {0, 0, 1e-5F} };
    INFO("Unresolved planet disappears behind the ground");
    checkSky({0, 0, 8}, {0, 0, 14});
    for (Session* session : {&sessions.Cpu, &sessions.Gpu})
    {
        rr::ReferenceCapture handle;
        const S begun = session->Renderer.BeginReferenceCapture(session->Snapshot, capture, handle);
        REQUIRE((begun == S::Ready || begun == S::Pending));
        REQUIRE(Run(session->Renderer, handle, sessions.Surface) == S::Ready);
        rr::ReferenceCaptureInfo before;
        REQUIRE(session->Renderer.GetReferenceCaptureInfo(handle, before) == S::Ready);

        // Update reads the new fields even though the caller reused the description and its address.
        sky.Celestial.LunarAngularRadiusRadians = -1;
        CHECK(session->Renderer.UpdateReferenceCapture(handle, session->Snapshot, capture) == S::InvalidDescription);
        CHECK(session->Renderer.GetStatus(handle) == S::Ready);
        sky.Celestial.LunarAngularRadiusRadians = 0;
        sky.SurfaceUpWorld = -directionWorld;
        const S replaced = session->Renderer.UpdateReferenceCapture(handle, session->Snapshot, capture);
        REQUIRE((replaced == S::Ready || replaced == S::Pending));
        REQUIRE(Run(session->Renderer, handle, sessions.Surface) == S::Ready);
        rr::ReferenceCaptureInfo after;
        REQUIRE(session->Renderer.GetReferenceCaptureInfo(handle, after) == S::Ready);
        CHECK(after.Revision > before.Revision);
        rr::ReferencePixel pixel;
        REQUIRE(session->Renderer.CopyReferencePixels(handle, &pixel, 1) == S::Ready);
        CHECK(pixel.Radiance == math::Vector3{});
        CHECK(pixel.Samples == capture.TargetSamples);
        REQUIRE(session->Renderer.Release(handle) == S::Ready);
        sky.SurfaceUpWorld = directionWorld;
    }

    sky.SurfaceUpWorld = directionWorld;
    plane = MakeTestPlane();
    for (math::Vector3& cornerWorld : plane.CornersWorld)
    {
        cornerWorld.Z = 1;
    }
    UsePlane(sessions, plane);
    INFO("Geometry occludes the celestial background");
    checkSky({}, {});
}

TEST_CASE("RT1 draws the same procedural star field on CPU and GPU", "[renderer][gpu][reference-compute]")
{
    struct Shutdown final
    {
        ~Shutdown()
        {
            rhi::Shutdown();
        }
    } shutdown;
    PairedSessions sessions;
    const char* skipReasonOrNull = StartPairedSessions(sessions);
    if (skipReasonOrNull != nullptr)
    {
        SKIP(skipReasonOrNull);
    }
    QuadItem plane = MakeTestPlane();
    for (math::Vector3& cornerWorld : plane.CornersWorld)
    {
        cornerWorld.X += 10;
    }
    UsePlane(sessions, plane);

    constexpr uint32 EXTENT = 32;
    rr::ReferenceCaptureDescription capture = MakePerspectiveCapture({0, 0, -2}, EXTENT, EXTENT);
    capture.TargetSamples = 2;
    capture.MaxDepth = 1;
    rr::AtmosphereDescription sky;
    sky.IsEnabled = true;
    sky.SurfaceAnchorWorld = capture.CameraPosition;
    sky.SurfaceUpWorld = {0, 0, 1};
    sky.Celestial.IsEnabled = true;
    sky.Celestial.StarRadiance = 4;
    capture.AtmosphereBackgroundOrNull = &sky;

    Array<rr::ReferencePixel> oracle;
    Array<rr::ReferencePixel> compute;
    rr::ReferenceCaptureInfo oracleInfo;
    rr::ReferenceCaptureInfo computeInfo;
    Capture(sessions.Cpu, capture, sessions.Surface, oracle, oracleInfo);
    Capture(sessions.Gpu, capture, sessions.Surface, compute, computeInfo);
    const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
    constexpr float32 PARITY_TOLERANCE = 2e-3F;
    CHECK(comparison.MaximumRadiance <= PARITY_TOLERANCE);
    CHECK(comparison.MaximumEmission <= PARITY_TOLERANCE);
    CHECK(comparison.MaximumDirect == 0);
    CHECK(comparison.MaximumIndirect == 0);
    CHECK(comparison.CpuMean.X > 0.01F);
}

TEST_CASE("RT1 multiple-scattering GGX white furnace approaches unit radiance on CPU and GPU",
          "[renderer][gpu][reference-compute][materials][multiple-scattering]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    PairedSessions sessions;
    if (const char* skipReasonOrNull = StartPairedSessions(sessions); skipReasonOrNull != nullptr)
    {
        SKIP(skipReasonOrNull);
    }
    // The single-scattering baseline is the rough-metal furnace above. The compensated profile restores the energy
    // lost between microfacets at one scattering event, so its furnace target is one at every depth. The grazing case
    // (roughness 0.06, 89.75 degrees) and the fully rough normal-incidence baseline come from the Imageworks review.
    struct FurnaceCase final
    {
        float32 Roughness = 0;
        float64 Degrees = 0;
    };
    const FurnaceCase cases[]{{1, 0}, {1, 60}, {1, 85}, {0.5F, 0}, {0.5F, 75}, {0.25F, 45}, {0.06F, 0}, {0.06F, 89.75}};
    constexpr uint32 IMAGE_EXTENT = 16;
    constexpr uint32 SAMPLES_PER_PIXEL = 64;
    constexpr uint32 DEEP_PATH_LIMIT = 4;
    constexpr uint64 SEEDS[]{23, 59};
    // As in the single-scattering furnace, each MIS-weighted trial is bounded and 16,384 trials per seed bound the
    // mean error by 0.02 except with probability about 4e-6; the BSDF-level sweep checks 0.01 deterministically.
    constexpr float64 MEAN_RADIANCE_TOLERANCE = 0.02;
    constexpr float32 MATCHED_PIXEL_TOLERANCE = 1e-3F;
    float64 largestError = 0;
    for (const FurnaceCase& furnace : cases)
    {
        UsePlane(sessions, MakeWhiteMetalPlane(furnace.Roughness));
        rr::ReferenceCaptureDescription capture =
            MakeTiltedCapture(MakeTangentDirection(furnace.Degrees), 0.005F, IMAGE_EXTENT);
        capture.Environment = {1, 1, 1};
        capture.TargetSamples = SAMPLES_PER_PIXEL;
        capture.RouletteDepth = DEEP_PATH_LIMIT;
        capture.MaterialProfile = MULTIPLE_SCATTERING;
        for (const uint64 seed : SEEDS)
        {
            capture.Seed = seed;
            for (const uint32 pathLimit : {uint32{1}, DEEP_PATH_LIMIT})
            {
                capture.MaxDepth = pathLimit;
                CAPTURE(furnace.Roughness, furnace.Degrees, seed, pathLimit);
                Array<rr::ReferencePixel> oracle;
                Array<rr::ReferencePixel> compute;
                rr::ReferenceCaptureInfo oracleInfo;
                rr::ReferenceCaptureInfo computeInfo;
                Capture(sessions.Cpu, capture, sessions.Surface, oracle, oracleInfo);
                Capture(sessions.Gpu, capture, sessions.Surface, compute, computeInfo);
                CHECK(oracleInfo.MaterialProfile == MULTIPLE_SCATTERING);
                CHECK(computeInfo.MaterialProfile == MULTIPLE_SCATTERING);
                CHECK(oracleInfo.InvalidContributions == 0);
                CHECK(computeInfo.InvalidContributions == 0);
                const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
                CAPTURE(comparison.MaximumRadiance, comparison.MaximumDirect);
                CHECK(comparison.MaximumRadiance <= MATCHED_PIXEL_TOLERANCE);
                for (const Array<rr::ReferencePixel>* pixels : {&oracle, &compute})
                {
                    INFO((pixels == &oracle ? "CPU oracle" : "GPU compute"));
                    const ImageMeans means = MeasureImage(*pixels);
                    CAPTURE(means.Radiance.X);
                    CHECK(means.Indirect == math::Vector3{});
                    CHECK(means.Radiance.X == means.Radiance.Y);
                    CHECK(means.Radiance.X == means.Radiance.Z);
                    const float64 error = std::fabs(Widen(means.Radiance.X) - 1);
                    largestError = std::fmax(largestError, error);
                    CHECK(error <= MEAN_RADIANCE_TOLERANCE);
                }
            }
        }
    }
    WARN("Largest multiple-scattering furnace capture error: " << largestError);
}

TEST_CASE("RT1 multiple-scattering GGX resolves the directional lobe of absorbing metals on CPU and GPU",
          "[renderer][gpu][reference-compute][materials][multiple-scattering][oracle]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    PairedSessions sessions;
    if (const char* skipReasonOrNull = StartPairedSessions(sessions); skipReasonOrNull != nullptr)
    {
        SKIP(skipReasonOrNull);
    }
    // Thanks to James F. Blinn, "Models of Light Reflection for Computer Synthesized Pictures", SIGGRAPH '77,
    // pp. 192-198, Fig. 5: hold the incident direction fixed and plot the reflected intensity against the view on both
    // sides of the mirror direction. Each capture measures f(view, light) cos(light) under a unit white directional
    // light; the lobe compares the intensity f cos(light) cos(view), which peaks inside the hemisphere.
    constexpr float32 ALPHA = 0.64F;
    constexpr usize VIEW_COUNT = 12;
    const StaticArray<float64, VIEW_COUNT> views{{-60, -30, 0, 20, 35, 45, 55, 65, 75, 82, 86, 89}};
    constexpr uint32 WALKS = 1U << 15;
    constexpr float64 PAIRED_TOLERANCE = 1e-4;
    // Measured on Metal: relative intensity error up to 0.049 and width error up to 4.8 degrees (single scattering:
    // 0.46 and 17 degrees). Normalized RGB stays within 0.0096 of the reference over the slice and 0.0026 at the mirror
    // view, where an equal-energy gray compensation is 0.0104 away.
    constexpr float64 RELATIVE_TOLERANCE = 0.07;
    constexpr float64 WIDTH_TOLERANCE_DEGREES = 6;
    constexpr float64 COLOR_TOLERANCE = 0.015;
    constexpr float64 MIRROR_COLOR_TOLERANCE = 0.005;
    OracleRandom random(0x51ab);
    float64 largestOffPeakError = 0;
    float64 largestWidthError = 0;
    float64 largestColorError = 0;
    float64 largestMirrorColorError = 0;
    float64 smallestGrayColorError = 1;
    for (const bool isGold : {true, false})
    {
        const StaticArray<float32, 3>& eta = isGold == true ? GOLD_ETA : ACHROMATIC_ETA;
        const StaticArray<float32, 3>& extinction = isGold == true ? GOLD_K : ACHROMATIC_K;
        UsePlane(sessions, MakeConductorPlane(ALPHA, eta, extinction));
        const MicrofacetRandomWalk walk(Widen(ALPHA), Widen(eta), Widen(extinction), false);
        for (const float64 incidentDegrees : {45.0, 75.0})
        {
            const math::Vector3 lightTangent = MakeTangentDirection(-incidentDegrees);
            StaticArray<float64, VIEW_COUNT> compensatedEnergy{};
            StaticArray<float64, VIEW_COUNT> singleEnergy{};
            StaticArray<float64, VIEW_COUNT> referenceEnergy{};
            for (usize view = 0; view < VIEW_COUNT; ++view)
            {
                const math::Vector3 viewTangent = MakeTangentDirection(views[view]);
                CAPTURE(isGold, incidentDegrees, views[view]);
                const DirectionalResponse compensated =
                    CaptureDirectionalResponse(sessions, viewTangent, lightTangent, MULTIPLE_SCATTERING);
                const DirectionalResponse single =
                    CaptureDirectionalResponse(sessions, viewTangent, lightTangent, SINGLE_SCATTERING);
                rr::ReferenceBsdfEvaluation evaluated;
                REQUIRE(rr::EvaluateReferenceBsdf(MakeConductorBsdf(ALPHA, eta, extinction, MULTIPLE_SCATTERING),
                                                  {0, 0, 1},
                                                  viewTangent,
                                                  lightTangent,
                                                  evaluated) == rhi::RasterStatus::Ready);
                const Double3 expected = Widen(evaluated.Value) * Widen(lightTangent.Z);
                const Double3 reference = walk.Evaluate(Widen(viewTangent), Widen(lightTangent), WALKS, random);
                CAPTURE(compensated.Cpu.X, compensated.Cpu.Y, compensated.Cpu.Z);
                CAPTURE(compensated.Gpu.X, compensated.Gpu.Y, compensated.Gpu.Z);
                CAPTURE(reference.X, reference.Y, reference.Z, single.Cpu.X, single.Cpu.Y, single.Cpu.Z);
                // The CPU capture shades with the same evaluation; the GPU capture repeats it in binary32.
                CHECK(std::fabs(SumChannels(Widen(compensated.Cpu)) - SumChannels(expected)) <=
                      PAIRED_TOLERANCE * (1 + SumChannels(expected)));
                for (const DirectionalResponse* response : {&compensated, &single})
                {
                    CHECK(std::fabs(Widen(response->Gpu.X) - Widen(response->Cpu.X)) <=
                          PAIRED_TOLERANCE * (1 + Widen(response->Cpu.X)));
                    CHECK(std::fabs(Widen(response->Gpu.Z) - Widen(response->Cpu.Z)) <=
                          PAIRED_TOLERANCE * (1 + Widen(response->Cpu.Z)));
                }
                const float64 viewCosine = Widen(viewTangent.Z);
                // Normalized RGB against the reference, and for an equal-energy achromatic compensation that adds the
                // mean of the compensated lobe's channels: the separate color tolerance detects the latter.
                const Double3 compensatedRgb = Widen(compensated.Gpu);
                const Double3 singleRgb = Widen(single.Gpu);
                const float64 addedMean = (SumChannels(compensatedRgb) - SumChannels(singleRgb)) / 3;
                const Double3 grayRgb = singleRgb + Double3{addedMean, addedMean, addedMean};
                const auto colorDistance = [&reference](Double3 rgb) noexcept {
                    const Double3 normalized = rgb * (1 / SumChannels(rgb));
                    const Double3 target = reference * (1 / SumChannels(reference));
                    return std::fmax(std::fabs(normalized.X - target.X),
                                     std::fmax(std::fabs(normalized.Y - target.Y), std::fabs(normalized.Z - target.Z)));
                };
                largestColorError = std::fmax(largestColorError, colorDistance(compensatedRgb));
                CHECK(colorDistance(compensatedRgb) <= COLOR_TOLERANCE);
                if (isGold == true && views[view] == incidentDegrees)
                {
                    smallestGrayColorError = std::fmin(smallestGrayColorError, colorDistance(grayRgb));
                    largestMirrorColorError = std::fmax(largestMirrorColorError, colorDistance(compensatedRgb));
                    CHECK(colorDistance(compensatedRgb) <= MIRROR_COLOR_TOLERANCE);
                    CHECK(colorDistance(grayRgb) > MIRROR_COLOR_TOLERANCE);
                }
                compensatedEnergy[view] = SumChannels(Widen(compensated.Gpu)) * viewCosine;
                singleEnergy[view] = SumChannels(Widen(single.Gpu)) * viewCosine;
                referenceEnergy[view] = SumChannels(reference) * viewCosine;
            }
            const LobeShape compensatedShape = MeasureLobe(views, compensatedEnergy);
            const LobeShape singleShape = MeasureLobe(views, singleEnergy);
            const LobeShape referenceShape = MeasureLobe(views, referenceEnergy);
            CAPTURE(compensatedShape.PeakDegrees, compensatedShape.WidthDegrees);
            CAPTURE(singleShape.PeakDegrees, singleShape.WidthDegrees);
            CAPTURE(referenceShape.PeakDegrees, referenceShape.WidthDegrees);
            CHECK(compensatedShape.PeakDegrees == referenceShape.PeakDegrees);
            const float64 widthError = std::fabs(compensatedShape.WidthDegrees - referenceShape.WidthDegrees);
            largestWidthError = std::fmax(largestWidthError, widthError);
            CHECK(widthError <= WIDTH_TOLERANCE_DEGREES);
            for (usize view = 0; view < VIEW_COUNT; ++view)
            {
                const float64 relative =
                    std::fabs(compensatedEnergy[view] - referenceEnergy[view]) / referenceEnergy[view];
                const float64 singleRelative =
                    std::fabs(singleEnergy[view] - referenceEnergy[view]) / referenceEnergy[view];
                CAPTURE(views[view], compensatedEnergy[view], singleEnergy[view], referenceEnergy[view]);
                largestOffPeakError = std::fmax(largestOffPeakError, relative);
                CHECK(relative <= RELATIVE_TOLERANCE);
                CHECK(relative <= singleRelative + 0.01);
            }
        }
    }
    WARN("Directional lobe: largest relative error "
         << largestOffPeakError << ", largest width error " << largestWidthError << " degrees, largest color error "
         << largestColorError << " (mirror " << largestMirrorColorError << "), gray compensation color error "
         << smallestGrayColorError);
}

TEST_CASE("RT1 multiple-scattering GGX keeps the grazing Fresnel color of a colored conductor on CPU and GPU",
          "[renderer][gpu][reference-compute][materials][multiple-scattering][oracle]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    PairedSessions sessions;
    if (const char* skipReasonOrNull = StartPairedSessions(sessions); skipReasonOrNull != nullptr)
    {
        SKIP(skipReasonOrNull);
    }
    // Thanks to Robert L. Cook and Kenneth E. Torrance, "A Reflectance Model for Computer Graphics", SIGGRAPH '81,
    // pp. 307-316: a colored metal's highlight under white light shifts toward the light's color at grazing angles,
    // as its Fresnel reflectance approaches one. Illumination and observation move together along the mirror
    // direction. Energy and normalized RGB have separate tolerances, so a compensation that matches total energy while
    // shifting the highlight's color fails.
    constexpr float32 ALPHA = 0.09F;
    constexpr uint32 WALKS = 1U << 15;
    // Measured on Metal: energy error up to 0.059 at 85 degrees, where the reference gains higher-order energy that the
    // tabulated lobe underestimates, and normalized RGB error up to 2.3e-4.
    constexpr float64 ENERGY_TOLERANCE = 0.08;
    constexpr float64 COLOR_TOLERANCE = 0.002;
    constexpr float64 PAIRED_TOLERANCE = 1e-4;
    OracleRandom random(0xc0101);
    float64 largestEnergyError = 0;
    float64 largestColorError = 0;
    for (const bool isGold : {true, false})
    {
        const StaticArray<float32, 3>& eta = isGold == true ? GOLD_ETA : ACHROMATIC_ETA;
        const StaticArray<float32, 3>& extinction = isGold == true ? GOLD_K : ACHROMATIC_K;
        UsePlane(sessions, MakeConductorPlane(ALPHA, eta, extinction));
        const MicrofacetRandomWalk walk(Widen(ALPHA), Widen(eta), Widen(extinction), false);
        float64 previousSpread = 1;
        for (const float64 degrees : {0.0, 30.0, 60.0, 75.0, 85.0, 89.0})
        {
            const math::Vector3 viewTangent = MakeTangentDirection(degrees);
            const math::Vector3 lightTangent = MakeTangentDirection(-degrees);
            CAPTURE(isGold, degrees);
            const DirectionalResponse compensated =
                CaptureDirectionalResponse(sessions, viewTangent, lightTangent, MULTIPLE_SCATTERING);
            const Double3 reference = walk.Evaluate(Widen(viewTangent), Widen(lightTangent), WALKS, random);
            // At the mirror configuration the half vector is the normal: the declared Fresnel model at cos(theta).
            const float64 cosine = std::cos(degrees * math::kPi / 180);
            const Double3 fresnel{
                ludus::renderer_tests::ComputeComplexFresnel(cosine, Widen(eta[0]), Widen(extinction[0])),
                ludus::renderer_tests::ComputeComplexFresnel(cosine, Widen(eta[1]), Widen(extinction[1])),
                ludus::renderer_tests::ComputeComplexFresnel(cosine, Widen(eta[2]), Widen(extinction[2]))};
            for (const math::Vector3 pixel : {compensated.Cpu, compensated.Gpu})
            {
                const Double3 raw = Widen(pixel);
                const Double3 normalized = raw * (1 / SumChannels(raw));
                const Double3 referenceNormalized = reference * (1 / SumChannels(reference));
                const Double3 fresnelNormalized = fresnel * (1 / SumChannels(fresnel));
                CAPTURE(raw.X, raw.Y, raw.Z, normalized.X, normalized.Y, normalized.Z);
                CAPTURE(reference.X,
                        reference.Y,
                        reference.Z,
                        referenceNormalized.X,
                        referenceNormalized.Y,
                        referenceNormalized.Z);
                CAPTURE(fresnelNormalized.X, fresnelNormalized.Y, fresnelNormalized.Z);
                const float64 energyError =
                    std::fabs(SumChannels(raw) - SumChannels(reference)) / SumChannels(reference);
                const float64 colorError = std::fmax(std::fabs(normalized.X - referenceNormalized.X),
                                                     std::fmax(std::fabs(normalized.Y - referenceNormalized.Y),
                                                               std::fabs(normalized.Z - referenceNormalized.Z)));
                const float64 fresnelColorError = std::fmax(std::fabs(normalized.X - fresnelNormalized.X),
                                                            std::fmax(std::fabs(normalized.Y - fresnelNormalized.Y),
                                                                      std::fabs(normalized.Z - fresnelNormalized.Z)));
                largestEnergyError = std::fmax(largestEnergyError, energyError);
                largestColorError = std::fmax(largestColorError, colorError);
                CHECK(energyError <= ENERGY_TOLERANCE);
                CHECK(colorError <= COLOR_TOLERANCE);
                CHECK(fresnelColorError <= COLOR_TOLERANCE);
                if (isGold == false)
                {
                    CHECK(std::fabs(normalized.X - 1.0 / 3) <= 1e-6);
                    CHECK(std::fabs(normalized.Z - 1.0 / 3) <= 1e-6);
                }
            }
            CHECK(std::fabs(Widen(compensated.Gpu.X) - Widen(compensated.Cpu.X)) <=
                  PAIRED_TOLERANCE * (1 + Widen(compensated.Cpu.X)));
            CHECK(std::fabs(Widen(compensated.Gpu.Z) - Widen(compensated.Cpu.Z)) <=
                  PAIRED_TOLERANCE * (1 + Widen(compensated.Cpu.Z)));
            // Gold's highlight whitens toward grazing: the spread of its normalized channels shrinks.
            const Double3 normalized = Widen(compensated.Gpu) * (1 / SumChannels(Widen(compensated.Gpu)));
            const float64 spread = std::fmax(normalized.X, std::fmax(normalized.Y, normalized.Z)) -
                                   std::fmin(normalized.X, std::fmin(normalized.Y, normalized.Z));
            if (isGold == true && degrees >= 60)
            {
                CHECK(spread < previousSpread);
            }
            previousSpread = spread;
        }
    }
    WARN("Grazing color: largest energy error " << largestEnergyError << ", largest normalized RGB error "
                                                << largestColorError);
}

TEST_CASE("RT1 multiple-scattering GGX matches between CPU and GPU on a rough-metal scene and records its cost",
          "[renderer][gpu][reference-compute][materials][multiple-scattering]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    PairedSessions sessions;
    if (const char* skipReasonOrNull = StartPairedSessions(sessions); skipReasonOrNull != nullptr)
    {
        SKIP(skipReasonOrNull);
    }
    rr::Snapshot spheres[2];
    REQUIRE(BuildSphereSnapshot(sessions.Device,
                                sessions.Cpu.Renderer,
                                sessions.Cpu.Program,
                                spheres[0],
                                true,
                                SphereSurfaces::RoughMetal) == S::Ready);
    REQUIRE(BuildSphereSnapshot(sessions.Device,
                                sessions.Gpu.Renderer,
                                sessions.Gpu.Program,
                                spheres[1],
                                true,
                                SphereSurfaces::RoughMetal) == S::Ready);
    REQUIRE(sessions.Cpu.Renderer.Release(sessions.Cpu.Snapshot) == S::Ready);
    REQUIRE(sessions.Gpu.Renderer.Release(sessions.Gpu.Snapshot) == S::Ready);
    sessions.Cpu.Snapshot = spheres[0];
    sessions.Gpu.Snapshot = spheres[1];
    rr::DirectLight sun;
    sun.SourceId = 7;
    sun.Direction = math::Vector3{-.3F, -.4F, 1} / math::Length(math::Vector3{-.3F, -.4F, 1});
    sun.Intensity = .8F;
    rr::ReferenceCaptureDescription capture = MakePerspectiveCapture({.3F, .2F, -2}, 32, 24);
    capture.TargetSamples = 16;
    capture.MaxDepth = 6;
    capture.Environment = {.2F, .25F, .3F};
    capture.Lights = &sun;
    capture.LightCount = 1;
    StaticArray<math::Vector3, 2> profileMeans{};
    for (const rr::ReferenceMaterialProfile profile : {SINGLE_SCATTERING, MULTIPLE_SCATTERING})
    {
        capture.MaterialProfile = profile;
        for (const uint64 seed : {uint64{99}, uint64{131}})
        {
            capture.Seed = seed;
            CAPTURE(static_cast<uint32>(profile), seed);
            Array<rr::ReferencePixel> oracle;
            Array<rr::ReferencePixel> compute;
            rr::ReferenceCaptureInfo oracleInfo;
            rr::ReferenceCaptureInfo computeInfo;
            const uint64 cpuStart = time::NowTicks();
            Capture(sessions.Cpu, capture, sessions.Surface, oracle, oracleInfo);
            const uint64 gpuStart = time::NowTicks();
            Capture(sessions.Gpu, capture, sessions.Surface, compute, computeInfo);
            const uint64 gpuEnd = time::NowTicks();
            const Comparison comparison = Compare(oracle, compute, capture.TargetSamples);
            CAPTURE(comparison.MaximumRadiance, math::Sqrt(comparison.SquaredRadiance));
            CHECK(oracleInfo.MaterialProfile == profile);
            CHECK(computeInfo.MaterialProfile == profile);
            CHECK(computeInfo.InvalidContributions == 0);
            CHECK(computeInfo.CommittedSamples == oracleInfo.CommittedSamples);
            CHECK(computeInfo.NullBsdfSamples == oracleInfo.NullBsdfSamples);
            CHECK(math::Sqrt(comparison.SquaredRadiance) <= 1e-3F);
            CHECK(math::Distance(comparison.CpuMean, comparison.GpuMean) <= 1e-3F * float32(oracle.GetSize()));
            profileMeans[profile == MULTIPLE_SCATTERING ? 1 : 0] =
                profileMeans[profile == MULTIPLE_SCATTERING ? 1 : 0] + comparison.CpuMean;
            WARN("Rough-metal scene, profile " << static_cast<uint32>(profile) << ", seed " << seed << ": CPU oracle "
                                               << (gpuStart - cpuStart) / NANOSECONDS_PER_MILLISECOND
                                               << " ms, GPU compute "
                                               << (gpuEnd - gpuStart) / NANOSECONDS_PER_MILLISECOND << " ms");
        }
    }
    // The compensated profile only adds reflected energy.
    CHECK(profileMeans[1].X > profileMeans[0].X);
    CHECK(profileMeans[1].Y > profileMeans[0].Y);
}

TEST_CASE("An emitter seen through smooth glass keeps the Fresnel-transmitted radiance",
          "[renderer][gpu][reference-compute][dielectric]")
{
    namespace format = ludus::content::scene_format;
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
    if (StartComputeDevice(device, surface) == false)
    {
        SKIP("No Metal adapter");
    }
    Session gpu;
    const S enabled = Prepare(device, rr::ReferenceTraversal::GpuCompute, gpu);
    if (enabled == S::Unsupported)
    {
        SKIP("Compute storage is unavailable on this device");
    }
    REQUIRE(enabled == S::Ready);
    Session cpu;
    REQUIRE(Prepare(device, rr::ReferenceTraversal::CpuOracle, cpu) == S::Ready);
    // A one-sided white emitter behind glass, seen along the glass normal by orthographic camera rays, with a black
    // environment: every path either reaches the emitter (contributing exactly one) or leaves the scene.
    QuadItem emitter;
    emitter.CornersWorld = {{{-2, -2, 1}, {-2, 2, 1}, {2, 2, 1}, {2, -2, 1}}};
    emitter.Emission = {1, 1, 1};
    emitter.IsEmissionTwoSided = false;
    emitter.BaseColor = {};
    emitter.Model = format::MaterialModel::Diffuse;
    QuadItem front;
    front.CornersWorld = {{{-1, -1, -.1F}, {-1, 1, -.1F}, {1, 1, -.1F}, {1, -1, -.1F}}};
    front.Model = format::MaterialModel::Dielectric;
    QuadItem back;
    back.CornersWorld = {{{-1, -1, .1F}, {1, -1, .1F}, {1, 1, .1F}, {-1, 1, .1F}}};
    back.Model = format::MaterialModel::Dielectric;
    QuadItem pane = front;
    pane.Model = format::MaterialModel::ThinDielectric;
    // Normal-incidence Fresnel reflectance ((eta - 1) / (eta + 1))^2 = 0.04 of index 1.5.
    const float64 reflectance = 0.04;
    const float64 transmittance = 1 - reflectance;
    constexpr uint32 MAX_DEPTH = 6;
    // The slab passes paths with 2, 4 and 6 events: two refractions around k internal reflection pairs. Radiance
    // scales by eta^-2 entering and eta^2 leaving, so each reaching path carries exactly one.
    const float64 slab = transmittance * transmittance *
                         (1 + reflectance * reflectance + reflectance * reflectance * reflectance * reflectance);
    // A thin slab sums all its interreflections: T' = 1 - (R + T^2 R / (1 - R^2)) = (1 - R) / (1 + R).
    const float64 thin = transmittance / (1 + reflectance);
    // Orthographic rays along +z start on the near plane z = -0.7; every pixel is an independent normal-incidence
    // trial.
    constexpr uint32 IMAGE_EXTENT = 16;
    rr::ReferenceCaptureDescription capture;
    capture.Width = IMAGE_EXTENT;
    capture.Height = IMAGE_EXTENT;
    capture.OrthographicCamera = true;
    capture.WorldToClip.Columns[3].Z = .7F;
    capture.CenterRays = true;
    capture.TargetSamples = 64;
    capture.MaxDepth = MAX_DEPTH;
    capture.RouletteDepth = 64;
    capture.Seed = 23;
    for (const bool isThin : {false, true})
    {
        const QuadItem slabQuads[]{emitter, front, back};
        const QuadItem thinQuads[]{emitter, pane};
        const float64 expected = isThin == true ? thin : slab;
        // Each sample is a Bernoulli trial; allow five standard errors of the image mean.
        const float64 trials = static_cast<float64>(capture.TargetSamples) * IMAGE_EXTENT * IMAGE_EXTENT;
        const float64 tolerance = 5 * math::Sqrt(expected * (1 - expected) / trials);
        for (Session* session : {&cpu, &gpu})
        {
            REQUIRE(session->Renderer.Release(session->Snapshot) == S::Ready);
            REQUIRE(BuildQuadSnapshot(session->Renderer,
                                      session->Program,
                                      isThin == true ? thinQuads : slabQuads,
                                      isThin == true ? 2 : 3,
                                      session->Snapshot) == S::Ready);
            Array<rr::ReferencePixel> pixels;
            rr::ReferenceCaptureInfo info;
            Capture(*session, capture, surface, pixels, info);
            float64 mean = 0;
            for (const rr::ReferencePixel& pixel : pixels)
            {
                mean += static_cast<float64>(pixel.Radiance.X) / static_cast<float64>(pixels.GetSize());
                CHECK(pixel.Radiance.X == pixel.Radiance.Z);
            }
            CAPTURE(isThin, static_cast<uint32>(info.Traversal), mean, expected, tolerance);
            CHECK(info.InvalidContributions == 0);
            // Specular vertices skip next-event estimation; the emitter is reached only through the glass.
            CHECK(info.VisibilityQueries == 0);
            CHECK(math::Abs(mean - expected) <= tolerance);
        }
    }
}

namespace
{
using BoxFaces = StaticArray<QuadCorners, BOX_FACE_COUNT>;

/// Counterclockwise about each outward normal.
BoxFaces MakeBoxFaces(math::Vector3 minimum, math::Vector3 maximum)
{
    const float32 x0 = minimum.X;
    const float32 y0 = minimum.Y;
    const float32 z0 = minimum.Z;
    const float32 x1 = maximum.X;
    const float32 y1 = maximum.Y;
    const float32 z1 = maximum.Z;
    return {{
        {{{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}}},
        {{{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}}},
        {{{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}}},
        {{{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}},
        {{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}}},
        {{{x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}}},
    }};
}
/// Append the twelve outward-wound triangles of the axis-aligned box [minimum, maximum]. Each face is a quad
/// c0..c3 with outward normal cross(c1 - c0, c3 - c0), split along its c0-c2 diagonal like BuildQuadSnapshot.
void AppendMediumBox(math::Vector3 minimum, math::Vector3 maximum, Array<math::Vector3>& inoutCorners)
{
    for (const QuadCorners& face : MakeBoxFaces(minimum, maximum))
    {
        for (const usize corner : {0, 1, 2, 0, 2, 3})
        {
            inoutCorners.Add(face[corner]);
        }
    }
}
rr::ReferenceMediumDescription MakeMedium(const Array<math::Vector3>& corners, math::Vector3 absorptionPerWorld)
{
    rr::ReferenceMediumDescription medium;
    medium.BoundaryCornersWorld = {corners.GetData(), corners.GetSize()};
    medium.AbsorptionPerWorld = absorptionPerWorld;
    return medium;
}
/// Orthographic center rays along +z from the near plane z = -3 to the far plane z = 3, one per pixel of a
/// 16 x 16 image covering x, y in [-1, 1]. No pixel centre lies on x or y = +/-0.05, 0.5 or 0.75.
constexpr uint32 MEDIUM_IMAGE_EXTENT = 16;
constexpr float32 MEDIUM_NEAR_WORLD = -3;
rr::ReferenceCaptureDescription MakeMediumCapture()
{
    rr::ReferenceCaptureDescription capture;
    capture.Width = MEDIUM_IMAGE_EXTENT;
    capture.Height = MEDIUM_IMAGE_EXTENT;
    capture.OrthographicCamera = true;
    capture.WorldToClip.Columns[2].Z = 1.F / 6;
    capture.WorldToClip.Columns[3].Z = .5F;
    capture.CenterRays = true;
    capture.TargetSamples = 2;
    capture.MaxDepth = 1;
    capture.RouletteDepth = 64;
    capture.Seed = 1369;
    return capture;
}
/// World x and y of a pixel centre of MakeMediumCapture.
math::Vector2 GetMediumPixelCenter(usize pixel)
{
    const float64 column = static_cast<float64>(pixel % MEDIUM_IMAGE_EXTENT) + .5;
    const usize rowIndex = pixel / MEDIUM_IMAGE_EXTENT;
    const float64 row = static_cast<float64>(rowIndex) + .5;
    return {static_cast<float32>(2 * column / MEDIUM_IMAGE_EXTENT - 1),
            static_cast<float32>(1 - 2 * row / MEDIUM_IMAGE_EXTENT)};
}
float64 GetBeerLambert(float32 absorptionPerWorld, float64 insideWorld)
{
    return math::Exp(-static_cast<float64>(absorptionPerWorld) * insideWorld);
}
/// Check one channel against an analytic value within a relative float32 rounding allowance.
void CheckAnalytic(float32 actual, float64 expected)
{
    constexpr float64 RELATIVE_TOLERANCE = 1e-4;
    constexpr float64 ABSOLUTE_TOLERANCE = 1e-7;
    CAPTURE(actual, expected);
    CHECK(math::Abs(static_cast<float64>(actual) - expected) <= RELATIVE_TOLERANCE * expected + ABSOLUTE_TOLERANCE);
}
struct MediumDevice final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    Session Cpu;
    Session Gpu;
    bool HasGpu = false;
};
/// Start one device with a CPU-oracle session and, where compute storage exists, a GPU-compute session.
bool StartMediumDevice(MediumDevice& outDevice)
{
    if (StartComputeDevice(outDevice.Device, outDevice.Surface) == false)
    {
        return false;
    }
    const S enabled = Prepare(outDevice.Device, rr::ReferenceTraversal::GpuCompute, outDevice.Gpu);
    REQUIRE((enabled == S::Ready || enabled == S::Unsupported));
    outDevice.HasGpu = enabled == S::Ready;
    if (outDevice.HasGpu == false)
    {
        WARN("GPU compute storage is unavailable; checking the CPU oracle only");
    }
    REQUIRE(Prepare(outDevice.Device, rr::ReferenceTraversal::CpuOracle, outDevice.Cpu) == S::Ready);
    return true;
}
/// Rebuild every session's snapshot from the same quads.
void BuildMediumScene(MediumDevice& inoutDevice, const QuadItem* quads, usize count)
{
    for (Session* session : {&inoutDevice.Cpu, &inoutDevice.Gpu})
    {
        if (session == &inoutDevice.Gpu && inoutDevice.HasGpu == false)
        {
            continue;
        }
        REQUIRE(session->Renderer.Release(session->Snapshot) == S::Ready);
        REQUIRE(BuildQuadSnapshot(session->Renderer, session->Program, quads, count, session->Snapshot) == S::Ready);
    }
}
/// Capture with both implementations, check matched counts and per-pixel parity, and return both images.
void CaptureMediumPair(MediumDevice& inoutDevice,
                       const rr::ReferenceCaptureDescription& capture,
                       Array<rr::ReferencePixel>& outCpu,
                       Array<rr::ReferencePixel>& outGpu)
{
    // Float32 crossing distances and exp differ by rounding only; matched samples follow matched paths.
    constexpr float32 PARITY_TOLERANCE = 1e-5F;
    rr::ReferenceCaptureInfo cpuInfo;
    Capture(inoutDevice.Cpu, capture, inoutDevice.Surface, outCpu, cpuInfo);
    CHECK(cpuInfo.InvalidContributions == 0);
    if (inoutDevice.HasGpu == false)
    {
        return;
    }
    rr::ReferenceCaptureInfo gpuInfo;
    Capture(inoutDevice.Gpu, capture, inoutDevice.Surface, outGpu, gpuInfo);
    CHECK(gpuInfo.InvalidContributions == 0);
    CHECK(gpuInfo.ClosestQueries == cpuInfo.ClosestQueries);
    CHECK(gpuInfo.VisibilityQueries == cpuInfo.VisibilityQueries);
    CHECK(gpuInfo.SceneFingerprintLow == cpuInfo.SceneFingerprintLow);
    const Comparison comparison = Compare(outCpu, outGpu, capture.TargetSamples);
    CAPTURE(comparison.MaximumRadiance, comparison.MaximumEmission, comparison.MaximumDirect);
    CHECK(comparison.MaximumRadiance <= PARITY_TOLERANCE);
}
QuadItem MakeMediumEmitter()
{
    // One-sided toward the camera, reflecting nothing.
    QuadItem emitter;
    emitter.CornersWorld = {{{-2, -2, 2.5F}, {-2, 2, 2.5F}, {2, 2, 2.5F}, {2, -2, 2.5F}}};
    emitter.Emission = {2, 3, 4};
    emitter.IsEmissionTwoSided = false;
    emitter.BaseColor = {};
    emitter.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    return emitter;
}
} // namespace

TEST_CASE("A bounded absorbing medium attenuates camera rays by Beer-Lambert transmittance on CPU and GPU",
          "[renderer][gpu][reference-compute][medium]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    MediumDevice devices;
    if (StartMediumDevice(devices) == false)
    {
        SKIP("No Metal adapter");
    }
    const QuadItem emitter = MakeMediumEmitter();
    BuildMediumScene(devices, &emitter, 1);
    const math::Vector3 absorption{.5F, 1, 2};
    // Two disjoint boxes form one medium: unit thickness at negative x and a quarter at positive x. Their face
    // diagonals pass through pixel centres, which the crossing tie rule must count once.
    constexpr float32 THICK_WORLD = 1;
    constexpr float32 THIN_WORLD = .25F;
    Array<math::Vector3> corners;
    AppendMediumBox({-.75F, -.5F, -.5F}, {-.05F, .5F, .5F}, corners);
    AppendMediumBox({.05F, -.5F, 0}, {.75F, .5F, THIN_WORLD}, corners);
    const rr::ReferenceMediumDescription medium = MakeMedium(corners, absorption);
    rr::ReferenceCaptureDescription capture = MakeMediumCapture();
    capture.MediumOrNull = &medium;
    Array<rr::ReferencePixel> cpu;
    Array<rr::ReferencePixel> gpu;
    CaptureMediumPair(devices, capture, cpu, gpu);
    for (const Array<rr::ReferencePixel>* pixels : {&cpu, &gpu})
    {
        if (pixels->GetSize() == 0)
        {
            continue;
        }
        for (usize pixel = 0; pixel < pixels->GetSize(); ++pixel)
        {
            const math::Vector2 center = GetMediumPixelCenter(pixel);
            const bool isInsideFootprintY = math::Abs(center.Y) < .5F;
            float64 inside = 0;
            if (isInsideFootprintY == true && center.X > -.75F && center.X < -.05F)
            {
                inside = Widen(THICK_WORLD);
            }
            else if (isInsideFootprintY == true && center.X > .05F && center.X < .75F)
            {
                inside = Widen(THIN_WORLD);
            }
            CAPTURE(pixel, center.X, center.Y, inside);
            const rr::ReferencePixel& value = (*pixels)[pixel];
            CheckAnalytic(value.Emission.X, Widen(emitter.Emission.X) * GetBeerLambert(absorption.X, inside));
            CheckAnalytic(value.Emission.Y, Widen(emitter.Emission.Y) * GetBeerLambert(absorption.Y, inside));
            CheckAnalytic(value.Emission.Z, Widen(emitter.Emission.Z) * GetBeerLambert(absorption.Z, inside));
            CHECK(value.Direct == math::Vector3{});
        }
    }
    // A vacuum medium (zero absorption) and no medium at all keep the unattenuated pixels bit for bit. Without a
    // medium the GPU kernel must also ignore the preview tile words that share the medium's parameters.
    Array<rr::ReferencePixel> vacuumCpu;
    Array<rr::ReferencePixel> vacuumGpu;
    const rr::ReferenceMediumDescription clear = MakeMedium(corners, {});
    capture.MediumOrNull = &clear;
    CaptureMediumPair(devices, capture, vacuumCpu, vacuumGpu);
    Array<rr::ReferencePixel> noneCpu;
    Array<rr::ReferencePixel> noneGpu;
    capture.MediumOrNull = nullptr;
    CaptureMediumPair(devices, capture, noneCpu, noneGpu);
    for (usize pixel = 0; pixel < noneCpu.GetSize(); ++pixel)
    {
        CHECK(vacuumCpu[pixel].Radiance == noneCpu[pixel].Radiance);
        CHECK(noneCpu[pixel].Emission == emitter.Emission);
    }
    for (usize pixel = 0; pixel < noneGpu.GetSize(); ++pixel)
    {
        CHECK(vacuumGpu[pixel].Radiance == noneGpu[pixel].Radiance);
        CHECK(noneGpu[pixel].Emission == emitter.Emission);
    }
}

TEST_CASE("Camera origins inside an absorbing medium follow its boundary crossings on CPU and GPU",
          "[renderer][gpu][reference-compute][medium]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    MediumDevice devices;
    if (StartMediumDevice(devices) == false)
    {
        SKIP("No Metal adapter");
    }
    const QuadItem emitter = MakeMediumEmitter();
    BuildMediumScene(devices, &emitter, 1);
    const math::Vector3 absorption{.1F, .2F, .4F};
    // The near plane z = -3 lies inside; every ray leaves through z = 0 after three metres.
    Array<math::Vector3> corners;
    AppendMediumBox({-2, -2, -4}, {2, 2, 0}, corners);
    const rr::ReferenceMediumDescription medium = MakeMedium(corners, absorption);
    rr::ReferenceCaptureDescription capture = MakeMediumCapture();
    capture.MediumOrNull = &medium;
    Array<rr::ReferencePixel> cpu;
    Array<rr::ReferencePixel> gpu;
    CaptureMediumPair(devices, capture, cpu, gpu);
    const float64 inside = -Widen(MEDIUM_NEAR_WORLD);
    for (const Array<rr::ReferencePixel>* pixels : {&cpu, &gpu})
    {
        for (const rr::ReferencePixel& value : *pixels)
        {
            CheckAnalytic(value.Emission.X, Widen(emitter.Emission.X) * GetBeerLambert(absorption.X, inside));
            CheckAnalytic(value.Emission.Z, Widen(emitter.Emission.Z) * GetBeerLambert(absorption.Z, inside));
        }
    }
    // A jittered pinhole inside the medium: each ray stays inside for 3 / cos(theta) metres before z = 0.
    capture.OrthographicCamera = false;
    capture.CenterRays = false;
    capture.TargetSamples = 16;
    math::Matrix4 view;
    math::Matrix4 projection;
    capture.CameraPosition = {0, 0, MEDIUM_NEAR_WORLD};
    REQUIRE(math::TryLookAt(capture.CameraPosition, {0, 0, 0}, {0, 1, 0}, view) == math::MathStatus::Success);
    constexpr float32 VERTICAL_FIELD_RADIANS = .5F;
    REQUIRE(math::TryPerspectiveReverseZ(VERTICAL_FIELD_RADIANS, 1, .1F, 20, projection) == math::MathStatus::Success);
    capture.WorldToClip = projection * view;
    capture.ReverseZ = true;
    CaptureMediumPair(devices, capture, cpu, gpu);
    // The image corners subtend at most this cosine with the optical axis.
    const float64 halfAngle = Widen(VERTICAL_FIELD_RADIANS) / 2;
    const float64 halfTangent = math::Sin(halfAngle) / math::Cos(halfAngle);
    const float64 minimumCosine = 1 / math::Sqrt(1 + 2 * halfTangent * halfTangent);
    for (const rr::ReferencePixel& value : cpu)
    {
        CHECK(Widen(value.Emission.X) <= Widen(emitter.Emission.X) * GetBeerLambert(absorption.X, inside) * (1 + 1e-5));
        CHECK(Widen(value.Emission.X) >=
              Widen(emitter.Emission.X) * GetBeerLambert(absorption.X, inside / minimumCosine));
    }
}

TEST_CASE("Light paths through an absorbing medium attenuate next-event estimation unless an opaque blocker occludes",
          "[renderer][gpu][reference-compute][medium]")
{
    namespace format = ludus::content::scene_format;
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    MediumDevice devices;
    if (StartMediumDevice(devices) == false)
    {
        SKIP("No Metal adapter");
    }
    // A white Lambertian wall at z = 1 faces the camera; a point light at z = -1 lights it through a slab
    // medium at z in [0, 0.5], which the camera rays also cross.
    QuadItem wall;
    wall.CornersWorld = {{{-2, -2, 1}, {-2, 2, 1}, {2, 2, 1}, {2, -2, 1}}};
    wall.BaseColor = {.8F, .6F, .4F};
    wall.Model = format::MaterialModel::Diffuse;
    // A black square at z = -0.5 of half size 0.1 shadows the wall within half size 0.4.
    QuadItem blocker;
    blocker.CornersWorld = {{{-.1F, -.1F, -.5F}, {-.1F, .1F, -.5F}, {.1F, .1F, -.5F}, {.1F, -.1F, -.5F}}};
    blocker.BaseColor = {};
    blocker.Model = format::MaterialModel::Diffuse;
    rr::DirectLight light;
    light.SourceId = 1;
    light.Kind = rr::LightKind::Point;
    light.Intensity = 2;
    light.Position = {0, 0, -1};
    light.Range = 1000000;
    const math::Vector3 absorption{.5F, 1, 2};
    constexpr float32 SLAB_WORLD = .5F;
    Array<math::Vector3> corners;
    AppendMediumBox({-2, -2, 0}, {2, 2, SLAB_WORLD}, corners);
    rr::ReferenceCaptureDescription capture = MakeMediumCapture();
    capture.Lights = &light;
    capture.LightCount = 1;
    for (const bool hasBlocker : {false, true})
    {
        const QuadItem quads[]{wall, blocker};
        BuildMediumScene(devices, quads, hasBlocker == true ? 2 : 1);
        for (const bool hasAbsorption : {false, true})
        {
            CAPTURE(hasBlocker, hasAbsorption);
            const math::Vector3 sigma = hasAbsorption == true ? absorption : math::Vector3{};
            const rr::ReferenceMediumDescription medium = MakeMedium(corners, sigma);
            capture.MediumOrNull = &medium;
            Array<rr::ReferencePixel> cpu;
            Array<rr::ReferencePixel> gpu;
            CaptureMediumPair(devices, capture, cpu, gpu);
            for (const Array<rr::ReferencePixel>* pixels : {&cpu, &gpu})
            {
                for (usize pixel = 0; pixel < pixels->GetSize(); ++pixel)
                {
                    const math::Vector2 center = GetMediumPixelCenter(pixel);
                    const float32 extent = math::Max(math::Abs(center.X), math::Abs(center.Y));
                    if (hasBlocker == true && extent < .1F)
                    {
                        continue; // The camera sees the black blocker.
                    }
                    const rr::ReferencePixel& value = (*pixels)[pixel];
                    CAPTURE(pixel, center.X, center.Y);
                    if (hasBlocker == true && extent < .4F)
                    {
                        CHECK(value.Direct == math::Vector3{});
                        continue;
                    }
                    const float64 dx = Widen(center.X);
                    const float64 dy = Widen(center.Y);
                    const float64 distance = math::Sqrt(dx * dx + dy * dy + 4);
                    const float64 cosine = 2 / distance;
                    // The camera crosses the slab normally, the light ray obliquely.
                    const float64 lightInside = Widen(SLAB_WORLD) * distance / 2;
                    const float64 irradiance = Widen(light.Intensity) * cosine / (distance * distance) / math::kPi;
                    const StaticArray<float32, 3> albedo{{wall.BaseColor.X, wall.BaseColor.Y, wall.BaseColor.Z}};
                    const StaticArray<float32, 3> sigmas{{sigma.X, sigma.Y, sigma.Z}};
                    const StaticArray<float32, 3> actual{{value.Direct.X, value.Direct.Y, value.Direct.Z}};
                    for (usize channel = 0; channel < 3; ++channel)
                    {
                        CheckAnalytic(actual[channel],
                                      Widen(albedo[channel]) * irradiance *
                                          GetBeerLambert(sigmas[channel], Widen(SLAB_WORLD)) *
                                          GetBeerLambert(sigmas[channel], lightInside));
                    }
                }
            }
        }
    }
}

TEST_CASE("Reference captures admit only closed bounded media on the supported traversals",
          "[renderer][gpu][reference-compute][medium]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    MediumDevice devices;
    if (StartMediumDevice(devices) == false)
    {
        SKIP("No Metal adapter");
    }
    const QuadItem emitter = MakeMediumEmitter();
    BuildMediumScene(devices, &emitter, 1);
    Session& session = devices.Cpu;
    Array<math::Vector3> box;
    AppendMediumBox({-.5F, -.5F, -.5F}, {.5F, .5F, .5F}, box);
    rr::ReferenceCaptureDescription capture = MakeMediumCapture();
    const auto begin = [&](const rr::ReferenceMediumDescription& medium) {
        capture.MediumOrNull = &medium;
        rr::ReferenceCapture handle;
        const S status = session.Renderer.BeginReferenceCapture(session.Snapshot, capture, handle);
        if (status == S::Ready || status == S::Pending)
        {
            REQUIRE(session.Renderer.Release(handle) == S::Ready);
        }
        return status;
    };
    const S admitted = begin(MakeMedium(box, {1, 1, 1}));
    CHECK((admitted == S::Ready || admitted == S::Pending));
    // Open: one missing triangle leaves an unmatched edge.
    Array<math::Vector3> open;
    for (usize corner = 3; corner < box.GetSize(); ++corner)
    {
        open.Add(box[corner]);
    }
    CHECK(begin(MakeMedium(open, {1, 1, 1})) == S::InvalidDescription);
    // Inconsistent orientation: one triangle wound against its neighbours.
    Array<math::Vector3> flipped;
    for (const math::Vector3 corner : box)
    {
        flipped.Add(corner);
    }
    const math::Vector3 swapped = flipped[1];
    flipped[1] = flipped[2];
    flipped[2] = swapped;
    CHECK(begin(MakeMedium(flipped, {1, 1, 1})) == S::InvalidDescription);
    // Absorption must be finite, nonnegative and bounded.
    CHECK(begin(MakeMedium(box, {-1, 0, 0})) == S::InvalidDescription);
    CHECK(begin(MakeMedium(box, {__builtin_nanf(""), 0, 0})) == S::InvalidDescription);
    CHECK(begin(MakeMedium(box, {ludus::content::scene_format::MAX_MEDIUM_ABSORPTION_PER_WORLD * 2, 0, 0})) ==
          S::InvalidDescription);
    rr::ReferenceMediumDescription empty = MakeMedium(box, {1, 1, 1});
    empty.BoundaryCornersWorld = {};
    CHECK(begin(empty) == S::InvalidDescription);
    empty.BoundaryCornersWorld = {nullptr, box.GetSize()};
    CHECK(begin(empty) == S::InvalidDescription);
    // A partial triangle and too many triangles.
    empty.BoundaryCornersWorld = {box.GetData(), box.GetSize() - 1};
    CHECK(begin(empty) == S::InvalidDescription);
    Array<math::Vector3> many;
    while (many.GetSize() / rr::REFERENCE_MEDIUM_TRIANGLE_CORNERS <= rr::MAX_REFERENCE_MEDIUM_TRIANGLES)
    {
        AppendMediumBox({-.5F, -.5F, -.5F}, {.5F, .5F, .5F}, many);
    }
    CHECK(begin(MakeMedium(many, {1, 1, 1})) == S::InvalidDescription);
    // Wave connections have no medium transport and report it rather than ignoring the medium.
    capture.OpticalWavelengthWorld = 5e-7F;
    CHECK(begin(MakeMedium(box, {1, 1, 1})) == S::Unsupported);
    capture.OpticalWavelengthWorld = 0;
    // Edge diffraction operators are optical connection operators: without a wavelength they are rejected, not
    // ignored.
    capture.OpticalConnection = rr::ReferenceOpticalConnection::UtdEdgeDiffraction;
    CHECK(begin(MakeMedium(box, {1, 1, 1})) == S::InvalidDescription);
    capture.OpticalConnection = rr::ReferenceOpticalConnection::FraunhoferEdgeDiffraction;
    CHECK(begin(MakeMedium(box, {1, 1, 1})) == S::InvalidDescription);
    capture.OpticalConnection = rr::ReferenceOpticalConnection::AngularSpectrum;
    // The visible spectrum scales a declared optical wavelength: without one it is rejected, not ignored.
    capture.OpticalSpectrum = rr::ReferenceOpticalSpectrum::Visible;
    CHECK(begin(MakeMedium(box, {1, 1, 1})) == S::InvalidDescription);
    capture.OpticalSpectrum = rr::ReferenceOpticalSpectrum::Monochromatic;
    // Changing the medium replaces the integral.
    const rr::ReferenceMediumDescription first = MakeMedium(box, {1, 1, 1});
    const rr::ReferenceMediumDescription second = MakeMedium(box, {2, 1, 1});
    capture.MediumOrNull = &first;
    rr::ReferenceCapture handle;
    const S begun = session.Renderer.BeginReferenceCapture(session.Snapshot, capture, handle);
    REQUIRE((begun == S::Ready || begun == S::Pending));
    rr::ReferenceCaptureInfo info;
    const S same = session.Renderer.UpdateReferenceCapture(handle, session.Snapshot, capture);
    CHECK((same == S::Ready || same == S::Pending));
    REQUIRE(session.Renderer.GetReferenceCaptureInfo(handle, info) == S::Ready);
    CHECK(info.Revision == 1);
    capture.MediumOrNull = &second;
    const S updated = session.Renderer.UpdateReferenceCapture(handle, session.Snapshot, capture);
    CHECK((updated == S::Ready || updated == S::Pending));
    REQUIRE(session.Renderer.GetReferenceCaptureInfo(handle, info) == S::Ready);
    CHECK(info.Revision == 2);
    REQUIRE(session.Renderer.Release(handle) == S::Ready);
    // Two nested closed boxes overlap: the camera inside both counts two and fails explicitly.
    Array<math::Vector3> nested;
    AppendMediumBox({-2, -2, -4}, {2, 2, 0}, nested);
    AppendMediumBox({-1, -1, -3.5F}, {1, 1, -.5F}, nested);
    const rr::ReferenceMediumDescription overlapping = MakeMedium(nested, {1, 1, 1});
    capture.MediumOrNull = &overlapping;
    for (Session* traversal : {&devices.Cpu, &devices.Gpu})
    {
        if (traversal == &devices.Gpu && devices.HasGpu == false)
        {
            continue;
        }
        const S started = traversal->Renderer.BeginReferenceCapture(traversal->Snapshot, capture, handle);
        REQUIRE((started == S::Ready || started == S::Pending));
        CHECK(Run(traversal->Renderer, handle, devices.Surface) == S::Failed);
        REQUIRE(traversal->Renderer.GetReferenceCaptureInfo(handle, info) == S::Ready);
        CHECK(info.Failure == rr::ReferenceFailure::InvalidMediumCrossing);
        CHECK(info.CommittedSamples == 0);
        REQUIRE(traversal->Renderer.Release(handle) == S::Ready);
    }
}

namespace
{
constexpr math::Vector3 ENCLOSURE_EMISSION{1, 0.5F, 0.25F};
constexpr SeedRadiances ZERO_RADIANCE{};

void BuildDiffuseEnclosure(PairedSessions& inoutSessions, float32 reflectance)
{
    const BoxFaces faces = MakeBoxFaces({-1, -1, -1}, {1, 1, 1});
    StaticArray<QuadItem, BOX_FACE_COUNT> walls{};
    for (usize i = 0; i < walls.GetSize(); ++i)
    {
        QuadItem& wall = walls[i];
        const QuadCorners& face = faces[i];
        wall.CornersWorld = {{face[0], face[3], face[2], face[1]}};
        wall.Emission = ENCLOSURE_EMISSION;
        wall.IsEmissionTwoSided = false;
        wall.BaseColor = {reflectance, reflectance, reflectance};
        wall.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    }
    for (Session* session : {&inoutSessions.Cpu, &inoutSessions.Gpu})
    {
        REQUIRE(session->Renderer.Release(session->Snapshot) == S::Ready);
        REQUIRE(BuildQuadSnapshot(session->Renderer,
                                  session->Program,
                                  walls.GetData(),
                                  walls.GetSize(),
                                  session->Snapshot) == S::Ready);
    }
}

// Thanks to James T. Kajiya, "The Rendering Equation", SIGGRAPH '86, section 3,
// https://doi.org/10.1145/15922.15902. Every inward direction sees the same radiance
// in this enclosure. Integrating rho/pi against cosine adds exactly rho times the previous order.
float64 SumEnclosureOrders(float64 reflectance, uint32 depth)
{
    float64 sum = 1;
    float64 term = 1;
    for (uint32 order = 1; order <= depth; ++order)
    {
        term *= reflectance;
        sum += term;
    }
    return sum;
}

struct EnclosureEstimate final
{
    float64 Mean;
    float64 StandardError;
};

// Pair the observations by seed before estimating uncertainty, including for depth differences.
// Thanks to Arvo, Torrance and Smits, "A Framework for the Analysis of Error in Global Illumination
// Algorithms", SIGGRAPH '94, pp. 75-84, https://doi.org/10.1145/192161.192179, for comparing distinct errors
// to a theoretical solution.
EnclosureEstimate
MeasureEnclosureChannel(const SeedRadiances& values, const SeedRadiances& baseline, float32 math::Vector3::* channel)
{
    const float64 emission = Widen(ENCLOSURE_EMISSION.*channel);
    StaticArray<float64, REFERENCE_SEED_COUNT> observations{};
    float64 mean = 0;
    for (usize seed = 0; seed < observations.GetSize(); ++seed)
    {
        observations[seed] = (Widen(values[seed].*channel) - Widen(baseline[seed].*channel)) / emission;
        mean += observations[seed] / REFERENCE_SEED_COUNT;
    }
    float64 squaredDeviations = 0;
    for (const float64 observation : observations)
    {
        squaredDeviations += (observation - mean) * (observation - mean);
    }
    return {mean, math::Sqrt(squaredDeviations / (REFERENCE_SEED_COUNT * (REFERENCE_SEED_COUNT - 1)))};
}

void CheckEnclosureEstimate(const SeedRadiances& values,
                            const SeedRadiances& baseline,
                            const char* quantity,
                            float64 expectedInEmissionUnits)
{
    for (float32 math::Vector3::* channel : {&math::Vector3::X, &math::Vector3::Y, &math::Vector3::Z})
    {
        const EnclosureEstimate estimate = MeasureEnclosureChannel(values, baseline, channel);
        // Even the smallest fourth-order term is 0.5^4. Cap the tolerance to resolve a missing order.
        constexpr float64 MAXIMUM_ERROR = 0.03;
        constexpr float64 NUMERICAL_FLOOR = 0.0002;
        const float64 tolerance = math::Min(MAXIMUM_ERROR, NUMERICAL_FLOOR + 6 * estimate.StandardError);
        CAPTURE(quantity, ENCLOSURE_EMISSION.*channel, expectedInEmissionUnits, estimate.Mean, estimate.StandardError);
        CHECK(math::Abs(estimate.Mean - expectedInEmissionUnits) <= tolerance);
    }
}

SeedMeans CaptureEnclosureSeeds(Session& inoutSession,
                                rhi::SurfaceHandle surface,
                                rr::ReferenceCaptureDescription capture,
                                rr::ReferenceTraversal traversal)
{
    SeedMeans result;
    for (usize seed = 0; seed < REFERENCE_SEED_COUNT; ++seed)
    {
        capture.Seed = 40 + seed;
        CAPTURE(capture.Seed, traversal);
        rr::ReferenceCaptureInfo info;
        Array<rr::ReferencePixel> pixels;
        Capture(inoutSession, capture, surface, pixels, info);
        REQUIRE(info.Traversal == traversal);
        REQUIRE(info.InvalidContributions == 0);
        REQUIRE(info.MinimumSamples == capture.TargetSamples);
        REQUIRE(info.MaximumSamples == capture.TargetSamples);
        CHECK((info.RouletteTerminations > 0) == (capture.MaxDepth >= capture.RouletteDepth));
        RecordSeed(pixels, seed, result);
    }
    return result;
}

void CheckDiffuseEnclosure(float32 reflectance)
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard()
        {
            rhi::Shutdown();
        }
    } guard;
    PairedSessions sessions;
    if (const char* skipReasonOrNull = StartPairedSessions(sessions); skipReasonOrNull != nullptr)
    {
        SKIP(skipReasonOrNull);
    }
    const rhi::Backend backend = rhi::GetStartup().SelectedBackend;
    const char* backendName = backend == rhi::Backend::Metal    ? "Metal"
                              : backend == rhi::Backend::Vulkan ? "Vulkan"
                                                                : "other RHI backend";
    WARN("RT1 diffuse enclosure on " << backendName << " (RHI " << static_cast<uint32>(backend) << ")");
    BuildDiffuseEnclosure(sessions, reflectance);
    constexpr uint32 EXTENT = 16;
    rr::ReferenceCaptureDescription capture = MakePerspectiveCapture({0, 0, -0.25F}, EXTENT, EXTENT);
    capture.SecondaryDistance = 4; // Cube diameter is sqrt(12) metres.
    capture.RouletteDepth = 2;
    struct DepthCase final
    {
        uint32 Depth;
        uint32 Samples;
    };
    constexpr DepthCase CASES[]{{1, 32}, {2, 32}, {4, 32}, {4, 8}};
    SeedRadiances previousCpu{};
    SeedRadiances previousGpu{};
    uint32 previousDepth = 0;
    for (const DepthCase& setting : CASES)
    {
        capture.TargetSamples = setting.Samples;
        capture.MaxDepth = setting.Depth;
        const float64 rho = Widen(reflectance);
        const float64 total = SumEnclosureOrders(rho, setting.Depth);
        const float64 tail = math::Pow(rho, setting.Depth + 1) / (1 - rho);
        CAPTURE(reflectance, setting.Depth, setting.Samples, tail);
        const SeedMeans cpu =
            CaptureEnclosureSeeds(sessions.Cpu, sessions.Surface, capture, rr::ReferenceTraversal::CpuOracle);
        const SeedMeans gpu =
            CaptureEnclosureSeeds(sessions.Gpu, sessions.Surface, capture, rr::ReferenceTraversal::GpuCompute);
        for (const SeedMeans* means : {&cpu, &gpu})
        {
            const bool isCpu = means == &cpu;
            INFO((isCpu == true ? "CPU oracle" : "GPU compute"));
            CheckEnclosureEstimate(means->Emission, ZERO_RADIANCE, "emission", 1);
            CheckEnclosureEstimate(means->Direct, ZERO_RADIANCE, "direct", rho);
            CheckEnclosureEstimate(means->Indirect, ZERO_RADIANCE, "indirect", total - 1 - rho);
            CheckEnclosureEstimate(means->Radiance, ZERO_RADIANCE, "total", total);
            const EnclosureEstimate red = MeasureEnclosureChannel(means->Radiance, ZERO_RADIANCE, &math::Vector3::X);
            WARN((isCpu == true ? "CPU" : "GPU")
                 << " rho=" << rho << " depth=" << setting.Depth << " spp=" << setting.Samples << " tail/E=" << tail
                 << " red mean=" << red.Mean << " residual=" << (red.Mean - total) << " SE=" << red.StandardError);
            if (previousDepth > 0 && previousDepth < setting.Depth)
            {
                CheckEnclosureEstimate(means->Radiance,
                                       isCpu == true ? previousCpu : previousGpu,
                                       "added orders",
                                       total - SumEnclosureOrders(rho, previousDepth));
            }
        }
        CheckEnclosureEstimate(cpu.Emission, gpu.Emission, "CPU minus GPU emission", 0);
        CheckEnclosureEstimate(cpu.Direct, gpu.Direct, "CPU minus GPU direct", 0);
        CheckEnclosureEstimate(cpu.Indirect, gpu.Indirect, "CPU minus GPU indirect", 0);
        CheckEnclosureEstimate(cpu.Radiance, gpu.Radiance, "CPU minus GPU total", 0);
        previousCpu = cpu.Radiance;
        previousGpu = gpu.Radiance;
        previousDepth = setting.Depth;
    }
}
} // namespace

TEST_CASE("RT1 diffuse enclosure resolves successive bounce orders at half reflectance",
          "[renderer][gpu][reference-compute][diffuse-enclosure]")
{
    CheckDiffuseEnclosure(0.5F);
}

TEST_CASE("RT1 diffuse enclosure resolves successive bounce orders at high reflectance",
          "[renderer][gpu][reference-compute][diffuse-enclosure]")
{
    CheckDiffuseEnclosure(0.875F);
}
