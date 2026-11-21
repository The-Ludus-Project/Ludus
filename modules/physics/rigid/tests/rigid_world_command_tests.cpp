// Command admission, deterministic ordering, per-command results, force lifetime and the preflight contract.

#include "rigid_test_support.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/commands.h>
#include <ludus/physics/rigid/rigid_world.h>
#include <ludus/physics/rigid/status.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::physics::rigid;
using namespace ludus::physics::rigid::test;
using Catch::Approx;
namespace collision = ludus::physics::collision;

namespace
{
constexpr float64 NAN64 = std::numeric_limits<float64>::quiet_NaN();
constexpr float64 BOX_MASS = 6000.0;
} // namespace

TEST_CASE("Command admission rejects invalid commands without admitting them", "[rigid][commands]")
{
    WorldFixture fixture({ .MaxPendingCommands = 2 });
    RigidWorld& world = fixture.World;
    const BodyHandle dynamic = fixture.CreateDynamic(fixture.Box, {0.0, 0.0, 0.0});
    const BodyHandle kinematic = fixture.CreateKinematic({5.0, 0.0, 0.0});
    const ImpulseCommand impulse
    {
        .Key =
        {
            .TargetTick = 1,
        },
        .Body = dynamic,
        .LinearImpulseInFrame = {1.0, 0.0, 0.0},
    };

    ImpulseCommand command = impulse;
    command.Body = {};
    CHECK(world.TryEnqueueImpulse(command) == Status::InvalidHandle);
    command = impulse;
    command.Body = kinematic;
    CHECK(world.TryEnqueueImpulse(command) == Status::InvalidArgument);
    CHECK(world.TryEnqueueForce({ .Key = { .TargetTick = 1 }, .Body = kinematic }) == Status::InvalidArgument);
    CHECK(world.TrySetKinematicTarget({ .Key = { .TargetTick = 1 }, .Body = dynamic }) == Status::InvalidArgument);
    command = impulse;
    command.Frame = static_cast<CommandFrame>(9);
    CHECK(world.TryEnqueueImpulse(command) == Status::InvalidArgument);
    command = impulse;
    command.LinearImpulseInFrame.Y = NAN64;
    CHECK(world.TryEnqueueImpulse(command) == Status::NonFiniteInput);
    command = impulse;
    command.AngularImpulseInFrame.Z = 2.0 * MAX_COMMAND_COMPONENT;
    CHECK(world.TryEnqueueImpulse(command) == Status::OutOfRange);
    command = impulse;
    command.HasApplicationPoint = true;
    command.ApplicationPointInFrame = {0.0, 0.0, 1.0e6};
    CHECK(world.TryEnqueueImpulse(command) == Status::OutOfRange);
    KinematicTargetCommand target{ .Key = { .TargetTick = 1 }, .Body = kinematic };
    target.TargetPose.OrientationWorld = {0.0f, 0.0f, 0.0f, 0.5f};
    CHECK(world.TrySetKinematicTarget(target) == Status::InvalidArgument);
    CHECK(world.GetDiagnostics().PendingCommandCount == 0);

    REQUIRE(world.TryEnqueueImpulse(impulse) == Status::Success);
    CHECK(world.TryEnqueueImpulse(impulse) == Status::DuplicateCommand);
    command = impulse;
    command.Key.Sequence = 1;
    REQUIRE(world.TryEnqueueImpulse(command) == Status::Success);
    command.Key.Sequence = 2;
    CHECK(world.TryEnqueueImpulse(command) == Status::CapacityExceeded);
    CHECK(world.GetDiagnostics().PendingCommandCount == 2);
    CHECK(world.GetDiagnostics().PendingCommandHighWater == 2);

    // Tick 0 runs no command; tick 1 applies both, after which tick 1 is closed.
    fixture.StepTicks(2);
    CHECK(world.GetCommandResults().GetSize() == 2);
    command.Key.TargetTick = 1;
    CHECK(world.TryEnqueueImpulse(command) == Status::PastTick);
    command.Key.TargetTick = world.GetNextTick();
    CHECK(world.TryEnqueueImpulse(command) == Status::Success);
}

