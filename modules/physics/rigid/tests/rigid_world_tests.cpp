// RigidWorld initialization, body creation and destruction, identities, read-view protection, teleports,
// collider attachment, state export and the Faulted phase.

#include "rigid_test_support.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/collision/triangle_mesh.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/rigid_world.h>
#include <ludus/physics/rigid/status.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::physics::rigid;
using namespace ludus::physics::rigid::test;
using Catch::Approx;
using ludus::foundation::Array;
using ludus::foundation::StaticArray;
using ludus::foundation::math::Vector3;
namespace collision = ludus::physics::collision;

namespace
{
constexpr float64 NAN64 = std::numeric_limits<float64>::quiet_NaN();

collision::ShapeHandle AddFloorMesh(collision::GeometryStore& inoutStore)
{
    const StaticArray<Vector3, 4> positions{Vector3{-1.0f, 0.0f, -1.0f},
                                            Vector3{-1.0f, 0.0f, 1.0f},
                                            Vector3{1.0f, 0.0f, 1.0f},
                                            Vector3{1.0f, 0.0f, -1.0f}};
    const StaticArray<uint32, 6> indices{0, 1, 2, 0, 2, 3};
    collision::TriangleMesh mesh;
    REQUIRE(collision::TriangleMesh::TryCook(
                {
                    .PositionsMesh = BorrowedView<const Vector3>(positions.GetData(), 4),
                    .Indices = BorrowedView<const uint32>(indices.GetData(), 6),
                },
                {},
                mesh)
                .Result == collision::Status::Success);
    collision::ShapeHandle shape;
    REQUIRE(inoutStore.TryAddTriangleMesh(ludus::foundation::Move(mesh), shape) == collision::Status::Success);
    return shape;
}

BodyDescription MakeDescription(const BodyColliderDescription& collider)
{
    return { .Colliders = BorrowedView<const BodyColliderDescription>(&collider, 1) };
}
} // namespace

TEST_CASE("Initialization validates settings and leaves the world unchanged on failure", "[rigid][world]")
{
    collision::GeometryStore store;
    RigidWorld world;
    const RigidWorldSettings valid{ .MaxBodies = 4, .MaxColliders = 4, .MaxPendingCommands = 4 };
    CHECK(world.TryInitialize(store, valid) == Status::InvalidPhase);
    REQUIRE(store.TryInitialize({ .MaxShapes = 4 }) == collision::Status::Success);

    RigidWorldSettings settings = valid;
    settings.MaxBodies = 0;
    CHECK(world.TryInitialize(store, settings) == Status::InvalidArgument);
    settings = valid;
    settings.MaxPendingCommands = MAX_PENDING_COMMANDS + 1;
    CHECK(world.TryInitialize(store, settings) == Status::CapacityExceeded);
    settings = valid;
    settings.MaxColliders = collision::MAX_COLLISION_WORLD_COLLIDERS + 1;
    CHECK(world.TryInitialize(store, settings) == Status::CapacityExceeded);
    settings = valid;
    settings.TickSeconds = NAN64;
    CHECK(world.TryInitialize(store, settings) == Status::NonFiniteInput);
    settings = valid;
    settings.TickSeconds = 2.0;
    CHECK(world.TryInitialize(store, settings) == Status::OutOfRange);
    settings = valid;
    settings.SubstepCount = 0;
    CHECK(world.TryInitialize(store, settings) == Status::InvalidArgument);
    settings = valid;
    settings.SubstepCount = MAX_SUBSTEP_COUNT + 1;
    CHECK(world.TryInitialize(store, settings) == Status::OutOfRange);
    settings = valid;
    settings.GravityWorld = {0.0, -2.0e3, 0.0};
    CHECK(world.TryInitialize(store, settings) == Status::OutOfRange);
    CHECK(world.IsInitialized() == false);
    CHECK(world.GetPhase() == WorldPhase::Uninitialized);

    BodyHandle body;
    CHECK(world.TryCreateBody({}, body) == Status::InvalidPhase);
    CHECK(world.Step().Result == Status::InvalidPhase);
    CHECK(world.TryEnqueueImpulse({}) == Status::InvalidPhase);
    BodyExportResult exported;
    CHECK(world.TryExportBodyStates({}, exported) == Status::InvalidPhase);

    REQUIRE(world.TryInitialize(store, valid) == Status::Success);
    CHECK(world.TryInitialize(store, valid) == Status::InvalidPhase);
    CHECK(world.GetPhase() == WorldPhase::Idle);
    CHECK(world.GetNextTick() == 0);
}

