// RT2 roughness-aware reflections (issue 1248): a rough metal receiver faces an orthographic camera and reflects a
// Lambertian panel lit by one directional light. The panel and a black blocker in front of it lie behind the camera's
// near plane, so no pixel sees them directly; the receiver shows the panel, the blocker's silhouette and the shadow it
// casts on the panel only through reflection. States sweep the receiver roughness, then move the blocker. A float64
// quadrature oracle integrates the receiver's GGX reflection over the incident directions. The RT1 reference (CPU
// oracle and GPU compute, two scattering events), the hybrid reflections template (raster first hits, one ray from
// the specular lobe per pixel and frame, Accumulate) and the default raster graph selected as the RT2 fallback are
// compared with it.
// Thanks to Bruce Walter, Stephen R. Marschner, Hongsong Li and Kenneth E. Torrance, "Microfacet Models for Refraction
// through Rough Surfaces", EGSR 2007, Section 5.2 and equations 33 and 35-36, for the GGX distribution and its
// microfacet-normal sampling, which the oracle stratifies; and to Eric Heitz, "Understanding the Masking-Shadowing
// Function in Microfacet-Based BRDFs", JCGT 3(2), 2014, Section 6, for the height-correlated Smith term.
// https://www.cs.cornell.edu/~srm/publications/EGSR07-btdf.pdf · https://jcgt.org/published/0003/02/03/

#include "render_graph_gpu_fixture.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_reference.h"
#include "renderer_reference_reduce.h"
#include "renderer_shadow.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>

#include <string_view>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
namespace threading = ludus::foundation::threading;
namespace format = ludus::content::scene_format;

