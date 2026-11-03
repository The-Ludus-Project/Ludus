#include "shadow_occlusion_oracle.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

// The exact visible-emitter oracle of the analytic shadow-ray test (render_graph_ray_tests.cpp) against independent
// evidence: FoundationMath's projected solid angle of the unoccluded emitter, and a midpoint quadrature of the
// visible irradiance over the emitter's area whose visibility tests every quadrature point's segment against every
// slat. The receivers are the louvre configurations that the GPU test renders.
namespace
{
using namespace ludus::foundation;
namespace testing = ludus::graphics::renderer::testing;

// 256^2 emitter cells. The cells that visibility edges cut give the quadrature an error that falls as 1 / 256; it is
// largest 2 cm below the louvre near the emitter, where the slats' projections are magnified about eight times. Over
// these receivers the largest error was 2.2e-3 of the emitter's projected solid angle at 256^2 and 5.5e-4 at 1024^2, of
// alternating sign around the oracle.
constexpr usize QUADRATURE_SUBDIVISIONS = 256;
constexpr float64 QUADRATURE_TOLERANCE = 3e-3;
constexpr math::Vector3d WALL_NORMAL_WORLD{0, 0, 1};
constexpr math::Vector3d EMITTING_NORMAL_WORLD{0, -1, 0};

/// The visible projected solid angle by a midpoint rule over the rectangular emitter, testing each cell's center.
float64 IntegrateVisibleProjectedSolidAngle(const testing::OcclusionPolygon& emitter,
                                            const testing::Louvre& louvre,
                                            math::Vector3d receiverWorld)
{
    const math::Vector3d origin = emitter.VerticesWorld[0];
    const math::Vector3d edge1 = emitter.VerticesWorld[1] - origin;
    const math::Vector3d edge3 = emitter.VerticesWorld[3] - origin;
    const float64 cellArea = math::Length(math::Cross(edge1, edge3)) /
                             static_cast<float64>(QUADRATURE_SUBDIVISIONS * QUADRATURE_SUBDIVISIONS);
    float64 sum = 0;
    for (usize row = 0; row < QUADRATURE_SUBDIVISIONS; ++row)
    {
        for (usize column = 0; column < QUADRATURE_SUBDIVISIONS; ++column)
        {
            const float64 u = (static_cast<float64>(column) + 0.5) / static_cast<float64>(QUADRATURE_SUBDIVISIONS);
            const float64 v = (static_cast<float64>(row) + 0.5) / static_cast<float64>(QUADRATURE_SUBDIVISIONS);
            const math::Vector3d pointWorld = origin + edge1 * u + edge3 * v;
            bool isVisible = true;
            for (usize slat = 0; slat < louvre.SlatCount && isVisible == true; ++slat)
            {
                isVisible = testing::DoesSegmentCrossPolygon(receiverWorld, pointWorld, louvre.Slats[slat]) == false;
            }
            if (isVisible == false)
            {
                continue;
            }
            const math::Vector3d offset = pointWorld - receiverWorld;
            const float64 distanceSquared = math::LengthSquared(offset);
            const float64 receiverCosine = math::Dot(WALL_NORMAL_WORLD, offset);
            const float64 emitterCosine = -math::Dot(EMITTING_NORMAL_WORLD, offset);
            sum += receiverCosine * emitterCosine / (distanceSquared * distanceSquared) * cellArea;
        }
    }
    return sum;
}

testing::OccludedPolygonLight MakeLight(const testing::Louvre& louvre)
{
    testing::OccludedPolygonLight light;
    light.Emitter = testing::MakeCeilingEmitter();
    light.EmittingNormalWorld = EMITTING_NORMAL_WORLD;
    light.Occluders = {louvre.Slats.GetData(), louvre.SlatCount};
    return light;
}
} // namespace