TEST_CASE("Body creation validates descriptions and creates nothing on failure", "[rigid][world][body]")
{
    WorldFixture fixture;
    RigidWorld& world = fixture.World;
    const BodyColliderDescription sphere{ .Shape = fixture.Sphere };
    BodyHandle body{ .Owner = 9, .Slot = 9, .Generation = 9 };
    const BodyHandle untouched = body;

    BodyDescription description = MakeDescription(sphere);
    description.Motion = static_cast<MotionType>(7);
    CHECK(world.TryCreateBody(description, body) == Status::InvalidArgument);
    description = MakeDescription(sphere);
    description.Pose.OrientationWorld = {0.0f, 0.0f, 0.0f, 2.0f};
    CHECK(world.TryCreateBody(description, body) == Status::InvalidArgument);
    description = MakeDescription(sphere);
    description.Pose.PositionWorld = {0.0, NAN64, 0.0};
    CHECK(world.TryCreateBody(description, body) == Status::NonFiniteInput);
    description = MakeDescription(sphere);
    description.Pose.PositionWorld = {2.0e5, 0.0, 0.0};
    CHECK(world.TryCreateBody(description, body) == Status::OutOfRange);
    description = MakeDescription(sphere);
    description.LinearVelocityWorld = {0.0, 2.0e5, 0.0};
    CHECK(world.TryCreateBody(description, body) == Status::OutOfRange);
    description = MakeDescription(sphere);
    description.Motion = MotionType::Static;
    description.AngularVelocityWorld = {0.0, 1.0, 0.0};
    CHECK(world.TryCreateBody(description, body) == Status::InvalidArgument);
    description = MakeDescription(sphere);
    description.GravityScale = 2.0 * MAX_GRAVITY_SCALE;
    CHECK(world.TryCreateBody(description, body) == Status::OutOfRange);

    SECTION("collider and mass-source failures")
    {
        BodyColliderDescription collider = sphere;
        collider.Pose.PositionBody = {NAN64, 0.0, 0.0};
        CHECK(world.TryCreateBody(MakeDescription(collider), body) == Status::NonFiniteInput);
        collider = sphere;
        collider.Shape = {};
        CHECK(world.TryCreateBody(MakeDescription(collider), body) == Status::InvalidHandle);
        collider = sphere;
        collider.Participation = { .IsSolid = true, .IsSensor = true };
        CHECK(world.TryCreateBody(MakeDescription(collider), body) == Status::InvalidArgument);
        collider = sphere;
        collider.DensityKilogramsPerCubicMeter = 0.0;
        CHECK(world.TryCreateBody(MakeDescription(collider), body) == Status::InvalidArgument);
        // Triangle meshes are not mass sources.
        collider = sphere;
        collider.Shape = AddFloorMesh(fixture.Store);
        CHECK(world.TryCreateBody(MakeDescription(collider), body) == Status::InvalidMassProperties);
        // ... but they may be static colliders.
        BodyDescription floor = MakeDescription(collider);
        floor.Motion = MotionType::Static;
        BodyHandle floorBody;
        CHECK(world.TryCreateBody(floor, floorBody) == Status::Success);
        // Sensors and massless colliders contribute no mass, so they cannot be a dynamic body's only source.
        collider = sphere;
        collider.Participation = { .IsSolid = false, .IsSensor = true };
        CHECK(world.TryCreateBody(MakeDescription(collider), body) == Status::InvalidMassProperties);
        collider = sphere;
        collider.ShouldContributeMass = false;
        CHECK(world.TryCreateBody(MakeDescription(collider), body) == Status::InvalidMassProperties);
        BodyDescription pointBody{};
        CHECK(world.TryCreateBody(pointBody, body) == Status::InvalidMassProperties);
        pointBody.Mass = MassSource::Explicit;
        pointBody.ExplicitMass =
        {
            .MassKilograms = 1.0,
            .InertiaAboutCenterOfMass =
            {
                .Xx = 1.0,
                .Yy = 1.0,
                .Zz = 10.0,
            },
        };
        CHECK(world.TryCreateBody(pointBody, body) == Status::InvalidMassProperties);
        Array<BodyColliderDescription> many;
        REQUIRE(many.TryResize(MAX_BODY_COLLIDERS + 1, sphere) == true);
        description = {};
        description.Colliders = many.AsView();
        CHECK(world.TryCreateBody(description, body) == Status::CapacityExceeded);
        CHECK(body == untouched);
        CHECK(world.GetBodyCount() == 1);
        CHECK(world.GetCollisionWorld().GetColliderCount() == 1);
    }
    SECTION("collider capacity is checked before any collider is created")
    {
        Array<BodyColliderDescription> colliders;
        REQUIRE(colliders.TryResize(TEST_COLLIDER_CAPACITY / 2 + 1, sphere) == true);
        description = {};
        description.Colliders = colliders.AsView();
        BodyHandle first;
        REQUIRE(world.TryCreateBody(description, first) == Status::Success);
        CHECK(world.TryCreateBody(description, body) == Status::CapacityExceeded);
        CHECK(world.GetCollisionWorld().GetColliderCount() == colliders.GetSize());
        CHECK(body == untouched);
    }
    SECTION("body capacity")
    {
        for (uint32 index = 0; index < TEST_BODY_CAPACITY; ++index)
        {
            BodyHandle created;
            REQUIRE(world.TryCreateBody(MakeDescription(sphere), created) == Status::Success);
        }
        CHECK(world.TryCreateBody(MakeDescription(sphere), body) == Status::CapacityExceeded);
        CHECK(world.GetCollisionWorld().GetColliderCount() == TEST_BODY_CAPACITY);
    }
}

