#include "../src/internal/camera_inverse.h"
#include "../src/internal/wave_connection.h"
#include "../src/internal/wave_scene_queries.h"
#include <catch2/catch_test_macros.hpp>
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
    internal::WaveInteractionRegion accepted;
    accepted.TriangleCount = 1;
    accepted.Triangles[0] = 123;
    CHECK(internal::FindWaveInteractionRegion(scene, cone, accepted) == rhi::RasterStatus::CapacityExceeded);
    CHECK(accepted.TriangleCount == 1);
    CHECK(accepted.Triangles[0] == 123);
    cone.MinorToMajor = 0;
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