TEST_CASE("The occlusion oracle's unoccluded emitter matches FoundationMath's projected solid angle",
          "[shadow-occlusion-oracle]")
{
    const testing::Louvre noLouvre;
    const testing::OccludedPolygonLight light = MakeLight(noLouvre);
    constexpr usize RECEIVER_COUNT = 4;
    const StaticArray<math::Vector3d, RECEIVER_COUNT> receiversWorld{{
        {0, 0, testing::WALL_Z_WORLD},
        {-1.5, 0.9, testing::WALL_Z_WORLD},
        {2.5, -1.8, testing::WALL_Z_WORLD},
        {0.3, 1.05, testing::WALL_Z_WORLD},
    }};
    for (const math::Vector3d receiverWorld : receiversWorld)
    {
        testing::VisibleProjectedSolidAngle solidAngle;
        REQUIRE(testing::TryComputeVisibleProjectedSolidAngle(light, receiverWorld, WALL_NORMAL_WORLD, solidAngle) ==
                testing::OcclusionOracleStatus::Success);
        CHECK(solidAngle.Visible == solidAngle.Emitter);
        CHECK(solidAngle.CoveringOccluder == testing::NO_COVERING_OCCLUDER);
        // The wall's tangent frame: x and y stay, z is the normal.
        math::ProjectedPolygonInput input;
        input.VertexCount = static_cast<uint32>(light.Emitter.VertexCount);
        for (usize i = 0; i < light.Emitter.VertexCount; ++i)
        {
            const math::Vector3d offset = light.Emitter.VerticesWorld[i] - receiverWorld;
            input.PositionsTangent[i] = {static_cast<float32>(offset.X),
                                         static_cast<float32>(offset.Y),
                                         static_cast<float32>(offset.Z)};
        }
        math::ProjectedSolidAnglePolygon prepared;
        REQUIRE(math::TryPrepareProjectedSolidAnglePolygon(input, prepared) == math::MathStatus::Success);
        INFO("receiver " << receiverWorld.X << ',' << receiverWorld.Y << ": oracle " << solidAngle.Emitter
                         << ", FoundationMath " << prepared.ProjectedSolidAngle);
        // FoundationMath evaluates in float32.
        constexpr float64 SINGLE_PRECISION_TOLERANCE = 1e-5;
        CHECK(math::Abs(solidAngle.Emitter - static_cast<float64>(prepared.ProjectedSolidAngle)) <
              SINGLE_PRECISION_TOLERANCE * solidAngle.Emitter);
    }
}

TEST_CASE("The occlusion oracle matches an area quadrature of the visible emitter behind each louvre",
          "[shadow-occlusion-oracle]")
{
    struct Case final
    {
        testing::LouvreDescription Description;
        float64 ReceiverMaximumYWorld = 0; ///< Receivers below the louvre's plane see the emitter through it.
    };
    const StaticArray<Case, 2> cases{{
        {testing::NEAR_EMITTER_LOUVRE, testing::NEAR_EMITTER_LOUVRE.HeightWorld},
        {testing::WALL_LOUVRE, testing::WALL_LOUVRE.HeightWorld},
    }};
    // Receivers over the lit part of the wall, including some within a slat width of the louvre's plane.
    constexpr usize RECEIVER_COLUMNS = 5;
    constexpr usize RECEIVER_ROWS = 4;
    constexpr float64 RECEIVER_MINIMUM_X_WORLD = -2.2;
    constexpr float64 RECEIVER_X_STEP_WORLD = 1.1;
    constexpr StaticArray<float64, RECEIVER_ROWS> RECEIVER_DEPTHS_BELOW_LOUVRE_WORLD{{0.02, 0.1, 0.45, 1.3}};
    for (const Case& louvreCase : cases)
    {
        testing::Louvre louvre;
        REQUIRE(testing::TryBuildLouvre(louvreCase.Description, louvre) == true);
        REQUIRE(louvre.SlatCount > 2);
        const testing::OccludedPolygonLight light = MakeLight(louvre);
        usize partialCount = 0;
        for (usize row = 0; row < RECEIVER_ROWS; ++row)
        {
            for (usize column = 0; column < RECEIVER_COLUMNS; ++column)
            {
                const math::Vector3d receiverWorld{
                    RECEIVER_MINIMUM_X_WORLD + static_cast<float64>(column) * RECEIVER_X_STEP_WORLD,
                    louvreCase.ReceiverMaximumYWorld - RECEIVER_DEPTHS_BELOW_LOUVRE_WORLD[row],
                    testing::WALL_Z_WORLD};
                testing::VisibleProjectedSolidAngle solidAngle;
                REQUIRE(testing::TryComputeVisibleProjectedSolidAngle(light,
                                                                      receiverWorld,
                                                                      WALL_NORMAL_WORLD,
                                                                      solidAngle) ==
                        testing::OcclusionOracleStatus::Success);
                const float64 quadrature = IntegrateVisibleProjectedSolidAngle(light.Emitter, louvre, receiverWorld);
                INFO("louvre at y=" << louvreCase.Description.HeightWorld << ", receiver " << receiverWorld.X << ','
                                    << receiverWorld.Y << ": oracle " << solidAngle.Visible << " of "
                                    << solidAngle.Emitter << ", quadrature " << quadrature);
                CHECK(math::Abs(solidAngle.Visible - quadrature) < QUADRATURE_TOLERANCE * solidAngle.Emitter);
                CHECK(solidAngle.Visible <= solidAngle.Emitter);
                const bool isPartial = solidAngle.Visible > 0 && solidAngle.Visible < solidAngle.Emitter;
                partialCount += isPartial == true ? 1 : 0;
            }
        }
        // The configurations exercise partial visibility, not only umbra and full light.
        CHECK(partialCount > RECEIVER_COLUMNS * RECEIVER_ROWS / 2);
    }
}