namespace
{
using S = rhi::RasterStatus;
constexpr uint64 TIMEOUT_NANOSECONDS = LUDUS_RENDERER_TEST_COLD_GPU_TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND;
constexpr usize CHANNEL_COUNT = 4;
constexpr float32 MAXIMUM_CODE = 255;
constexpr float32 LINEAR_COLOR_CODE = 1.0F / MAXIMUM_CODE;
constexpr usize PIXEL_COUNT = static_cast<usize>(WIDTH) * HEIGHT;

/// A float32 scene value widened for the float64 oracle.
constexpr float64 Widen(float32 value)
{
    return static_cast<float64>(value);
}

// The orthographic identity camera looks along +Z from its near plane z = 0, so the image spans x and y in [-1, 1] of
// the receiver. The panel and the blocker lie behind the near plane.
constexpr float32 RECEIVER_DEPTH_WORLD = .5F;
/// The receiver covers the image and stops short of the light's path to the panel.
constexpr float32 RECEIVER_HALF_EXTENT_WORLD = 1.2F;
/// Metallic one: the standard BSDF is its GGX specular lobe alone, with Schlick Fresnel of this reflectance at normal
/// incidence.
constexpr float32 RECEIVER_REFLECTANCE = .9F;
/// The lit panel faces +Z, towards the receiver.
constexpr float32 PANEL_DEPTH_WORLD = -.4F;
constexpr float32 PANEL_HALF_EXTENT_WORLD = 1.6F;
constexpr float32 PANEL_ALBEDO = .8F;
/// A black, two-sided blocker 0.1 m in front of the panel; it moves along X through its placement's translation.
constexpr float32 BLOCKER_DEPTH_WORLD = -.3F;
constexpr math::Vector2 BLOCKER_HALF_EXTENT_WORLD{.15F, .3F};
/// Direction of travel of the directional light: it reaches the panel's front at a grazing angle from +X and the
/// receiver's back, so the receiver has no direct light. Its blocker's shadow lies 0.5 m towards -X of the blocker.
constexpr math::Vector3 LIGHT_TRAVEL_WORLD{-1, 0, -.2F};
/// Illuminance at normal incidence in lux; it puts the panel's radiance near 0.55 and the receiver's below one.
constexpr float32 LIGHT_ILLUMINANCE = 11;

/// Receiver roughness and blocker centre of each state: a roughness sweep, then the blocker's motion.
struct SceneState final
{
    float32 Roughness = 1;
    float32 BlockerCenterWorld = 0;
};
constexpr usize STATE_COUNT = 6;
/// The roughness controls precede the first motion state.
constexpr usize ROUGHNESS_CONTROL_COUNT = 2;
constexpr float32 SMOOTH_ROUGHNESS = .2F;
constexpr float32 ROUGH_ROUGHNESS = .5F;
constexpr float32 MOTION_ROUGHNESS = .35F;
using SceneStates = StaticArray<SceneState, STATE_COUNT>;
constexpr SceneStates STATES{{{SMOOTH_ROUGHNESS, .5F},
                              {ROUGH_ROUGHNESS, .5F},
                              {MOTION_ROUGHNESS, .5F},
                              {MOTION_ROUGHNESS, .3F},
                              {MOTION_ROUGHNESS, .1F},
                              {MOTION_ROUGHNESS, -.1F}}};
/// The distinct receiver roughness values, one RT1 material each.
constexpr usize ROUGHNESS_COUNT = 3;
constexpr StaticArray<float32, ROUGHNESS_COUNT> ROUGHNESS_VALUES{{SMOOTH_ROUGHNESS, ROUGH_ROUGHNESS, MOTION_ROUGHNESS}};
usize GetRoughnessIndex(const SceneState& state)
{
    for (usize index = 0; index < ROUGHNESS_COUNT; ++index)
    {
        if (ROUGHNESS_VALUES[index] == state.Roughness)
        {
            return index;
        }
    }
    FAIL("state roughness missing from ROUGHNESS_VALUES");
    return 0;
}
std::string_view DescribeState(usize state)
{
    constexpr StaticArray<std::string_view, STATE_COUNT> NAMES{
        {"roughness 0.2", "roughness 0.5", "motion 0", "motion 1", "motion 2", "motion 3"}};
    return NAMES[state];
}

// --- Reflection oracle, in float64 ---

math::Vector3d GetTowardLightWorld()
{
    const math::Vector3d travel{Widen(LIGHT_TRAVEL_WORLD.X), Widen(LIGHT_TRAVEL_WORLD.Y), Widen(LIGHT_TRAVEL_WORLD.Z)};
    const float64 length = math::Sqrt(travel.X * travel.X + travel.Y * travel.Y + travel.Z * travel.Z);
    return {-travel.X / length, -travel.Y / length, -travel.Z / length};
}

/// Radiance of the Lambertian panel where it is lit: albedo / pi times the illuminance projected on its normal, +Z.
float64 ComputePanelRadiance()
{
    return Widen(PANEL_ALBEDO) / math::kPi * Widen(LIGHT_ILLUMINANCE) * GetTowardLightWorld().Z;
}

bool IsInsideBlocker(const SceneState& state, float64 x, float64 y)
{
    return math::Abs(x - Widen(state.BlockerCenterWorld)) <= Widen(BLOCKER_HALF_EXTENT_WORLD.X) &&
           math::Abs(y) <= Widen(BLOCKER_HALF_EXTENT_WORLD.Y);
}

/// Radiance leaving the panel plane's point (x, y) towards the receiver: zero off the panel and where the blocker,
/// crossed by the point's ray towards the light, shadows it.
float64 EvaluatePanelRadiance(const SceneState& state, float64 x, float64 y)
{
    const float64 extent = Widen(PANEL_HALF_EXTENT_WORLD);
    if (math::Abs(x) > extent || math::Abs(y) > extent)
    {
        return 0;
    }
    const math::Vector3d towardLight = GetTowardLightWorld();
    const float64 rise = (Widen(BLOCKER_DEPTH_WORLD) - Widen(PANEL_DEPTH_WORLD)) / towardLight.Z;
    return IsInsideBlocker(state, x + rise * towardLight.X, y + rise * towardLight.Y) == true ? 0
                                                                                              : ComputePanelRadiance();
}

/// Radiance arriving at the receiver point from the unit direction: the black blocker, then the panel, then the black
/// environment.
float64 EvaluateIncidentRadiance(const SceneState& state, math::Vector3d receiverWorld, math::Vector3d directionWorld)
{
    if (directionWorld.Z >= 0)
    {
        return 0;
    }
    const float64 blockerDistance = (Widen(BLOCKER_DEPTH_WORLD) - receiverWorld.Z) / directionWorld.Z;
    if (IsInsideBlocker(state,
                        receiverWorld.X + blockerDistance * directionWorld.X,
                        receiverWorld.Y + blockerDistance * directionWorld.Y) == true)
    {
        return 0;
    }
    const float64 panelDistance = (Widen(PANEL_DEPTH_WORLD) - receiverWorld.Z) / directionWorld.Z;
    return EvaluatePanelRadiance(state,
                                 receiverWorld.X + panelDistance * directionWorld.X,
                                 receiverWorld.Y + panelDistance * directionWorld.Y);
}

/// The receiver point at the centre of pixel (column, row), row 0 at the top.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (column, row) is the image's conventional order.
math::Vector3d GetReceiverPointWorld(float64 column, float64 row)
{
    const float64 clipX = 2 * (column + .5) / WIDTH - 1;
    const float64 clipY = 1 - 2 * (row + .5) / HEIGHT;
    return {clipX, clipY, Widen(RECEIVER_DEPTH_WORLD)};
}

/// Reflected radiance towards the camera, -Z, which is also the receiver's normal. The stratified midpoints of a
/// side by side grid sample microfacet normals with density D(h) cos(theta_h) by Walter et al.'s inverse CDF,
/// tan^2(theta_h) = alpha^2 u / (1 - u). With the view along the normal, v.h = n.h, so f cos(theta_l) / pdf(l) is
/// 4 F(v.h) V(n.l) n.l with V the height-correlated Smith visibility term G2 / (4 n.v n.l).
float64 ComputeReflectedRadiance(const SceneState& state, math::Vector3d receiverWorld, usize side)
{
    const float64 roughness = Widen(state.Roughness);
    const float64 alphaSquared = roughness * roughness * roughness * roughness;
    const float64 reflectance = Widen(RECEIVER_REFLECTANCE);
    constexpr float64 FRESNEL_POWER = 5;
    float64 sum = 0;
    for (usize j = 0; j < side; ++j)
    {
        const float64 angle = 2 * math::kPi * (static_cast<float64>(j) + .5) / static_cast<float64>(side);
        const float64 cosAngle = math::Cos(angle);
        const float64 sinAngle = math::Sin(angle);
        for (usize i = 0; i < side; ++i)
        {
            const float64 u = (static_cast<float64>(i) + .5) / static_cast<float64>(side);
            const float64 tanSquared = alphaSquared * u / (1 - u);
            const float64 cosHalf = 1 / math::Sqrt(1 + tanSquared);
            const float64 sinHalf = math::Sqrt(1 - cosHalf * cosHalf);
            // The half vector in the frame whose normal is -Z, and the view reflected about it.
            const math::Vector3d halfWorld{(sinHalf * cosAngle), (sinHalf * sinAngle), -cosHalf};
            const math::Vector3d directionWorld{2 * cosHalf * halfWorld.X,
                                                2 * cosHalf * halfWorld.Y,
                                                2 * cosHalf * halfWorld.Z + 1};
            const float64 cosLight = -directionWorld.Z;
            if (cosLight <= 0)
            {
                continue;
            }
            const float64 incident = EvaluateIncidentRadiance(state, receiverWorld, directionWorld);
            if (incident <= 0)
            {
                continue;
            }
            const float64 fresnel = reflectance + (1 - reflectance) * math::Pow(1 - cosHalf, FRESNEL_POWER);
            const float64 lightLambda = math::Sqrt(cosLight * cosLight * (1 - alphaSquared) + alphaSquared);
            // The view lambda is one at normal incidence.
            const float64 visibility = .5 / (cosLight + lightLambda);
            sum += 4 * fresnel * visibility * cosLight * incident;
        }
    }
    return sum / static_cast<float64>(side * side);
}

/// Oracle strata per side; halving the grid changes no probe by more than ORACLE_CONVERGENCE_TOLERANCE.
constexpr usize ORACLE_SIDE = 256;
constexpr float64 ORACLE_CONVERGENCE_TOLERANCE = 2e-3;
/// Oracle probes sit at the centres of PROBE_STRIDE_PIXELS-square blocks.
constexpr usize PROBE_STRIDE_PIXELS = 4;
constexpr usize PROBE_COLUMNS = WIDTH / PROBE_STRIDE_PIXELS;
constexpr usize PROBE_ROWS = HEIGHT / PROBE_STRIDE_PIXELS;
constexpr usize PROBE_COUNT = PROBE_COLUMNS * PROBE_ROWS;
usize GetProbePixel(usize probe)
{
    const usize column = (probe % PROBE_COLUMNS) * PROBE_STRIDE_PIXELS + PROBE_STRIDE_PIXELS / 2;
    const usize row = (probe / PROBE_COLUMNS) * PROBE_STRIDE_PIXELS + PROBE_STRIDE_PIXELS / 2;
    return row * WIDTH + column;
}
using ProbeImage = StaticArray<float32, PROBE_COUNT>;

/// The oracle at every probe of one state, clamped like linear RGBA8.
ProbeImage MakeOracleProbes(const SceneState& state, usize side = ORACLE_SIDE)
{
    ProbeImage result{};
    for (usize probe = 0; probe < PROBE_COUNT; ++probe)
    {
        const usize pixel = GetProbePixel(probe);
        const usize column = pixel % WIDTH;
        const usize row = pixel / WIDTH;
        const math::Vector3d receiverWorld =
            GetReceiverPointWorld(static_cast<float64>(column), static_cast<float64>(row));
        result[probe] = static_cast<float32>(math::Min(ComputeReflectedRadiance(state, receiverWorld, side), 1.0));
    }
    return result;
}

/// A red-channel linear image of WIDTH by HEIGHT pixels, row 0 at the top.
using LinearImage = StaticArray<float32, PIXEL_COUNT>;

/// Signed and absolute error of measured values against expected ones.
struct ImageError final
{
    float64 MeanSigned = 0;
    float64 Rms = 0;
    float32 MaximumAbsolute = 0;
    usize FalseLight = 0;     ///< Values brighter than expected by more than the tolerance.
    usize FalseDarkening = 0; ///< Values darker than expected by more than the tolerance.
};

/// Running sums of the signed and squared errors.
struct ErrorSums final
{
    float64 Signed = 0;
    float64 Squares = 0;
};

void AccumulateError(float32 error, float32 tolerance, ImageError& inoutError, ErrorSums& inoutSums)
{
    inoutSums.Signed += Widen(error);
    inoutSums.Squares += Widen(error) * Widen(error);
    inoutError.MaximumAbsolute = math::Max(inoutError.MaximumAbsolute, math::Abs(error));
    inoutError.FalseLight += error > tolerance ? 1 : 0;
    inoutError.FalseDarkening += error < -tolerance ? 1 : 0;
}

/// Error of an image at the probes against the oracle.
ImageError MeasureProbeError(const LinearImage& image, const ProbeImage& oracle, float32 tolerance)
{
    ImageError result;
    ErrorSums sums;
    for (usize probe = 0; probe < PROBE_COUNT; ++probe)
    {
        AccumulateError(image[GetProbePixel(probe)] - oracle[probe], tolerance, result, sums);
    }
    result.MeanSigned = sums.Signed / static_cast<float64>(PROBE_COUNT);
    result.Rms = math::Sqrt(sums.Squares / static_cast<float64>(PROBE_COUNT));
    return result;
}

/// Error of one image against another over every pixel.
ImageError MeasureImageError(const LinearImage& image, const LinearImage& expected, float32 tolerance)
{
    ImageError result;
    ErrorSums sums;
    for (usize pixel = 0; pixel < PIXEL_COUNT; ++pixel)
    {
        AccumulateError(image[pixel] - expected[pixel], tolerance, result, sums);
    }
    result.MeanSigned = sums.Signed / static_cast<float64>(PIXEL_COUNT);
    result.Rms = math::Sqrt(sums.Squares / static_cast<float64>(PIXEL_COUNT));
    return result;
}

void ReportError(std::string_view route, usize state, const ImageError& error)
{
    WARN(route << ", " << DescribeState(state) << ": mean signed " << error.MeanSigned << ", RMS " << error.Rms
               << ", largest " << error.MaximumAbsolute << ", false light " << error.FalseLight << ", false darkening "
               << error.FalseDarkening);
}

/// A per-value tolerance of about seven standard errors of a mean of the given samples. One reflection sample's
/// radiance is at most the panel's, about 0.55, times a visible-normal weight below one, so its standard deviation
/// is at most a quarter.
float32 GetStatisticalTolerance(uint32 samples)
{
    constexpr float64 STANDARD_DEVIATION_BOUND = .25;
    constexpr float64 STANDARD_ERRORS = 7;
    return static_cast<float32>(STANDARD_ERRORS * STANDARD_DEVIATION_BOUND / math::Sqrt(static_cast<float64>(samples)));
}

// --- Scene geometry shared by the render graphs and RT1 ---

constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize ONE_SIDED_INDEX_COUNT = 6;
constexpr usize TWO_SIDED_INDEX_COUNT = 2 * ONE_SIDED_INDEX_COUNT;
/// Corners counterclockwise about +Z.
StaticArray<math::Vector3, QUAD_VERTEX_COUNT>
MakeQuadCorners(math::Vector2 minimumWorld, math::Vector2 maximumWorld, float32 depthWorld)
{
    return {{{minimumWorld.X, minimumWorld.Y, depthWorld},
             {maximumWorld.X, minimumWorld.Y, depthWorld},
             {maximumWorld.X, maximumWorld.Y, depthWorld},
             {minimumWorld.X, maximumWorld.Y, depthWorld}}};
}
/// Geometric normal +Z: cross(c1 - c0, c2 - c0).
constexpr StaticArray<uint32, ONE_SIDED_INDEX_COUNT> FRONT_INDICES{{0, 1, 2, 0, 2, 3}};
/// Geometric normal -Z, towards the camera.
constexpr StaticArray<uint32, ONE_SIDED_INDEX_COUNT> BACK_INDICES{{0, 2, 1, 0, 3, 2}};
/// Both windings, so the blocker occludes from either side in every route.
constexpr StaticArray<uint32, TWO_SIDED_INDEX_COUNT> TWO_SIDED_INDICES{{0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2}};

constexpr usize MESH_COUNT = 3;
constexpr uint32 RECEIVER_MESH = 0;
constexpr uint32 PANEL_MESH = 1;
constexpr uint32 BLOCKER_MESH = 2;
/// One material per mesh, in mesh order.
constexpr usize MATERIAL_COUNT = MESH_COUNT;

/// The scene's static geometry; the blocker is centred on x = 0 and placed by its item's translation.
struct ReflectionGeometry final
{
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> ReceiverCorners =
        MakeQuadCorners({-RECEIVER_HALF_EXTENT_WORLD, -RECEIVER_HALF_EXTENT_WORLD},
                        {RECEIVER_HALF_EXTENT_WORLD, RECEIVER_HALF_EXTENT_WORLD},
                        RECEIVER_DEPTH_WORLD);
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> ReceiverNormals{{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}}};
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> PanelCorners =
        MakeQuadCorners({-PANEL_HALF_EXTENT_WORLD, -PANEL_HALF_EXTENT_WORLD},
                        {PANEL_HALF_EXTENT_WORLD, PANEL_HALF_EXTENT_WORLD},
                        PANEL_DEPTH_WORLD);
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> FrontNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> BlockerCorners =
        MakeQuadCorners({-BLOCKER_HALF_EXTENT_WORLD.X, -BLOCKER_HALF_EXTENT_WORLD.Y},
                        {BLOCKER_HALF_EXTENT_WORLD.X, BLOCKER_HALF_EXTENT_WORLD.Y},
                        BLOCKER_DEPTH_WORLD);
    StaticArray<renderer::MeshDescription, MESH_COUNT> Meshes{};
    ReflectionGeometry()
    {
        Meshes[RECEIVER_MESH] = {ReceiverCorners.GetData(),
                                 ReceiverCorners.GetSize(),
                                 BACK_INDICES.GetData(),
                                 BACK_INDICES.GetSize()};
        Meshes[RECEIVER_MESH].Normals = ReceiverNormals.GetData();
        Meshes[PANEL_MESH] = {PanelCorners.GetData(),
                              PanelCorners.GetSize(),
                              FRONT_INDICES.GetData(),
                              FRONT_INDICES.GetSize()};
        Meshes[PANEL_MESH].Normals = FrontNormals.GetData();
        Meshes[BLOCKER_MESH] = {BlockerCorners.GetData(),
                                BlockerCorners.GetSize(),
                                TWO_SIDED_INDICES.GetData(),
                                TWO_SIDED_INDICES.GetSize()};
        Meshes[BLOCKER_MESH].Normals = FrontNormals.GetData();
    }
    ReflectionGeometry(const ReflectionGeometry&) = delete;
    ReflectionGeometry& operator=(const ReflectionGeometry&) = delete;
};

