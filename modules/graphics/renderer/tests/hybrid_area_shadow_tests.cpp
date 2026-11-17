// RT2 partial area-light shadows (issue 1248): a planar Lambertian receiver lit by one rectangular one-sided mesh
// emitter, with two opaque planar blockers at different depths. A short motion of the far blocker takes the two
// occluder projections, seen from the receiver, from separate to overlapping. The fixture oracle projects both
// blockers from each receiver point onto the emitter plane, clips them to the emitter and subtracts their union, then
// integrates the receiver cosine over the visible solid angle in closed form. The RT1 reference (CPU oracle and GPU
// compute), the tiled vbuffer NEE template (raster first hits, ray-traced emitter visibility, Accumulate) and the
// default raster graph selected as the RT2 fallback (cube shadow maps of one point per caster) are compared with it.
// Thanks to George Drettakis and Eugene Fiume, "A Fast Shadow Algorithm for Area Light Sources Using Backprojection",
// SIGGRAPH 1994, pp. 223-230, Section 2, for computing the visible part of an area emitter from a receiver point and
// evaluating the diffuse illumination of that visible part. https://www-sop.inria.fr/reves/Basilic/1994/DF94/sig94.pdf

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

/// A float32 scene value widened for the float64 oracle.
constexpr float64 Widen(float32 value)
{
    return static_cast<float64>(value);
}

// The orthographic identity camera looks along +Z from its near plane z = 0, so the image spans x and y in [-1, 1] of
// the receiver. The emitter and both blockers lie behind the near plane: every pixel sees the receiver.
constexpr float32 RECEIVER_DEPTH_WORLD = .5F;
constexpr StaticArray<math::Vector3, 4> RECEIVER_CORNERS_WORLD{{{-3, -2, RECEIVER_DEPTH_WORLD},
                                                                {3, -2, RECEIVER_DEPTH_WORLD},
                                                                {3, 2, RECEIVER_DEPTH_WORLD},
                                                                {-3, 2, RECEIVER_DEPTH_WORLD}}};
/// Wound so the geometric normal faces the camera, -Z.
constexpr StaticArray<uint32, 6> RECEIVER_INDICES{{0, 2, 1, 0, 3, 2}};
constexpr float32 RECEIVER_ALBEDO = .8F;
/// A square emitter facing +Z, towards the receiver; one-sided, so the raster fallback places its caster in front.
constexpr float32 EMITTER_DEPTH_WORLD = -.6F;
constexpr float32 EMITTER_HALF_EXTENT_WORLD = .4F;
/// Linear radiance in nits; it puts the unoccluded receiver centre near 0.5, inside linear RGBA8.
constexpr float32 EMITTER_RADIANCE = 4.4F;
/// The near blocker, 0.25 m in front of the emitter.
constexpr float32 NEAR_BLOCKER_DEPTH_WORLD = -.35F;
constexpr math::Vector2 NEAR_BLOCKER_MINIMUM_WORLD{-.5F, -.25F};
constexpr math::Vector2 NEAR_BLOCKER_MAXIMUM_WORLD{-.1F, .25F};
/// The far blocker, nearer the receiver, moves along +X through its placement's translation.
constexpr float32 FAR_BLOCKER_DEPTH_WORLD = -.1F;
constexpr math::Vector2 FAR_BLOCKER_HALF_EXTENT_WORLD{.15F, .2F};
/// The far blocker's centre X at each motion step: its projection starts beside the near blocker's and ends over it.
constexpr usize MOTION_STEP_COUNT = 4;
constexpr StaticArray<float32, MOTION_STEP_COUNT> FAR_BLOCKER_CENTERS_WORLD{{.6F, .4F, .2F, 0}};

/// Which blockers a scene state contains: the unobstructed and one-blocker controls, then the two-blocker motion.
enum class BlockerSet : uint8
{
    None,
    Near,
    Both,
};
struct SceneState final
{
    BlockerSet Blockers = BlockerSet::None;
    float32 FarBlockerCenterWorld = 0;
};
constexpr usize CONTROL_STATE_COUNT = 2;
constexpr usize STATE_COUNT = CONTROL_STATE_COUNT + MOTION_STEP_COUNT;
using SceneStates = StaticArray<SceneState, STATE_COUNT>;
constexpr SceneStates MakeStates()
{
    SceneStates states{};
    states[0] = {BlockerSet::None, 0};
    states[1] = {BlockerSet::Near, 0};
    for (usize step = 0; step < MOTION_STEP_COUNT; ++step)
    {
        states[CONTROL_STATE_COUNT + step] = {BlockerSet::Both, FAR_BLOCKER_CENTERS_WORLD[step]};
    }
    return states;
}
constexpr SceneStates STATES = MakeStates();
std::string_view DescribeState(usize state)
{
    constexpr StaticArray<std::string_view, STATE_COUNT> NAMES{
        {"unobstructed", "near blocker only", "motion 0", "motion 1", "motion 2", "motion 3"}};
    return NAMES[state];
}

// --- Backprojection oracle, in float64 ---

/// An axis-aligned rectangle of one plane of constant Z.
struct Rectangle final
{
    float64 MinimumX = 0;
    float64 MinimumY = 0;
    float64 MaximumX = 0;
    float64 MaximumY = 0;
};

bool IsEmpty(const Rectangle& rectangle)
{
    return rectangle.MaximumX <= rectangle.MinimumX || rectangle.MaximumY <= rectangle.MinimumY;
}