TEST_CASE("The occlusion oracle reports umbra, the emitter's back side and unsupported occluders",
          "[shadow-occlusion-oracle]")
{
    const math::Vector3d receiverWorld{0, 0, testing::WALL_Z_WORLD};
    // A plate below the emitter large enough to hide all of it from the receiver.
    testing::OcclusionPolygon plate;
    plate.VerticesWorld[0] = {-1, 0.5, -1};
    plate.VerticesWorld[1] = {1, 0.5, -1};
    plate.VerticesWorld[2] = {1, 0.5, 0.5};
    plate.VerticesWorld[3] = {-1, 0.5, 0.5};
    plate.VertexCount = 4;
    testing::OccludedPolygonLight light;
    light.Emitter = testing::MakeCeilingEmitter();
    light.EmittingNormalWorld = EMITTING_NORMAL_WORLD;
    light.Occluders = {&plate, 1};
    testing::VisibleProjectedSolidAngle solidAngle;
    REQUIRE(testing::TryComputeVisibleProjectedSolidAngle(light, receiverWorld, WALL_NORMAL_WORLD, solidAngle) ==
            testing::OcclusionOracleStatus::Success);
    CHECK(solidAngle.CoveringOccluder == 0);
    CHECK(solidAngle.Visible < 1e-12);
    CHECK(solidAngle.Emitter > 0);
    // Above the emitter's plane the one-sided emitter faces away.
    REQUIRE(testing::TryComputeVisibleProjectedSolidAngle(light,
                                                          {0, testing::EMITTER_HEIGHT_WORLD + 0.1, -1},
                                                          WALL_NORMAL_WORLD,
                                                          solidAngle) == testing::OcclusionOracleStatus::Success);
    CHECK(solidAngle.Emitter == 0);
    CHECK(solidAngle.Visible == 0);
    // A plate tilted through the receiver's height has no single central projection.
    plate.VerticesWorld[0].Y = -0.5;
    CHECK(testing::TryComputeVisibleProjectedSolidAngle(light, receiverWorld, WALL_NORMAL_WORLD, solidAngle) ==
          testing::OcclusionOracleStatus::StraddlingOccluder);
    // The receiver's horizon would cut the emitter.
    CHECK(testing::TryComputeVisibleProjectedSolidAngle(light, {0, 0, 0}, WALL_NORMAL_WORLD, solidAngle) ==
          testing::OcclusionOracleStatus::BelowHorizon);
    // A zero or nonfinite emitting normal defines no emitting side.
    light.EmittingNormalWorld = {};
    CHECK(testing::TryComputeVisibleProjectedSolidAngle(light, receiverWorld, WALL_NORMAL_WORLD, solidAngle) ==
          testing::OcclusionOracleStatus::InvalidInput);
    light.EmittingNormalWorld = {0, std::numeric_limits<float64>::quiet_NaN(), 0};
    CHECK(testing::TryComputeVisibleProjectedSolidAngle(light, receiverWorld, WALL_NORMAL_WORLD, solidAngle) ==
          testing::OcclusionOracleStatus::InvalidInput);
}