/// Translation of the blocker's placement in a state.
math::Affine3 GetBlockerPlacement(const SceneState& state)
{
    math::Affine3 placement = math::Affine3::Identity();
    placement.Translation.X = state.BlockerCenterWorld;
    return placement;
}

renderer::DirectLight MakeLight()
{
    renderer::DirectLight light;
    light.SourceId = 1;
    light.Kind = renderer::LightKind::Directional;
    light.Intensity = LIGHT_ILLUMINANCE;
    light.Direction = LIGHT_TRAVEL_WORLD;
    return light;
}

// --- Render graph routes ---

struct Device final
{
    RhiGuard Session;
    rhi::DeviceHandle Handle{};
    rhi::SurfaceHandle Surface{};
    bool HasRays = false;
    explicit Device(bool shouldRequestRays, bool shouldEnableGpuTiming = false)
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Required.Compute = true;
        description.Preferred.RayQueries = shouldRequestRays;
        description.EnableGpuTiming = shouldEnableGpuTiming;
        description.Storage.ResourcesPerGraph = renderer::VISIBILITY_GRAPH_RESOURCE_CAPACITY;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus started =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Handle, Surface, &failure);
        if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            SKIP("No GPU adapter on this host");
        }
        REQUIRE(started == rhi::DeviceStatus::Ready);
        rhi::RayCapabilities capabilities;
        HasRays = rhi::GetRayCapabilities(Handle, capabilities) == S::Ready;
    }
};

/// RenderGraph inputs of every state; Update copies them, so one table serves each revision.
struct GraphScene final
{
    ReflectionGeometry Geometry;
    StaticArray<uint32, MATERIAL_COUNT> MaterialIndices{{RECEIVER_MESH, PANEL_MESH, BLOCKER_MESH}};
    StaticArray<renderer::RenderGraphMesh, MESH_COUNT> Meshes{};
    StaticArray<renderer::RenderGraphItem, MESH_COUNT> Items{};
    StaticArray<renderer::SceneShaderMaterial, MATERIAL_COUNT> Materials{};
    renderer::DirectLight Light = MakeLight();
    renderer::SceneShaderInputs Inputs{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    GraphScene()
    {
        for (usize mesh = 0; mesh < MESH_COUNT; ++mesh)
        {
            Meshes[mesh].Geometry = Geometry.Meshes[mesh];
            Meshes[mesh].MaterialBindings = {&MaterialIndices[mesh], 1};
            Items[mesh].SourceId = mesh + 1;
            Items[mesh].MeshIndex = static_cast<uint32>(mesh);
        }
        for (renderer::SceneShaderMaterial& material : Materials)
        {
            material.BaseColor = {0, 0, 0};
            material.Roughness = 1;
            material.Metallic = 0;
            material.Scattering.Model = format::MaterialModel::Diffuse;
        }
        Materials[RECEIVER_MESH].BaseColor = {RECEIVER_REFLECTANCE, RECEIVER_REFLECTANCE, RECEIVER_REFLECTANCE};
        Materials[RECEIVER_MESH].Metallic = 1;
        Materials[RECEIVER_MESH].Scattering.Model = format::MaterialModel::MetallicRoughness;
        Materials[PANEL_MESH].BaseColor = {PANEL_ALBEDO, PANEL_ALBEDO, PANEL_ALBEDO};
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        Inputs.Lights = {&Light, 1};
        Cameras[0].OrthographicCamera = true;
        Cameras[0].CameraForward = {0, 0, 1};
        Cameras[0].CameraPosition = {0, 0, -1};
    }
    void SetState(const SceneState& state)
    {
        Materials[RECEIVER_MESH].Roughness = state.Roughness;
        Items[BLOCKER_MESH].MeshToWorld = GetBlockerPlacement(state);
    }
};

S Poll(renderer::RenderGraph& inoutGraph, S status)
{
    const uint64 beginning = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        threading::SleepFor(time::Milliseconds(1));
        status = inoutGraph.Poll();
    }
    return status;
}