Rectangle Intersect(const Rectangle& first, const Rectangle& second)
{
    return {math::Max(first.MinimumX, second.MinimumX),
            math::Max(first.MinimumY, second.MinimumY),
            math::Min(first.MaximumX, second.MaximumX),
            math::Min(first.MaximumY, second.MaximumY)};
}

float64 GetArea(const Rectangle& rectangle)
{
    return IsEmpty(rectangle) == true
               ? 0
               : (rectangle.MaximumX - rectangle.MinimumX) * (rectangle.MaximumY - rectangle.MinimumY);
}

Rectangle GetEmitterRectangle()
{
    const float64 halfExtent = Widen(EMITTER_HALF_EXTENT_WORLD);
    return {-halfExtent, -halfExtent, halfExtent, halfExtent};
}

/// One blocker of a state: its rectangle and depth.
struct Blocker final
{
    Rectangle Extent;
    float64 DepthWorld = 0;
};

Blocker GetNearBlocker()
{
    return {{Widen(NEAR_BLOCKER_MINIMUM_WORLD.X),
             Widen(NEAR_BLOCKER_MINIMUM_WORLD.Y),
             Widen(NEAR_BLOCKER_MAXIMUM_WORLD.X),
             Widen(NEAR_BLOCKER_MAXIMUM_WORLD.Y)},
            Widen(NEAR_BLOCKER_DEPTH_WORLD)};
}

Blocker GetFarBlocker(float32 centerWorld)
{
    return {{Widen(centerWorld) - Widen(FAR_BLOCKER_HALF_EXTENT_WORLD.X),
             -Widen(FAR_BLOCKER_HALF_EXTENT_WORLD.Y),
             Widen(centerWorld) + Widen(FAR_BLOCKER_HALF_EXTENT_WORLD.X),
             Widen(FAR_BLOCKER_HALF_EXTENT_WORLD.Y)},
            Widen(FAR_BLOCKER_DEPTH_WORLD)};
}

/// The receiver point at the centre of pixel (column, row), row 0 at the top.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (column, row) is the image's conventional order.
math::Vector3d GetReceiverPointWorld(float64 column, float64 row)
{
    const float64 clipX = 2 * (column + .5) / WIDTH - 1;
    const float64 clipY = 1 - 2 * (row + .5) / HEIGHT;
    return {clipX, clipY, static_cast<float64>(RECEIVER_DEPTH_WORLD)};
}

/// The blocker seen from the receiver point, projected onto the emitter plane. Both planes are parallel to the
/// receiver, so the projection is the rectangle scaled about the point's foot by the ratio of plane distances.
Rectangle ProjectToEmitterPlane(const Blocker& blocker, math::Vector3d receiverWorld)
{
    const float64 scale =
        (static_cast<float64>(EMITTER_DEPTH_WORLD) - receiverWorld.Z) / (blocker.DepthWorld - receiverWorld.Z);
    return {receiverWorld.X + scale * (blocker.Extent.MinimumX - receiverWorld.X),
            receiverWorld.Y + scale * (blocker.Extent.MinimumY - receiverWorld.Y),
            receiverWorld.X + scale * (blocker.Extent.MaximumX - receiverWorld.X),
            receiverWorld.Y + scale * (blocker.Extent.MaximumY - receiverWorld.Y)};
}

/// Integral of cos(receiver) cos(emitter) / distance^2 over [0, x] x [0, y] of a plane parallel to the receiver at
/// the given height above it, measured from the point's foot: pi times the point-to-rectangle form factor. Odd in x
/// and in y, so signed corners combine into any rectangle.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (x, y, height) mirrors the form factor's coordinates.
float64 ComputeCornerProjectedSolidAngle(float64 x, float64 y, float64 height)
{
    const float64 a = x / height;
    const float64 b = y / height;
    const float64 aRoot = math::Sqrt(1 + a * a);
    const float64 bRoot = math::Sqrt(1 + b * b);
    return .5 * (a / aRoot * math::Atan2(b, aRoot) + b / bRoot * math::Atan2(a, bRoot));
}

/// Projected solid angle of an emitter-plane rectangle seen from the receiver point.
float64 ComputeProjectedSolidAngle(const Rectangle& rectangle, math::Vector3d receiverWorld)
{
    if (IsEmpty(rectangle) == true)
    {
        return 0;
    }
    const float64 height = receiverWorld.Z - static_cast<float64>(EMITTER_DEPTH_WORLD);
    const float64 left = rectangle.MinimumX - receiverWorld.X;
    const float64 right = rectangle.MaximumX - receiverWorld.X;
    const float64 bottom = rectangle.MinimumY - receiverWorld.Y;
    const float64 top = rectangle.MaximumY - receiverWorld.Y;
    return ComputeCornerProjectedSolidAngle(right, top, height) - ComputeCornerProjectedSolidAngle(left, top, height) -
           ComputeCornerProjectedSolidAngle(right, bottom, height) +
           ComputeCornerProjectedSolidAngle(left, bottom, height);
}

/// The oracle at one receiver point.
struct OracleSample final
{
    float64 Unoccluded = 0;  ///< Projected solid angle of the whole emitter.
    float64 Visible = 0;     ///< Projected solid angle of the emitter minus the union of the clipped projections.
    float64 OverlapArea = 0; ///< Emitter-plane area where the two clipped projections overlap.
    bool HasFarOcclusion = false;
};

