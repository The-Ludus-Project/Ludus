#include "renderer_clustered_authored.h"
#include "renderer_clustered_authored_shadow.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_output.h"
#include "renderer_reference.h"
#include "renderer_reference_reduce.h"
#include "renderer_shadow.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/measurement.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

// The opt-in [.reference-light-sampling-benchmark]: acceptance item 5 of
// docs/architecture/many-area-light-sampling-reference-review.md. RT1's UniformArea, ResampledProjectedSolidAngle and
// ResampledFormFactor modes render the ris-projected-solid-angle template's 40 x 40 emitter field
// (render_graph_ray_tests.cpp, EmitterFieldRoom): the Cornell room lit by 3200 small one-sided emissive triangles whose
// power spans a factor of 64, here with one tint's emitters multiplied by an emission map. Error is measured against a
// high-sample UniformArea image with an independent seed, as MAPE (Ishaan Shah, Aakash KT and P. J. Narayanan,
// "Combining Resampled Importance and Projected Solid Angle Samplings for Many Area Light Rendering", SIGGRAPH Asia
// 2023 Technical Communications, section 5, https://doi.org/10.1145/3610543.3626165, with the template benchmark's 0.01
// guard) and RMSE of the linear Direct + Indirect AOVs. Primary emission is left out: both modes evaluate it
// identically from unchanged random dimensions. Equal time compares the GPU-compute transport kernel's device time per
// sample. Recorded results: the review's "RT1 equal-time evaluation" and docs/architecture/renderer-systems.md,
// "Light-sampling modes".
namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
using S = rhi::RasterStatus;

constexpr usize FIELD_GRID = 40;
constexpr usize FIELD_TILE_COUNT = FIELD_GRID * FIELD_GRID;
constexpr usize FIELD_LEVEL_COUNT = 4;
constexpr usize FIELD_TINT_COUNT = 3;
constexpr usize FIELD_EMITTER_MATERIAL_COUNT = FIELD_LEVEL_COUNT * FIELD_TINT_COUNT;
/// The tint whose emitters multiply their emission by the emission checker.
constexpr usize FIELD_TEXTURED_TINT = 2;
constexpr usize ROOM_WHITE = 0;
constexpr usize ROOM_RED = 1;
constexpr usize ROOM_GREEN = 2;
constexpr usize ROOM_MATERIAL_COUNT = 3;
/// One mesh and one item per material.
constexpr usize FIELD_MATERIAL_COUNT = ROOM_MATERIAL_COUNT + FIELD_EMITTER_MATERIAL_COUNT;
constexpr usize QUAD_CORNERS = 4;
constexpr usize QUAD_INDICES = 6;
constexpr usize ROOM_QUAD_COUNT = 6;
/// Floor, ceiling, back, left and right walls, and the top of a block that shadows the floor: the Cornell room of
/// render_graph_ray_tests.cpp without its ceiling light. Each quad winds counterclockwise seen from inside the room, so
/// cross(c1 - c0, c3 - c0) faces inward.
constexpr StaticArray<math::Vector3, ROOM_QUAD_COUNT * QUAD_CORNERS> ROOM_CORNERS_WORLD{{
    {-1, -1, 1}, {1, -1, 1},   {1, -1, -1},     {-1, -1, -1}, {-1, 1, -1},     {1, 1, -1},
    {1, 1, 1},   {-1, 1, 1},   {-1, -1, -1},    {1, -1, -1},  {1, 1, -1},      {-1, 1, -1},
    {-1, -1, 1}, {-1, -1, -1}, {-1, 1, -1},     {-1, 1, 1},   {1, -1, -1},     {1, -1, 1},
    {1, 1, 1},   {1, 1, -1},   {-.5F, -.3F, 0}, {0, -.3F, 0}, {0, -.3F, -.5F}, {-.5F, -.3F, -.5F},
}};
constexpr StaticArray<usize, ROOM_QUAD_COUNT> ROOM_QUAD_MATERIALS{
    {ROOM_WHITE, ROOM_WHITE, ROOM_WHITE, ROOM_RED, ROOM_GREEN, ROOM_WHITE}};
/// The Cornell camera: eye on +Z looking down -Z, vertical focal length 2.2, conventional depth.
constexpr float32 EYE_Z_WORLD = 3.4F;
constexpr float32 FOCAL_LENGTH = 2.2F;
constexpr float32 NEAR_WORLD = .5F;
constexpr float32 FAR_WORLD = 10;
/// Linear RGBA8 checker multiplying the textured emitters' emission; its texels are 1 and 0.25.
constexpr usize EMISSION_CHECKER_EXTENT = 4;
constexpr usize RGBA8_BYTES = 4;
constexpr uint8 EMISSION_CHECKER_BRIGHT = 255;
constexpr uint8 EMISSION_CHECKER_DIM = 64;