void DrawGraphAndWait(const Device& device, renderer::RenderGraph& inoutGraph)
{
    rhi::SubmissionToken completion;
    REQUIRE(inoutGraph.Render(device.Surface, WIDTH, HEIGHT, completion) == S::Ready);
    const uint64 beginning = time::NowTicks();
    S status = rhi::GetStatus(device.Handle, completion);
    while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        threading::YieldThread();
        status = rhi::GetStatus(device.Handle, completion);
    }
    REQUIRE(status == S::Ready);
}

/// Read the red channel of an exported linear RGBA8 resource, after the latest drawn frame.
void ReadExport(const Device& device, renderer::RenderGraph& inoutGraph, uint32 resource, LinearImage& outImage)
{
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(inoutGraph.GetExport(resource, texture, aspect) == S::Ready);
    rhi::ReadbackTicket ticket;
    const uint64 beginning = time::NowTicks();
    S status = rhi::RequestTextureReadback(device.Handle,
                                           texture,
                                           {0, 0, 0, WIDTH, HEIGHT},
                                           static_cast<usize>(WIDTH) * CHANNEL_COUNT,
                                           ticket);
    while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        threading::YieldThread();
        status = rhi::GetStatus(device.Handle, ticket);
    }
    REQUIRE(status == S::Ready);
    StaticArray<uint8, PIXEL_BYTES> pixels{};
    REQUIRE(rhi::CopyReadback(device.Handle, ticket, pixels.GetData(), pixels.GetSize()) == S::Ready);
    REQUIRE(rhi::Release(device.Handle, ticket) == S::Ready);
    for (usize pixel = 0; pixel < outImage.GetSize(); ++pixel)
    {
        outImage[pixel] = static_cast<float32>(pixels[pixel * CHANNEL_COUNT]) / MAXIMUM_CODE;
    }
}

S UpdateGraphState(renderer::RenderGraph& inoutGraph,
                   GraphScene& inoutScene,
                   const SceneState& state,
                   uint64 revision,
                   renderer::RenderGraphActivation* activationOrNull = nullptr)
{
    inoutScene.SetState(state);
    renderer::RenderGraphReport report;
    const BorrowedView<const renderer::RenderGraphMesh> meshes{inoutScene.Meshes.GetData(),
                                                               inoutScene.Meshes.GetSize()};
    const BorrowedView<const renderer::RenderGraphItem> items{inoutScene.Items.GetData(), inoutScene.Items.GetSize()};
    const BorrowedView<const renderer::ViewDescription> cameras{inoutScene.Cameras.GetData(),
                                                                inoutScene.Cameras.GetSize()};
    const S updated = activationOrNull != nullptr
                          ? activationOrNull->Update(meshes,
                                                     items,
                                                     cameras,
                                                     revision,
                                                     renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                     report,
                                                     &inoutScene.Inputs,
                                                     revision)
                          : inoutGraph.Update(meshes,
                                              items,
                                              cameras,
                                              revision,
                                              renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                              report,
                                              &inoutScene.Inputs,
                                              revision);
    const S status = Poll(inoutGraph, updated);
    if (status == S::Ready)
    {
        REQUIRE(inoutGraph.GetDrawableSceneRevision() == revision);
    }
    return status;
}

constexpr PackageLocation RASTER_PACKAGE{LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"};
constexpr PackageLocation REFLECTIONS_PACKAGE{LUDUS_RG3_HYBRID_REFLECTIONS_LINEAR_DIRECTORY,
                                              "rg2_hybrid_reflections_linear.lurgrt"};
/// The visibility raster producer, then the shade ray pass; Accumulate fuses into shade.
constexpr usize REFLECTIONS_PASS_COUNT = 2;

// --- RT1 reference ---

/// Sample counts of the RT1 captures; the CPU oracle's are fewer because it is far slower.
constexpr uint32 GPU_REFERENCE_SAMPLES = 1024;
constexpr uint32 CPU_REFERENCE_SAMPLES = 256;
/// States the CPU oracle captures: the smoothest control and the last motion step. GPU compute captures every state.
using StateMask = StaticArray<bool, STATE_COUNT>;
constexpr StateMask ALL_STATES{{true, true, true, true, true, true}};
constexpr StateMask CPU_ORACLE_STATES{{true, false, false, false, false, true}};
/// The receiver and the panel: the reflection path's two scattering events.
constexpr uint32 REFERENCE_DEPTH = 2;

/// RT1 radiance (CPU oracle or GPU compute) of the selected states, two scattering events with no roulette.
void RenderReferenceImages(const Device& device,
                           renderer::ReferenceTraversal traversal,
                           uint32 samples,
                           const StateMask& states,
                           StaticArray<LinearImage, STATE_COUNT>& outImages)
{
    constexpr usize BUDGET_BYTES = usize{64} * 1024 * 1024;
    /// The largest CPU tile, so each Advance does the most work; GPU compute ignores it.
    constexpr uint32 TILE_PIXELS = 2048;
    renderer::Renderer reference;
    REQUIRE(reference.Initialize(device.Handle, shaders::renderer_flat::Vertex(), shaders::renderer_flat::Fragment()) ==
            S::Ready);
    const renderer::ReferenceRenderingOptions options{traversal, BUDGET_BYTES, 0, TILE_PIXELS, BUDGET_BYTES};
    REQUIRE((traversal == renderer::ReferenceTraversal::GpuCompute
                 ? reference.EnableReferenceRendering(shaders::renderer_reference::Compute(),
                                                      shaders::renderer_reference_reduce::Compute(),
                                                      options)
                 : reference.EnableReferenceRendering({}, options)) == S::Ready);
    REQUIRE(reference.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) ==
            S::Ready);
    REQUIRE(reference.InitializeLighting(shaders::renderer_output::Vertex(),
                                         shaders::renderer_output::Fragment(),
                                         shaders::renderer_shadow::Fragment()) == S::Ready);
    renderer::MaterialProgram program;
    S status = reference.PrewarmLitProgram(shaders::renderer_lit::Vertex(),
                                           shaders::renderer_lit::Fragment(),
                                           shaders::renderer_lit_shadow::Fragment(),
                                           program);
    uint64 beginning = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        status = reference.GetStatus(program);
    }
    REQUIRE(status == S::Ready);
    const ReflectionGeometry geometry;
    StaticArray<renderer::Mesh, MESH_COUNT> meshes{};
    for (usize mesh = 0; mesh < meshes.GetSize(); ++mesh)
    {
        status = reference.CreateMesh(geometry.Meshes[mesh], meshes[mesh]);
        beginning = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
        {
            status = reference.GetStatus(meshes[mesh]);
        }
        REQUIRE(status == S::Ready);
    }
    // The panel, the blocker, then one receiver material per roughness.
    constexpr usize REFERENCE_MATERIAL_COUNT = 2 + ROUGHNESS_COUNT;
    constexpr usize PANEL_MATERIAL = 0;
    constexpr usize BLOCKER_MATERIAL = 1;
    constexpr usize FIRST_RECEIVER_MATERIAL = 2;
    StaticArray<renderer::MaterialVersion, REFERENCE_MATERIAL_COUNT> materials{};
    for (usize material = 0; material < REFERENCE_MATERIAL_COUNT; ++material)
    {
        renderer::LitMaterialDescription description;
        description.Roughness = 1;
        description.Metallic = 0;
        description.Scattering.Model = format::MaterialModel::Diffuse;
        description.Surface.Tint = {0, 0, 0, 1};
        if (material == PANEL_MATERIAL)
        {
            description.Surface.Tint = {PANEL_ALBEDO, PANEL_ALBEDO, PANEL_ALBEDO, 1};
        }
        else if (material >= FIRST_RECEIVER_MATERIAL)
        {
            description.Surface.Tint = {RECEIVER_REFLECTANCE, RECEIVER_REFLECTANCE, RECEIVER_REFLECTANCE, 1};
            description.Roughness = ROUGHNESS_VALUES[material - FIRST_RECEIVER_MATERIAL];
            description.Metallic = 1;
            description.Scattering.Model = format::MaterialModel::MetallicRoughness;
        }
        renderer::LightSelection selection;
        status = reference.CreateLitMaterial(program, description, materials[material], selection);
        beginning = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
        {
            status = reference.GetStatus(materials[material]);
        }
        REQUIRE(status == S::Ready);
    }
    const renderer::DirectLight light = MakeLight();
    renderer::ReferenceCaptureDescription capture;
    capture.Width = WIDTH;
    capture.Height = HEIGHT;
    capture.OrthographicCamera = true;
    capture.CenterRays = true;
    capture.TargetSamples = samples;
    capture.MaxDepth = REFERENCE_DEPTH;
    capture.RouletteDepth = REFERENCE_DEPTH;
    capture.Lights = &light;
    capture.LightCount = 1;
    Array<renderer::ReferencePixel> pixels;
    REQUIRE(pixels.TryResize(PIXEL_COUNT) == true);
    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        if (states[state] == false)
        {
            continue;
        }
        StaticArray<renderer::SceneItem, MESH_COUNT> items{};
        for (usize item = 0; item < MESH_COUNT; ++item)
        {
            items[item].Geometry = meshes[item];
            items[item].SourceId = item + 1;
        }
        items[RECEIVER_MESH].Surface = materials[FIRST_RECEIVER_MATERIAL + GetRoughnessIndex(STATES[state])];
        items[PANEL_MESH].Surface = materials[PANEL_MATERIAL];
        items[BLOCKER_MESH].Surface = materials[BLOCKER_MATERIAL];
        items[BLOCKER_MESH].Transform = GetBlockerPlacement(STATES[state]);
        renderer::Snapshot snapshot;
        REQUIRE(reference.CreateSnapshot(items.GetData(), items.GetSize(), snapshot) == S::Ready);
        renderer::ReferenceCapture handle;
        status = reference.BeginReferenceCapture(snapshot, capture, handle);
        REQUIRE((status == S::Ready || status == S::Pending));
        beginning = time::NowTicks();
        status = S::Pending;
        while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
        {
            threading::YieldThread();
            status = reference.AdvanceReferenceCapture(handle, device.Surface);
        }
        REQUIRE(status == S::Ready);
        REQUIRE(reference.CopyReferencePixels(handle, pixels.GetData(), pixels.GetSize()) == S::Ready);
        for (usize pixel = 0; pixel < pixels.GetSize(); ++pixel)
        {
            REQUIRE(pixels[pixel].Samples >= samples);
            // Linear RGBA8 clamps the graphs' radiance; clamp the reference the same way.
            outImages[state][pixel] = math::Min(pixels[pixel].Radiance.X, 1.0F);
        }
        REQUIRE(reference.Release(handle) == S::Ready);
        static_cast<void>(reference.Release(snapshot));
    }
    for (renderer::MaterialVersion& material : materials)
    {
        static_cast<void>(reference.Release(material));
    }
    for (renderer::Mesh& mesh : meshes)
    {
        static_cast<void>(reference.Release(mesh));
    }
}

