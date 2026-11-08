#include "internal/world_lighting_coverage.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus;
using namespace ludus::editor;
using foundation::float32;
namespace math = foundation::math;

namespace
{
/// Tolerance of unit directions recovered from Euler degrees.
constexpr float32 DIRECTION_TOLERANCE = 1e-4F;

/// Intel Sponza's bounds, in metres, and its 22 bulbs plus a 1 m2 swatch at radiance one: about 4.2 lumens.
WorldLightingSummary MakeSponza()
{
    WorldLightingSummary world;
    world.HasBounds = true;
    world.BoundsWorld = { .Min = {-17, -1, -9}, .Max = {16, 18, 9} };
    world.DisabledLightCount = 24;
    world.EmissiveObjectCount = 1;
    world.EmittedLumens = 4.2;
    world.HasSunDirection = true;
    world.SunDirectionWorld = {0.5505702F, -0.8281203F, 0.1053066F};
    world.CanRepair = true;
    return world;
}

/// The direction a light's -Z takes after the World transform's rotation (render_scene.cpp: Z, then Y, then X).
math::Vector3 RotateForward(math::Vector3 degrees)
{
    math::Quaternion qx;
    math::Quaternion qy;
    math::Quaternion qz;
    REQUIRE(math::TryFromAxisAngle({1, 0, 0}, math::DegreesToRadians(degrees.X), qx) == math::MathStatus::Success);
    REQUIRE(math::TryFromAxisAngle({0, 1, 0}, math::DegreesToRadians(degrees.Y), qy) == math::MathStatus::Success);
    REQUIRE(math::TryFromAxisAngle({0, 0, 1}, math::DegreesToRadians(degrees.Z), qz) == math::MathStatus::Success);
    return math::Rotate(qz * qy * qx, {0, 0, -1});
}

bool IsNear(math::Vector3 a, math::Vector3 b)
{
    return math::Abs(a.X - b.X) <= DIRECTION_TOLERANCE && math::Abs(a.Y - b.Y) <= DIRECTION_TOLERANCE &&
           math::Abs(a.Z - b.Z) <= DIRECTION_TOLERANCE;
}
} // namespace

TEST_CASE("A World lit only by faint emitters offers a sun aimed like its disabled one",
          "[editor][world][world-lighting]")
{
    const WorldLightingSummary sponza = MakeSponza();
    const WorldLightingCoverage dark = DescribeWorldLighting(sponza);
    INFO(dark.Warning.toStdString());
    CHECK(dark.Warning.contains(QStringLiteral("renders almost black")) == true);
    CHECK(dark.Warning.contains(QStringLiteral("24 imported lights are disabled")) == true);
    CHECK(dark.Warning.contains(QStringLiteral("about 0.0013 lux on average (1 emissive object)")) == true);
    CHECK(dark.Warning.contains(QStringLiteral("Add a sun from Add World Preset")) == true);
    REQUIRE(dark.Repair == WorldLightingRepair::AddSun);
    CHECK(dark.Sun.IlluminanceLux == DEFAULT_WORLD_SUN_ILLUMINANCE_LUX);
    CHECK(IsNear(RotateForward(dark.Sun.RotationDegrees), sponza.SunDirectionWorld) == true);
    CHECK(dark.Sun.PositionWorld.Y == sponza.BoundsWorld.Max.Y);

    // Any presented light, environment or luminaire lights the World.
    WorldLightingSummary lit = sponza;
    lit.LightCount = 1;
    CHECK(DescribeWorldLighting(lit).Warning.isEmpty() == true);
    lit = sponza;
    lit.HasEnvironment = true;
    CHECK(DescribeWorldLighting(lit).Warning.isEmpty() == true);
    lit = sponza;
    lit.LuminaireCount = 1;
    CHECK(DescribeWorldLighting(lit).Warning.isEmpty() == true);
    // So do emitters bright enough for the bounds: a 1 m2 panel of 30 nits in a 2 m room gives about 3.9 lux.
    lit = sponza;
    lit.BoundsWorld = { .Min = {-1, -1, -1}, .Max = {1, 1, 1} };
    lit.EmittedLumens = math::kPi * 30;
    CHECK(DescribeWorldLighting(lit).Warning.isEmpty() == true);
    // A World with nothing to show is not dark.
    lit = sponza;
    lit.HasBounds = false;
    CHECK(DescribeWorldLighting(lit).Warning.isEmpty() == true);
}

TEST_CASE("A dark World's sun falls back to a default angle and needs the default schema",
          "[editor][world][world-lighting]")
{
    WorldLightingSummary world = MakeSponza();
    world.HasSunDirection = false;
    world.DisabledLightCount = 0;
    world.EmissiveObjectCount = 0;
    world.EmittedLumens = 0;
    const WorldLightingCoverage fallback = DescribeWorldLighting(world);
    INFO(fallback.Warning.toStdString());
    CHECK(fallback.Warning.contains(QStringLiteral("disabled")) == false);
    REQUIRE(fallback.Repair == WorldLightingRepair::AddSun);
    CHECK(fallback.Sun.RotationDegrees == DEFAULT_WORLD_SUN_ROTATION_DEGREES);
    CHECK(RotateForward(fallback.Sun.RotationDegrees).Y < 0);

    // Without bounds, the menu's sun hangs two metres above the origin.
    WorldLightingSummary empty;
    const WorldSunPlacement unbounded = FitWorldSun(empty);
    CHECK(unbounded.PositionWorld == math::Vector3{0, 2, 0});
    CHECK(unbounded.RotationDegrees == DEFAULT_WORLD_SUN_ROTATION_DEGREES);
    CHECK(unbounded.IlluminanceLux == DEFAULT_WORLD_SUN_ILLUMINANCE_LUX);

    // A disabled light shining up cannot light the ground, so the sun ignores it.
    world.HasSunDirection = true;
    world.SunDirectionWorld = {0, 1, 0};
    CHECK(DescribeWorldLighting(world).Sun.RotationDegrees == DEFAULT_WORLD_SUN_ROTATION_DEGREES);

    // A project schema gets the instruction without a repair.
    world.CanRepair = false;
    const WorldLightingCoverage manual = DescribeWorldLighting(world);
    CHECK(manual.Repair == WorldLightingRepair::None);
    CHECK(manual.Warning.contains(QStringLiteral("Add a directional light entity")) == true);
}

TEST_CASE("World light rotations aim -Z along a direction", "[editor][world][world-lighting]")
{
    for (const math::Vector3 direction : {math::Vector3{0, -1, 0},
                                          math::Vector3{0, 0, -1},
                                          math::Vector3{1, 0, 0},
                                          math::Vector3{-0.6F, -0.64F, 0.48F},
                                          math::Vector3{0.5505702F, -0.8281203F, 0.1053066F}})
    {
        INFO(direction.X << ", " << direction.Y << ", " << direction.Z);
        CHECK(IsNear(RotateForward(ComputeWorldLightRotationDegrees(direction)), direction) == true);
    }
}
