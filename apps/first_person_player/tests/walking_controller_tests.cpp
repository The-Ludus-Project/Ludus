#include "walking_controller.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using ludus::first_person::StaticColliderInstance;
using ludus::first_person::WalkingController;
using ludus::foundation::float32;
using ludus::foundation::float64;
using ludus::foundation::uint32;
using ludus::foundation::usize;
using ludus::foundation::math::Affine3;
using ludus::foundation::math::Vector3;
using ludus::foundation::math::Vector3d;

namespace
{
constexpr float64 FRAME_SECONDS = 1.0 / 60.0;
constexpr float64 HALF_TICK_SECONDS = 1.0 / 240.0;
constexpr float64 EYE_HEIGHT_METRES = 1.6;
constexpr float64 CONTACT_MARGIN_METRES = 0.01;
constexpr Vector3d SPAWN_EYE_WORLD{0, EYE_HEIGHT_METRES, 0};
constexpr Vector3d WALK_VELOCITY_WORLD{3, 0, 0};
constexpr float64 WALL_DISTANCE_METRES = 2;
constexpr float64 BODY_RADIUS_METRES = 0.24;
constexpr usize WALK_FRAME_COUNT = 120;
constexpr Vector3 FLOOR_POSITIONS_MESH[]{{-10, 0, -10}, {-10, 0, 10}, {10, 0, -10}, {10, 0, 10}};
constexpr Vector3 WALL_POSITIONS_MESH[]{{0, 0, -5}, {0, 0, 5}, {0, 5, -5}, {0, 5, 5}};
constexpr uint32 QUAD_TRIANGLE_INDICES[]{0, 1, 2, 2, 1, 3};

StaticColliderInstance MakeFloor()
{
    return { .PositionsMesh = FLOOR_POSITIONS_MESH, .TriangleIndices = QUAD_TRIANGLE_INDICES, .MeshToWorld = {} };
}

StaticColliderInstance MakeWall()
{
    Affine3 meshToWorld;
    meshToWorld.Translation = {static_cast<float32>(WALL_DISTANCE_METRES), 0, 0};
    return
    {
        .PositionsMesh = WALL_POSITIONS_MESH,
        .TriangleIndices = QUAD_TRIANGLE_INDICES,
        .MeshToWorld = meshToWorld,
    };
}
} // namespace

TEST_CASE("First-person walking stops at an instanced wall and stays supported by the floor", "[first-person]")
{
    const StaticColliderInstance room[]{MakeFloor(), MakeWall()};
    WalkingController walk;
    REQUIRE(walk.Start(room, SPAWN_EYE_WORLD) == WalkingController::StartStatus::Ready);

    Vector3d eyeWorld;
    for (usize frame = 0; frame < WALK_FRAME_COUNT; ++frame)
    {
        eyeWorld = walk.Advance(WALK_VELOCITY_WORLD, FRAME_SECONDS, false);
    }
    const float64 contactX = WALL_DISTANCE_METRES - BODY_RADIUS_METRES;
    CHECK(eyeWorld.X == Catch::Approx(contactX).margin(CONTACT_MARGIN_METRES));
    CHECK(eyeWorld.Y == Catch::Approx(EYE_HEIGHT_METRES).margin(CONTACT_MARGIN_METRES));
    CHECK(eyeWorld.Z == 0);

    const Vector3d awayWorld = walk.Advance(-WALK_VELOCITY_WORLD, FRAME_SECONDS, false);
    CHECK(awayWorld.X < eyeWorld.X);
}

TEST_CASE("First-person walking retains a jump until the next fixed tick", "[first-person]")
{
    const StaticColliderInstance floor[]{MakeFloor()};
    WalkingController walk;
    REQUIRE(walk.Start(floor, SPAWN_EYE_WORLD) == WalkingController::StartStatus::Ready);

    CHECK(walk.Advance({}, HALF_TICK_SECONDS, true) == SPAWN_EYE_WORLD);
    CHECK(walk.Advance({}, HALF_TICK_SECONDS, false).Y > EYE_HEIGHT_METRES);
}

TEST_CASE("Rejected collision and spawn inputs preserve the accepted walk", "[first-person]")
{
    using StartStatus = WalkingController::StartStatus;
    const StaticColliderInstance room[]{MakeFloor(), MakeWall()};
    WalkingController walk;
    REQUIRE(walk.Start(room, SPAWN_EYE_WORLD) == StartStatus::Ready);
    const Vector3d acceptedEyeWorld = walk.Advance(WALK_VELOCITY_WORLD, FRAME_SECONDS, false);
    REQUIRE(acceptedEyeWorld.X > SPAWN_EYE_WORLD.X);

    const StaticColliderInstance missingPositions[]{
        { .PositionsMesh = {nullptr, 1}, .TriangleIndices = QUAD_TRIANGLE_INDICES, .MeshToWorld = {} },
    };
    const Vector3d obstructedEyeWorld{WALL_DISTANCE_METRES, EYE_HEIGHT_METRES, 0};
    CHECK(walk.Start({}, SPAWN_EYE_WORLD) == StartStatus::NoGeometry);
    CHECK(walk.Start({nullptr, 1}, SPAWN_EYE_WORLD) == StartStatus::GeometryUnavailable);
    CHECK(walk.Start(missingPositions, SPAWN_EYE_WORLD) == StartStatus::GeometryUnavailable);
    CHECK(walk.Start(room, obstructedEyeWorld) == StartStatus::SpawnObstructed);
    CHECK(walk.Advance({}, 0, false) == acceptedEyeWorld);
    CHECK(walk.Advance(WALK_VELOCITY_WORLD, FRAME_SECONDS, false).X > acceptedEyeWorld.X);
}