OracleSample EvaluateOracle(const SceneState& state, math::Vector3d receiverWorld)
{
    const Rectangle emitter = GetEmitterRectangle();
    OracleSample result;
    result.Unoccluded = ComputeProjectedSolidAngle(emitter, receiverWorld);
    result.Visible = result.Unoccluded;
    if (state.Blockers == BlockerSet::None)
    {
        return result;
    }
    const Rectangle nearShadow = Intersect(ProjectToEmitterPlane(GetNearBlocker(), receiverWorld), emitter);
    result.Visible -= ComputeProjectedSolidAngle(nearShadow, receiverWorld);
    if (state.Blockers == BlockerSet::Near)
    {
        return result;
    }
    const Rectangle farShadow =
        Intersect(ProjectToEmitterPlane(GetFarBlocker(state.FarBlockerCenterWorld), receiverWorld), emitter);
    const Rectangle overlap = Intersect(nearShadow, farShadow);
    // Inclusion-exclusion over the union: the overlap is subtracted once, not twice.
    result.Visible -= ComputeProjectedSolidAngle(farShadow, receiverWorld);
    result.Visible += ComputeProjectedSolidAngle(overlap, receiverWorld);
    result.OverlapArea = GetArea(overlap);
    result.HasFarOcclusion = IsEmpty(farShadow) == false;
    return result;
}

/// Reflected radiance of the Lambertian receiver from the visible projected solid angle.
float64 ComputeRadiance(float64 projectedSolidAngle)
{
    return static_cast<float64>(RECEIVER_ALBEDO) / math::kPi * static_cast<float64>(EMITTER_RADIANCE) *
           projectedSolidAngle;
}

/// A red-channel linear image of WIDTH by HEIGHT pixels, row 0 at the top.
using LinearImage = StaticArray<float32, static_cast<usize>(WIDTH) * HEIGHT>;

/// The oracle image of one state, clamped like linear RGBA8, and whether each pixel sees overlapping projections.
struct OracleImage final
{
    LinearImage Radiance{};
    StaticArray<bool, static_cast<usize>(WIDTH) * HEIGHT> IsOverlapping{};
};

OracleImage MakeOracleImage(const SceneState& state)
{
    OracleImage result;
    for (usize row = 0; row < HEIGHT; ++row)
    {
        for (usize column = 0; column < WIDTH; ++column)
        {
            const OracleSample sample =
                EvaluateOracle(state, GetReceiverPointWorld(static_cast<float64>(column), static_cast<float64>(row)));
            const usize pixel = row * WIDTH + column;
            result.Radiance[pixel] = static_cast<float32>(math::Min(ComputeRadiance(sample.Visible), 1.0));
            result.IsOverlapping[pixel] = sample.OverlapArea > 0;
        }
    }
    return result;
}

/// Signed and absolute error of one image against the oracle, over all pixels and over the overlap pixels.
struct ImageError final
{
    float64 MeanSigned = 0;
    float64 Rms = 0;
    float32 MaximumAbsolute = 0;
    usize FalseLight = 0;     ///< Pixels brighter than the oracle by more than the tolerance.
    usize FalseDarkening = 0; ///< Pixels darker than the oracle by more than the tolerance.
    usize OverlapPixels = 0;
    float64 OverlapMeanSigned = 0;
};

ImageError MeasureError(const LinearImage& image, const OracleImage& oracle, float32 tolerance)
{
    ImageError result;
    float64 sum = 0;
    float64 squares = 0;
    float64 overlapSum = 0;
    for (usize pixel = 0; pixel < image.GetSize(); ++pixel)
    {
        const float32 error = image[pixel] - oracle.Radiance[pixel];
        sum += Widen(error);
        squares += Widen(error) * Widen(error);
        result.MaximumAbsolute = math::Max(result.MaximumAbsolute, math::Abs(error));
        result.FalseLight += error > tolerance ? 1 : 0;
        result.FalseDarkening += error < -tolerance ? 1 : 0;
        if (oracle.IsOverlapping[pixel] == true)
        {
            ++result.OverlapPixels;
            overlapSum += Widen(error);
        }
    }
    const float64 count = static_cast<float64>(image.GetSize());
    result.MeanSigned = sum / count;
    result.Rms = math::Sqrt(squares / count);
    result.OverlapMeanSigned = result.OverlapPixels == 0 ? 0 : overlapSum / static_cast<float64>(result.OverlapPixels);
    return result;
}

