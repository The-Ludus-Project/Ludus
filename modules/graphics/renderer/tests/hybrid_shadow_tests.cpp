// RT2 hybrid shadows (issue 1248): a closed blocker that leaves the camera frustum must keep its shadow on the
// visible receiver. One scene is drawn by the hybrid-shadows template (raster first hits, one shadow ray per light),
// by the default clustered deferred graph (its cached directional shadow map, the raster baseline) and by the RT1
// reference (CPU oracle and GPU compute). Each is compared with an independent float64 projection of the blocker
// along the light rays, and the hybrid image with the paired RT1 first-surface direct output.
// Thanks to Franklin C. Crow, "Shadow Algorithms for Computer Graphics", SIGGRAPH 1977, pp. 242-248, Fig. 4, for
// separating light-space shadow boundaries from viewer-space hidden surface removal.
// https://www.cs.rpi.edu/~cutler/classes/advancedgraphics/S10/papers/crow77_shadows.pdf

#include "render_graph_gpu_fixture.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_lit.h"
#include "renderer_lit_shadow.h"
#include "renderer_output.h"
#include "renderer_reference.h"
#include "renderer_reference_reduce.h"
#include "renderer_shadow.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/device.h>

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
BlockerMesh BuildBlocker()
{
    BlockerMesh result;
    const StaticArray<StaticArray<float32, 3>, 2> bounds{
        {{{BLOCKER_MINIMUM_WORLD.X, BLOCKER_MINIMUM_WORLD.Y, BLOCKER_MINIMUM_WORLD.Z}},
         {{BLOCKER_MAXIMUM_WORLD.X, BLOCKER_MAXIMUM_WORLD.Y, BLOCKER_MAXIMUM_WORLD.Z}}}};
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
bool DoesRayHitBlocker(math::Vector3d originWorld, math::Vector3d directionWorld)
{
    const StaticArray<float64, 3> origin{{originWorld.X, originWorld.Y, originWorld.Z}};
    const StaticArray<float64, 3> direction{{directionWorld.X, directionWorld.Y, directionWorld.Z}};
    const math::Vector3d minimumWorld = Widen(BLOCKER_MINIMUM_WORLD);
    const math::Vector3d maximumWorld = Widen(BLOCKER_MAXIMUM_WORLD);
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
    Device()
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Required.Compute = true;
        description.Preferred.RayQueries = true;
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

/// Draw one frame and read the red channel of an exported linear RGBA8 resource.
void ReadGraphImage(const Device& device, renderer::RenderGraph& inoutGraph, uint32 resource, LinearImage& outImage)
{
    rhi::SubmissionToken completion;
    REQUIRE(inoutGraph.Render(device.Surface, WIDTH, HEIGHT, completion) == S::Ready);
    const uint64 beginning = time::NowTicks();
    S status = rhi::GetStatus(device.Handle, completion);
    while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
        status = rhi::GetStatus(device.Handle, completion);
    }
    REQUIRE(status == S::Ready);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(inoutGraph.GetExport(resource, texture, aspect) == S::Ready);
    rhi::ReadbackTicket ticket;
    status = rhi::RequestTextureReadback(device.Handle,
                                         texture,
                                         {0, 0, 0, WIDTH, HEIGHT},
                                         static_cast<usize>(WIDTH) * CHANNEL_COUNT,
                                         ticket);
    while (status == S::Pending && time::NowTicks() - beginning < TIMEOUT_NANOSECONDS)
    {
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
    constexpr usize RAY_PASS = 1;
    REQUIRE((source == ColorSource::FirstResource || RAY_PASS < plan.Passes.GetSize()));
    const uint32 colorResource = source == ColorSource::FirstResource ? 0 : plan.Passes[RAY_PASS].Color;
    uint64 revision = 0;
    for (LinearImage* image : {&outBlockerVisible, &outBlockerOffscreen})
    {
        const float32 offsetWorld = image == &outBlockerVisible ? BLOCKER_VISIBLE_CAMERA_OFFSET_WORLD : 0;
        scene.Cameras[0].WorldToClip = BuildCamera(offsetWorld);
        ++revision;
        renderer::RenderGraphReport report;
        REQUIRE(Poll(graph,
                     graph.Update({scene.Meshes.GetData(), scene.Meshes.GetSize()},
                                  {scene.Items.GetData(), scene.Items.GetSize()},
                                  {scene.Cameras.GetData(), scene.Cameras.GetSize()},
                                  revision,
                                  renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                  report,
                                  &scene.Inputs,
                                  revision)) == S::Ready);
        // The default graph draws its directional map before the first resolve; read the second frame.
        ReadGraphImage(device, graph, colorResource, *image);
        ReadGraphImage(device, graph, colorResource, *image);
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
