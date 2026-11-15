// RT2 hybrid shadows (issue 1248): a closed blocker that leaves the camera frustum must keep its shadow on the
// visible receiver. One scene is drawn by the hybrid-shadows template (raster first hits, one shadow ray per light),
// by the default clustered deferred graph (its cached directional shadow map, the raster baseline) and by the RT1
// reference (CPU oracle and GPU compute). Each is compared with an independent float64 projection of the blocker
// along the light rays, and the hybrid image with the paired RT1 first-surface direct output.
// Thanks to Franklin C. Crow, "Shadow Algorithms for Computer Graphics", SIGGRAPH 1977, pp. 242-248, Fig. 4, for
// separating light-space shadow boundaries from viewer-space hidden surface removal.
// https://www.cs.rpi.edu/~cutler/classes/advancedgraphics/S10/papers/crow77_shadows.pdf

#include "../src/internal/raster_mesh_light_shadows.h"
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

namespace
{
using S = rhi::RasterStatus;
constexpr uint64 TIMEOUT_NANOSECONDS = LUDUS_RENDERER_TEST_COLD_GPU_TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND;
constexpr usize CHANNEL_COUNT = 4;
constexpr usize HYBRID_RAY_PASS_INDEX = 1;
constexpr float32 MAXIMUM_CODE = 255;
constexpr float32 PIXEL_WIDTH_CLIP = 2.0F / static_cast<float32>(WIDTH);

/// The receiver plane faces the camera, which looks along +Z from the near plane z = 0.
constexpr float32 RECEIVER_DEPTH_WORLD = .5F;
constexpr StaticArray<math::Vector3, 4> RECEIVER_CORNERS_WORLD{{{-3, -2, RECEIVER_DEPTH_WORLD},
                                                                {3, -2, RECEIVER_DEPTH_WORLD},
                                                                {3, 2, RECEIVER_DEPTH_WORLD},
                                                                {-3, 2, RECEIVER_DEPTH_WORLD}}};
constexpr StaticArray<uint32, 6> RECEIVER_INDICES{{0, 2, 1, 0, 3, 2}};
/// A closed opaque box between the camera and the receiver. It lies right of x = 1, the edge of the moved camera's
/// window, and its shadow falls on x in [0.2, 0.8], y in [-0.4, 0.4] of the receiver.
constexpr math::Vector3 BLOCKER_MINIMUM_WORLD{1.1F, -.4F, .05F};
constexpr math::Vector3 BLOCKER_MAXIMUM_WORLD{1.4F, .4F, .2F};
constexpr usize BLOCKER_FACE_COUNT = 6;
constexpr usize BLOCKER_VERTEX_COUNT = BLOCKER_FACE_COUNT * 4;
constexpr usize BLOCKER_INDEX_COUNT = BLOCKER_FACE_COUNT * 6;
/// The directional light travels along this direction: towards -X and away from the camera.
constexpr math::Vector3 LIGHT_TRAVEL_WORLD{-2, 0, 1};
/// Illuminance in lux; with the receiver's albedo this puts lit radiance near 0.45, inside linear RGBA8.
constexpr float32 LIGHT_ILLUMINANCE = 4;
constexpr math::Vector3 RECEIVER_ALBEDO{.8F, .8F, .8F};
/// The left edge of the shadow on the receiver's row y = 0: the blocker's near-left edge projected along the light.
constexpr float64 SHADOW_LEFT_EDGE_WORLD = 0.2;
/// World x at the centre of the moved camera's window, which then contains the blocker.
constexpr float32 BLOCKER_VISIBLE_CAMERA_OFFSET_WORLD = .8F;

struct BlockerMesh final
{
    StaticArray<math::Vector3, BLOCKER_VERTEX_COUNT> Positions{};
    StaticArray<math::Vector3, BLOCKER_VERTEX_COUNT> Normals{};
    StaticArray<uint32, BLOCKER_INDEX_COUNT> Indices{};
};

/// Six outward faces with their own corners and normals, so the box is closed and flat shaded.
BlockerMesh BuildBlocker(math::Vector3 minimumWorld = BLOCKER_MINIMUM_WORLD,
                         math::Vector3 maximumWorld = BLOCKER_MAXIMUM_WORLD)
{
    BlockerMesh result;
    const StaticArray<StaticArray<float32, 3>, 2> bounds{
        {{{minimumWorld.X, minimumWorld.Y, minimumWorld.Z}}, {{maximumWorld.X, maximumWorld.Y, maximumWorld.Z}}}};
    usize face = 0;
    for (usize axis = 0; axis < 3; ++axis)
    {
        for (usize side = 0; side < 2; ++side)
        {
            const usize first = (axis + 1) % 3;
            const usize second = (axis + 2) % 3;
            // (first, second) corners in counterclockwise order seen from outside the face.
            const StaticArray<StaticArray<usize, 2>, 4> corners =
                side == 1 ? StaticArray<StaticArray<usize, 2>, 4>{{{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}}}
                          : StaticArray<StaticArray<usize, 2>, 4>{{{{0, 0}}, {{0, 1}}, {{1, 1}}, {{1, 0}}}};
            for (usize corner = 0; corner < 4; ++corner)
            {
                StaticArray<float32, 3> coordinates{};
                coordinates[axis] = bounds[side][axis];
                coordinates[first] = bounds[corners[corner][0]][first];
                coordinates[second] = bounds[corners[corner][1]][second];
                StaticArray<float32, 3> normal{};
                normal[axis] = side == 1 ? 1.0F : -1.0F;
                result.Positions[face * 4 + corner] = {coordinates[0], coordinates[1], coordinates[2]};
                result.Normals[face * 4 + corner] = {normal[0], normal[1], normal[2]};
            }
            const StaticArray<uint32, 6> quad{{0, 1, 2, 0, 2, 3}};
            for (usize index = 0; index < quad.GetSize(); ++index)
            {
                result.Indices[face * 6 + index] = static_cast<uint32>(face * 4) + quad[index];
            }
            ++face;
        }
    }
    return result;
}

/// A float32 scene coordinate widened for the float64 oracle.
math::Vector3d Widen(math::Vector3 value)
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

/// Unit direction from a receiver towards the light, in float64.
math::Vector3d GetTowardLightWorld()
{
    const math::Vector3d travel = Widen(LIGHT_TRAVEL_WORLD);
    const float64 length = math::Sqrt(travel.X * travel.X + travel.Y * travel.Y + travel.Z * travel.Z);
    return {-travel.X / length, -travel.Y / length, -travel.Z / length};
}

/// Slab test of the ray origin + t direction, t > 0, against the blocker: the independent geometric oracle.
bool DoesRayHitBlocker(math::Vector3d originWorld,
                       math::Vector3d directionWorld,
                       math::Vector3d minimumWorld = Widen(BLOCKER_MINIMUM_WORLD),
                       math::Vector3d maximumWorld = Widen(BLOCKER_MAXIMUM_WORLD))
{
    const StaticArray<float64, 3> origin{{originWorld.X, originWorld.Y, originWorld.Z}};
    const StaticArray<float64, 3> direction{{directionWorld.X, directionWorld.Y, directionWorld.Z}};
    const StaticArray<float64, 3> minimum{{minimumWorld.X, minimumWorld.Y, minimumWorld.Z}};
    const StaticArray<float64, 3> maximum{{maximumWorld.X, maximumWorld.Y, maximumWorld.Z}};
    float64 entry = 0;
    float64 exit = 1e30;
    for (usize axis = 0; axis < 3; ++axis)
    {
        if (direction[axis] == 0)
        {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
            {
                return false;
            }
            continue;
        }
        float64 near = (minimum[axis] - origin[axis]) / direction[axis];
        float64 far = (maximum[axis] - origin[axis]) / direction[axis];
        if (near > far)
        {
            const float64 swapped = near;
            near = far;
            far = swapped;
        }
        entry = math::Max(entry, near);
        exit = math::Min(exit, far);
    }
    return entry <= exit && exit > 0;
}

/// What one pixel centre sees, by the oracle: the blocker itself, a lit receiver or a shadowed receiver.
enum class PixelClass : uint8
{
    Blocker,
    Lit,
    Shadowed,
};

/// World position on the receiver behind the image point (column + 0.5, row + 0.5) for a camera window centred on
/// cameraOffsetWorld; fractional and out-of-image coordinates are admitted.
math::Vector3d GetReceiverPointWorld(float64 column, float64 row, float32 cameraOffsetWorld)
{
    const float64 clipX = 2 * (column + .5) / WIDTH - 1;
    const float64 clipY = 1 - 2 * (row + .5) / HEIGHT;
    return {clipX + static_cast<float64>(cameraOffsetWorld), clipY, static_cast<float64>(RECEIVER_DEPTH_WORLD)};
}

PixelClass ClassifyPoint(math::Vector3d receiverWorld)
{
    if (DoesRayHitBlocker({receiverWorld.X, receiverWorld.Y, 0}, {0, 0, 1}) == true)
    {
        return PixelClass::Blocker;
    }
    return DoesRayHitBlocker(receiverWorld, GetTowardLightWorld()) == true ? PixelClass::Shadowed : PixelClass::Lit;
}

/// The oracle class of a pixel centre, or Blocker when a neighbour one pixel away differs (a boundary pixel).
bool TryClassifyInterior(usize column, usize row, float32 cameraOffsetWorld, PixelClass& outClass)
{
    const PixelClass centre = ClassifyPoint(
        GetReceiverPointWorld(static_cast<float64>(column), static_cast<float64>(row), cameraOffsetWorld));
    constexpr StaticArray<StaticArray<float64, 2>, 4> NEIGHBOURS{{{{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}}}};
    for (const StaticArray<float64, 2>& offset : NEIGHBOURS)
    {
        const math::Vector3d neighbourWorld = GetReceiverPointWorld(static_cast<float64>(column) + offset[0],
                                                                    static_cast<float64>(row) + offset[1],
                                                                    cameraOffsetWorld);
        if (ClassifyPoint(neighbourWorld) != centre)
        {
            return false;
        }
    }
    outClass = centre;
    return centre != PixelClass::Blocker;
}

/// Linear radiance of a lit receiver: the RT1 CPU BSDF times illuminance and the receiver cosine.
float32 ComputeLitRadiance()
{
    renderer::ReferenceBsdf bsdf;
    bsdf.BaseColor = RECEIVER_ALBEDO;
    bsdf.Roughness = 1;
    bsdf.Metallic = 0;
    const math::Vector3d towardLight = GetTowardLightWorld();
    const math::Vector3 lightWorld{static_cast<float32>(towardLight.X),
                                   static_cast<float32>(towardLight.Y),
                                   static_cast<float32>(towardLight.Z)};
    renderer::ReferenceBsdfEvaluation evaluation;
    REQUIRE(renderer::EvaluateReferenceBsdf(bsdf, {0, 0, -1}, {0, 0, -1}, lightWorld, evaluation) == S::Ready);
    return evaluation.Value.X * LIGHT_ILLUMINANCE * -lightWorld.Z;
}

/// A red-channel linear image of WIDTH by HEIGHT pixels, row 0 at the top.
using LinearImage = StaticArray<float32, static_cast<usize>(WIDTH) * HEIGHT>;

/// Comparison of one route's image with the geometric oracle.
struct ShadowEvidence final
{
    usize InteriorPixels = 0;     ///< Pixels away from every boundary that the oracle classifies as receiver.
    usize Misclassified = 0;      ///< Interior pixels whose lit/shadowed state disagrees with the oracle.
    usize FalseOcclusion = 0;     ///< Interior lit pixels the route darkens.
    float32 MaximumLitError = 0;  ///< Largest |radiance - expected| over interior lit pixels.
    float32 MaximumShadowed = 0;  ///< Largest radiance over interior shadowed pixels.
    float64 EdgeDisplacement = 0; ///< Measured minus expected left shadow edge on row y = 0, in pixels.
};

ShadowEvidence Compare(const LinearImage& image, float32 cameraOffsetWorld, float32 litRadiance)
{
    ShadowEvidence result;
    for (usize row = 0; row < HEIGHT; ++row)
    {
        for (usize column = 0; column < WIDTH; ++column)
        {
            PixelClass expected = PixelClass::Blocker;
            if (TryClassifyInterior(column, row, cameraOffsetWorld, expected) == false)
            {
                continue;
            }
            ++result.InteriorPixels;
            const float32 radiance = image[row * WIDTH + column];
            const bool isLit = radiance > .5F * litRadiance;
            if (isLit != (expected == PixelClass::Lit))
            {
                ++result.Misclassified;
            }
            if (expected == PixelClass::Lit)
            {
                result.FalseOcclusion += isLit == false ? 1 : 0;
                result.MaximumLitError = math::Max(result.MaximumLitError, math::Abs(radiance - litRadiance));
            }
            else
            {
                result.MaximumShadowed = math::Max(result.MaximumShadowed, radiance);
            }
        }
    }
    // Scan the middle row left to right for the first shadowed pixel; the edge lies between it and its lit neighbour.
    const usize row = HEIGHT / 2;
    for (usize column = 1; column < WIDTH; ++column)
    {
        if (image[row * WIDTH + column] <= .5F * litRadiance && image[row * WIDTH + column - 1] > .5F * litRadiance)
        {
            const float64 edgeWorld =
                GetReceiverPointWorld(static_cast<float64>(column) - .5, row, cameraOffsetWorld).X;
            result.EdgeDisplacement = (edgeWorld - SHADOW_LEFT_EDGE_WORLD) / static_cast<float64>(PIXEL_WIDTH_CLIP);
            break;
        }
    }
    return result;
}

void RequireNoMisclassification(const ShadowEvidence& evidence, float32 litTolerance, float32 shadowTolerance)
{
    CAPTURE(evidence.InteriorPixels,
            evidence.Misclassified,
            evidence.FalseOcclusion,
            evidence.MaximumLitError,
            evidence.MaximumShadowed,
            evidence.EdgeDisplacement);
    CHECK(evidence.InteriorPixels > static_cast<usize>(WIDTH) * HEIGHT / 2);
    CHECK(evidence.Misclassified == 0);
    CHECK(evidence.FalseOcclusion == 0);
    CHECK(evidence.MaximumLitError <= litTolerance);
    CHECK(evidence.MaximumShadowed <= shadowTolerance);
    // Pixel-centre sampling places the measured edge within half a pixel of the projected one.
    CHECK(math::Abs(evidence.EdgeDisplacement) <= .5 + 1e-6);
}

math::Matrix4 BuildCamera(float32 offsetWorld)
{
    math::Matrix4 worldToClip = math::Matrix4::Identity();
    worldToClip.Columns[3].X = -offsetWorld;
    return worldToClip;
}

struct Device final
{
    RhiGuard Session;
    rhi::DeviceHandle Handle{};
    rhi::SurfaceHandle Surface{};
    bool HasRays = false;
    explicit Device(bool shouldRequestRays = true, bool shouldEnableGpuTiming = false)
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

/// The scene as RenderGraph inputs: the receiver and the blocker share one material.
struct GraphScene final
{
    BlockerMesh Blocker = BuildBlocker();
    StaticArray<math::Vector3, 4> ReceiverNormals{{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}}};
    uint32 Material = 0;
    StaticArray<renderer::RenderGraphMesh, 2> Meshes{};
    StaticArray<renderer::RenderGraphItem, 2> Items{};
    StaticArray<renderer::SceneShaderMaterial, 1> Materials{};
    renderer::DirectLight Light{};
    renderer::SceneShaderInputs Inputs{};
    StaticArray<renderer::ViewDescription, 1> Cameras{};
    GraphScene()
    {
        Meshes[0].Geometry = {RECEIVER_CORNERS_WORLD.GetData(),
                              RECEIVER_CORNERS_WORLD.GetSize(),
                              RECEIVER_INDICES.GetData(),
                              RECEIVER_INDICES.GetSize()};
        Meshes[0].Geometry.Normals = ReceiverNormals.GetData();
        Meshes[1].Geometry = {Blocker.Positions.GetData(),
                              Blocker.Positions.GetSize(),
                              Blocker.Indices.GetData(),
                              Blocker.Indices.GetSize()};
        Meshes[1].Geometry.Normals = Blocker.Normals.GetData();
        for (renderer::RenderGraphMesh& mesh : Meshes)
        {
            mesh.MaterialBindings = {&Material, 1};
        }
        Items[0].SourceId = 1;
        Items[1].SourceId = 2;
        Items[1].MeshIndex = 1;
        Materials[0].BaseColor = RECEIVER_ALBEDO;
        Materials[0].Roughness = 1;
        Materials[0].Metallic = 0;
        Light.SourceId = 1;
        Light.Kind = renderer::LightKind::Directional;
        Light.Intensity = LIGHT_ILLUMINANCE;
        Light.Direction = LIGHT_TRAVEL_WORLD;
        Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        Inputs.Lights = {&Light, 1};
        // The identity projection looks along +Z from the near plane z = 0; lit raster shading reads both fields.
        Cameras[0].OrthographicCamera = true;
        Cameras[0].CameraForward = {0, 0, 1};
        Cameras[0].CameraPosition = {0, 0, -1};
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

void UpdateGraph(renderer::RenderGraph& inoutGraph, const GraphScene& scene, uint64 revision)
{
    renderer::RenderGraphReport report;
    REQUIRE(Poll(inoutGraph,
                 inoutGraph.Update({scene.Meshes.GetData(), scene.Meshes.GetSize()},
                                   {scene.Items.GetData(), scene.Items.GetSize()},
                                   {scene.Cameras.GetData(), scene.Cameras.GetSize()},
                                   revision,
                                   renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                   report,
                                   &scene.Inputs,
                                   revision)) == S::Ready);
    REQUIRE(inoutGraph.GetDrawableSceneRevision() == revision);
}

/// Draw one frame and read the red channel of an exported linear RGBA8 resource.
void ReadGraphImage(const Device& device, renderer::RenderGraph& inoutGraph, uint32 resource, LinearImage& outImage)
{
    DrawGraphAndWait(device, inoutGraph);
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

/// Which graph resource holds the linear colour to read.
enum class ColorSource : uint8
{
    FirstResource, ///< Resource 0, the default graph's resolved colour export.
    RayPassColor,  ///< The colour of the plan's second pass, the hybrid template's shadow-ray pass.
};

/// Draw the scene through a cooked graph from both camera windows.
/// @return False when the device cannot prepare the graph (Unsupported), leaving the images unchanged.
bool RenderGraphImages(const Device& device,
                       const PackageLocation& location,
                       ColorSource source,
                       LinearImage& outBlockerVisible,
                       LinearImage& outBlockerOffscreen)
{
    GraphScene scene;
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(location);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    renderer::RenderGraph graph;
    const S initialized = Poll(graph, graph.Initialize(device.Handle, plan, programs, nullptr, WIDTH, HEIGHT));
    if (initialized == S::Unsupported)
    {
        return false;
    }
    REQUIRE(initialized == S::Ready);
    REQUIRE((source == ColorSource::FirstResource || HYBRID_RAY_PASS_INDEX < plan.Passes.GetSize()));
    const uint32 colorResource = source == ColorSource::FirstResource ? 0 : plan.Passes[HYBRID_RAY_PASS_INDEX].Color;
    uint64 revision = 0;
    for (LinearImage* image : {&outBlockerVisible, &outBlockerOffscreen})
    {
        const float32 offsetWorld = image == &outBlockerVisible ? BLOCKER_VISIBLE_CAMERA_OFFSET_WORLD : 0;
        scene.Cameras[0].WorldToClip = BuildCamera(offsetWorld);
        ++revision;
        UpdateGraph(graph, scene, revision);
        LinearImage firstFrame{};
        // The second raster frame uses the cached map. The ray pass must give the same
        // visibility on the first and second frame immediately after the camera move.
        ReadGraphImage(device, graph, colorResource, firstFrame);
        ReadGraphImage(device, graph, colorResource, *image);
        if (source == ColorSource::RayPassColor)
        {
            CHECK(firstFrame == *image);
        }
    }
    return true;
}

/// RT1 first-surface direct light of the same scene, from both camera windows.
void RenderReferenceImages(const Device& device,
                           renderer::ReferenceTraversal traversal,
                           LinearImage& outBlockerVisible,
                           LinearImage& outBlockerOffscreen)
{
    constexpr usize BUDGET_BYTES = usize{64} * 1024 * 1024;
    constexpr uint32 TILE_PIXELS = 256;
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
    const GraphScene scene;
    StaticArray<renderer::Mesh, 2> meshes{};
    for (usize mesh = 0; mesh < meshes.GetSize(); ++mesh)
    {
        status = reference.CreateMesh(scene.Meshes[mesh].Geometry, meshes[mesh]);
        beginning = time::NowTicks();
        while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
        {
            status = reference.GetStatus(meshes[mesh]);
        }
        REQUIRE(status == S::Ready);
    }
    renderer::LitMaterialDescription description;
    description.Surface.Tint = {RECEIVER_ALBEDO.X, RECEIVER_ALBEDO.Y, RECEIVER_ALBEDO.Z, 1};
    description.Roughness = 1;
    description.Metallic = 0;
    renderer::MaterialVersion material;
    renderer::LightSelection selection;
    status = reference.CreateLitMaterial(program, description, material, selection);
    beginning = time::NowTicks();
    while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        status = reference.GetStatus(material);
    }
    REQUIRE(status == S::Ready);
    StaticArray<renderer::SceneItem, 2> items{};
    for (usize item = 0; item < items.GetSize(); ++item)
    {
        items[item].Geometry = meshes[item];
        items[item].Surface = material;
        items[item].SourceId = item + 1;
    }
    renderer::Snapshot snapshot;
    REQUIRE(reference.CreateSnapshot(items.GetData(), items.GetSize(), snapshot) == S::Ready);
    renderer::ReferenceCaptureDescription capture;
    capture.Width = WIDTH;
    capture.Height = HEIGHT;
    capture.OrthographicCamera = true;
    capture.CenterRays = true;
    capture.TargetSamples = 1;
    capture.MaxDepth = 1;
    capture.RouletteDepth = 1;
    capture.Lights = &scene.Light;
    capture.LightCount = 1;
    Array<renderer::ReferencePixel> pixels;
    REQUIRE(pixels.TryResize(static_cast<usize>(WIDTH) * HEIGHT) == true);
    for (LinearImage* image : {&outBlockerVisible, &outBlockerOffscreen})
    {
        capture.WorldToClip = BuildCamera(image == &outBlockerVisible ? BLOCKER_VISIBLE_CAMERA_OFFSET_WORLD : 0);
        renderer::ReferenceCapture handle;
        status = reference.BeginReferenceCapture(snapshot, capture, handle);
        REQUIRE((status == S::Ready || status == S::Pending));
        beginning = time::NowTicks();
        status = S::Pending;
        while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
        {
            threading::SleepFor(time::Milliseconds(1));
            status = reference.AdvanceReferenceCapture(handle, device.Surface);
        }
        REQUIRE(status == S::Ready);
        REQUIRE(reference.CopyReferencePixels(handle, pixels.GetData(), pixels.GetSize()) == S::Ready);
        for (usize pixel = 0; pixel < pixels.GetSize(); ++pixel)
        {
            REQUIRE(pixels[pixel].Samples == 1);
            // Linear RGBA8 clamps the graph's radiance; clamp the reference the same way.
            (*image)[pixel] = math::Min(pixels[pixel].Direct.X, 1.0F);
        }
        REQUIRE(reference.Release(handle) == S::Ready);
    }
    static_cast<void>(reference.Release(snapshot));
    static_cast<void>(reference.Release(material));
    for (renderer::Mesh& mesh : meshes)
    {
        static_cast<void>(reference.Release(mesh));
    }
}
} // namespace

TEST_CASE("Hybrid shadows keep an offscreen blocker's shadow and match the RT1 direct output",
          "[render-graph][gpu][hybrid-shadows][reference-compute]")
{
    Device device;
    const float32 litRadiance = ComputeLitRadiance();
    INFO("expected lit radiance=" << litRadiance);
    // One 8-bit code of linear RGBA8, the graph outputs' resolution.
    constexpr float32 CODE = 1.0F / MAXIMUM_CODE;
    for (const float32 offsetWorld : {BLOCKER_VISIBLE_CAMERA_OFFSET_WORLD, 0.0F})
    {
        // The probes of the oracle itself: the blocker is inside the first window and outside the second.
        const bool isBlockerVisible = offsetWorld != 0;
        bool hasBlockerPixel = false;
        for (usize column = 0; column < WIDTH; ++column)
        {
            hasBlockerPixel =
                hasBlockerPixel ||
                ClassifyPoint(GetReceiverPointWorld(static_cast<float64>(column), HEIGHT / 2, offsetWorld)) ==
                    PixelClass::Blocker;
        }
        REQUIRE(hasBlockerPixel == isBlockerVisible);
    }

    // RT1 reference: CPU oracle and GPU compute, center rays, one scattering event.
    LinearImage cpuVisible{};
    LinearImage cpuOffscreen{};
    RenderReferenceImages(device, renderer::ReferenceTraversal::CpuOracle, cpuVisible, cpuOffscreen);
    LinearImage gpuVisible{};
    LinearImage gpuOffscreen{};
    RenderReferenceImages(device, renderer::ReferenceTraversal::GpuCompute, gpuVisible, gpuOffscreen);
    for (const LinearImage* image : {&cpuVisible, &cpuOffscreen, &gpuVisible, &gpuOffscreen})
    {
        const float32 offsetWorld =
            image == &cpuVisible || image == &gpuVisible ? BLOCKER_VISIBLE_CAMERA_OFFSET_WORLD : 0;
        CAPTURE(offsetWorld, image == &gpuVisible || image == &gpuOffscreen);
        // Float32 RT1 transport against the float64 BSDF evaluation of the same profile.
        RequireNoMisclassification(Compare(*image, offsetWorld, litRadiance), 1e-4F * litRadiance + 1e-6F, 0);
    }

    // Raster baseline: the default clustered deferred graph's cached directional shadow map.
    LinearImage rasterVisible{};
    LinearImage rasterOffscreen{};
    REQUIRE(RenderGraphImages(device,
                              {LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"},
                              ColorSource::FirstResource,
                              rasterVisible,
                              rasterOffscreen) == true);
    for (const LinearImage* image : {&rasterVisible, &rasterOffscreen})
    {
        const float32 offsetWorld = image == &rasterVisible ? BLOCKER_VISIBLE_CAMERA_OFFSET_WORLD : 0;
        const ShadowEvidence evidence = Compare(*image, offsetWorld, litRadiance);
        WARN("raster window offset " << offsetWorld << ": misclassified " << evidence.Misclassified << " of "
                                     << evidence.InteriorPixels << ", largest lit error " << evidence.MaximumLitError
                                     << ", edge displacement " << evidence.EdgeDisplacement << " px");
        // The map is fitted to the whole scene's casters and receivers, so the shadow survives the camera move.
        CAPTURE(offsetWorld);
        RequireNoMisclassification(evidence, CODE + .01F * litRadiance, CODE);
    }

    // Hybrid shadows: raster first hits, one shadow ray per light.
    LinearImage hybridVisible{};
    LinearImage hybridOffscreen{};
    if (device.HasRays == false ||
        RenderGraphImages(device,
                          {LUDUS_RG3_HYBRID_SHADOWS_LINEAR_DIRECTORY, "rg2_hybrid_shadows_linear.lurgrt"},
                          ColorSource::RayPassColor,
                          hybridVisible,
                          hybridOffscreen) == false)
    {
        SKIP("Native ray tracing is unavailable; the raster default graph is this device's shadow route");
    }
    for (const LinearImage* image : {&hybridVisible, &hybridOffscreen})
    {
        const bool isVisibleWindow = image == &hybridVisible;
        const float32 offsetWorld = isVisibleWindow == true ? BLOCKER_VISIBLE_CAMERA_OFFSET_WORLD : 0;
        CAPTURE(offsetWorld);
        const ShadowEvidence evidence = Compare(*image, offsetWorld, litRadiance);
        WARN("hybrid window offset " << offsetWorld << ": misclassified " << evidence.Misclassified << " of "
                                     << evidence.InteriorPixels << ", largest lit error " << evidence.MaximumLitError
                                     << ", edge displacement " << evidence.EdgeDisplacement << " px");
        RequireNoMisclassification(evidence, CODE + .01F * litRadiance, CODE);
        // Paired with RT1 at every interior receiver pixel.
        const LinearImage& paired = isVisibleWindow == true ? gpuVisible : gpuOffscreen;
        float32 maximumDifference = 0;
        for (usize row = 0; row < HEIGHT; ++row)
        {
            for (usize column = 0; column < WIDTH; ++column)
            {
                PixelClass expected = PixelClass::Blocker;
                if (TryClassifyInterior(column, row, offsetWorld, expected) == true)
                {
                    const usize pixel = row * WIDTH + column;
                    maximumDifference = math::Max(maximumDifference, math::Abs((*image)[pixel] - paired[pixel]));
                }
            }
        }
        WARN("largest hybrid/RT1 difference " << maximumDifference);
        CHECK(maximumDifference <= CODE + .01F * litRadiance);
    }
}

namespace
{
// Thanks to Lance Williams, "Casting Curved Shadows on Curved Surfaces", SIGGRAPH 1978, pp. 270–274,
// Figs. 1–6, for distinguishing false self-shadowing from a bias-induced shift of the contact boundary.
// https://cg.informatik.uni-freiburg.de/intern/seminar/shadowMapping_1978_Williams.pdf
constexpr usize CURVED_STRIP_COUNT = 16;
constexpr usize CURVED_VERTEX_COUNT = 2 * (CURVED_STRIP_COUNT + 1);
constexpr usize TRIANGLE_INDEX_COUNT = 3;
constexpr usize CURVED_INDEX_COUNT = 2 * TRIANGLE_INDEX_COUNT * CURVED_STRIP_COUNT;
constexpr float32 CURVATURE_PER_METER = .34F;
constexpr float32 CURVED_CENTER_DEPTH_WORLD = .54F;
constexpr math::Vector3 CONTACT_MINIMUM_WORLD{-.46F, -.35F, .41F};
constexpr math::Vector3 CONTACT_MAXIMUM_WORLD{-.30F, .35F, .55F};
constexpr float64 PIXEL_WIDTH_WORLD = 2.0 / WIDTH;

struct CurvedShadowScene final
{
    GraphScene Scene;
    StaticArray<math::Vector3, CURVED_VERTEX_COUNT> Positions{};
    StaticArray<math::Vector3, CURVED_VERTEX_COUNT> Normals{};
    StaticArray<uint32, CURVED_INDEX_COUNT> Indices{};
    math::Vector3d TowardLightWorld;

    explicit CurvedShadowScene(math::Vector3 lightTravelWorld) : TowardLightWorld(-Widen(lightTravelWorld))
    {
        for (usize strip = 0; strip <= CURVED_STRIP_COUNT; ++strip)
        {
            const float32 x = -1 + 2 * static_cast<float32>(strip) / CURVED_STRIP_COUNT;
            const float32 z = CURVED_CENTER_DEPTH_WORLD + CURVATURE_PER_METER * x * x;
            const float32 slope = 2 * CURVATURE_PER_METER * x;
            const math::Vector3 normalWorld = math::Vector3{slope, 0, -1} / math::Sqrt(1 + slope * slope);
            Positions[2 * strip] = {x, -1, z};
            Positions[2 * strip + 1] = {x, 1, z};
            Normals[2 * strip] = normalWorld;
            Normals[2 * strip + 1] = normalWorld;
        }
        for (usize strip = 0; strip < CURVED_STRIP_COUNT; ++strip)
        {
            const uint32 left = 2 * static_cast<uint32>(strip);
            const uint32 right = left + 2;
            const StaticArray<uint32, 2 * TRIANGLE_INDEX_COUNT> quad{
                {left, left + 1, right, right, left + 1, right + 1}};
            for (usize index = 0; index < quad.GetSize(); ++index)
            {
                Indices[strip * quad.GetSize() + index] = quad[index];
            }
        }
        Scene.Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Scene.Meshes[0].Geometry.Normals = Normals.GetData();
        Scene.Blocker = BuildBlocker(CONTACT_MINIMUM_WORLD, CONTACT_MAXIMUM_WORLD);
        Scene.Light.Direction = lightTravelWorld;
        Scene.Light.Intensity = 2;
        Scene.Cameras[0].WorldToClip = BuildCamera(0);
    }

    // Each strip is an extrusion in Y and the light has no Y component. Intersecting the XZ segments
    // therefore tests the exact triangles, including self-occlusion by a different strip.
    PixelClass Classify(float64 column, usize row) const
    {
        math::Vector3d pointWorld = GetReceiverPointWorld(column, row, 0);
        const math::Vector3d minimumWorld = Widen(CONTACT_MINIMUM_WORLD);
        const math::Vector3d maximumWorld = Widen(CONTACT_MAXIMUM_WORLD);
        if (DoesRayHitBlocker({pointWorld.X, pointWorld.Y, 0}, {0, 0, 1}, minimumWorld, maximumWorld) == true)
        {
            return PixelClass::Blocker;
        }
        const usize sourceStrip =
            math::Min(static_cast<usize>((pointWorld.X + 1) * CURVED_STRIP_COUNT / 2), CURVED_STRIP_COUNT - 1);
        const math::Vector3d startWorld = Widen(Positions[2 * sourceStrip]);
        const math::Vector3d endWorld = Widen(Positions[2 * (sourceStrip + 1)]);
        pointWorld.Z =
            startWorld.Z + (endWorld.Z - startWorld.Z) * (pointWorld.X - startWorld.X) / (endWorld.X - startWorld.X);
        if (DoesRayHitBlocker(pointWorld, TowardLightWorld, minimumWorld, maximumWorld) == true)
        {
            return PixelClass::Shadowed;
        }
        const float64 slope = TowardLightWorld.Z / TowardLightWorld.X;
        for (usize strip = 0; strip < sourceStrip; ++strip)
        {
            const math::Vector3d leftWorld = Widen(Positions[2 * strip]);
            const math::Vector3d rightWorld = Widen(Positions[2 * (strip + 1)]);
            // A sign change in the signed light-ray height brackets an intersection on this strip.
            const float64 leftHeight = leftWorld.Z - (pointWorld.Z + slope * (leftWorld.X - pointWorld.X));
            const float64 rightHeight = rightWorld.Z - (pointWorld.Z + slope * (rightWorld.X - pointWorld.X));
            if ((leftHeight < 0 && rightHeight >= 0) || (leftHeight > 0 && rightHeight <= 0))
            {
                return PixelClass::Shadowed;
            }
        }
        return PixelClass::Lit;
    }
};

constexpr PackageLocation RASTER_PACKAGE{LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"};
constexpr PackageLocation HYBRID_PACKAGE{LUDUS_RG3_HYBRID_SHADOWS_LINEAR_DIRECTORY, "rg2_hybrid_shadows_linear.lurgrt"};

void RenderCurvedShadowImages(const Device& device,
                              CurvedShadowScene& inoutFixture,
                              bool shouldSelectFallback,
                              LinearImage& outUnoccluded,
                              LinearImage& outShadowed)
{
    core::UniquePtr<renderer::RenderGraphPackage> raster = LoadPackage(RASTER_PACKAGE);
    renderer::RenderGraphActivation activation;
    REQUIRE(activation.Initialize(device.Handle,
                                  raster->GetInterfaceIdentity(),
                                  shaders::renderer_composite::Vertex(),
                                  shaders::renderer_composite::Fragment()) == S::Ready);
    REQUIRE(activation.Resize(WIDTH, HEIGHT) == S::Ready);
    const renderer::RenderGraphPreference preference = device.HasRays == true
                                                           ? renderer::RenderGraphPreference::ForceRaster
                                                           : renderer::RenderGraphPreference::Automatic;
    S status = shouldSelectFallback == true
                   ? activation.StageSelected(core::Move(raster), LoadPackage(HYBRID_PACKAGE), preference)
                   : activation.Stage(core::Move(raster));
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
    INFO((shouldSelectFallback == false                              ? "direct raster baseline"
          : preference == renderer::RenderGraphPreference::Automatic ? "automatic raster fallback"
                                                                     : "forced raster"));
    GraphScene& scene = inoutFixture.Scene;
    uint64 revision = 0;
    for (LinearImage* image : {&outUnoccluded, &outShadowed})
    {
        scene.Light.ShouldCastRasterShadows = image == &outShadowed;
        ++revision;
        renderer::RenderGraphReport report;
        REQUIRE(Poll(*graphOrNull,
                     activation.Update({scene.Meshes.GetData(), scene.Meshes.GetSize()},
                                       {scene.Items.GetData(), scene.Items.GetSize()},
                                       {scene.Cameras.GetData(), scene.Cameras.GetSize()},
                                       revision,
                                       renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                       report,
                                       &scene.Inputs,
                                       revision)) == S::Ready);
        constexpr uint32 COLOR_EXPORT = 0;
        ReadGraphImage(device, *graphOrNull, COLOR_EXPORT, *image);
        ReadGraphImage(device, *graphOrNull, COLOR_EXPORT, *image);
    }
}

void CheckCurvedShadow(std::string_view route,
                       const CurvedShadowScene& fixture,
                       const LinearImage& unoccluded,
                       const LinearImage& shadowed,
                       bool isGrazing)
{
    // Ignore probes below eight linear codes, where a one-code error moves the visibility ratio by over 12%.
    constexpr float32 MINIMUM_CONTROL_CODES = 8;
    constexpr float32 MINIMUM_CONTROL_RADIANCE = MINIMUM_CONTROL_CODES / MAXIMUM_CODE;
    constexpr float64 CONTACT_SEARCH_PIXELS = 3;
    constexpr float32 MINIMUM_LIT_VISIBILITY = .5F;
    constexpr float32 MAXIMUM_INTERIOR_VISIBILITY_LOSS = .05F;
    constexpr float64 MAXIMUM_CONTACT_DISPLACEMENT_PIXELS = 1.5;
    constexpr usize CONTACT_ROW = HEIGHT / 2;
    usize litProbes = 0;
    usize falseOcclusions = 0;
    usize attenuatedLitProbes = 0;
    usize shadowedProbes = 0;
    usize falseLights = 0;
    float32 maximumLitLoss = 0;
    float32 maximumShadowVisibility = 0;
    for (usize row = 1; row + 1 < HEIGHT; ++row)
    {
        for (usize column = 1; column + 1 < WIDTH; ++column)
        {
            const usize pixel = row * WIDTH + column;
            const float64 probe = static_cast<float64>(column);
            const PixelClass expected = fixture.Classify(probe, row);
            if (expected == PixelClass::Blocker || unoccluded[pixel] < MINIMUM_CONTROL_RADIANCE ||
                fixture.Classify(probe - 1, row) != expected || fixture.Classify(probe + 1, row) != expected ||
                fixture.Classify(probe, row - 1) != expected || fixture.Classify(probe, row + 1) != expected)
            {
                continue;
            }
            const float32 visibility = shadowed[pixel] / unoccluded[pixel];
            if (expected == PixelClass::Lit)
            {
                ++litProbes;
                if (visibility <= MINIMUM_LIT_VISIBILITY)
                {
                    if (falseOcclusions == 0)
                    {
                        WARN("first darkened lit probe: column " << column << ", row " << row << ", unoccluded "
                                                                 << unoccluded[pixel] << ", visibility " << visibility);
                    }
                    ++falseOcclusions;
                }
                const float32 litLoss = 1 - visibility;
                attenuatedLitProbes += litLoss > MAXIMUM_INTERIOR_VISIBILITY_LOSS ? 1 : 0;
                maximumLitLoss = math::Max(maximumLitLoss, litLoss);
            }
            else
            {
                ++shadowedProbes;
                falseLights += visibility > MINIMUM_LIT_VISIBILITY ? 1 : 0;
                maximumShadowVisibility = math::Max(maximumShadowVisibility, visibility);
            }
        }
    }
    REQUIRE(litProbes > 0);
    REQUIRE(shadowedProbes > 0);

    // Use the right edge of the nearby blocker's shadow, where the camera sees both sides of the edge.
    // Locate its subpixel oracle location and the raster's half-visibility crossing independently.
    bool hasContactEdge = false;
    float64 expectedEdge = 0;
    for (usize column = 1; column < WIDTH; ++column)
    {
        float64 left = static_cast<float64>(column) - 1;
        float64 right = static_cast<float64>(column);
        if (fixture.Classify(left, CONTACT_ROW) != PixelClass::Shadowed ||
            fixture.Classify(right, CONTACT_ROW) != PixelClass::Lit)
        {
            continue;
        }
        constexpr float64 EDGE_PRECISION_PIXELS = 1e-4;
        while (right - left > EDGE_PRECISION_PIXELS)
        {
            const float64 middle = (left + right) / 2;
            if (fixture.Classify(middle, CONTACT_ROW) == PixelClass::Shadowed)
            {
                left = middle;
            }
            else
            {
                right = middle;
            }
        }
        expectedEdge = (left + right) / 2;
        hasContactEdge = true;
        break;
    }
    REQUIRE(hasContactEdge == true);
    usize crossings = 0;
    float64 edgeDisplacement = 0;
    for (usize column = 1; column < WIDTH; ++column)
    {
        const float64 probe = static_cast<float64>(column);
        const usize pixel = CONTACT_ROW * WIDTH + column;
        if (math::Abs(probe - expectedEdge) > CONTACT_SEARCH_PIXELS ||
            fixture.Classify(probe - 1, CONTACT_ROW) == PixelClass::Blocker ||
            fixture.Classify(probe, CONTACT_ROW) == PixelClass::Blocker ||
            unoccluded[pixel - 1] < MINIMUM_CONTROL_RADIANCE || unoccluded[pixel] < MINIMUM_CONTROL_RADIANCE)
        {
            continue;
        }
        const float32 left = shadowed[pixel - 1] / unoccluded[pixel - 1];
        const float32 right = shadowed[pixel] / unoccluded[pixel];
        if (left <= MINIMUM_LIT_VISIBILITY && right > MINIMUM_LIT_VISIBILITY)
        {
            ++crossings;
            const float32 fraction = (MINIMUM_LIT_VISIBILITY - left) / (right - left);
            edgeDisplacement = probe - 1 + static_cast<float64>(fraction) - expectedEdge;
        }
    }
    WARN(route << ": lit probes " << litProbes << ", false occlusions " << falseOcclusions << ", maximum lit loss "
               << maximumLitLoss << ", shadowed probes " << shadowedProbes << ", false lights " << falseLights
               << ", maximum shadow visibility " << maximumShadowVisibility << ", contact edge displacement "
               << edgeDisplacement << " px (" << edgeDisplacement * PIXEL_WIDTH_WORLD << " m), crossings "
               << crossings);
    // At grazing incidence, adjacent curved facets can share a shadow-map filter footprint.
    // Bound the observed seam artifact to one column's worth of probes and half visibility,
    // with one RGBA8 code of slack at the minimum admitted control radiance.
    const usize allowedAttenuatedProbes = isGrazing == true ? HEIGHT - 2 : 0;
    const float32 allowedLitLoss =
        isGrazing == true ? .5F + 1 / MINIMUM_CONTROL_CODES : MAXIMUM_INTERIOR_VISIBILITY_LOSS;
    CAPTURE(attenuatedLitProbes, allowedAttenuatedProbes, allowedLitLoss);
    REQUIRE(attenuatedLitProbes <= allowedAttenuatedProbes);
    REQUIRE(maximumLitLoss <= allowedLitLoss);
    REQUIRE(falseLights == 0);
    REQUIRE(crossings == 1);
    REQUIRE(math::Abs(edgeDisplacement) <= MAXIMUM_CONTACT_DISPLACEMENT_PIXELS);
}
} // namespace

TEST_CASE("RT2 raster fallback retains curved-receiver shadow bias and contact visibility",
          "[render-graph][gpu][hybrid-shadows][reference-compute]")
{
    // Vulkan can run the actual automatic fallback with native queries unrequested. Metal may still
    // expose them, so exercise the documented ForceRaster selection on that backend.
    const Device device(false);
    const bool isAutomatic = device.HasRays == false;
    constexpr math::Vector3 STEEP_LIGHT_TRAVEL_WORLD{.5F, 0, 1};
    constexpr math::Vector3 GRAZING_LIGHT_TRAVEL_WORLD{2.6F, 0, 1};
    for (const math::Vector3 travelWorld : {STEEP_LIGHT_TRAVEL_WORLD, GRAZING_LIGHT_TRAVEL_WORLD})
    {
        WARN((isAutomatic == true ? "Automatic (native rays unavailable)" : "ForceRaster")
             << "; light travel X/Z=" << travelWorld.X << "/" << travelWorld.Z << "; shadow slot "
             << renderer::internal::DIRECTIONAL_SHADOW_TEXELS << " texels; normalized depth base bias "
             << renderer::ShadowRegion{}.Bias << " plus per-tap projected reconstruction uncertainty");
        const bool isGrazing = travelWorld == GRAZING_LIGHT_TRAVEL_WORLD;
        CurvedShadowScene fixture(travelWorld);
        LinearImage rasterControl{};
        LinearImage rasterShadowed{};
        RenderCurvedShadowImages(device, fixture, false, rasterControl, rasterShadowed);
        CheckCurvedShadow("Default raster", fixture, rasterControl, rasterShadowed, isGrazing);

        LinearImage fallbackControl{};
        LinearImage fallbackShadowed{};
        RenderCurvedShadowImages(device, fixture, true, fallbackControl, fallbackShadowed);
        CheckCurvedShadow(isAutomatic == true ? "Automatic fallback" : "ForceRaster",
                          fixture,
                          fallbackControl,
                          fallbackShadowed,
                          isGrazing);
        REQUIRE(fallbackControl == rasterControl);
        REQUIRE(fallbackShadowed == rasterShadowed);
    }
}

namespace
{
constexpr usize TIMING_SAMPLE_COUNT = 21;
constexpr usize TIMING_TAIL_PERCENTILE = 95;
constexpr usize PERCENTILE_DENOMINATOR = 100;
using TimingSamples = StaticArray<float64, TIMING_SAMPLE_COUNT>;

void ReportShadowTimings(std::string_view phase, TimingSamples& inoutSamples)
{
    Sort(inoutSamples.GetData(), inoutSamples.GetSize(), [](uint64 left, uint64 right) noexcept {
        return left < right;
    });
    constexpr usize TAIL_INDEX =
        (TIMING_TAIL_PERCENTILE * TIMING_SAMPLE_COUNT + PERCENTILE_DENOMINATOR - 1) / PERCENTILE_DENOMINATOR - 1;
    const float64 median = inoutSamples[TIMING_SAMPLE_COUNT / 2] / time::NANOSECONDS_PER_MILLISECOND;
    const float64 tail = inoutSamples[TAIL_INDEX] / time::NANOSECONDS_PER_MILLISECOND;
    WARN(phase << ": " << TIMING_SAMPLE_COUNT << " samples, p50 " << median << " ms, p95 " << tail << " ms");
}
} // namespace

TEST_CASE("Measure RT2 punctual shadow scene preparation and ray latency", "[.benchmark][hybrid-shadows]")
{
    const Device device(/*shouldRequestRays=*/true, /*shouldEnableGpuTiming=*/true);
    if (device.HasRays == false)
    {
        SKIP("Native inline ray queries unavailable");
    }
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(HYBRID_PACKAGE);
    const renderer::RenderGraphPlan plan = package->GetPlan();
    REQUIRE(renderer::RequiresHardwareRayTracing(plan) == true);
    REQUIRE(plan.Passes.GetSize() == HYBRID_RAY_PASS_INDEX + 1);
    const renderer::RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    renderer::RenderGraph graph;
    REQUIRE(Poll(graph, graph.Initialize(device.Handle, plan, programs, nullptr, WIDTH, HEIGHT)) == S::Ready);

    GraphScene scene;
    scene.Cameras[0].WorldToClip = BuildCamera(0);
    uint64 revision = 1;
    UpdateGraph(graph, scene, revision);
    DrawGraphAndWait(device, graph);

    TimingSamples preparation{};
    constexpr float32 EDIT_STEP_METERS = .001F;
    for (float64& durationNanoseconds : preparation)
    {
        // Edit geometry to exercise new acceleration data rather than measuring a camera-only reuse path.
        for (math::Vector3& positionWorld : scene.Blocker.Positions)
        {
            positionWorld.X += EDIT_STEP_METERS;
        }
        const uint64 beginning = time::NowTicks();
        UpdateGraph(graph, scene, ++revision);
        durationNanoseconds = static_cast<float64>(time::NowTicks() - beginning);
        DrawGraphAndWait(device, graph);
    }
    ReportShadowTimings("Geometry edit to drawable scene (CPU wall time, includes acceleration build and polling)",
                        preparation);

    TimingSamples ray{};
    for (float64& durationNanoseconds : ray)
    {
        const S armed = rhi::ArmGpuPassTimings(device.Handle);
        if (armed == S::Unsupported)
        {
            SKIP("Per-pass GPU timing is unavailable");
        }
        REQUIRE(armed == S::Ready);
        DrawGraphAndWait(device, graph);
        rhi::GpuPassTimings timings;
        const uint64 beginning = time::NowTicks();
        REQUIRE(rhi::GetGpuPassTimings(device.Handle, timings) == S::Ready);
        while (timings.State == rhi::GpuTimingState::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
        {
            threading::YieldThread();
            REQUIRE(rhi::GetGpuPassTimings(device.Handle, timings) == S::Ready);
        }
        REQUIRE(timings.State == rhi::GpuTimingState::Available);
        REQUIRE(timings.DroppedPasses == 0);
        constexpr uint64 RAY_SCOPE_ID = HYBRID_RAY_PASS_INDEX + 1;
        usize matchingPasses = 0;
        for (usize index = 0; index < timings.Count; ++index)
        {
            const rhi::GpuPassTiming& pass = timings.Passes[index];
            // The ordered graph includes visibility rasterization and the image copy under this scope.
            if (pass.Scope.Identity == RAY_SCOPE_ID && pass.Kind == rhi::GpuPassKind::Compute)
            {
                REQUIRE(pass.Timing.State == rhi::GpuTimingState::Available);
                REQUIRE(math::IsFinite(pass.Timing.DurationNanoseconds) == true);
                REQUIRE(pass.Timing.DurationNanoseconds > 0);
                durationNanoseconds = pass.Timing.DurationNanoseconds;
                ++matchingPasses;
            }
        }
        REQUIRE(matchingPasses == 1);
    }
    ReportShadowTimings("Hybrid shadow compute pass (native GPU duration)", ray);
}