TEST_CASE("Commands apply in tick, producer and sequence order with keyed results", "[rigid][commands]")
{
    WorldFixture fixture({ .GravityWorld = {} });
    RigidWorld& world = fixture.World;
    const BodyHandle body = fixture.CreateDynamic(fixture.Box, {0.0, 0.0, 0.0});
    const BodyHandle platform = fixture.CreateKinematic({0.0, 5.0, 0.0});
    // Admitted out of order; results follow the key order, never the admission order.
    REQUIRE(world.TryEnqueueImpulse(
    {
        .Key = { .TargetTick = 0, .ProducerId = 2, .Sequence = 0 },
        .Body = body,
        .LinearImpulseInFrame = {BOX_MASS, 0.0, 0.0},
    }) == Status::Success);
    REQUIRE(world.TrySetKinematicTarget(
    {
        .Key = { .TargetTick = 0, .ProducerId = 3, .Sequence = 0 },
        .Body = platform,
        .TargetPose = { .PositionWorld = {0.0, 6.0, 0.0} },
    }) == Status::Success);
    REQUIRE(world.TryEnqueueImpulse(
    {
        .Key = { .TargetTick = 0, .ProducerId = 1, .Sequence = 7 },
        .Body = body,
        .LinearImpulseInFrame = {0.0, BOX_MASS, 0.0},
    }) == Status::Success);
    REQUIRE(world.TrySetKinematicTarget(
    {
        .Key = { .TargetTick = 0, .ProducerId = 1, .Sequence = 9 },
        .Body = platform,
        .TargetPose = { .PositionWorld = {0.0, 9.0, 0.0} },
    }) == Status::Success);
    REQUIRE(world.TryEnqueueImpulse(
    {
        .Key = { .TargetTick = 1, .ProducerId = 0, .Sequence = 0 },
        .Body = body,
        .LinearImpulseInFrame = {0.0, 0.0, BOX_MASS},
    }) == Status::Success);

    const StepResult result = world.Step();
    REQUIRE(result.Result == Status::Success);
    CHECK(result.Tick == 0);
    CHECK(result.AppliedCommandCount == 3);
    CHECK(result.UnappliedCommandCount == 1);
    const BorrowedView<const CommandResult> results = world.GetCommandResults();
    REQUIRE(results.GetSize() == 4);
    CHECK(results[0].Key == CommandKey{ .TargetTick = 0, .ProducerId = 1, .Sequence = 7 });
    CHECK(results[1].Key == CommandKey{ .TargetTick = 0, .ProducerId = 1, .Sequence = 9 });
    CHECK(results[1].Outcome == CommandOutcome::Superseded);
    CHECK(results[2].Key.ProducerId == 2);
    CHECK(results[2].Outcome == CommandOutcome::Applied);
    CHECK(results[3].Kind == CommandKind::KinematicTarget);
    CHECK(results[3].Outcome == CommandOutcome::Applied);
    // Producer 3's target is the last one applied for the platform.
    CHECK(fixture.GetState(platform).Pose.PositionWorld.Y == Approx(6.0).epsilon(1.0e-12));
    CHECK(GetMaxDifference(fixture.GetState(body).LinearVelocityWorld, {1.0, 1.0, 0.0}) <= 1.0e-12);
    CHECK(world.GetDiagnostics().PendingCommandCount == 1);

    REQUIRE(world.Step().Result == Status::Success);
    CHECK(GetMaxDifference(fixture.GetState(body).LinearVelocityWorld, {1.0, 1.0, 1.0}) <= 1.0e-12);
    CHECK(world.GetCommandResults().GetSize() == 1);
    REQUIRE(world.Step().Result == Status::Success);
    CHECK(world.GetCommandResults().GetSize() == 0);
}

TEST_CASE("Forces persist for exactly one tick", "[rigid][commands][force]")
{
    WorldFixture fixture;
    RigidWorld& world = fixture.World;
    const BodyHandle body = fixture.CreateDynamic(fixture.Box, {0.0, 10.0, 0.0});
    const float64 g = STANDARD_GRAVITY_METERS_PER_SECOND_SQUARED;
    // A force that cancels gravity for tick 0 only.
    REQUIRE(world.TryEnqueueForce(
    {
        .Key =
        {
            .TargetTick = 0,
        },
        .Body = body,
        .ForceInFrame = {0.0, BOX_MASS * g, 0.0},
    }) == Status::Success);
    fixture.StepTicks(1);
    CHECK(math::Abs(fixture.GetState(body).LinearVelocityWorld.Y) <= 1.0e-12);
    CHECK(fixture.GetState(body).Pose.PositionWorld.Y == Approx(10.0).epsilon(1.0e-12));
    fixture.StepTicks(1);
    CHECK(fixture.GetState(body).LinearVelocityWorld.Y == Approx(-g * DEFAULT_TICK_SECONDS).epsilon(1.0e-12));
}