/// Largest per-pixel difference of two images and their mean signed difference.
struct ImageDifference final
{
    float32 MaximumAbsolute = 0;
    float64 MeanSigned = 0;
};

ImageDifference Compare(const LinearImage& first, const LinearImage& second)
{
    ImageDifference result;
    float64 sum = 0;
    for (usize pixel = 0; pixel < first.GetSize(); ++pixel)
    {
        const float32 difference = first[pixel] - second[pixel];
        sum += Widen(difference);
        result.MaximumAbsolute = math::Max(result.MaximumAbsolute, math::Abs(difference));
    }
    result.MeanSigned = sum / static_cast<float64>(first.GetSize());
    return result;
}

/// Mean of the oracle over its probes.
float64 GetMean(const ProbeImage& probes)
{
    float64 sum = 0;
    for (const float32 value : probes)
    {
        sum += Widen(value);
    }
    return sum / static_cast<float64>(PROBE_COUNT);
}
} // namespace

TEST_CASE("Rough reflection oracle converges and shows the offscreen panel with its blocker and shadow",
          "[hybrid-reflections]")
{
    // Halving the strata per side changes no probe by more than the convergence tolerance.
    float64 largestDifference = 0;
    for (const usize state : {usize{0}, usize{1}})
    {
        const ProbeImage fine = MakeOracleProbes(STATES[state]);
        const ProbeImage coarse = MakeOracleProbes(STATES[state], ORACLE_SIDE / 2);
        for (usize probe = 0; probe < PROBE_COUNT; ++probe)
        {
            largestDifference = math::Max(largestDifference, math::Abs(Widen(fine[probe]) - Widen(coarse[probe])));
        }
    }
    WARN("largest oracle change from halving the strata per side: " << largestDifference);
    CHECK(largestDifference <= ORACLE_CONVERGENCE_TOLERANCE);

    // The smoothest state shows the lit panel, the blocker's silhouette and its shadow along the image's centre row.
    const SceneState& smooth = STATES[0];
    const float64 lit = ComputeReflectedRadiance(smooth, {-.8, 0, Widen(RECEIVER_DEPTH_WORLD)}, ORACLE_SIDE);
    const float64 silhouette =
        ComputeReflectedRadiance(smooth,
                                 {Widen(smooth.BlockerCenterWorld), 0, Widen(RECEIVER_DEPTH_WORLD)},
                                 ORACLE_SIDE);
    constexpr float64 SHADOW_OFFSET_WORLD = .5;
    const float64 shadow = ComputeReflectedRadiance(
        smooth,
        {Widen(smooth.BlockerCenterWorld) - SHADOW_OFFSET_WORLD, 0, Widen(RECEIVER_DEPTH_WORLD)},
        ORACLE_SIDE);
    INFO("lit " << lit << ", silhouette " << silhouette << ", shadow " << shadow << ", panel radiance "
                << ComputePanelRadiance());
    CHECK(lit > .4);
    CHECK(lit < 1);
    // Roughness 0.2 still blurs some of the lit panel into both 0.3 m wide features.
    constexpr float64 FEATURE_CONTRAST = .15;
    CHECK(silhouette < FEATURE_CONTRAST * lit);
    CHECK(shadow < FEATURE_CONTRAST * lit);
    // Roughness blurs the same features.
    const float64 roughSilhouette =
        ComputeReflectedRadiance(STATES[1],
                                 {Widen(STATES[1].BlockerCenterWorld), 0, Widen(RECEIVER_DEPTH_WORLD)},
                                 ORACLE_SIDE);
    CHECK(roughSilhouette > silhouette);
}

TEST_CASE("RT1 matches the rough reflection oracle", "[gpu][hybrid-reflections][reference-compute]")
{
    const Device device(false);
    StaticArray<LinearImage, STATE_COUNT> gpuImages{};
    RenderReferenceImages(device,
                          renderer::ReferenceTraversal::GpuCompute,
                          GPU_REFERENCE_SAMPLES,
                          ALL_STATES,
                          gpuImages);
    StaticArray<LinearImage, STATE_COUNT> cpuImages{};
    RenderReferenceImages(device,
                          renderer::ReferenceTraversal::CpuOracle,
                          CPU_REFERENCE_SAMPLES,
                          CPU_ORACLE_STATES,
                          cpuImages);
    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        const ProbeImage oracle = MakeOracleProbes(STATES[state]);
        CAPTURE(DescribeState(state));
        for (const bool isGpu : {true, false})
        {
            if (isGpu == false && CPU_ORACLE_STATES[state] == false)
            {
                continue;
            }
            const LinearImage& image = isGpu == true ? gpuImages[state] : cpuImages[state];
            const ImageError error = MeasureProbeError(
                image,
                oracle,
                GetStatisticalTolerance(isGpu == true ? GPU_REFERENCE_SAMPLES : CPU_REFERENCE_SAMPLES));
            ReportError(isGpu == true ? "RT1 GPU compute" : "RT1 CPU oracle", state, error);
            CHECK(math::Abs(error.MeanSigned) <= 4e-3);
            CHECK(error.FalseLight == 0);
            CHECK(error.FalseDarkening == 0);
        }
        if (CPU_ORACLE_STATES[state] == true)
        {
            const ImageDifference pair = Compare(gpuImages[state], cpuImages[state]);
            WARN(DescribeState(state) << ": RT1 GPU - CPU mean signed " << pair.MeanSigned << ", largest "
                                      << pair.MaximumAbsolute);
            CHECK(math::Abs(pair.MeanSigned) <= 3e-3);
        }
    }
}