TEST_CASE("Body handles are owner-checked and go stale on destruction", "[rigid][world][body]")
{
    WorldFixture first;
    WorldFixture second;
    const BodyHandle body = first.CreateDynamic(first.Sphere, {0.0, 1.0, 0.0});
    CHECK(first.World.IsAlive(body) == true);
    CHECK(second.World.IsAlive(body) == false);
    BodyState state;
    CHECK(second.World.TryGetBodyState(body, state) == Status::InvalidHandle);
    CHECK(second.World.TryDestroyBody(body) == Status::InvalidHandle);

    REQUIRE(first.World.GetCollisionWorld().GetColliderCount() == 1);
    REQUIRE(first.World.TryDestroyBody(body) == Status::Success);
    CHECK(first.World.GetCollisionWorld().GetColliderCount() == 0);
    CHECK(first.World.IsAlive(body) == false);
    CHECK(first.World.TryDestroyBody(body) == Status::InvalidHandle);
    CHECK(first.World.TryTeleportBody(body, {}) == Status::InvalidHandle);

    // The slot is reused with a new generation.
    const BodyHandle reused = first.CreateDynamic(first.Sphere, {0.0, 1.0, 0.0});
    CHECK(reused.Slot == body.Slot);
    CHECK(reused.Generation == body.Generation + 1);
    CHECK(first.World.IsAlive(body) == false);
}

