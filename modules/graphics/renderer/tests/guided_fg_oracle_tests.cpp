#include "guided_fg_oracle.h"
#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/math/scalar.hpp>

using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
namespace renderer = ludus::graphics::renderer;

TEST_CASE("FG photon oracle traces scattering and estimates diffuse photon density", "[renderer][guided-fg][oracle]")
{
    FgOracleTriangle floor{{-10, -10, 0}, {10, -10, 0}, {0, 10, 0}, {}};
    floor.Material.Scattering.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    floor.Material.BaseColor = {0.5F, 0.5F, 0.5F};
    const StaticArray<math::Vector3, FG_ORACLE_DEPTH> samples{{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};
    FgOraclePhotons photons{};
    REQUIRE(TraceFgOraclePhotons({&floor, 1}, {0, 0, 1}, {0, 0, -1}, {2, 2, 2}, samples, photons) == true);
    REQUIRE(photons[0].IsValid == true);
    CHECK(photons[0].IsCaustic == false);
    CHECK(photons[1].IsValid == false);
    CHECK(photons[0].PositionWorld.Z == 0);
    const math::Vector3d density = GatherFgOraclePhotons({photons.GetData(), photons.GetSize()},
                                                         {0, 0, 0},
                                                         {0, 0, 1},
                                                         {0, 0, 1},
                                                         floor.Material,
                                                         1,
                                                         false);
    constexpr float64 TOLERANCE = 1e-7;
    CHECK(math::Abs(density.X - 1 / (math::kPi * math::kPi)) < TOLERANCE);
    CHECK(GatherFgOraclePhotons({photons.GetData(), photons.GetSize()},
                                {0, 0, 0},
                                {0, 0, 1},
                                {0, 0, 1},
                                floor.Material,
                                1,
                                true)
              .X == 0);
    CHECK(GatherFgOraclePhotons({photons.GetData(), photons.GetSize()},
                                {0, 0, 2},
                                {0, 0, 1},
                                {0, 0, 1},
                                floor.Material,
                                1,
                                false)
              .X == 0);
}
TEST_CASE("FG guide mixture preserves emitted energy and reconnection is reversible", "[renderer][guided-fg][oracle]")
{
    constexpr uint32 CELLS = 16;
    constexpr uint32 UNIFORM_PATHS = 7;
    constexpr float64 TOLERANCE = 1e-12;
    float64 energy = 0;
    for (uint32 cell = 0; cell < CELLS; ++cell)
    {
        const uint32 paths = cell + 1;
        const float64 expectedCount = paths + static_cast<float64>(UNIFORM_PATHS) / CELLS;
        energy += expectedCount * ComputeFgOracleFluxScale(paths, UNIFORM_PATHS);
    }
    CHECK(math::Abs(energy - 1) < TOLERANCE);
    const math::Vector3d oldOffsetWorld{0, 0, 1};
    const math::Vector3d newOffsetWorld{1, 0, 2};
    const math::Vector3d normalWorld{0, 0, 1};
    const float64 forward = ComputeFgOracleJacobian(oldOffsetWorld, newOffsetWorld, normalWorld);
    const float64 reverse = ComputeFgOracleJacobian(newOffsetWorld, oldOffsetWorld, normalWorld);
    CHECK(math::Abs(forward * reverse - 1) < TOLERANCE);
    CHECK(ComputeFgOracleJacobian({}, newOffsetWorld, normalWorld) == 0);
    const float64 first = ComputeFgOraclePairWeight(2, 1, 0.5, 1);
    const float64 second = ComputeFgOraclePairWeight(1, 2, 1, 0.5);
    CHECK(first + second == 1);
    CHECK(ComputeFgOraclePairWeight(1, 1, 0, 0) == 0);
}
TEST_CASE("FG photon oracle distinguishes a glossy-to-diffuse caustic", "[renderer][guided-fg][oracle]")
{
    StaticArray<FgOracleTriangle, 2> triangles{};
    triangles[0] = {{-10, -10, 10}, {10, -10, -10}, {0, 10, 0}, {}};
    triangles[0].Material.Scattering.Model = ludus::content::scene_format::MaterialModel::Conductor;
    triangles[0].Material.Scattering.Eta = {{0.2F, 0.2F, 0.2F}};
    triangles[0].Material.Scattering.K = {{3, 3, 3}};
    triangles[0].Material.Scattering.AlphaU = 0.0001F;
    triangles[0].Material.Scattering.AlphaV = 0.0001F;
    triangles[1] = {{2, -10, -10}, {2, -10, 10}, {2, 10, 0}, {}};
    triangles[1].Material.Scattering.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    const StaticArray<math::Vector3, FG_ORACLE_DEPTH> samples{{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};
    FgOraclePhotons photons{};
    REQUIRE(TraceFgOraclePhotons({triangles.GetData(), triangles.GetSize()},
                                 {0, 0, 1},
                                 {0, 0, -1},
                                 {1, 1, 1},
                                 samples,
                                 photons) == true);
    CHECK(photons[0].IsValid == false);
    REQUIRE(photons[1].IsValid == true);
    CHECK(photons[1].IsCaustic == true);
    CHECK(photons[1].Flux.X > 0);
    CHECK(photons[1].Flux.X <= 1);
}