namespace
{
/// Frames of the hybrid running mean read at each checkpoint after a scene change.
constexpr usize CHECKPOINT_COUNT = 7;
constexpr StaticArray<usize, CHECKPOINT_COUNT> CHECKPOINT_FRAMES{{1, 4, 16, 32, 64, 128, 256}};
/// The declared reconstruction tolerance: RMS error over the pixels a motion step changes.
constexpr float64 RECONSTRUCTED_RMS_TOLERANCE = .02;
/// Pixels whose RT1 radiance changes by more than this between consecutive states form the changed region: about
/// six standard errors of the GPU reference's 1024-sample mean.
constexpr float32 CHANGE_THRESHOLD = 12 * LINEAR_COLOR_CODE;
using PixelMask = StaticArray<bool, PIXEL_COUNT>;

/// RMS error over the pixels the mask selects.
float64 MeasureRegionRms(const LinearImage& image, const LinearImage& expected, const PixelMask& mask)
{
    float64 squares = 0;
    usize count = 0;
    for (usize pixel = 0; pixel < image.GetSize(); ++pixel)
    {
        if (mask[pixel] == true)
        {
            const float64 error = Widen(image[pixel]) - Widen(expected[pixel]);
            squares += error * error;
            ++count;
        }
    }
    return count == 0 ? 0 : math::Sqrt(squares / static_cast<float64>(count));
}

/// Render the checkpoints of one state after its Update: frame 1 is the raw single-sample estimate.
void RenderCheckpoints(const Device& device,
                       renderer::RenderGraph& inoutGraph,
                       uint32 colorResource,
                       StaticArray<LinearImage, CHECKPOINT_COUNT>& outImages)
{
    usize frame = 0;
    for (usize checkpoint = 0; checkpoint < CHECKPOINT_COUNT; ++checkpoint)
    {
        while (frame < CHECKPOINT_FRAMES[checkpoint])
        {
            DrawGraphAndWait(device, inoutGraph);
            ++frame;
        }
        ReadExport(device, inoutGraph, colorResource, outImages[checkpoint]);
    }
}
} // namespace

TEST_CASE("Hybrid rough reflections of offscreen geometry match the oracle and RT1",
          "[gpu][hybrid-reflections][native-ray-queries][reference-compute]")
{
    const Device device(true);
    if (device.HasRays == false)
    {
        SKIP("Native ray queries are unavailable; the portable cases cover RT1 and the raster fallback");
    }
    StaticArray<LinearImage, STATE_COUNT> referenceImages{};
    RenderReferenceImages(device,
                          renderer::ReferenceTraversal::GpuCompute,
                          GPU_REFERENCE_SAMPLES,
                          ALL_STATES,
                          referenceImages);

    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(REFLECTIONS_PACKAGE);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(renderer::RequiresHardwareRayTracing(plan) == true);
    REQUIRE(plan.Passes.GetSize() == REFLECTIONS_PASS_COUNT);
    const uint32 colorResource = plan.Passes[REFLECTIONS_PASS_COUNT - 1].Color;
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    renderer::RenderGraph graph;
    REQUIRE(Poll(graph, graph.Initialize(device.Handle, plan, programs, nullptr, WIDTH, HEIGHT)) == S::Ready);
    GraphScene scene;

    StaticArray<StaticArray<LinearImage, CHECKPOINT_COUNT>, STATE_COUNT> checkpoints{};
    uint64 revision = 0;
    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        CAPTURE(DescribeState(state));
        ++revision;
        REQUIRE(UpdateGraphState(graph, scene, STATES[state], revision) == S::Ready);
        RenderCheckpoints(device, graph, colorResource, checkpoints[state]);
        const ProbeImage oracle = MakeOracleProbes(STATES[state]);

        // Raw: one specular-lobe reflection ray per pixel; its image mean is unbiased against RT1.
        const ImageError raw = MeasureImageError(checkpoints[state][0], referenceImages[state], 1);
        ReportError("Hybrid raw frame against RT1 GPU", state, raw);
        CHECK(math::Abs(raw.MeanSigned) <= .01);

        // Reconstructed: the template's running mean after the last checkpoint, 256 frames.
        constexpr uint32 RECONSTRUCTED_SAMPLES = 256;
        const LinearImage& reconstructed = checkpoints[state][CHECKPOINT_COUNT - 1];
        const ImageError error =
            MeasureProbeError(reconstructed, oracle, GetStatisticalTolerance(RECONSTRUCTED_SAMPLES));
        ReportError("Hybrid accumulated against the oracle", state, error);
        CHECK(math::Abs(error.MeanSigned) <= 5e-3);
        CHECK(error.FalseLight == 0);
        CHECK(error.FalseDarkening == 0);
        // Both images are sample means: the per-pixel tolerance combines their standard errors.
        const float32 pairTolerance =
            GetStatisticalTolerance(RECONSTRUCTED_SAMPLES) + GetStatisticalTolerance(GPU_REFERENCE_SAMPLES);
        const ImageError pair = MeasureImageError(reconstructed, referenceImages[state], pairTolerance);
        ReportError("Hybrid accumulated against RT1 GPU", state, pair);
        CHECK(math::Abs(pair.MeanSigned) <= 4e-3);
        CHECK(pair.FalseLight == 0);
        CHECK(pair.FalseDarkening == 0);

        // Temporal error across each motion step, over the pixels the step changes.
        if (state > ROUGHNESS_CONTROL_COUNT)
        {
            PixelMask changed{};
            usize changedCount = 0;
            for (usize pixel = 0; pixel < changed.GetSize(); ++pixel)
            {
                changed[pixel] =
                    math::Abs(referenceImages[state][pixel] - referenceImages[state - 1][pixel]) > CHANGE_THRESHOLD;
                changedCount += changed[pixel] == true ? 1 : 0;
            }
            REQUIRE(changedCount > 0);
            usize framesToTolerance = 0;
            for (usize checkpoint = 0; checkpoint < CHECKPOINT_COUNT; ++checkpoint)
            {
                const float64 rms = MeasureRegionRms(checkpoints[state][checkpoint], referenceImages[state], changed);
                const float64 staleRms =
                    MeasureRegionRms(checkpoints[state][checkpoint], referenceImages[state - 1], changed);
                WARN(DescribeState(state)
                     << ", frame " << CHECKPOINT_FRAMES[checkpoint] << ": changed-region RMS " << rms
                     << " against the new RT1 image, " << staleRms << " against the previous state's");
                if (framesToTolerance == 0 && rms <= RECONSTRUCTED_RMS_TOLERANCE)
                {
                    framesToTolerance = CHECKPOINT_FRAMES[checkpoint];
                }
            }
            WARN(DescribeState(state) << ": " << changedCount << " changed pixels reach RMS "
                                      << RECONSTRUCTED_RMS_TOLERANCE << " by frame " << framesToTolerance);
            CHECK(framesToTolerance != 0);
        }
    }

    // History rejection: a fresh graph given only the last state's revision reproduces the moved graph's frames.
    renderer::RenderGraph fresh;
    REQUIRE(Poll(fresh, fresh.Initialize(device.Handle, plan, programs, nullptr, WIDTH, HEIGHT)) == S::Ready);
    GraphScene freshScene;
    REQUIRE(UpdateGraphState(fresh, freshScene, STATES[STATE_COUNT - 1], revision) == S::Ready);
    StaticArray<LinearImage, CHECKPOINT_COUNT> replay{};
    RenderCheckpoints(device, fresh, colorResource, replay);
    for (usize checkpoint = 0; checkpoint < CHECKPOINT_COUNT; ++checkpoint)
    {
        CAPTURE(CHECKPOINT_FRAMES[checkpoint]);
        const ImageDifference difference = Compare(replay[checkpoint], checkpoints[STATE_COUNT - 1][checkpoint]);
        CHECK(difference.MaximumAbsolute == 0);
    }
}