void ReportError(std::string_view route, usize state, const ImageError& error)
{
    WARN(route << ", " << DescribeState(state) << ": mean signed " << error.MeanSigned << ", RMS " << error.Rms
               << ", largest " << error.MaximumAbsolute << ", false light " << error.FalseLight << ", false darkening "
               << error.FalseDarkening << ", overlap pixels " << error.OverlapPixels << " with mean signed "
               << error.OverlapMeanSigned);
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
/// Both windings, so the blockers occlude from either side in every route.
constexpr StaticArray<uint32, TWO_SIDED_INDEX_COUNT> TWO_SIDED_INDICES{{0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2}};

constexpr uint32 RECEIVER_MATERIAL = 0;
constexpr uint32 EMITTER_MATERIAL = 1;
constexpr uint32 BLOCKER_MATERIAL = 2;
constexpr usize MATERIAL_COUNT = 3;
constexpr usize MESH_COUNT = 4;
constexpr uint32 RECEIVER_MESH = 0;
constexpr uint32 EMITTER_MESH = 1;
constexpr uint32 NEAR_BLOCKER_MESH = 2;
constexpr uint32 FAR_BLOCKER_MESH = 3;

/// The scene's static geometry; the far blocker is centred on x = 0 and placed by its item's translation.
struct AreaLightGeometry final
{
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> ReceiverNormals{{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}}};
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> EmitterCorners =
        MakeQuadCorners({-EMITTER_HALF_EXTENT_WORLD, -EMITTER_HALF_EXTENT_WORLD},
                        {EMITTER_HALF_EXTENT_WORLD, EMITTER_HALF_EXTENT_WORLD},
                        EMITTER_DEPTH_WORLD);
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> FrontNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> NearBlockerCorners =
        MakeQuadCorners(NEAR_BLOCKER_MINIMUM_WORLD, NEAR_BLOCKER_MAXIMUM_WORLD, NEAR_BLOCKER_DEPTH_WORLD);
    StaticArray<math::Vector3, QUAD_VERTEX_COUNT> FarBlockerCorners =
        MakeQuadCorners(-FAR_BLOCKER_HALF_EXTENT_WORLD, FAR_BLOCKER_HALF_EXTENT_WORLD, FAR_BLOCKER_DEPTH_WORLD);
    StaticArray<renderer::MeshDescription, MESH_COUNT> Meshes{};
    AreaLightGeometry()
    {
        Meshes[RECEIVER_MESH] = {RECEIVER_CORNERS_WORLD.GetData(),
                                 RECEIVER_CORNERS_WORLD.GetSize(),
                                 RECEIVER_INDICES.GetData(),
                                 RECEIVER_INDICES.GetSize()};
        Meshes[RECEIVER_MESH].Normals = ReceiverNormals.GetData();
        Meshes[EMITTER_MESH] = {EmitterCorners.GetData(),
                                EmitterCorners.GetSize(),
                                FRONT_INDICES.GetData(),
                                FRONT_INDICES.GetSize()};
        Meshes[NEAR_BLOCKER_MESH] = {NearBlockerCorners.GetData(),
                                     NearBlockerCorners.GetSize(),
                                     TWO_SIDED_INDICES.GetData(),
                                     TWO_SIDED_INDICES.GetSize()};
        Meshes[FAR_BLOCKER_MESH] = {FarBlockerCorners.GetData(),
                                    FarBlockerCorners.GetSize(),
                                    TWO_SIDED_INDICES.GetData(),
                                    TWO_SIDED_INDICES.GetSize()};
        for (usize mesh = EMITTER_MESH; mesh < MESH_COUNT; ++mesh)
        {
            Meshes[mesh].Normals = FrontNormals.GetData();
        }
    }
    AreaLightGeometry(const AreaLightGeometry&) = delete;
    AreaLightGeometry& operator=(const AreaLightGeometry&) = delete;
};

/// Translation of the far blocker's placement in a state.
math::Affine3 GetFarBlockerPlacement(const SceneState& state)
{
    math::Affine3 placement = math::Affine3::Identity();
    placement.Translation.X = state.FarBlockerCenterWorld;
    return placement;
}

