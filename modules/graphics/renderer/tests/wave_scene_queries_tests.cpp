#include "../src/internal/camera_inverse.h"
#include "../src/internal/wave_connection.h"
#include "../src/internal/wave_scene_queries.h"
#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace internal = ludus::graphics::renderer::internal;
namespace rhi = ludus::graphics::rhi;
void AddWaveTriangle(internal::ReferenceSceneData& inoutScene, float32 x, float32 z)
{
    internal::ReferenceTriangle triangle;
    triangle.Positions[0] = {x, -1, z};
    triangle.Positions[1] = {x + 1, -1, z};
    triangle.Positions[2] = {x, 1, z};
    triangle.Area = 1;
    triangle.GeometricNormal = {0, 0, 1};
    REQUIRE(inoutScene.Triangles.TryAdd(triangle) == true);
}
math::Vector3d WidenVector(math::Vector3 value)
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
} // namespace
TEST_CASE("Wave BVH regions match brute force and retain every triangle within the interaction depth",
          "[renderer][wave-region]")
{
    internal::ReferenceSceneData scene;
    // Every layer has geometry outside the centre ray but inside some finite beams.
    for (uint32 layer = 0; layer < 10; ++layer)
    {
        for (uint32 column = 0; column < 10; ++column)
        {
            AddWaveTriangle(scene, static_cast<float32>(column) * 0.3F - 1, static_cast<float32>(layer) * 0.2F + 2);
        }
    }
    REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
    for (uint32 sample = 0; sample < 32; ++sample)
    {
        math::EllipticalCone cone;
        cone.OriginWorld.X = static_cast<float64>(sample) * 0.05;
        cone.FarWorld = 10;
        cone.RadiusWorld = 0.2;
        cone.Slope = 0.02;
        cone.MinorToMajor = 0.3;
        internal::WaveInteractionRegion region;
        REQUIRE(internal::FindWaveInteractionRegion(scene, cone, region) == rhi::RasterStatus::Ready);
        float64 nearestWorld = cone.FarWorld;
        for (const internal::ReferenceTriangle& triangle : scene.Triangles)
        {
            const math::ConeTriangle geometry{WidenVector(triangle.Positions[0]),
                                              WidenVector(triangle.Positions[1]),
                                              WidenVector(triangle.Positions[2])};
            math::ConeTriangleIntersection intersection;
            REQUIRE(math::TryIntersectEllipticalConeTriangle(cone, geometry, intersection) == true);
            if (intersection.IsHit == true)
            {
                nearestWorld = math::Min(nearestWorld, intersection.NearWorld);
            }
        }
        CHECK(math::Abs(region.Frustum.NearWorld - nearestWorld) < 1e-10);
        uint32 expectedCount = 0;
        for (usize index = 0; index < scene.Triangles.GetSize(); ++index)
        {
            const internal::ReferenceTriangle& triangle = scene.Triangles[index];
            const math::ConeTriangle geometry{WidenVector(triangle.Positions[0]),
                                              WidenVector(triangle.Positions[1]),
                                              WidenVector(triangle.Positions[2])};
            math::ConeTriangleIntersection intersection;
            REQUIRE(math::TryIntersectEllipticalConeTriangle(region.Frustum, geometry, intersection) == true);
            if (intersection.IsHit == true)
            {
                REQUIRE(expectedCount < region.TriangleCount);
                CHECK(region.Triangles[expectedCount++] == index);
            }
        }
        CHECK(expectedCount == region.TriangleCount);
    }
}
TEST_CASE("Wave region exhaustion and malformed input preserve the accepted region", "[renderer][wave-region]")
{
    internal::ReferenceSceneData scene;
    for (uint32 index = 0; index <= LUDUS_WAVE_REGION_TRIANGLES; ++index)
    {
        AddWaveTriangle(scene, 0.1F, 2);
    }
    REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
    math::EllipticalCone cone;
    cone.RadiusWorld = 1;
    cone.FarWorld = 10;
    bool hasHit = false;
    REQUIRE(internal::TestWaveInteraction(scene, cone, hasHit) == rhi::RasterStatus::Ready);
    CHECK(hasHit == true);
    internal::WaveInteractionRegion accepted;
    accepted.TriangleCount = 1;
    accepted.Triangles[0] = 123;
    CHECK(internal::FindWaveInteractionRegion(scene, cone, accepted) == rhi::RasterStatus::CapacityExceeded);
    CHECK(accepted.TriangleCount == 1);
    CHECK(accepted.Triangles[0] == 123);
    cone.MinorToMajor = 0;
    CHECK(internal::TestWaveInteraction(scene, cone, hasHit) == rhi::RasterStatus::InvalidDescription);
    CHECK(hasHit == true);
    CHECK(internal::FindWaveInteractionRegion(scene, cone, accepted) == rhi::RasterStatus::InvalidDescription);
    CHECK(accepted.Triangles[0] == 123);
}