TEST_CASE("Attached colliders follow their body and keep their local pose", "[rigid][world][collider]")
{
    WorldFixture fixture({ .GravityWorld = {} });
    const StaticArray<BodyColliderDescription, 2> colliders{
        BodyColliderDescription{ .Shape = fixture.Sphere, .Pose = { .PositionBody = {1.0, 0.0, 0.0} }, .Binding = 11 },
        BodyColliderDescription{ .Shape = fixture.Box, .Pose = { .PositionBody = {-1.0, 0.0, 0.0} }, .Binding = 12 },
    };
    BodyHandle body;
    REQUIRE(fixture.World.TryCreateBody(
                {
                    .Pose = { .PositionWorld = {0.0, 2.0, 0.0} },
                    .LinearVelocityWorld = {0.0, 0.0, 6.0},
                    .Colliders = BorrowedView<const BodyColliderDescription>(colliders.GetData(), 2),
                },
                body) == Status::Success);
    StaticArray<collision::ColliderHandle, 2> handles{};
    usize count = 0;
    REQUIRE(fixture.World.TryGetBodyColliders(body,
                                              BorrowedView<collision::ColliderHandle>(handles.GetData(), 2),
                                              count) == Status::Success);
    REQUIRE(count == 2);
    fixture.StepTicks(10);
    const BodyState state = fixture.GetState(body);
    collision::ColliderDescription sphereCollider;
    REQUIRE(fixture.World.GetCollisionWorld().TryGetCollider(handles[0], sphereCollider) == collision::Status::Success);
    CHECK(sphereCollider.Binding == 11);
    CHECK(GetMaxDifference(sphereCollider.Pose.PositionWorld, state.Pose.PositionWorld + Vector3d{1.0, 0.0, 0.0}) <=
          1.0e-12);
    CHECK(state.Pose.PositionWorld.Z == Approx(1.0).epsilon(1.0e-12));

    // A protected read view freezes the collision world, so mutations and Step wait for its release.
    collision::CollisionReadView view;
    REQUIRE(fixture.World.TryAcquireCollisionReadView(view) == Status::Success);
    const StepResult blocked = fixture.World.Step();
    CHECK(blocked.Result == Status::InvalidPhase);
    CHECK(blocked.Phase == WorldPhase::Idle);
    CHECK(fixture.World.GetNextTick() == 10);
    CHECK(fixture.World.TryTeleportBody(body, {}) == Status::InvalidPhase);
    CHECK(fixture.World.TryDestroyBody(body) == Status::InvalidPhase);
    BodyHandle other;
    CHECK(fixture.World.TryCreateBody(MakeDescription(colliders[0]), other) == Status::InvalidPhase);
    // Reads remain available.
    BodyState protectedState;
    CHECK(fixture.World.TryGetBodyState(body, protectedState) == Status::Success);
    view.Release();
    CHECK(fixture.World.Step().Result == Status::Success);
}

TEST_CASE("Teleport is discontinuous and resets interpolation", "[rigid][world][interpolation]")
{
    WorldFixture fixture;
    const BodyHandle body = fixture.CreateDynamic(fixture.Sphere, {0.0, 10.0, 0.0});
    const BodyState created = fixture.GetState(body);
    CHECK(created.Pose.PositionWorld == created.PreviousPose.PositionWorld);

    fixture.StepTicks(1);
    const BodyState moved = fixture.GetState(body);
    CHECK(moved.PreviousPose.PositionWorld == created.Pose.PositionWorld);
    CHECK(moved.Pose.PositionWorld.Y < created.Pose.PositionWorld.Y);
    collision::RigidPose halfway;
    REQUIRE(fixture.World.TryGetInterpolatedPose(body, 0.5, halfway) == Status::Success);
    CHECK(halfway.PositionWorld.Y ==
          Approx(0.5 * (moved.Pose.PositionWorld.Y + moved.PreviousPose.PositionWorld.Y)).epsilon(1.0e-14));
    CHECK(fixture.World.TryGetInterpolatedPose(body, 1.5, halfway) == Status::OutOfRange);
    CHECK(fixture.World.TryGetInterpolatedPose(body, NAN64, halfway) == Status::NonFiniteInput);

    const collision::RigidPose destination
    {
        .PositionWorld = {5.0, 1.0, -3.0},
        .OrientationWorld = MakeAxisAngle({1.0, 0.0, 0.0}, 1.0),
    };
    REQUIRE(fixture.World.TryTeleportBody(body, destination) == Status::Success);
    const BodyState teleported = fixture.GetState(body);
    CHECK(teleported.Pose.PositionWorld == destination.PositionWorld);
    CHECK(teleported.PreviousPose.PositionWorld == destination.PositionWorld);
    CHECK(GetAngleBetween(teleported.PreviousPose.OrientationWorld, destination.OrientationWorld) <= 1.0e-6);
    // Velocity is kept.
    CHECK(teleported.LinearVelocityWorld == moved.LinearVelocityWorld);
    REQUIRE(fixture.World.TryGetInterpolatedPose(body, 0.25, halfway) == Status::Success);
    CHECK(halfway.PositionWorld == destination.PositionWorld);

    collision::RigidPose invalid = destination;
    invalid.PositionWorld.X = 1.0e6;
    CHECK(fixture.World.TryTeleportBody(body, invalid) == Status::OutOfRange);
    CHECK(fixture.GetState(body).Pose.PositionWorld == destination.PositionWorld);
}