constexpr uint32 DEVICE_RESOURCES_PER_KIND = 1024;
constexpr uint32 DEVICE_BINDING_DESCRIPTORS = 8192;
/// Renderer state for FIELD_MATERIAL_COUNT clustered materials and their shared light-cluster scratch.
constexpr usize RENDERER_HOST_BYTES = usize{256} * 1024 * 1024;
constexpr usize CPU_BUDGET = usize{256} * 1024 * 1024;
constexpr usize GPU_BUDGET = usize{256} * 1024 * 1024;
constexpr uint32 CPU_TILE_PIXELS = 2048; // The largest tile ReferenceRenderingOptions admits.
constexpr uint64 TIMEOUT = 3600 * time::NANOSECONDS_PER_SECOND;
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1e6;
/// The template benchmark's extent: the Cornell camera's aspect ratio at nine times its CTest image area.
constexpr uint32 GPU_WIDTH = 288;
constexpr uint32 GPU_HEIGHT = 192;
/// The CPU oracle's extent, the template's CTest image.
constexpr uint32 CPU_WIDTH = 96;
constexpr uint32 CPU_HEIGHT = 64;
constexpr float64 ROUNDING_OFFSET = .5;
constexpr uint64 MEASURED_SEED = 871;
/// Reseeds sampling, so the reference is independent of the measured images.
constexpr uint64 REFERENCE_SEED = 1871;
constexpr uint32 REFERENCE_SAMPLES = 8192;
constexpr uint32 TIMING_SAMPLES = 32;
constexpr usize SAMPLE_BUDGET_COUNT = 3;
/// Samples per pixel of the resampled image at each budget; UniformArea gets as many at equal samples, and the same
/// transport kernel time at equal time.
constexpr StaticArray<uint32, SAMPLE_BUDGET_COUNT> SAMPLE_BUDGETS{{4, 16, 64}};
constexpr usize CPU_SAMPLE_BUDGET_COUNT = 2;
constexpr StaticArray<uint32, CPU_SAMPLE_BUDGET_COUNT> CPU_SAMPLE_BUDGETS{{4, 16}};
/// The resampled modes measured against UniformArea.
constexpr usize RESAMPLED_MODE_COUNT = 2;
/// UniformArea is read at each budget's equal samples and at each resampled mode's equal time.
constexpr usize MAXIMUM_CHECKPOINTS = SAMPLE_BUDGET_COUNT * (1 + RESAMPLED_MODE_COUNT);
/// Guards MAPE's relative error against black reference channels, as in the template benchmark.
constexpr float64 PERCENTAGE_GUARD = .01;
/// The ordered graph scopes each pass by name; see rhi::GpuPassTimings and ludus_reference_benchmark.
constexpr const char* TRANSPORT_PASS_NAME = "Reference compute transport";

/// Reflectance of the room's white surfaces, after the diffuse and glossy scenes of Shah et al.'s Figure 3.
enum class SurfaceFinish : uint8
{
    Diffuse,  ///< Rough dielectric, roughness 1.
    Specular, ///< Glossy metal, roughness 0.2, which mirrors the emitter field.
};

template <typename Handle>
S Wait(rr::Renderer& inoutRenderer, const Handle& handle, S status)
{
    const uint64 start = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - start < TIMEOUT)
    {
        status = inoutRenderer.GetStatus(handle);
    }
    return status;
}