TEST_CASE("Scene optical connections preserve open and opaque limits and diffract at an edge",
          "[renderer][wave-connection]")
{
    internal::ReferenceSceneData scene;
    ludus::graphics::renderer::WaveField field{7, 8};
    REQUIRE(internal::EvaluateWaveConnection(scene, {0, 0, 0}, {0, 0, 1}, 4, 0.01, field) == rhi::RasterStatus::Ready);
    CHECK(field.Real == 1);
    CHECK(field.Imaginary == 0);
    AddWaveTriangle(scene, -10, 2);
    scene.Triangles[0].Positions[0] = {-10, -10, 2};
    scene.Triangles[0].Positions[1] = {10, -10, 2};
    scene.Triangles[0].Positions[2] = {0, 10, 2};
    REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
    REQUIRE(internal::EvaluateWaveConnection(scene, {0, 0, 0}, {0, 0, 1}, 4, 0.01, field) == rhi::RasterStatus::Ready);
    CHECK(field.Real == 0);
    CHECK(field.Imaginary == 0);
    scene.Triangles[0].Positions[0] = {-10, -10, 2};
    scene.Triangles[0].Positions[1] = {0, -10, 2};
    scene.Triangles[0].Positions[2] = {0, 10, 2};
    REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
    REQUIRE(internal::EvaluateWaveConnection(scene, {0, 0, 0}, {0, 0, 1}, 4, 0.01, field) == rhi::RasterStatus::Ready);
    const float64 power = field.Real * field.Real + field.Imaginary * field.Imaginary;
    INFO("half-plane field " << field.Real << ", " << field.Imaginary);
    CHECK(math::Abs(power - 0.25) < 0.001);
}

TEST_CASE("Scene diffraction retains cross-opening interference and propagates across two regions",
          "[renderer][wave-connection]")
{
    using ludus::graphics::renderer::WaveField;
    const auto addRectangle = [](internal::ReferenceSceneData& inoutScene, float32 left, float32 right, float32 z) {
        internal::ReferenceTriangle first;
        first.Positions[0] = {left, -10, z};
        first.Positions[1] = {right, -10, z};
        first.Positions[2] = {left, 10, z};
        first.Area = (right - left) * 10;
        first.GeometricNormal = {0, 0, 1};
        REQUIRE(inoutScene.Triangles.TryAdd(first) == true);
        first.Positions[0] = {right, -10, z};
        first.Positions[1] = {right, 10, z};
        first.Positions[2] = {left, 10, z};
        REQUIRE(inoutScene.Triangles.TryAdd(first) == true);
    };
    constexpr usize VARIANT_COUNT = 3;
    StaticArray<WaveField, VARIANT_COUNT> fields;
    for (uint32 variant = 0; variant < 3; ++variant)
    {
        internal::ReferenceSceneData scene;
        addRectangle(scene, -10, -0.1F, 2);
        addRectangle(scene, -0.02F, 0.02F, 2);
        addRectangle(scene, 0.1F, 10, 2);
        if (variant == 1)
        {
            addRectangle(scene, 0.02F, 0.1F, 2);
        }
        if (variant == 2)
        {
            addRectangle(scene, -0.1F, -0.02F, 2);
        }
        REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
        REQUIRE(internal::EvaluateWaveConnection(scene, {0, 0, 0}, {0, 0, 1}, 4, 0.01, fields[variant]) ==
                rhi::RasterStatus::Ready);
    }
    CHECK(math::Abs(fields[0].Real - fields[1].Real - fields[2].Real) < 1e-10);
    CHECK(math::Abs(fields[0].Imaginary - fields[1].Imaginary - fields[2].Imaginary) < 1e-10);
    const float64 combinedPower = fields[0].Real * fields[0].Real + fields[0].Imaginary * fields[0].Imaginary;
    const float64 incoherentPower = fields[1].Real * fields[1].Real + fields[1].Imaginary * fields[1].Imaginary +
                                    fields[2].Real * fields[2].Real + fields[2].Imaginary * fields[2].Imaginary;
    CHECK(combinedPower > incoherentPower * 1.5);
    internal::ReferenceSceneData twoRegions;
    addRectangle(twoRegions, -10, -0.1F, 2);
    addRectangle(twoRegions, -0.02F, 0.02F, 2);
    addRectangle(twoRegions, 0.1F, 10, 2);
    addRectangle(twoRegions, -10, 0, 3);
    REQUIRE(internal::BuildReferenceBvh(twoRegions) == rhi::RasterStatus::Ready);
    math::EllipticalCone probe;
    probe.RadiusWorld = 3 * math::Sqrt(2 * 4 / (2 * math::kPi / 0.01));
    probe.FarWorld = 3.98;
    internal::WaveInteractionRegion firstRegion;
    REQUIRE(internal::FindWaveInteractionRegion(twoRegions, probe, firstRegion) == rhi::RasterStatus::Ready);
    CHECK(firstRegion.TriangleCount == 4);
    probe.NearWorld = firstRegion.Frustum.FarWorld + 1e-6;
    internal::WaveInteractionRegion secondRegion;
    REQUIRE(internal::FindWaveInteractionRegion(twoRegions, probe, secondRegion) == rhi::RasterStatus::Ready);
    CHECK(secondRegion.TriangleCount == 1);
    WaveField field;
    REQUIRE(internal::EvaluateWaveConnection(twoRegions, {0, 0, 0}, {0, 0, 1}, 4, 0.01, field) ==
            rhi::RasterStatus::Ready);
    INFO("one region " << fields[0].Real << "," << fields[0].Imaginary << " two regions " << field.Real << ","
                       << field.Imaginary);
    CHECK(math::Abs(field.Real - fields[0].Real) + math::Abs(field.Imaginary - fields[0].Imaginary) > 0.01);
}