namespace
{
// A ground grid seen at a grazing angle through a perspective camera with a centimetre near plane, as the Editor's
// World viewport draws terrain. A graph with visibility-buffer consumers draws in conventional depth, whose float32
// texels place a pixel's surface centimetres along its ray at tens of metres. At a grazing angle that error lies
// almost entirely in the ground plane, so a first hit rebuilt from depth slides off the triangle the raster stored
// at its edges; with the 0.001 barycentric tolerance this rebuild once used, 84 pixels of this view failed. The
// first surface must instead lie on the pixel-centre camera ray. The camera stands near the world origin, where its
// float32 clip-to-world matrix keeps camera rays on the raster's projection.
constexpr uint32 GROUND_CELLS_PER_SIDE = 128;
constexpr float32 GROUND_CELL_WORLD = .25F;
constexpr uint32 GROUND_VERTICES_PER_SIDE = GROUND_CELLS_PER_SIDE + 1;
constexpr usize GROUND_VERTEX_COUNT = static_cast<usize>(GROUND_VERTICES_PER_SIDE) * GROUND_VERTICES_PER_SIDE;
constexpr usize GROUND_INDEX_COUNT =
    static_cast<usize>(GROUND_CELLS_PER_SIDE) * GROUND_CELLS_PER_SIDE * ONE_SIDED_INDEX_COUNT;
/// The grid spans x in [-16, 16] m and z in [-32, 0] m in quarter-metre cells; the camera stands over its near edge.
constexpr float32 GROUND_HALF_WIDTH_WORLD = GROUND_CELLS_PER_SIDE * GROUND_CELL_WORLD / 2;
constexpr math::Vector3 GRAZING_EYE_WORLD{0, 1.5F, 0};
/// The view centre points about 2 degrees below the horizon, at the ground 40 m ahead beyond the grid's far edge.
constexpr math::Vector3 GRAZING_TARGET_WORLD{0, 0, -40};
constexpr float32 GRAZING_VERTICAL_FOV_RADIANS = 1;
constexpr float32 GRAZING_NEAR_WORLD = .01F;
constexpr float32 GRAZING_FAR_WORLD = 1000;
/// Frames drawn before the failure summary is read; each draws one sample per pixel.
constexpr uint32 GRAZING_FRAMES = 4;
constexpr uint32 SHADE_PASS = REFLECTIONS_PASS_COUNT - 1;

/// The grid's vertices and triangles, two per cell with geometric normal +Y, and one rough dielectric material.
struct GroundScene final
{
    Array<math::Vector3> Positions;
    Array<math::Vector3> Normals;
    Array<uint32> Indices;
    uint32 MaterialIndex = 0;
    renderer::RenderGraphMesh Mesh{};
    renderer::RenderGraphItem Item{};
    renderer::SceneShaderMaterial Material{};
    renderer::DirectLight Light = MakeLight();
    renderer::SceneShaderInputs Inputs{};
    renderer::ViewDescription Camera{};
    GroundScene()
    {
        REQUIRE(Positions.TryResize(GROUND_VERTEX_COUNT) == true);
        REQUIRE(Normals.TryResize(GROUND_VERTEX_COUNT) == true);
        REQUIRE(Indices.TryResize(GROUND_INDEX_COUNT) == true);
        for (uint32 row = 0; row < GROUND_VERTICES_PER_SIDE; ++row)
        {
            for (uint32 column = 0; column < GROUND_VERTICES_PER_SIDE; ++column)
            {
                const usize vertex = static_cast<usize>(row) * GROUND_VERTICES_PER_SIDE + column;
                Positions[vertex] = {static_cast<float32>(column) * GROUND_CELL_WORLD - GROUND_HALF_WIDTH_WORLD,
                                     0,
                                     -static_cast<float32>(row) * GROUND_CELL_WORLD};
                Normals[vertex] = {0, 1, 0};
            }
        }
        usize index = 0;
        for (uint32 row = 0; row < GROUND_CELLS_PER_SIDE; ++row)
        {
            for (uint32 column = 0; column < GROUND_CELLS_PER_SIDE; ++column)
            {
                const uint32 corner = row * GROUND_VERTICES_PER_SIDE + column;
                const uint32 right = corner + 1;
                const uint32 farCorner = corner + GROUND_VERTICES_PER_SIDE;
                // Counterclockwise about +Y; the shared diagonal runs from the cell's corner to its far right.
                const StaticArray<uint32, ONE_SIDED_INDEX_COUNT> cell{
                    {corner, right, farCorner + 1, corner, farCorner + 1, farCorner}};
                for (const uint32 vertex : cell)
                {
                    Indices[index++] = vertex;
                }
            }
        }
        Mesh.Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Mesh.Geometry.Normals = Normals.GetData();
        Mesh.MaterialBindings = {&MaterialIndex, 1};
        Item.SourceId = 1;
        Material.BaseColor = {PANEL_ALBEDO, PANEL_ALBEDO, PANEL_ALBEDO};
        Material.Roughness = MOTION_ROUGHNESS;
        Material.Metallic = 0;
        Material.Scattering.Model = format::MaterialModel::MetallicRoughness;
        Light.Direction = {0, -1, -1};
        Inputs.Materials = {&Material, 1};
        Inputs.Lights = {&Light, 1};
        math::Matrix4 view;
        math::Matrix4 projection;
        REQUIRE(math::TryLookAt(GRAZING_EYE_WORLD, GRAZING_TARGET_WORLD, {0, 1, 0}, view) == math::MathStatus::Success);
        REQUIRE(math::TryPerspectiveReverseZ(GRAZING_VERTICAL_FOV_RADIANS,
                                             static_cast<float32>(WIDTH) / static_cast<float32>(HEIGHT),
                                             GRAZING_NEAR_WORLD,
                                             GRAZING_FAR_WORLD,
                                             projection) == math::MathStatus::Success);
        Camera.WorldToClip = projection * view;
        Camera.Depth = renderer::DepthConvention::ReverseZ;
        Camera.CameraPosition = GRAZING_EYE_WORLD;
        Camera.CameraForward = GRAZING_TARGET_WORLD - GRAZING_EYE_WORLD;
    }
    GroundScene(const GroundScene&) = delete;
    GroundScene& operator=(const GroundScene&) = delete;
};

/// Poll a graph's failure summary of one ray pass until its read-back completes.
renderer::RenderGraphRayFailures ReadRayFailures(renderer::RenderGraph& inoutGraph, uint32 pass)
{
    renderer::RenderGraphRayFailures failures;
    const uint64 beginning = time::NowTicks();
    S status = inoutGraph.PollRayFailures(pass, failures);
    while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        threading::YieldThread();
        status = inoutGraph.PollRayFailures(pass, failures);
    }
    REQUIRE(status == S::Ready);
    return failures;
}
} // namespace

TEST_CASE("Hybrid reflections rebuild grazing first hits at triangle edges without failures",
          "[render-graph][gpu][hybrid-reflections][native-ray-queries]")
{
    const Device device(true);
    if (device.HasRays == false)
    {
        SKIP("Native ray queries are unavailable");
    }
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(REFLECTIONS_PACKAGE);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(plan.Passes.GetSize() == REFLECTIONS_PASS_COUNT);
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    renderer::RenderGraph graph;
    REQUIRE(Poll(graph, graph.Initialize(device.Handle, plan, programs, nullptr, WIDTH, HEIGHT)) == S::Ready);
    // Visibility consumers rebuild positions in conventional depth, so the graph converts the reverse-Z camera.
    REQUIRE(graph.CanUseReverseDepth() == false);
    GroundScene scene;
    renderer::RenderGraphReport report;
    constexpr uint64 REVISION = 1;
    const S updated = graph.Update({&scene.Mesh, 1},
                                   {&scene.Item, 1},
                                   {&scene.Camera, 1},
                                   REVISION,
                                   renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                   report,
                                   &scene.Inputs,
                                   REVISION);
    REQUIRE(Poll(graph, updated) == S::Ready);
    for (uint32 frame = 0; frame < GRAZING_FRAMES; ++frame)
    {
        DrawGraphAndWait(device, graph);
    }
    // The ground fills much of the lower half of the image; the black environment fills the rest.
    LinearImage image{};
    ReadExport(device, graph, plan.Passes[SHADE_PASS].Color, image);
    usize litPixels = 0;
    for (const float32 value : image)
    {
        litPixels += value > 0 ? 1 : 0;
    }
    CHECK(litPixels > PIXEL_COUNT / 4);
    const renderer::RenderGraphRayFailures failures = ReadRayFailures(graph, SHADE_PASS);
    CAPTURE(failures.MainReason, failures.Pixels[static_cast<usize>(renderer::RenderGraphFailureReason::StoredRecord)]);
    CHECK(failures.TotalPixels == 0);
}