/// Quads of one material, with flat normals and unit-square texture coordinates.
struct QuadMesh final
{
    Array<math::Vector3> PositionsMesh;
    Array<math::Vector3> NormalsMesh;
    Array<math::Vector2> Texcoords;
    Array<uint32> Indices;
    void AddQuad(const math::Vector3* cornersMesh)
    {
        const math::Vector3 normal = math::Cross(cornersMesh[1] - cornersMesh[0], cornersMesh[3] - cornersMesh[0]);
        const math::Vector3 unitNormal = normal * (1 / math::Length(normal));
        const uint32 first = static_cast<uint32>(PositionsMesh.GetSize());
        constexpr StaticArray<math::Vector2, QUAD_CORNERS> CORNER_TEXCOORDS{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (usize corner = 0; corner < QUAD_CORNERS; ++corner)
        {
            REQUIRE(PositionsMesh.TryAdd(cornersMesh[corner]) == true);
            REQUIRE(NormalsMesh.TryAdd(unitNormal) == true);
            REQUIRE(Texcoords.TryAdd(CORNER_TEXCOORDS[corner]) == true);
        }
        // Counterclockwise about the normal: (0, 1, 2) and (0, 2, 3).
        constexpr StaticArray<uint32, QUAD_INDICES> CORNER_INDICES{{0, 1, 2, 0, 2, 3}};
        for (const uint32 corner : CORNER_INDICES)
        {
            REQUIRE(Indices.TryAdd(first + corner) == true);
        }
    }
};

/// The emitter field's quads, grouped by material. Tiles face down like the Cornell light, with centers 0.05 apart
/// and sides 0.03 long, as in the template's EmitterFieldRoom.
void BuildFieldMeshes(StaticArray<QuadMesh, FIELD_MATERIAL_COUNT>& outMeshes)
{
    for (usize quad = 0; quad < ROOM_QUAD_COUNT; ++quad)
    {
        outMeshes[ROOM_QUAD_MATERIALS[quad]].AddQuad(&ROOM_CORNERS_WORLD[quad * QUAD_CORNERS]);
    }
    constexpr float32 TILE_HEIGHT_WORLD = .98F;
    constexpr float32 TILE_SPACING_WORLD = 2.F / static_cast<float32>(FIELD_GRID);
    constexpr float32 TILE_FIRST_CENTER_WORLD = -1 + TILE_SPACING_WORLD / 2;
    constexpr float32 TILE_HALF_SIDE_WORLD = .015F;
    for (usize tile = 0; tile < FIELD_TILE_COUNT; ++tile)
    {
        const usize row = tile / FIELD_GRID;
        const usize column = tile % FIELD_GRID;
        const float32 centerX = TILE_FIRST_CENTER_WORLD + TILE_SPACING_WORLD * static_cast<float32>(column);
        const float32 centerZ = TILE_FIRST_CENTER_WORLD + TILE_SPACING_WORLD * static_cast<float32>(row);
        const StaticArray<math::Vector3, QUAD_CORNERS> cornersWorld{{
            {centerX - TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ - TILE_HALF_SIDE_WORLD},
            {centerX + TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ - TILE_HALF_SIDE_WORLD},
            {centerX + TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ + TILE_HALF_SIDE_WORLD},
            {centerX - TILE_HALF_SIDE_WORLD, TILE_HEIGHT_WORLD, centerZ + TILE_HALF_SIDE_WORLD},
        }};
        // Levels and tints cycle with different periods along and across rows, so neighbors differ in both.
        const usize level = (tile * 5 + row) % FIELD_LEVEL_COUNT;
        const usize tint = tile % FIELD_TINT_COUNT;
        outMeshes[ROOM_MATERIAL_COUNT + level * FIELD_TINT_COUNT + tint].AddQuad(cornersWorld.GetData());
    }
}

/// A renderer with RT1 enabled for one traversal and the emitter field's snapshot. Materials use the clustered
/// authored program, which admits emission maps; reference transport does not depend on the raster program.
struct FieldSession final
{
    rr::Renderer Renderer;
    rr::MaterialProgram Program;
    rr::Snapshot Snapshot;
};
S PrepareFieldSession(rhi::DeviceHandle device,
                      rr::ReferenceTraversal traversal,
                      SurfaceFinish finish,
                      FieldSession& outSession)
{
    rr::Renderer& renderer = outSession.Renderer;
    rr::Limits limits;
    limits.Meshes = static_cast<uint32>(FIELD_MATERIAL_COUNT);
    limits.Materials = static_cast<uint32>(FIELD_MATERIAL_COUNT);
    limits.HostBytes = RENDERER_HOST_BYTES;
    REQUIRE(renderer.Initialize(device, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment(), limits) ==
            S::Ready);
    const rr::ReferenceRenderingOptions options{traversal,
                                                CPU_BUDGET,
                                                0,
                                                CPU_TILE_PIXELS,
                                                traversal == rr::ReferenceTraversal::GpuCompute ? GPU_BUDGET : 0};
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
    REQUIRE(Wait(renderer,
                 outSession.Program,
                 renderer.PrewarmLitProgram(shaders::renderer_clustered_authored::Vertex(),
                                            shaders::renderer_clustered_authored::Fragment(),
                                            shaders::renderer_clustered_authored_shadow::Fragment(),
                                            outSession.Program)) == S::Ready);

    StaticArray<uint8, EMISSION_CHECKER_EXTENT * EMISSION_CHECKER_EXTENT * RGBA8_BYTES> checker{};
    for (usize texel = 0; texel < EMISSION_CHECKER_EXTENT * EMISSION_CHECKER_EXTENT; ++texel)
    {
        const bool isBright = (texel % EMISSION_CHECKER_EXTENT + texel / EMISSION_CHECKER_EXTENT) % 2 == 0;
        for (usize channel = 0; channel < RGBA8_BYTES; ++channel)
        {
            checker[texel * RGBA8_BYTES + channel] = isBright == true ? EMISSION_CHECKER_BRIGHT : EMISSION_CHECKER_DIM;
        }
    }
    rhi::TextureDescription checkerDescription{static_cast<uint32>(EMISSION_CHECKER_EXTENT),
                                               static_cast<uint32>(EMISSION_CHECKER_EXTENT),
                                               rhi::RasterFormat::Rgba8Unorm};
    checkerDescription.RetainCpuContent = true;
    rhi::TextureHandle checkerTexture;
    REQUIRE(rhi::CreateTexture(device,
                               checkerDescription,
                               {{checker.GetData(), checker.GetSize()}, EMISSION_CHECKER_EXTENT * RGBA8_BYTES},
                               checkerTexture) == S::Ready);

    constexpr StaticArray<float32, FIELD_LEVEL_COUNT> EMISSION_LEVELS{{.25F, 1, 4, 16}};
    constexpr StaticArray<math::Vector3, FIELD_TINT_COUNT> TINTS{{{1, 1, 1}, {1, .6F, .3F}, {.3F, .6F, 1}}};
    constexpr float32 GLOSSY_ROUGHNESS = .2F;
    const rr::LightClusterDescription clusters;
    StaticArray<rr::MaterialVersion, FIELD_MATERIAL_COUNT> materials{};
    for (usize material = 0; material < FIELD_MATERIAL_COUNT; ++material)
    {
        rr::LitMaterialDescription description;
        description.Clusters = &clusters;
        description.Roughness = 1;
        description.Surface.Tint = {.8F, .8F, .8F, 1};
        if (material == ROOM_WHITE && finish == SurfaceFinish::Specular)
        {
            description.Metallic = 1;
            description.Roughness = GLOSSY_ROUGHNESS;
        }
        description.Surface.Tint = material == ROOM_RED     ? math::Vector4{.8F, .1F, .1F, 1}
                                   : material == ROOM_GREEN ? math::Vector4{.1F, .8F, .1F, 1}
                                                            : description.Surface.Tint;
        if (material >= ROOM_MATERIAL_COUNT)
        {
            const usize emitter = material - ROOM_MATERIAL_COUNT;
            const usize tint = emitter % FIELD_TINT_COUNT;
            description.Emission = TINTS[tint] * EMISSION_LEVELS[emitter / FIELD_TINT_COUNT];
            description.IsEmissionTwoSided = false;
            description.EmissionMap = tint == FIELD_TEXTURED_TINT ? checkerTexture : rhi::TextureHandle{};
        }
        rr::LightSelection selection;
        CAPTURE(material);
        REQUIRE(Wait(renderer,
                     materials[material],
                     renderer.CreateLitMaterial(outSession.Program, description, materials[material], selection)) ==
                S::Ready);
    }
    StaticArray<QuadMesh, FIELD_MATERIAL_COUNT> quads;
    BuildFieldMeshes(quads);
    StaticArray<rr::Mesh, FIELD_MATERIAL_COUNT> meshes{};
    StaticArray<rr::SceneItem, FIELD_MATERIAL_COUNT> items{};
    for (usize material = 0; material < FIELD_MATERIAL_COUNT; ++material)
    {
        const QuadMesh& quad = quads[material];
        rr::MeshDescription description{quad.PositionsMesh.GetData(),
                                        quad.PositionsMesh.GetSize(),
                                        quad.Indices.GetData(),
                                        quad.Indices.GetSize()};
        description.Normals = quad.NormalsMesh.GetData();
        description.Texcoords = quad.Texcoords.GetData();
        REQUIRE(Wait(renderer, meshes[material], renderer.CreateMesh(description, meshes[material])) == S::Ready);
        items[material].Geometry = meshes[material];
        items[material].Surface = materials[material];
        items[material].SourceId = material + 1;
    }
    const S created = renderer.CreateSnapshot(items.GetData(), items.GetSize(), outSession.Snapshot);
    for (usize material = 0; material < FIELD_MATERIAL_COUNT; ++material)
    {
        static_cast<void>(renderer.Release(meshes[material]));
        static_cast<void>(renderer.Release(materials[material]));
    }
    static_cast<void>(rhi::Destroy(device, checkerTexture));
    return created;
}

/// Pixel dimensions of a capture.
struct CaptureExtent final
{
    uint32 Width = 0;
    uint32 Height = 0;
};
rr::ReferenceCaptureDescription MakeFieldCapture(CaptureExtent extent, uint32 maxDepth)
{
    rr::ReferenceCaptureDescription capture;
    capture.Width = extent.Width;
    capture.Height = extent.Height;
    capture.MaxDepth = maxDepth;
    capture.RouletteDepth = maxDepth;
    // Right-handed view down -Z from the eye; clip depth 0 at the near plane and 1 at the far plane.
    constexpr float32 DEPTH_SCALE = FAR_WORLD / (NEAR_WORLD - FAR_WORLD);
    const float32 aspect = static_cast<float32>(extent.Width) / static_cast<float32>(extent.Height);
    capture.WorldToClip.Columns[0] = {FOCAL_LENGTH / aspect, 0, 0, 0};
    capture.WorldToClip.Columns[1] = {0, FOCAL_LENGTH, 0, 0};
    capture.WorldToClip.Columns[2] = {0, 0, DEPTH_SCALE, -1};
    capture.WorldToClip.Columns[3] = {0, 0, DEPTH_SCALE * (NEAR_WORLD - EYE_Z_WORLD), EYE_Z_WORLD};
    capture.CameraPosition = {0, 0, EYE_Z_WORLD};
    return capture;
}

/// Appends a completed timing batch's transport dispatches, then arms the next batch.
void CollectTransportTimings(rhi::DeviceHandle device, Array<float64>& inoutDispatchMilliseconds)
{
    rhi::GpuPassTimings timings;
    if (rhi::GetGpuPassTimings(device, timings) == S::Ready && timings.State == rhi::GpuTimingState::Available)
    {
        for (usize i = 0; i < timings.Count; ++i)
        {
            const rhi::GpuPassTiming& pass = timings.Passes[i];
            if (pass.Timing.State == rhi::GpuTimingState::Available &&
                std::strcmp(pass.Scope.Name.GetData(), TRANSPORT_PASS_NAME) == 0)
            {
                REQUIRE(inoutDispatchMilliseconds.TryAdd(static_cast<float64>(pass.Timing.DurationNanoseconds) /
                                                         NANOSECONDS_PER_MILLISECOND) == true);
            }
        }
    }
    if (timings.State != rhi::GpuTimingState::Armed && timings.State != rhi::GpuTimingState::Pending)
    {
        static_cast<void>(rhi::ArmGpuPassTimings(device));
    }
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

/// One capture read after each of an ascending list of sample counts.
struct Checkpoints final
{
    StaticArray<uint32, MAXIMUM_CHECKPOINTS> Targets{};
    usize Count = 0;
    StaticArray<Array<rr::ReferencePixel>, MAXIMUM_CHECKPOINTS> Images;
    StaticArray<rr::ReferenceCaptureInfo, MAXIMUM_CHECKPOINTS> Infos{};
    /// Wall time from Begin to each checkpoint's readback.
    StaticArray<float64, MAXIMUM_CHECKPOINTS> WallMilliseconds{};
    /// Device time of each timed transport dispatch; GPU compute traces one sample of every pixel per dispatch.
    Array<float64> DispatchMilliseconds;
};

/// Render `description` with `inoutCheckpoints.Targets` raised in turn: a TargetSamples change alone continues the
/// same accumulation, so each checkpoint extends the previous one.
void RenderCheckpoints(FieldSession& inoutSession,
                       rhi::DeviceHandle device,
                       rhi::SurfaceHandle surface,
                       rr::ReferenceCaptureDescription description,
                       Checkpoints& inoutCheckpoints)
{
    REQUIRE(inoutCheckpoints.Count >= 1);
    rr::Renderer& renderer = inoutSession.Renderer;
    const uint64 start = time::NowTicks();
    rr::ReferenceCapture capture;
    for (usize checkpoint = 0; checkpoint < inoutCheckpoints.Count; ++checkpoint)
    {
        description.TargetSamples = inoutCheckpoints.Targets[checkpoint];
        const S requested = checkpoint == 0
                                ? renderer.BeginReferenceCapture(inoutSession.Snapshot, description, capture)
                                : renderer.UpdateReferenceCapture(capture, inoutSession.Snapshot, description);
        REQUIRE((requested == S::Ready || requested == S::Pending));
        if (checkpoint > 0)
        {
            REQUIRE(inoutCheckpoints.Targets[checkpoint] >= inoutCheckpoints.Targets[checkpoint - 1]);
        }
        // A Ready setup still has zero samples; only Advance reports completion.
        S status = S::Pending;
        while (status == S::Pending && time::NowTicks() - start < TIMEOUT)
        {
            CollectTransportTimings(device, inoutCheckpoints.DispatchMilliseconds);
            status = renderer.AdvanceReferenceCapture(capture, surface);
        }
        rr::ReferenceCaptureInfo& info = inoutCheckpoints.Infos[checkpoint];
        REQUIRE(renderer.GetReferenceCaptureInfo(capture, info) == S::Ready);
        CAPTURE(description.TargetSamples, info.FailurePixel, static_cast<uint32>(info.Failure));
        REQUIRE(status == S::Ready);
        REQUIRE(info.InvalidContributions == 0);
        REQUIRE(info.MinimumSamples == description.TargetSamples);
        REQUIRE(info.LightSampling == description.LightSampling);
        Array<rr::ReferencePixel>& pixels = inoutCheckpoints.Images[checkpoint];
        REQUIRE(pixels.TryResize(usize(description.Width) * description.Height) == true);
        REQUIRE(renderer.CopyReferencePixels(capture, pixels.GetData(), pixels.GetSize()) == S::Ready);
        inoutCheckpoints.WallMilliseconds[checkpoint] =
            static_cast<float64>(time::NowTicks() - start) / NANOSECONDS_PER_MILLISECOND;
    }
    REQUIRE(renderer.Release(capture) == S::Ready);
}

float64 GetChannel(math::Vector3 value, usize channel)
{
    return static_cast<float64>(channel == 0 ? value.X : channel == 1 ? value.Y : value.Z);
}

/// Error of one image's Direct + Indirect AOVs against the reference's, over every RGB channel.
struct ImageError final
{
    float64 MeanAbsolutePercentage = 0; ///< Mean |x - r| / (r + 0.01).
    float64 RootMeanSquare = 0;         ///< RMSE of linear radiance.
    float64 MeanLuminance = 0;          ///< The image's mean transport luminance, for a bias check.
};
ImageError ComputeTransportError(const Array<rr::ReferencePixel>& pixels, const Array<rr::ReferencePixel>& reference)
{
    REQUIRE(pixels.GetSize() == reference.GetSize());
    constexpr StaticArray<float64, 3> LUMINANCE_WEIGHTS{{.2126, .7152, .0722}};
    float64 percentageSum = 0;
    float64 squareSum = 0;
    float64 luminanceSum = 0;
    for (usize i = 0; i < pixels.GetSize(); ++i)
    {
        const math::Vector3 value = pixels[i].Direct + pixels[i].Indirect;
        const math::Vector3 expected = reference[i].Direct + reference[i].Indirect;
        for (usize channel = 0; channel < 3; ++channel)
        {
            const float64 difference = GetChannel(value, channel) - GetChannel(expected, channel);
            percentageSum += math::Abs(difference) / (GetChannel(expected, channel) + PERCENTAGE_GUARD);
            squareSum += difference * difference;
            luminanceSum += LUMINANCE_WEIGHTS[channel] * GetChannel(value, channel);
        }
    }
    const float64 channelCount = static_cast<float64>(pixels.GetSize() * 3);
    ImageError error;
    error.MeanAbsolutePercentage = percentageSum / channelCount;
    error.RootMeanSquare = math::Sqrt(squareSum / channelCount);
    error.MeanLuminance = luminanceSum / static_cast<float64>(pixels.GetSize());
    return error;
}

/// Queries per pixel and sample of one capture.
struct QueryCost final
{
    float64 ShadowRaysPerPixel = 0;
    float64 ClosestRaysPerPixel = 0;
};
QueryCost MeasureQueries(const rr::ReferenceCaptureInfo& info)
{
    REQUIRE(info.CommittedSamples > 0);
    QueryCost cost;
    cost.ShadowRaysPerPixel =
        static_cast<float64>(info.VisibilityQueries) / static_cast<float64>(info.CommittedSamples);
    cost.ClosestRaysPerPixel = static_cast<float64>(info.ClosestQueries) / static_cast<float64>(info.CommittedSamples);
    return cost;
}

const char* GetModeName(rr::ReferenceLightSampling mode)
{
    return mode == rr::ReferenceLightSampling::UniformArea                    ? "UniformArea"
           : mode == rr::ReferenceLightSampling::ResampledProjectedSolidAngle ? "ResampledProjectedSolidAngle"
                                                                              : "ResampledFormFactor";
}

bool StartTimedComputeDevice(rhi::DeviceHandle& outDevice, rhi::SurfaceHandle& outSurface)
{
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Required.Compute = true;
    description.EnableGpuTiming = true;
    // FIELD_MATERIAL_COUNT clustered authored materials exceed the default binding descriptors.
    description.Storage.ResourcesPerKind = DEVICE_RESOURCES_PER_KIND;
    description.Storage.BindingDescriptors = DEVICE_BINDING_DESCRIPTORS;
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

struct ShutdownGuard final
{
    ShutdownGuard()
    {
        rhi::Shutdown();
    }
    ~ShutdownGuard()
    {
        rhi::Shutdown();
    }
    ShutdownGuard(const ShutdownGuard&) = delete;
    ShutdownGuard& operator=(const ShutdownGuard&) = delete;
};

/// The high-sample UniformArea reference, rendered with the GPU-compute session.
Array<rr::ReferencePixel> RenderReference(FieldSession& inoutSession,
                                          rhi::DeviceHandle device,
                                          rhi::SurfaceHandle surface,
                                          rr::ReferenceCaptureDescription description)
{
    description.Seed = REFERENCE_SEED;
    description.LightSampling = rr::ReferenceLightSampling::UniformArea;
    Checkpoints reference;
    reference.Targets[0] = REFERENCE_SAMPLES;
    reference.Count = 1;
    RenderCheckpoints(inoutSession, device, surface, description, reference);
    return Move(reference.Images[0]);
}
} // namespace

TEST_CASE("Measure RT1 resampled and uniform-area light sampling error at equal samples and equal kernel time",
          "[.reference-light-sampling-benchmark]")
{
    struct Case final
    {
        const char* Name = "";
        SurfaceFinish Finish = SurfaceFinish::Diffuse;
        uint32 MaxDepth = 1;
    };
    constexpr usize CASE_COUNT = 4;
    constexpr StaticArray<Case, CASE_COUNT> CASES{{
        {"diffuse room, direct lighting", SurfaceFinish::Diffuse, 1},
        {"specular room, direct lighting", SurfaceFinish::Specular, 1},
        {"diffuse room, depth 4", SurfaceFinish::Diffuse, 4},
        {"specular room, depth 4", SurfaceFinish::Specular, 4},
    }};
    for (const Case& test : CASES)
    {
        SECTION(test.Name)
        {
            const ShutdownGuard guard;
            rhi::DeviceHandle device;
            rhi::SurfaceHandle surface;
            if (StartTimedComputeDevice(device, surface) == false)
            {
                SKIP("No Metal adapter");
            }
            rhi::MeasurementAdapter adapter;
            REQUIRE(rhi::GetMeasurementAdapter(device, adapter) == S::Ready);
            FieldSession session;
            const S prepared = PrepareFieldSession(device, rr::ReferenceTraversal::GpuCompute, test.Finish, session);
            if (prepared == S::Unsupported)
            {
                SKIP("Compute storage is unavailable on this device");
            }
            REQUIRE(prepared == S::Ready);
            rr::ReferenceCaptureDescription capture = MakeFieldCapture({GPU_WIDTH, GPU_HEIGHT}, test.MaxDepth);
            capture.Seed = MEASURED_SEED;
            // Timing rounds run UniformArea, each resampled mode, then the resampled modes in reverse and UniformArea
            // again, so drift in machine load or clocks affects every mode alike; each mode's cost is the mean of its
            // two rounds' median dispatch times.
            constexpr usize UNIFORM = 0;
            constexpr usize MODE_COUNT = 1 + RESAMPLED_MODE_COUNT;
            constexpr StaticArray<rr::ReferenceLightSampling, MODE_COUNT> MODES{
                {rr::ReferenceLightSampling::UniformArea,
                 rr::ReferenceLightSampling::ResampledProjectedSolidAngle,
                 rr::ReferenceLightSampling::ResampledFormFactor}};
            constexpr usize ROUND_COUNT = 2 * MODE_COUNT;
            StaticArray<usize, ROUND_COUNT> roundOrder{};
            for (usize mode = 0; mode < MODE_COUNT; ++mode)
            {
                roundOrder[mode] = mode;
                roundOrder[ROUND_COUNT - 1 - mode] = mode;
            }
            StaticArray<float64, MODE_COUNT> sampleMilliseconds{};
            StaticArray<QueryCost, MODE_COUNT> queries{};
            for (const usize mode : roundOrder)
            {
                capture.LightSampling = MODES[mode];
                Checkpoints round;
                round.Targets[0] = TIMING_SAMPLES;
                round.Count = 1;
                RenderCheckpoints(session, device, surface, capture, round);
                REQUIRE(round.DispatchMilliseconds.GetSize() > 0);
                sampleMilliseconds[mode] += ComputeMedian(round.DispatchMilliseconds) / 2;
                queries[mode] = MeasureQueries(round.Infos[0]);
            }
            for (usize mode = 0; mode < MODE_COUNT; ++mode)
            {
                REQUIRE(sampleMilliseconds[mode] > 0);
                WARN(test.Name << ", " << FIELD_TILE_COUNT * 2 << " emissive triangles, " << GPU_WIDTH << 'x'
                               << GPU_HEIGHT << ", " << adapter.Name << ": " << GetModeName(MODES[mode])
                               << " transport kernel " << sampleMilliseconds[mode] << " ms per sample; shadow rays per "
                               << "pixel " << queries[mode].ShadowRaysPerPixel << ", closest rays per pixel "
                               << queries[mode].ClosestRaysPerPixel);
            }
            const Array<rr::ReferencePixel> reference = RenderReference(session, device, surface, capture);
            // UniformArea at each budget's samples, then at each resampled mode's transport kernel time.
            Checkpoints uniform;
            StaticArray<StaticArray<uint32, SAMPLE_BUDGET_COUNT>, MODE_COUNT> equalTimeSamples{};
            for (usize budget = 0; budget < SAMPLE_BUDGET_COUNT; ++budget)
            {
                uniform.Targets[budget] = SAMPLE_BUDGETS[budget];
                for (usize mode = 1; mode < MODE_COUNT; ++mode)
                {
                    const float64 samples = static_cast<float64>(SAMPLE_BUDGETS[budget]) * sampleMilliseconds[mode] /
                                            sampleMilliseconds[UNIFORM];
                    equalTimeSamples[mode][budget] =
                        math::Max(static_cast<uint32>(math::Floor(samples + ROUNDING_OFFSET)), uint32{1});
                    uniform.Targets[SAMPLE_BUDGET_COUNT * mode + budget] = equalTimeSamples[mode][budget];
                }
            }
            uniform.Count = SAMPLE_BUDGET_COUNT * MODE_COUNT;
            Sort(uniform.Targets.GetData(), uniform.Count, [](uint32 left, uint32 right) noexcept {
                return left < right;
            });
            capture.LightSampling = rr::ReferenceLightSampling::UniformArea;
            RenderCheckpoints(session, device, surface, capture, uniform);
            const auto findUniform = [&uniform](uint32 samples) {
                for (usize checkpoint = 0; checkpoint < uniform.Count; ++checkpoint)
                {
                    if (uniform.Targets[checkpoint] == samples)
                    {
                        return checkpoint;
                    }
                }
                FAIL("missing UniformArea checkpoint");
                return usize{0};
            };
            const ImageError referenceMean = ComputeTransportError(reference, reference);
            WARN(test.Name << " reference: UniformArea at " << REFERENCE_SAMPLES
                           << " samples per pixel, independent seed; mean transport luminance "
                           << referenceMean.MeanLuminance);
            for (usize budget = 0; budget < SAMPLE_BUDGET_COUNT; ++budget)
            {
                const usize equalSamplesIndex = findUniform(SAMPLE_BUDGETS[budget]);
                const ImageError equalSamples = ComputeTransportError(uniform.Images[equalSamplesIndex], reference);
                const QueryCost uniformQueries = MeasureQueries(uniform.Infos[equalSamplesIndex]);
                WARN(test.Name << ", UniformArea at " << SAMPLE_BUDGETS[budget] << " samples: MAPE "
                               << equalSamples.MeanAbsolutePercentage << ", RMSE " << equalSamples.RootMeanSquare
                               << ", mean luminance " << equalSamples.MeanLuminance << ", shadow rays per pixel "
                               << uniformQueries.ShadowRaysPerPixel);
            }
            for (usize mode = 1; mode < MODE_COUNT; ++mode)
            {
                Checkpoints resampled;
                for (usize budget = 0; budget < SAMPLE_BUDGET_COUNT; ++budget)
                {
                    resampled.Targets[budget] = SAMPLE_BUDGETS[budget];
                }
                resampled.Count = SAMPLE_BUDGET_COUNT;
                capture.LightSampling = MODES[mode];
                RenderCheckpoints(session, device, surface, capture, resampled);
                for (usize budget = 0; budget < SAMPLE_BUDGET_COUNT; ++budget)
                {
                    const ImageError resampledError = ComputeTransportError(resampled.Images[budget], reference);
                    const ImageError equalTime =
                        ComputeTransportError(uniform.Images[findUniform(equalTimeSamples[mode][budget])], reference);
                    const QueryCost resampledQueries = MeasureQueries(resampled.Infos[budget]);
                    WARN(test.Name << ", " << GetModeName(MODES[mode]) << " at " << SAMPLE_BUDGETS[budget]
                                   << " samples ("
                                   << static_cast<float64>(SAMPLE_BUDGETS[budget]) * sampleMilliseconds[mode]
                                   << " ms of kernel time): MAPE " << resampledError.MeanAbsolutePercentage << ", RMSE "
                                   << resampledError.RootMeanSquare << ", mean luminance "
                                   << resampledError.MeanLuminance << ", shadow rays per pixel "
                                   << resampledQueries.ShadowRaysPerPixel << "; UniformArea at equal time ("
                                   << equalTimeSamples[mode][budget] << " samples): MAPE "
                                   << equalTime.MeanAbsolutePercentage << ", RMSE " << equalTime.RootMeanSquare
                                   << ", mean luminance " << equalTime.MeanLuminance);
                }
            }
            static_cast<void>(session.Renderer.Release(session.Snapshot));
        }
    }
}

TEST_CASE("Measure RT1 resampled and uniform-area light sampling error on the CPU oracle at equal samples",
          "[.reference-light-sampling-benchmark]")
{
    struct Case final
    {
        const char* Name = "";
        uint32 MaxDepth = 1;
    };
    constexpr usize CASE_COUNT = 2;
    constexpr StaticArray<Case, CASE_COUNT> CASES{{
        {"diffuse room, direct lighting", 1},
        {"diffuse room, depth 4", 4},
    }};
    for (const Case& test : CASES)
    {
        SECTION(test.Name)
        {
            const ShutdownGuard guard;
            rhi::DeviceHandle device;
            rhi::SurfaceHandle surface;
            if (StartTimedComputeDevice(device, surface) == false)
            {
                SKIP("No Metal adapter");
            }
            // The reference is a GPU-compute UniformArea image: CPU and GPU raw AOVs agree within 3.2e-5 per pixel
            // (renderer-systems.md, "GPU-compute reference"), far below the noise measured here.
            rr::ReferenceCaptureDescription capture = MakeFieldCapture({CPU_WIDTH, CPU_HEIGHT}, test.MaxDepth);
            capture.Seed = MEASURED_SEED;
            Array<rr::ReferencePixel> reference;
            {
                FieldSession gpu;
                const S prepared =
                    PrepareFieldSession(device, rr::ReferenceTraversal::GpuCompute, SurfaceFinish::Diffuse, gpu);
                if (prepared == S::Unsupported)
                {
                    SKIP("Compute storage is unavailable on this device");
                }
                REQUIRE(prepared == S::Ready);
                reference = RenderReference(gpu, device, surface, capture);
                static_cast<void>(gpu.Renderer.Release(gpu.Snapshot));
            }
            FieldSession cpu;
            REQUIRE(PrepareFieldSession(device, rr::ReferenceTraversal::CpuOracle, SurfaceFinish::Diffuse, cpu) ==
                    S::Ready);
            for (const rr::ReferenceLightSampling mode : {rr::ReferenceLightSampling::UniformArea,
                                                          rr::ReferenceLightSampling::ResampledProjectedSolidAngle,
                                                          rr::ReferenceLightSampling::ResampledFormFactor})
            {
                capture.LightSampling = mode;
                Checkpoints images;
                for (usize budget = 0; budget < CPU_SAMPLE_BUDGET_COUNT; ++budget)
                {
                    images.Targets[budget] = CPU_SAMPLE_BUDGETS[budget];
                }
                images.Count = CPU_SAMPLE_BUDGET_COUNT;
                RenderCheckpoints(cpu, device, surface, capture, images);
                for (usize budget = 0; budget < CPU_SAMPLE_BUDGET_COUNT; ++budget)
                {
                    const ImageError error = ComputeTransportError(images.Images[budget], reference);
                    const QueryCost queries = MeasureQueries(images.Infos[budget]);
                    WARN(test.Name << ", CPU oracle " << CPU_WIDTH << 'x' << CPU_HEIGHT << ", " << GetModeName(mode)
                                   << " at " << CPU_SAMPLE_BUDGETS[budget] << " samples: MAPE "
                                   << error.MeanAbsolutePercentage << ", RMSE " << error.RootMeanSquare
                                   << ", mean luminance " << error.MeanLuminance << ", shadow rays per pixel "
                                   << queries.ShadowRaysPerPixel << ", wall time since Begin "
                                   << images.WallMilliseconds[budget] << " ms");
                }
            }
            static_cast<void>(cpu.Renderer.Release(cpu.Snapshot));
        }
    }
}