TEST_CASE("Reference cameras retain a valid conventional projection far from the world origin", "[renderer][camera]")
{
    ludus::graphics::renderer::ViewDescription camera;
    camera.CameraPosition = {300, 0, 301.5F};
    const float32 depthScale = 100.0F / (100.0F - 0.01F);
    camera.WorldToClip.Columns[0] = {1, 0, 0, 0};
    camera.WorldToClip.Columns[1] = {0, 1, 0, 0};
    camera.WorldToClip.Columns[2] = {0, 0, -depthScale, -1};
    camera.WorldToClip.Columns[3] = {-300, 0, depthScale * (301.5F - 0.01F), 301.5F};
    math::Matrix4 inverse;
    CHECK(math::TryInverse(camera.WorldToClip, inverse) == math::MathStatus::IllConditioned);
    REQUIRE(internal::TryMakeCameraClipToWorld(camera, inverse) == true);
    internal::ReferenceJob job;
    job.Description.Width = 16;
    job.Description.Height = 16;
    job.Description.CameraPosition = camera.CameraPosition;
    job.Description.WorldToClip = camera.WorldToClip;
    REQUIRE(internal::PrepareReferenceCamera(job) == rhi::RasterStatus::Ready);
    CHECK(math::IsFinite(job.ClipToWorld) == true);
}