TEST_CASE("Body-frame forces and points convert at the application pose", "[rigid][commands][force]")
{
    WorldFixture fixture({ .GravityWorld = {} });
    RigidWorld& world = fixture.World;
    // Rotated a quarter turn about +Y: body +X is world -Z.
    const BodyHandle body =
        fixture.CreateDynamic(fixture.Sphere, {3.0, 0.0, 0.0}, MakeAxisAngle({0.0, 1.0, 0.0}, 0.5 * math::kPi));
    BodyMassInfo mass;
    REQUIRE(world.TryGetBodyMass(body, mass) == Status::Success);
    const float64 sphereMass = mass.Frame.MassKilograms;
    const float64 moment = mass.Frame.PrincipalMomentsKilogramMetersSquared.X;
    // A body-frame impulse at body point (0, 1, 0) along body +X: world impulse (0, 0, -m) at world (3, 1, 0).
    REQUIRE(world.TryEnqueueImpulse(
    {
        .Key = { .TargetTick = 0 },
        .Body = body,
        .Frame = CommandFrame::Body,
        .LinearImpulseInFrame = {sphereMass, 0.0, 0.0},
        .HasApplicationPoint = true,
        .ApplicationPointInFrame = {0.0, 1.0, 0.0},
    }) == Status::Success);
    fixture.StepTicks(1);
    const BodyState state = fixture.GetState(body);
    CHECK(GetMaxDifference(state.LinearVelocityWorld, {0.0, 0.0, -1.0}) <= 1.0e-6);
    // L = (0, 1, 0) x (0, 0, -m) = (-m, 0, 0).
    CHECK(GetMaxDifference(state.AngularVelocityWorld, {-sphereMass / moment, 0.0, 0.0}) <= 1.0e-6);
}

TEST_CASE("Commands for destroyed bodies report their outcome", "[rigid][commands]")
{
    WorldFixture fixture;
    RigidWorld& world = fixture.World;
    const BodyHandle body = fixture.CreateDynamic(fixture.Sphere, {0.0, 0.0, 0.0});
    REQUIRE(world.TryEnqueueImpulse({ .Key = { .TargetTick = 0 }, .Body = body }) == Status::Success);
    REQUIRE(world.TryDestroyBody(body) == Status::Success);
    // A new body may reuse the slot; its new generation keeps the old command from reaching it.
    const BodyHandle reused = fixture.CreateDynamic(fixture.Sphere, {0.0, 0.0, 0.0});
    REQUIRE(reused.Slot == body.Slot);
    const StepResult result = world.Step();
    REQUIRE(result.Result == Status::Success);
    CHECK(result.AppliedCommandCount == 0);
    CHECK(result.UnappliedCommandCount == 1);
    REQUIRE(world.GetCommandResults().GetSize() == 1);
    CHECK(world.GetCommandResults()[0].Outcome == CommandOutcome::BodyDestroyed);
    CHECK(world.GetCommandResults()[0].Body == body);
}

TEST_CASE("A preflight failure keeps the tick and its commands pending", "[rigid][commands][preflight]")
{
    WorldFixture fixture({ .GravityWorld = {} });
    RigidWorld& world = fixture.World;
    const BodyHandle body = fixture.CreateDynamic(fixture.Box, {0.0, 0.0, 0.0});
    REQUIRE(world.TryEnqueueImpulse(
                { .Key = { .TargetTick = 0 }, .Body = body, .LinearImpulseInFrame = {BOX_MASS, 0.0, 0.0} }) ==
            Status::Success);
    const uint64 epoch = world.GetDiagnostics().Epoch;
    collision::CollisionReadView view;
    REQUIRE(world.TryAcquireCollisionReadView(view) == Status::Success);
    const StepResult blocked = world.Step();
    CHECK(blocked.Result == Status::InvalidPhase);
    CHECK(blocked.Phase == WorldPhase::Idle);
    CHECK(world.GetNextTick() == 0);
    CHECK(world.GetDiagnostics().PendingCommandCount == 1);
    CHECK(world.GetDiagnostics().Epoch == epoch);
    CHECK(fixture.GetState(body).LinearVelocityWorld == Vector3d{});
    // Commands may still be admitted during a read phase.
    CHECK(world.TryEnqueueImpulse({ .Key = { .TargetTick = 0, .Sequence = 1 }, .Body = body }) == Status::Success);
    view.Release();
    const StepResult stepped = world.Step();
    REQUIRE(stepped.Result == Status::Success);
    CHECK(stepped.AppliedCommandCount == 2);
    CHECK(GetMaxDifference(fixture.GetState(body).LinearVelocityWorld, {1.0, 0.0, 0.0}) <= 1.0e-12);
}