TEST_CASE("Body states export as plain values every tick", "[rigid][world][export]")
{
    WorldFixture fixture;
    const BodyHandle first = fixture.CreateDynamic(fixture.Sphere, {0.0, 10.0, 0.0});
    const BodyHandle second = fixture.CreateKinematic({3.0, 0.0, 0.0});
    REQUIRE(fixture.World.TrySetBodyVelocity(second, {1.0, 0.0, 0.0}, {}) == Status::Success);
    StaticArray<BodyState, 4> states{};
    for (uint32 tick = 0; tick < 3; ++tick)
    {
        REQUIRE(fixture.World.Step().Result == Status::Success);
        BodyExportResult result;
        REQUIRE(fixture.World.TryExportBodyStates(BorrowedView<BodyState>(states.GetData(), 4), result) ==
                Status::Success);
        CHECK(result.IsComplete == true);
        CHECK(result.BodyCount == 2);
        CHECK(result.NextTick == tick + 1);
        CHECK(states[0].Body == first);
        CHECK(states[1].Body == second);
        CHECK(states[1].Motion == MotionType::Kinematic);
        CHECK(states[1].Pose.PositionWorld.X == Approx(3.0 + (tick + 1) / 60.0).epsilon(1.0e-12));
    }
    BodyExportResult partial;
    REQUIRE(fixture.World.TryExportBodyStates(BorrowedView<BodyState>(states.GetData(), 1), partial) ==
            Status::Success);
    CHECK(partial.IsComplete == false);
    CHECK(partial.WrittenCount == 1);
    CHECK(partial.BodyCount == 2);
    CHECK(fixture.World.TryExportBodyStates(BorrowedView<BodyState>(nullptr, 1), partial) == Status::InvalidArgument);
    CHECK(fixture.World.TrySetBodyVelocity(first, {NAN64, 0.0, 0.0}, {}) == Status::NonFiniteInput);
}

TEST_CASE("A tick that leaves the coordinate envelope faults the world", "[rigid][world][fault]")
{
    WorldFixture fixture({ .GravityWorld = {} });
    const BodyHandle body = fixture.CreateDynamic(fixture.Sphere, {9.99e4, 0.0, 0.0});
    const BodyHandle bystander = fixture.CreateDynamic(fixture.Sphere, {0.0, 0.0, 0.0});
    REQUIRE(fixture.World.TrySetBodyVelocity(body, {1.0e5, 0.0, 0.0}, {}) == Status::Success);
    REQUIRE(fixture.World.TryEnqueueImpulse({ .Key = { .TargetTick = 0 }, .Body = bystander }) == Status::Success);
    const StepResult result = fixture.World.Step();
    CHECK(result.Result == Status::Faulted);
    CHECK(result.Phase == WorldPhase::Faulted);
    CHECK(result.Fault == FaultReason::LeftCoordinateEnvelope);
    CHECK(result.FaultBody == body);
    CHECK(fixture.World.GetPhase() == WorldPhase::Faulted);
    // No tick is published.
    CHECK(fixture.World.GetNextTick() == 0);
    CHECK(fixture.World.GetCommandResults().GetSize() == 0);

    const StepResult again = fixture.World.Step();
    CHECK(again.Result == Status::Faulted);
    CHECK(again.Fault == FaultReason::LeftCoordinateEnvelope);
    BodyHandle created;
    CHECK(fixture.World.TryCreateBody({}, created) == Status::Faulted);
    CHECK(fixture.World.TryDestroyBody(bystander) == Status::Faulted);
    CHECK(fixture.World.TryEnqueueImpulse({ .Key = { .TargetTick = 0 }, .Body = bystander }) == Status::Faulted);
    // Diagnostics and reads remain available.
    const RigidWorldDiagnostics diagnostics = fixture.World.GetDiagnostics();
    CHECK(diagnostics.Phase == WorldPhase::Faulted);
    CHECK(diagnostics.Fault == FaultReason::LeftCoordinateEnvelope);
    CHECK(diagnostics.FaultBody == body);
    BodyState state;
    CHECK(fixture.World.TryGetBodyState(body, state) == Status::Success);
}