namespace
{
/// A camera about 108 m from the world origin with a 1 cm near plane, like the Sample project's terrain World view
/// (issue #1761), at that view's preview extent.
constexpr math::Vector3 DISTANT_EYE_WORLD{-62.5F, 31.25F, 83.5F};
constexpr math::Vector3 DISTANT_TARGET_WORLD{-40, 0, 50};
constexpr float32 DISTANT_VERTICAL_FOV_RADIANS = 1;
constexpr float32 DISTANT_NEAR_WORLD = .01F;
constexpr float32 DISTANT_FAR_WORLD = 1000;
/// Half the vertical extent of the orthographic variant, in metres.
constexpr float32 DISTANT_ORTHOGRAPHIC_HALF_HEIGHT_WORLD = 20;
constexpr uint32 DISTANT_WIDTH = 973;
constexpr uint32 DISTANT_HEIGHT = 543;
/// Pixels between the sampled pixel centres along each image axis.
constexpr uint32 DISTANT_STRIDE_PIXELS = 7;
/// Distances along each camera ray, from its origin, at which its point is projected back to the image.
constexpr usize PROBE_DISTANCE_COUNT = 3;
constexpr StaticArray<float64, PROBE_DISTANCE_COUNT> PROBE_DISTANCES_WORLD{{1, 10, 100}};
/// Largest accepted distance between a pixel centre and the projection of its camera ray, in pixels: well inside
/// the 1/16 pixel a visibility-buffer reconstruction accepts at a triangle edge.
constexpr float64 CAMERA_RAY_TOLERANCE_PIXELS = .01;
/// The world-space construction this replaced misses by more than this for the perspective camera.
constexpr float64 WORLD_SPACE_RAY_MISS_PIXELS = .1;

/// The camera a ray-tracing graph traces: reverse-Z drawn in conventional depth, as RenderGraph::Update converts it.
void PrepareDistantCameraJob(bool isOrthographic, internal::ReferenceJob& outJob)
{
    math::Matrix4 view;
    REQUIRE(math::TryLookAt(DISTANT_EYE_WORLD, DISTANT_TARGET_WORLD, {0, 1, 0}, view) == math::MathStatus::Success);
    const float32 aspect = static_cast<float32>(DISTANT_WIDTH) / static_cast<float32>(DISTANT_HEIGHT);
    math::Matrix4 projection;
    if (isOrthographic == true)
    {
        const float32 halfWidthWorld = DISTANT_ORTHOGRAPHIC_HALF_HEIGHT_WORLD * aspect;
        REQUIRE(math::TryOrthographicReverseZ(-halfWidthWorld,
                                              halfWidthWorld,
                                              -DISTANT_ORTHOGRAPHIC_HALF_HEIGHT_WORLD,
                                              DISTANT_ORTHOGRAPHIC_HALF_HEIGHT_WORLD,
                                              DISTANT_NEAR_WORLD,
                                              DISTANT_FAR_WORLD,
                                              projection) == math::MathStatus::Success);
    }
    else
    {
        REQUIRE(math::TryPerspectiveReverseZ(DISTANT_VERTICAL_FOV_RADIANS,
                                             aspect,
                                             DISTANT_NEAR_WORLD,
                                             DISTANT_FAR_WORLD,
                                             projection) == math::MathStatus::Success);
    }
    outJob.Description.Width = DISTANT_WIDTH;
    outJob.Description.Height = DISTANT_HEIGHT;
    outJob.Description.CenterRays = true;
    outJob.Description.OrthographicCamera = isOrthographic;
    outJob.Description.CameraPosition = DISTANT_EYE_WORLD;
    outJob.Description.WorldToClip = projection * view;
    for (math::Vector4& column : outJob.Description.WorldToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    REQUIRE(internal::PrepareReferenceCamera(outJob) == rhi::RasterStatus::Ready);
}
/// One row of a float32 homogeneous transform applied, in float64, to (pointWorld, 1).
float64 TransformComponent(float32 rowX, float32 rowY, float32 rowZ, float32 rowTranslation, math::Vector3d pointWorld)
{
    return static_cast<float64>(rowX) * pointWorld.X + static_cast<float64>(rowY) * pointWorld.Y +
           static_cast<float64>(rowZ) * pointWorld.Z + static_cast<float64>(rowTranslation);
}
/// A camera ray in float64: its world origin and unit direction.
struct CameraRayLine final
{
    math::Vector3d OriginWorld{};
    math::Vector3d Direction{};
};
/// Distance in pixels between the centre of a pixel, by row-major index, and the float64 projection of a world point
/// through worldToClip.
float64 MeasurePixelMiss(const math::Matrix4& worldToClip, math::Vector3d pointWorld, uint32 pixel)
{
    const uint32 x = pixel % DISTANT_WIDTH;
    const uint32 y = pixel / DISTANT_WIDTH;
    const math::Vector4* columns = worldToClip.Columns;
    const float64 clipX = TransformComponent(columns[0].X, columns[1].X, columns[2].X, columns[3].X, pointWorld);
    const float64 clipY = TransformComponent(columns[0].Y, columns[1].Y, columns[2].Y, columns[3].Y, pointWorld);
    const float64 clipW = TransformComponent(columns[0].W, columns[1].W, columns[2].W, columns[3].W, pointWorld);
    const float64 pixelX = (clipX / clipW + 1) / 2 * DISTANT_WIDTH;
    const float64 pixelY = (1 - clipY / clipW) / 2 * DISTANT_HEIGHT;
    const float64 missX = pixelX - (static_cast<float64>(x) + .5);
    const float64 missY = pixelY - (static_cast<float64>(y) + .5);
    return math::Sqrt(missX * missX + missY * missY);
}
/// The largest miss over sampled pixel centres, by row-major index, of makeRay's rays at each probe distance.
template <typename MakeRay>
float64 MeasureLargestRayMiss(const internal::ReferenceJob& job, MakeRay makeRay)
{
    float64 largestMiss = 0;
    for (uint32 y = 0; y < DISTANT_HEIGHT; y += DISTANT_STRIDE_PIXELS)
    {
        for (uint32 x = 0; x < DISTANT_WIDTH; x += DISTANT_STRIDE_PIXELS)
        {
            const uint32 pixel = y * DISTANT_WIDTH + x;
            const CameraRayLine ray = makeRay(pixel);
            for (const float64 distanceWorld : PROBE_DISTANCES_WORLD)
            {
                const math::Vector3d pointWorld{ray.OriginWorld.X + ray.Direction.X * distanceWorld,
                                                ray.OriginWorld.Y + ray.Direction.Y * distanceWorld,
                                                ray.OriginWorld.Z + ray.Direction.Z * distanceWorld};
                largestMiss = math::Max(largestMiss, MeasurePixelMiss(job.Description.WorldToClip, pointWorld, pixel));
            }
        }
    }
    return largestMiss;
}
/// The oracle's pixel-centre camera ray.
float64 MeasureOracleRayMiss(const internal::ReferenceJob& job)
{
    return MeasureLargestRayMiss(job, [&job](uint32 pixel) {
        internal::ReferencePath path;
        path.Pixel = pixel;
        rhi::RayInput ray;
        REQUIRE(internal::GenerateReferencePrimary(job, path, ray) == rhi::RasterStatus::Ready);
        return CameraRayLine{WidenVector({ray.Origin[0], ray.Origin[1], ray.Origin[2]}),
                             WidenVector({ray.Direction[0], ray.Direction[1], ray.Direction[2]})};
    });
}
} // namespace

TEST_CASE("Camera rays of a camera far from the world origin stay on the raster projection",
          "[renderer][camera][visibility]")
{
    SECTION("Perspective")
    {
        internal::ReferenceJob job;
        PrepareDistantCameraJob(false, job);
        // The direct inverse is ill-conditioned, so the world-space matrix comes from the eye-relative inverse.
        math::Matrix4 inverse;
        CHECK(math::TryInverse(job.Description.WorldToClip, inverse) == math::MathStatus::IllConditioned);
        const float64 oracleMiss = MeasureOracleRayMiss(job);
        CAPTURE(oracleMiss);
        CHECK(oracleMiss < CAMERA_RAY_TOLERANCE_PIXELS);
        // The float32 world-space ClipToWorld the rays were once built from carries eye * w terms that cost the
        // same pixels a sizeable fraction of a pixel, so this camera exercises the eye-relative construction.
        const float64 worldSpaceMiss = MeasureLargestRayMiss(job, [&job](uint32 pixel) {
            const uint32 x = pixel % DISTANT_WIDTH;
            const uint32 y = pixel / DISTANT_WIDTH;
            const float32 clipX = 2 * (static_cast<float32>(x) + .5F) / static_cast<float32>(DISTANT_WIDTH) - 1;
            const float32 clipY = 1 - 2 * (static_cast<float32>(y) + .5F) / static_cast<float32>(DISTANT_HEIGHT);
            const math::Vector4 farWorld = job.ClipToWorld * math::Vector4{clipX, clipY, 1, 1};
            const math::Vector3 farPointWorld{farWorld.X / farWorld.W,
                                              farWorld.Y / farWorld.W,
                                              farWorld.Z / farWorld.W};
            return CameraRayLine{WidenVector(DISTANT_EYE_WORLD),
                                 WidenVector(internal::ReferenceNormalize(farPointWorld - DISTANT_EYE_WORLD))};
        });
        CAPTURE(oracleMiss, worldSpaceMiss);
        CHECK(worldSpaceMiss > WORLD_SPACE_RAY_MISS_PIXELS);
    }
    SECTION("Orthographic")
    {
        internal::ReferenceJob job;
        PrepareDistantCameraJob(true, job);
        const float64 oracleMiss = MeasureOracleRayMiss(job);
        CAPTURE(oracleMiss);
        CHECK(oracleMiss < CAMERA_RAY_TOLERANCE_PIXELS);
    }
}