/// Meshes present in a state, in item order.
usize GetItemCount(const SceneState& state)
{
    constexpr usize CONTROL_ITEMS = 2;
    return state.Blockers == BlockerSet::None   ? CONTROL_ITEMS
           : state.Blockers == BlockerSet::Near ? CONTROL_ITEMS + 1
                                                : MESH_COUNT;
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
    AreaLightGeometry Geometry;
    StaticArray<uint32, MATERIAL_COUNT> MaterialIndices{{RECEIVER_MATERIAL, EMITTER_MATERIAL, BLOCKER_MATERIAL}};
    StaticArray<renderer::RenderGraphMesh, MESH_COUNT> Meshes{};
    StaticArray<renderer::RenderGraphItem, MESH_COUNT> Items{};
    StaticArray<renderer::SceneShaderMaterial, MATERIAL_COUNT> Materials{};
    renderer::SceneShaderInputs Inputs{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    GraphScene()
    {
        for (usize mesh = 0; mesh < MESH_COUNT; ++mesh)
        {
            Meshes[mesh].Geometry = Geometry.Meshes[mesh];
            const usize material = mesh == RECEIVER_MESH  ? RECEIVER_MATERIAL
                                   : mesh == EMITTER_MESH ? EMITTER_MATERIAL
                                                          : BLOCKER_MATERIAL;
            Meshes[mesh].MaterialBindings = {&MaterialIndices[material], 1};
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
        Materials[RECEIVER_MATERIAL].BaseColor = {RECEIVER_ALBEDO, RECEIVER_ALBEDO, RECEIVER_ALBEDO};
        Materials[EMITTER_MATERIAL].Emission = {EMITTER_RADIANCE, EMITTER_RADIANCE, EMITTER_RADIANCE};
        Materials[EMITTER_MATERIAL].IsEmissionTwoSided = false;
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        Cameras[0].OrthographicCamera = true;
        Cameras[0].CameraForward = {0, 0, 1};
        Cameras[0].CameraPosition = {0, 0, -1};
    }
    void SetState(const SceneState& state)
    {
        Items[FAR_BLOCKER_MESH].MeshToWorld = GetFarBlockerPlacement(state);
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
    const usize itemCount = GetItemCount(state);
    const BorrowedView<const renderer::RenderGraphMesh> meshes{inoutScene.Meshes.GetData(),
                                                               inoutScene.Meshes.GetSize()};
    const BorrowedView<const renderer::RenderGraphItem> items{inoutScene.Items.GetData(), itemCount};
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
constexpr PackageLocation TILED_NEE_PACKAGE{LUDUS_RG3_TILED_VBUFFER_NEE_LINEAR_DIRECTORY,
                                            "rg2_tiled_vbuffer_nee_linear.lurgrt"};
/// The visibility raster producer, then the tiles, prefix and shade ray passes; Accumulate fuses into shade.
constexpr usize TILED_NEE_PASS_COUNT = 4;

// --- RT1 reference ---

/// Sample counts of the RT1 captures; the CPU oracle's are fewer because it is far slower.
constexpr uint32 GPU_REFERENCE_SAMPLES = 1024;
constexpr uint32 CPU_REFERENCE_SAMPLES = 256;
/// States the CPU oracle captures: the one-blocker control and the last, most overlapping, motion step. GPU compute
/// captures every state.
using StateMask = StaticArray<bool, STATE_COUNT>;
constexpr StateMask ALL_STATES{{true, true, true, true, true, true}};
constexpr StateMask CPU_ORACLE_STATES{{false, true, false, false, false, true}};

/// RT1 first-surface direct light (CPU oracle or GPU compute) of the selected states, uniform-area emitter sampling.
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
    const AreaLightGeometry geometry;
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
    StaticArray<renderer::MaterialVersion, MATERIAL_COUNT> materials{};
    for (usize material = 0; material < MATERIAL_COUNT; ++material)
    {
        renderer::LitMaterialDescription description;
        const float32 albedo = material == RECEIVER_MATERIAL ? RECEIVER_ALBEDO : 0;
        description.Surface.Tint = {albedo, albedo, albedo, 1};
        description.Roughness = 1;
        description.Metallic = 0;
        description.Scattering.Model = format::MaterialModel::Diffuse;
        if (material == EMITTER_MATERIAL)
        {
            description.Emission = {EMITTER_RADIANCE, EMITTER_RADIANCE, EMITTER_RADIANCE};
            description.IsEmissionTwoSided = false;
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
    renderer::ReferenceCaptureDescription capture;
    capture.Width = WIDTH;
    capture.Height = HEIGHT;
    capture.OrthographicCamera = true;
    capture.CenterRays = true;
    capture.TargetSamples = samples;
    capture.MaxDepth = 1;
    capture.RouletteDepth = 1;
    Array<renderer::ReferencePixel> pixels;
    REQUIRE(pixels.TryResize(static_cast<usize>(WIDTH) * HEIGHT) == true);
    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        if (states[state] == false)
        {
            continue;
        }
        StaticArray<renderer::SceneItem, MESH_COUNT> items{};
        const usize itemCount = GetItemCount(STATES[state]);
        for (usize item = 0; item < itemCount; ++item)
        {
            items[item].Geometry = meshes[item];
            items[item].Surface = materials[item == RECEIVER_MESH  ? RECEIVER_MATERIAL
                                            : item == EMITTER_MESH ? EMITTER_MATERIAL
                                                                   : BLOCKER_MATERIAL];
            items[item].SourceId = item + 1;
        }
        items[FAR_BLOCKER_MESH].Transform = GetFarBlockerPlacement(STATES[state]);
        renderer::Snapshot snapshot;
        REQUIRE(reference.CreateSnapshot(items.GetData(), itemCount, snapshot) == S::Ready);
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
            outImages[state][pixel] = math::Min(pixels[pixel].Direct.X, 1.0F);
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
} // namespace

TEST_CASE("Area-light backprojection oracle matches an independent visibility quadrature", "[hybrid-shadows]")
{
    // Independent check of the closed form and the union: a midpoint grid over the emitter, each sample tested for
    // occlusion by intersecting its segment to the receiver with each blocker plane.
    constexpr usize GRID_SIDE = 1024;
    constexpr usize PROBE_STRIDE_PIXELS = 24;
    const Rectangle emitter = GetEmitterRectangle();
    const float64 cell = (emitter.MaximumX - emitter.MinimumX) / GRID_SIDE;
    const float64 height = static_cast<float64>(RECEIVER_DEPTH_WORLD) - static_cast<float64>(EMITTER_DEPTH_WORLD);
    float64 largestRelativeError = 0;
    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        for (usize row = PROBE_STRIDE_PIXELS / 2; row < HEIGHT; row += PROBE_STRIDE_PIXELS)
        {
            for (usize column = PROBE_STRIDE_PIXELS / 2; column < WIDTH; column += PROBE_STRIDE_PIXELS)
            {
                const math::Vector3d receiverWorld =
                    GetReceiverPointWorld(static_cast<float64>(column), static_cast<float64>(row));
                StaticArray<Blocker, 2> blockers{
                    {GetNearBlocker(), GetFarBlocker(STATES[state].FarBlockerCenterWorld)}};
                const usize blockerCount = STATES[state].Blockers == BlockerSet::None   ? 0
                                           : STATES[state].Blockers == BlockerSet::Near ? 1
                                                                                        : 2;
                float64 visible = 0;
                for (usize j = 0; j < GRID_SIDE; ++j)
                {
                    const float64 y = emitter.MinimumY + (static_cast<float64>(j) + .5) * cell;
                    for (usize i = 0; i < GRID_SIDE; ++i)
                    {
                        const float64 x = emitter.MinimumX + (static_cast<float64>(i) + .5) * cell;
                        bool isOccluded = false;
                        for (usize blocker = 0; blocker < blockerCount && isOccluded == false; ++blocker)
                        {
                            const float64 t = (blockers[blocker].DepthWorld - receiverWorld.Z) /
                                              (static_cast<float64>(EMITTER_DEPTH_WORLD) - receiverWorld.Z);
                            const float64 crossingX = receiverWorld.X + t * (x - receiverWorld.X);
                            const float64 crossingY = receiverWorld.Y + t * (y - receiverWorld.Y);
                            const Rectangle& extent = blockers[blocker].Extent;
                            isOccluded = crossingX >= extent.MinimumX && crossingX <= extent.MaximumX &&
                                         crossingY >= extent.MinimumY && crossingY <= extent.MaximumY;
                        }
                        if (isOccluded == false)
                        {
                            const float64 dx = x - receiverWorld.X;
                            const float64 dy = y - receiverWorld.Y;
                            const float64 squared = dx * dx + dy * dy + height * height;
                            visible += height * height / (squared * squared);
                        }
                    }
                }
                visible *= cell * cell;
                const OracleSample oracle = EvaluateOracle(STATES[state], receiverWorld);
                const float64 relativeError = math::Abs(visible - oracle.Visible) / oracle.Unoccluded;
                CAPTURE(state, column, row, visible, oracle.Visible, oracle.Unoccluded);
                // A midpoint grid misplaces each visibility edge by under half a cell.
                constexpr float64 QUADRATURE_TOLERANCE = 2e-3;
                CHECK(relativeError <= QUADRATURE_TOLERANCE);
                largestRelativeError = math::Max(largestRelativeError, relativeError);
            }
        }
    }
    WARN("largest quadrature/oracle difference relative to the unoccluded integral " << largestRelativeError);

    // The motion takes probes from separate to overlapping projections; each state keeps the receiver partly lit.
    usize transitioningProbes = 0;
    usize unlitProbes = 0;
    for (usize row = 0; row < HEIGHT; ++row)
    {
        for (usize column = 0; column < WIDTH; ++column)
        {
            const math::Vector3d receiverWorld =
                GetReceiverPointWorld(static_cast<float64>(column), static_cast<float64>(row));
            const OracleSample first = EvaluateOracle(STATES[CONTROL_STATE_COUNT], receiverWorld);
            const OracleSample last = EvaluateOracle(STATES[STATE_COUNT - 1], receiverWorld);
            transitioningProbes += first.OverlapArea == 0 && last.OverlapArea > 0 ? 1 : 0;
            for (const SceneState& state : STATES)
            {
                unlitProbes += EvaluateOracle(state, receiverWorld).Visible > 0 ? 0 : 1;
            }
        }
    }
    WARN("pixels whose projections go from separate to overlapping: " << transitioningProbes);
    CHECK(transitioningProbes > static_cast<usize>(WIDTH) * HEIGHT / 4);
    CHECK(unlitProbes == 0);
    // The pixel just right of and below the image centre.
    constexpr usize CENTER_COLUMN = WIDTH / 2;
    constexpr usize CENTER_ROW = HEIGHT / 2;
    const math::Vector3d centerWorld =
        GetReceiverPointWorld(static_cast<float64>(CENTER_COLUMN), static_cast<float64>(CENTER_ROW));
    const float64 centerRadiance = ComputeRadiance(EvaluateOracle(STATES[0], centerWorld).Unoccluded);
    INFO("unoccluded centre radiance " << centerRadiance);
    CHECK(centerRadiance < 1);
    CHECK(centerRadiance > .4);
}

TEST_CASE("RT1 matches the area-light backprojection oracle across overlapping blockers",
          "[gpu][hybrid-shadows][reference-compute]")
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
        const OracleImage oracle = MakeOracleImage(STATES[state]);
        CAPTURE(DescribeState(state));
        for (const bool isGpu : {true, false})
        {
            if (isGpu == false && CPU_ORACLE_STATES[state] == false)
            {
                continue;
            }
            const LinearImage& image = isGpu == true ? gpuImages[state] : cpuImages[state];
            // Uniform-area light sampling with BSDF MIS: about 0.15 standard deviation per sample, so roughly seven
            // standard errors of the per-pixel mean at each sample count.
            const float32 tolerance = isGpu == true ? .04F : .07F;
            const ImageError error = MeasureError(image, oracle, tolerance);
            ReportError(isGpu == true ? "RT1 GPU compute" : "RT1 CPU oracle", state, error);
            CHECK(math::Abs(error.MeanSigned) <= 2e-3);
            CHECK(math::Abs(error.OverlapMeanSigned) <= 4e-3);
            CHECK(error.FalseLight == 0);
            CHECK(error.FalseDarkening == 0);
        }
        if (CPU_ORACLE_STATES[state] == true)
        {
            const ImageDifference pair = Compare(gpuImages[state], cpuImages[state]);
            WARN(DescribeState(state) << ": RT1 GPU - CPU mean signed " << pair.MeanSigned << ", largest "
                                      << pair.MaximumAbsolute);
            CHECK(math::Abs(pair.MeanSigned) <= 2e-3);
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
/// Pixels whose oracle radiance changes by more than this between consecutive states form the changed region.
constexpr float32 CHANGE_THRESHOLD = 4 * LINEAR_COLOR_CODE;

/// RMS error over the pixels the mask selects.
float64 MeasureRegionRms(const LinearImage& image,
                         const LinearImage& oracle,
                         const StaticArray<bool, static_cast<usize>(WIDTH) * HEIGHT>& mask)
{
    float64 squares = 0;
    usize count = 0;
    for (usize pixel = 0; pixel < image.GetSize(); ++pixel)
    {
        if (mask[pixel] == true)
        {
            const float64 error = static_cast<float64>(image[pixel]) - static_cast<float64>(oracle[pixel]);
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

TEST_CASE("Hybrid tiled NEE partial area-light shadows match the backprojection oracle and RT1",
          "[gpu][hybrid-shadows][native-ray-queries][reference-compute]")
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

    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(TILED_NEE_PACKAGE);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(renderer::RequiresHardwareRayTracing(plan) == true);
    REQUIRE(plan.Passes.GetSize() == TILED_NEE_PASS_COUNT);
    const uint32 colorResource = plan.Passes[TILED_NEE_PASS_COUNT - 1].Color;
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    renderer::RenderGraph graph;
    REQUIRE(Poll(graph, graph.Initialize(device.Handle, plan, programs, nullptr, WIDTH, HEIGHT)) == S::Ready);
    GraphScene scene;

    StaticArray<OracleImage, STATE_COUNT> oracles{};
    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        oracles[state] = MakeOracleImage(STATES[state]);
    }
    StaticArray<StaticArray<LinearImage, CHECKPOINT_COUNT>, STATE_COUNT> checkpoints{};
    uint64 revision = 0;
    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        CAPTURE(DescribeState(state));
        ++revision;
        REQUIRE(UpdateGraphState(graph, scene, STATES[state], revision) == S::Ready);
        RenderCheckpoints(device, graph, colorResource, checkpoints[state]);
        const OracleImage& oracle = oracles[state];

        // Raw: one light sample and one BSDF sample per pixel, combined with the power heuristic.
        const ImageError raw = MeasureError(checkpoints[state][0], oracle, 1);
        ReportError("Hybrid raw frame", state, raw);
        CHECK(math::Abs(raw.MeanSigned) <= .01);

        // Reconstructed: the template's running mean after the last checkpoint, 256 frames, with about seven standard
        // errors of per-pixel tolerance.
        const LinearImage& reconstructed = checkpoints[state][CHECKPOINT_COUNT - 1];
        constexpr float32 RECONSTRUCTED_PIXEL_TOLERANCE = .07F;
        const ImageError error = MeasureError(reconstructed, oracle, RECONSTRUCTED_PIXEL_TOLERANCE);
        ReportError("Hybrid accumulated", state, error);
        CHECK(math::Abs(error.MeanSigned) <= 4e-3);
        CHECK(math::Abs(error.OverlapMeanSigned) <= 6e-3);
        CHECK(error.FalseLight == 0);
        CHECK(error.FalseDarkening == 0);
        const ImageDifference pair = Compare(reconstructed, referenceImages[state]);
        WARN(DescribeState(state) << ": hybrid accumulated - RT1 GPU mean signed " << pair.MeanSigned << ", largest "
                                  << pair.MaximumAbsolute);
        CHECK(math::Abs(pair.MeanSigned) <= 4e-3);

        // Temporal error across each motion step, over the pixels the step changes.
        if (state > CONTROL_STATE_COUNT)
        {
            StaticArray<bool, static_cast<usize>(WIDTH) * HEIGHT> changed{};
            usize changedCount = 0;
            for (usize pixel = 0; pixel < changed.GetSize(); ++pixel)
            {
                changed[pixel] =
                    math::Abs(oracle.Radiance[pixel] - oracles[state - 1].Radiance[pixel]) > CHANGE_THRESHOLD;
                changedCount += changed[pixel] == true ? 1 : 0;
            }
            REQUIRE(changedCount > 0);
            usize framesToTolerance = 0;
            for (usize checkpoint = 0; checkpoint < CHECKPOINT_COUNT; ++checkpoint)
            {
                const float64 rms = MeasureRegionRms(checkpoints[state][checkpoint], oracle.Radiance, changed);
                const float64 staleRms =
                    MeasureRegionRms(checkpoints[state][checkpoint], oracles[state - 1].Radiance, changed);
                WARN(DescribeState(state)
                     << ", frame " << CHECKPOINT_FRAMES[checkpoint] << ": changed-region RMS " << rms
                     << " against the new oracle, " << staleRms << " against the previous state's");
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

TEST_CASE("RT2 raster fallback approximates partial area-light shadows with one cube-map caster",
          "[render-graph][gpu][hybrid-shadows]")
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
    S status = activation.StageSelected(core::Move(raster), LoadPackage(TILED_NEE_PACKAGE), preference);
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

    GraphScene scene;
    StaticArray<LinearImage, STATE_COUNT> images{};
    for (usize state = 0; state < STATE_COUNT; ++state)
    {
        CAPTURE(DescribeState(state));
        REQUIRE(UpdateGraphState(*graphOrNull, scene, STATES[state], state + 1, &activation) == S::Ready);
        // Draw until every cube face of the caster is in the atlas; undrawn faces read as unoccluded.
        constexpr usize MAXIMUM_FACE_FRAMES = 16;
        usize frames = 0;
        do
        {
            DrawGraphAndWait(device, *graphOrNull);
            ++frames;
        } while (graphOrNull->GetReport().PendingMeshLightShadowFaces != 0 && frames < MAXIMUM_FACE_FRAMES);
        REQUIRE(graphOrNull->GetReport().PendingMeshLightShadowFaces == 0);
        constexpr uint32 COLOR_EXPORT = 0;
        ReadExport(device, *graphOrNull, COLOR_EXPORT, images[state]);
        const renderer::RenderGraphSceneResourceReport& resources = graphOrNull->GetReport().Resources;
        WARN(route << ", " << DescribeState(state) << ": " << resources.MeshLightShadowCasters
                   << " mesh-light caster(s), " << resources.UnshadowedMeshLights << " unshadowed record(s), " << frames
                   << " frame(s) to draw its faces");
        CHECK(resources.UnshadowedMeshLights == 0);
    }

    // The unoccluded control isolates shadow visibility from the raster's unshadowed polygon integral, which follows
    // the oracle within two percent of the peak radiance.
    const OracleImage unobstructed = MakeOracleImage(STATES[0]);
    constexpr float32 CONTROL_TOLERANCE = .02F;
    const ImageError control = MeasureError(images[0], unobstructed, CONTROL_TOLERANCE);
    ReportError(route, 0, control);
    CHECK(control.FalseLight == 0);
    CHECK(control.FalseDarkening == 0);
    // The caster's face near plane is at least a hundredth of its range, the emitter's culling reach of tens of
    // metres, so both blockers (0.22 and 0.47 m in front of the caster) lie inside it and the fallback ignores them.
    // The fixture records that documented approximation; it bounds only false darkening, which the fallback must
    // never add.
    // Visibility ratio slack: two RGBA8 codes over the dimmest control pixels, about 0.1 in the image corners.
    constexpr float32 MAXIMUM_FALSE_DARKENING = .05F;
    for (usize state = CONTROL_STATE_COUNT - 1; state < STATE_COUNT; ++state)
    {
        CAPTURE(DescribeState(state));
        const OracleImage oracle = MakeOracleImage(STATES[state]);
        ReportError(route, state, MeasureError(images[state], oracle, .05F));
        // Visibility: route ratio against its own control, oracle ratio against the unoccluded integral.
        float64 squares = 0;
        float64 sum = 0;
        float32 largestExcess = 0;
        float32 largestDeficit = 0;
        float64 overlapSum = 0;
        usize overlapCount = 0;
        for (usize pixel = 0; pixel < images[state].GetSize(); ++pixel)
        {
            const float32 measured = images[state][pixel] / images[0][pixel];
            const float32 expected = oracle.Radiance[pixel] / unobstructed.Radiance[pixel];
            const float32 error = measured - expected;
            sum += Widen(error);
            squares += Widen(error) * Widen(error);
            largestExcess = math::Max(largestExcess, error);
            largestDeficit = math::Min(largestDeficit, error);
            if (oracle.IsOverlapping[pixel] == true)
            {
                overlapSum += Widen(error);
                ++overlapCount;
            }
        }
        const float64 count = static_cast<float64>(images[state].GetSize());
        WARN(route << ", " << DescribeState(state) << ": visibility mean signed error " << sum / count << ", RMS "
                   << math::Sqrt(squares / count) << ", largest false light " << largestExcess
                   << ", largest false darkening " << -largestDeficit << ", overlap mean signed "
                   << (overlapCount == 0 ? 0 : overlapSum / static_cast<float64>(overlapCount)) << " over "
                   << overlapCount << " pixels");
        CHECK(-largestDeficit <= MAXIMUM_FALSE_DARKENING);
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

TEST_CASE("Measure RT2 area-light shadow scene preparation and ray pass latency", "[.benchmark][hybrid-shadows]")
{
    const Device device(/*shouldRequestRays=*/true, /*shouldEnableGpuTiming=*/true);
    if (device.HasRays == false)
    {
        SKIP("Native inline ray queries unavailable");
    }
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(TILED_NEE_PACKAGE);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(plan.Passes.GetSize() == TILED_NEE_PASS_COUNT);
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
        // Move the far blocker's placement, as the motion fixture does, so each update rebuilds the instance data.
        state.FarBlockerCenterWorld += MOTION_STEP_METERS;
        const uint64 begun = time::NowTicks();
        REQUIRE(UpdateGraphState(graph, scene, state, ++revision) == S::Ready);
        durationNanoseconds = static_cast<float64>(time::NowTicks() - begun);
        DrawGraphAndWait(device, graph);
    }
    ReportTimings("Blocker move to drawable scene (CPU wall time, includes acceleration update and polling)",
                  preparation);

    constexpr usize RAY_PASS_COUNT = TILED_NEE_PASS_COUNT - 1;
    StaticArray<TimingSamples, RAY_PASS_COUNT> passes{};
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
        // The ordered ray sequence is timed as compute encoders in pass order: tiles, prefix, then shade with the
        // fused running mean.
        usize computePasses = 0;
        for (usize index = 0; index < timings.Count; ++index)
        {
            const rhi::GpuPassTiming& pass = timings.Passes[index];
            if (pass.Kind == rhi::GpuPassKind::Compute)
            {
                REQUIRE(computePasses < RAY_PASS_COUNT);
                REQUIRE(pass.Timing.State == rhi::GpuTimingState::Available);
                REQUIRE(math::IsFinite(pass.Timing.DurationNanoseconds) == true);
                passes[computePasses][sample] = pass.Timing.DurationNanoseconds;
                ++computePasses;
            }
        }
        REQUIRE(computePasses == RAY_PASS_COUNT);
    }
    constexpr StaticArray<std::string_view, RAY_PASS_COUNT> NAMES{
        {"Tile emitter table (native GPU duration)",
         "Tile CDF prefix (native GPU duration)",
         "Shade with emitter ray, BSDF ray and running mean (native GPU duration)"}};
    for (usize pass = 0; pass < RAY_PASS_COUNT; ++pass)
    {
        ReportTimings(NAMES[pass], passes[pass]);
    }
}