TEST_CASE("RT2 raster fallback has no reflection of offscreen geometry", "[render-graph][gpu][hybrid-reflections]")
{
    // Vulkan can run the automatic fallback with native queries unrequested. Metal may still expose them,
    // so exercise the documented ForceRaster selection on that backend.
    const Device device(false);
    const bool isAutomatic = device.HasRays == false;
    core::UniquePtr<renderer::RenderGraphPackage> raster = LoadPackage(RASTER_PACKAGE);
    renderer::RenderGraphActivation activation;
    REQUIRE(activation.Initialize(device.Handle,
                                  raster->GetInterfaceIdentity(),
                                  shaders::renderer_composite::Vertex(),
                                  shaders::renderer_composite::Fragment()) == S::Ready);
    REQUIRE(activation.Resize(WIDTH, HEIGHT) == S::Ready);
    const renderer::RenderGraphPreference preference =
        isAutomatic == true ? renderer::RenderGraphPreference::Automatic : renderer::RenderGraphPreference::ForceRaster;
    S status = activation.StageSelected(core::Move(raster), LoadPackage(REFLECTIONS_PACKAGE), preference);
    const uint64 beginning = time::NowTicks();
    while ((status == S::Pending || status == S::Ready) && activation.GetActiveOrNull() == nullptr &&
           time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        threading::SleepFor(time::Milliseconds(1));
        status = activation.Poll();
    }
    REQUIRE(status == S::Ready);
    renderer::RenderGraph* graphOrNull = activation.GetActiveOrNull();
    REQUIRE(graphOrNull != nullptr);
    REQUIRE(activation.GetReport().ActiveRenderer == renderer::RenderGraphRenderer::Raster);
    const std::string_view route = isAutomatic == true ? "Automatic raster fallback" : "ForceRaster";

    // The default graph lights a surface with analytic lights, mesh emitters, the environment and the sky, and the
    // receiver sees none of them: the light reaches only its back, and the environment is black. It reflects no scene
    // geometry, so the offscreen panel is missing from the receiver.
    GraphScene scene;
    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        CAPTURE(DescribeState(state));
        REQUIRE(UpdateGraphState(*graphOrNull, scene, STATES[state], state + 1, &activation) == S::Ready);
        DrawGraphAndWait(device, *graphOrNull);
        constexpr uint32 COLOR_EXPORT = 0;
        LinearImage image{};
        ReadExport(device, *graphOrNull, COLOR_EXPORT, image);
        const ProbeImage oracle = MakeOracleProbes(STATES[state]);
        const ImageError error = MeasureProbeError(image, oracle, LINEAR_COLOR_CODE);
        ReportError(route, state, error);
        WARN(route << ", " << DescribeState(state) << ": oracle mean reflected radiance " << GetMean(oracle));
        float32 brightest = 0;
        for (const float32 value : image)
        {
            brightest = math::Max(brightest, value);
        }
        CHECK(brightest <= LINEAR_COLOR_CODE);
        CHECK(error.FalseLight == 0);
    }
}

namespace
{
constexpr usize TIMING_SAMPLE_COUNT = 21;
constexpr usize TIMING_TAIL_PERCENTILE = 95;
constexpr usize PERCENTILE_DENOMINATOR = 100;
using TimingSamples = StaticArray<float64, TIMING_SAMPLE_COUNT>;

void ReportTimings(std::string_view phase, TimingSamples& inoutSamples)
{
    Sort(inoutSamples.GetData(), inoutSamples.GetSize(), [](float64 left, float64 right) noexcept {
        return left < right;
    });
    constexpr usize TAIL_INDEX =
        (TIMING_TAIL_PERCENTILE * TIMING_SAMPLE_COUNT + PERCENTILE_DENOMINATOR - 1) / PERCENTILE_DENOMINATOR - 1;
    const float64 median = inoutSamples[TIMING_SAMPLE_COUNT / 2] / time::NANOSECONDS_PER_MILLISECOND;
    const float64 tail = inoutSamples[TAIL_INDEX] / time::NANOSECONDS_PER_MILLISECOND;
    WARN(phase << ": " << TIMING_SAMPLE_COUNT << " samples, p50 " << median << " ms, p95 " << tail << " ms");
}
} // namespace

TEST_CASE("Measure RT2 reflection scene preparation and ray pass latency", "[.benchmark][hybrid-reflections]")
{
    const Device device(/*shouldRequestRays=*/true, /*shouldEnableGpuTiming=*/true);
    if (device.HasRays == false)
    {
        SKIP("Native inline ray queries unavailable");
    }
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(REFLECTIONS_PACKAGE);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(plan.Passes.GetSize() == REFLECTIONS_PASS_COUNT);
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    renderer::RenderGraph graph;
    REQUIRE(Poll(graph, graph.Initialize(device.Handle, plan, programs, nullptr, WIDTH, HEIGHT)) == S::Ready);
    GraphScene scene;
    uint64 revision = 1;
    SceneState state = STATES[STATE_COUNT - 1];
    REQUIRE(UpdateGraphState(graph, scene, state, revision) == S::Ready);
    DrawGraphAndWait(device, graph);

    TimingSamples preparation{};
    constexpr float32 MOTION_STEP_METERS = .001F;
    for (float64& durationNanoseconds : preparation)
    {
        // Move the blocker's placement, as the motion fixture does, so each update rebuilds the instance data.
        state.BlockerCenterWorld += MOTION_STEP_METERS;
        const uint64 begun = time::NowTicks();
        REQUIRE(UpdateGraphState(graph, scene, state, ++revision) == S::Ready);
        durationNanoseconds = static_cast<float64>(time::NowTicks() - begun);
        DrawGraphAndWait(device, graph);
    }
    ReportTimings("Blocker move to drawable scene (CPU wall time, includes acceleration update and polling)",
                  preparation);

    TimingSamples shade{};
    for (usize sample = 0; sample < TIMING_SAMPLE_COUNT; ++sample)
    {
        const S armed = rhi::ArmGpuPassTimings(device.Handle);
        if (armed == S::Unsupported)
        {
            SKIP("Per-pass GPU timing is unavailable");
        }
        REQUIRE(armed == S::Ready);
        DrawGraphAndWait(device, graph);
        rhi::GpuPassTimings timings;
        const uint64 begun = time::NowTicks();
        REQUIRE(rhi::GetGpuPassTimings(device.Handle, timings) == S::Ready);
        while (timings.State == rhi::GpuTimingState::Pending && time::NowTicks() - begun < TIMEOUT_NANOSECONDS)
        {
            threading::YieldThread();
            REQUIRE(rhi::GetGpuPassTimings(device.Handle, timings) == S::Ready);
        }
        REQUIRE(timings.State == rhi::GpuTimingState::Available);
        REQUIRE(timings.DroppedPasses == 0);
        // The shade pass, with its shadow rays, reflection ray and fused running mean, is the one compute encoder.
        usize computePasses = 0;
        for (usize index = 0; index < timings.Count; ++index)
        {
            const rhi::GpuPassTiming& pass = timings.Passes[index];
            if (pass.Kind == rhi::GpuPassKind::Compute)
            {
                REQUIRE(computePasses == 0);
                REQUIRE(pass.Timing.State == rhi::GpuTimingState::Available);
                REQUIRE(math::IsFinite(pass.Timing.DurationNanoseconds) == true);
                shade[sample] = pass.Timing.DurationNanoseconds;
                ++computePasses;
            }
        }
        REQUIRE(computePasses == 1);
    }
    ReportTimings("Shade with shadow rays, a reflection ray and the running mean (native GPU duration)", shade);
}
