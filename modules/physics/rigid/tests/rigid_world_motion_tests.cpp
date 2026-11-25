// Integration against analytic answers: free fall, off-center impulse, asymmetric compound COM, rotated inertia,
// a fast asymmetric top with recorded drift, and kinematic targets with shortest-arc rotation.

#include "rigid_test_support.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/commands.h>
#include <ludus/physics/rigid/rigid_world.h>
#include <ludus/physics/rigid/status.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::physics::rigid;
using namespace ludus::physics::rigid::test;
using Catch::Approx;
using ludus::foundation::StaticArray;
namespace collision = ludus::physics::collision;

TEST_CASE("Free fall matches the discrete and continuous analytic answers", "[rigid][motion]")
{
    WorldFixture fixture;
    const BodyHandle body = fixture.CreateDynamic(fixture.Sphere, {0.0, 100.0, 0.0});
    constexpr uint32 TICK_COUNT = 60;
    fixture.StepTicks(TICK_COUNT);
    const BodyState state = fixture.GetState(body);
    const float64 g = STANDARD_GRAVITY_METERS_PER_SECOND_SQUARED;
    const float64 h = DEFAULT_TICK_SECONDS / DEFAULT_SUBSTEP_COUNT;
    const float64 steps = TICK_COUNT * DEFAULT_SUBSTEP_COUNT;
    // Semi-implicit Euler: v_k = -g k h and y_k = y_0 - g h^2 k (k + 1) / 2, exactly.
    CHECK(state.LinearVelocityWorld.Y == Approx(-g * steps * h).epsilon(1.0e-12));
    CHECK(state.Pose.PositionWorld.Y == Approx(100.0 - g * h * h * steps * (steps + 1.0) / 2.0).epsilon(1.0e-12));
    // The continuous answer y_0 - g t^2 / 2 is within the method's first-order error g t h / 2.
    const float64 seconds = TICK_COUNT * DEFAULT_TICK_SECONDS;
    CHECK(math::Abs(state.Pose.PositionWorld.Y - (100.0 - 0.5 * g * seconds * seconds)) <=
          0.5 * g * seconds * h + 1.0e-9);
    CHECK(state.Pose.PositionWorld.X == 0.0);
    CHECK(state.AngularVelocityWorld == Vector3d{});

    // Gravity scale scales the fall; zero holds a dynamic body in place.
    WorldFixture scaled;
    const BodyColliderDescription collider{ .Shape = scaled.Sphere };
    BodyHandle floating;
    REQUIRE(scaled.World.TryCreateBody(
                {
                    .Pose = { .PositionWorld = {0.0, 5.0, 0.0} },
                    .Colliders = BorrowedView<const BodyColliderDescription>(&collider, 1),
                    .GravityScale = 0.0,
                },
                floating) == Status::Success);
    scaled.StepTicks(TICK_COUNT);
    CHECK(scaled.GetState(floating).Pose.PositionWorld.Y == 5.0);
}

TEST_CASE("An off-center impulse sets the analytic linear and angular velocity", "[rigid][motion][impulse]")
{
    WorldFixture fixture({ .GravityWorld = {} });
    // A 1 x 2 x 3 m water box: m = 6000 kg, I = diag(6500, 5000, 2500) kg m^2.
    const BodyHandle body = fixture.CreateDynamic(fixture.Box, {0.0, 0.0, 0.0});
    BodyMassInfo mass;
    REQUIRE(fixture.World.TryGetBodyMass(body, mass) == Status::Success);
    CHECK(mass.Frame.MassKilograms == Approx(6000.0).epsilon(1.0e-12));

    // 600 N s along +Z at the +X face: dv = 0.1 m/s and L = r x J = (0, -300, 0) N m s about the COM.
    REQUIRE(fixture.World.TryEnqueueImpulse(
    {
        .Key = { .TargetTick = 0 },
        .Body = body,
        .Frame = CommandFrame::Body,
        .LinearImpulseInFrame = {0.0, 0.0, 600.0},
        .HasApplicationPoint = true,
        .ApplicationPointInFrame = {0.5, 0.0, 0.0},
    }) == Status::Success);
    fixture.StepTicks(1);
    const BodyState state = fixture.GetState(body);
    CHECK(GetMaxDifference(state.LinearVelocityWorld, {0.0, 0.0, 0.1}) <= 1.0e-12);
    // Rotation about a principal axis has no gyroscopic coupling.
    CHECK(GetMaxDifference(state.AngularVelocityWorld, {0.0, -300.0 / 5000.0, 0.0}) <= 1.0e-12);
    CHECK(GetMaxDifference(state.AngularMomentumWorld, {0.0, -300.0, 0.0}) <= 1.0e-9);
    CHECK(state.KineticEnergyJoules == Approx(0.5 * 6000.0 * 0.01 + 0.5 * 300.0 * 300.0 / 5000.0).epsilon(1.0e-12));
    CHECK(state.CenterOfMassWorld.Z == Approx(0.1 * DEFAULT_TICK_SECONDS).epsilon(1.0e-12));
    CHECK(GetAngleBetween(state.Pose.OrientationWorld, MakeAxisAngle({0.0, 1.0, 0.0}, -0.06 * DEFAULT_TICK_SECONDS)) <=
          1.0e-6);
}

TEST_CASE("An asymmetric compound rotates about its COM, not its body origin", "[rigid][motion][compound]")
{
    WorldFixture fixture({ .GravityWorld = {} });
    const StaticArray<BodyColliderDescription, 2> colliders{
        BodyColliderDescription{ .Shape = fixture.Sphere, .Pose = { .PositionBody = {-1.0, 0.0, 0.0} } },
        BodyColliderDescription
        {
            .Shape = fixture.Sphere,
            .Pose = { .PositionBody = {2.0, 0.0, 0.0} },
            .DensityKilogramsPerCubicMeter = 2.0 * DEFAULT_DENSITY_KILOGRAMS_PER_CUBIC_METER,
        },
    };
    BodyHandle body;
    REQUIRE(fixture.World.TryCreateBody(
                {
                    .Pose = {},
                    .AngularVelocityWorld = {0.0, 1.0, 0.0},
                    .Colliders = BorrowedView<const BodyColliderDescription>(colliders.GetData(), 2),
                },
                body) == Status::Success);
    BodyMassInfo mass;
    REQUIRE(fixture.World.TryGetBodyMass(body, mass) == Status::Success);
    CHECK(GetMaxDifference(mass.Frame.BodyFromCenterOfMass.PositionBody, {1.0, 0.0, 0.0}) <= 1.0e-12);

    constexpr uint32 TICK_COUNT = 30;
    fixture.StepTicks(TICK_COUNT);
    const BodyState state = fixture.GetState(body);
    const float64 angle = TICK_COUNT * DEFAULT_TICK_SECONDS;
    CHECK(GetMaxDifference(state.CenterOfMassWorld, {1.0, 0.0, 0.0}) <= 1.0e-12);
    // origin = COM - R_y(angle) * (1, 0, 0) = (1 - cos(angle), 0, sin(angle)).
    CHECK(GetMaxDifference(state.Pose.PositionWorld, {1.0 - math::Cos(angle), 0.0, math::Sin(angle)}) <= 1.0e-6);
    CHECK(GetAngleBetween(state.Pose.OrientationWorld, MakeAxisAngle({0.0, 1.0, 0.0}, angle)) <= 1.0e-6);
    CHECK(GetMaxDifference(state.AngularVelocityWorld, {0.0, 1.0, 0.0}) <= 1.0e-12);
}

TEST_CASE("Rotated inertia maps angular momentum to the analytic angular velocity", "[rigid][motion][inertia]")
{
    // A tiny tick makes the gyroscopic change during one tick negligible.
    WorldFixture fixture({ .TickSeconds = 1.0e-6, .SubstepCount = 1, .GravityWorld = {} });
    const BodyHandle body =
        fixture.CreateDynamic(fixture.Box, {0.0, 0.0, 0.0}, MakeAxisAngle({0.0, 0.0, 1.0}, 0.25 * math::kPi));
    BodyMassInfo mass;
    REQUIRE(fixture.World.TryGetBodyMass(body, mass) == Status::Success);
    const Vector3d moments = mass.Frame.PrincipalMomentsKilogramMetersSquared;
    CHECK(moments.X == Approx(6500.0).epsilon(1.0e-9));
    CHECK(moments.Y == Approx(5000.0).epsilon(1.0e-9));
    CHECK(moments.Z == Approx(2500.0).epsilon(1.0e-9));

    REQUIRE(fixture.World.TryEnqueueImpulse(
    {
        .Key = { .TargetTick = 0 },
        .Body = body,
        .AngularImpulseInFrame = {1000.0, 0.0, 0.0},
    }) == Status::Success);
    fixture.StepTicks(1);
    const BodyState state = fixture.GetState(body);
    // World tensor [[5750, 750, 0], [750, 5750, 0], [0, 0, 2500]] inverted on (1000, 0, 0).
    const float64 determinant = 5750.0 * 5750.0 - 750.0 * 750.0;
    CHECK(GetMaxDifference(state.AngularVelocityWorld,
                           {5750.0 * 1000.0 / determinant, -750.0 * 1000.0 / determinant, 0.0}) <= 1.0e-7);
    CHECK(GetMaxDifference(state.AngularMomentumWorld, {1000.0, 0.0, 0.0}) <= 1.0e-5);
}

TEST_CASE("A fast asymmetric top tumbles with bounded energy and momentum drift", "[rigid][motion][gyroscopic]")
{
    // Spin about the intermediate axis of a 1 x 2 x 3 m box is unstable: the body flips (the tennis racket
    // theorem), which only happens when the integrator keeps the gyroscopic term.
    WorldFixture fixture({ .GravityWorld = {} });
    BodyColliderDescription collider{ .Shape = fixture.Box };
    BodyHandle body;
    REQUIRE(fixture.World.TryCreateBody(
                {
                    .AngularVelocityWorld = {0.01, 20.0, 0.01},
                    .Colliders = BorrowedView<const BodyColliderDescription>(&collider, 1),
                },
                body) == Status::Success);
    const BodyState initial = fixture.GetState(body);
    const float64 initialEnergy = initial.KineticEnergyJoules;
    const float64 initialMomentum = math::Length(initial.AngularMomentumWorld);
    float64 maximumEnergy = initialEnergy;
    float64 maximumMomentumDrift = 0.0;
    bool hasFlipped = false;
    constexpr uint32 TICK_COUNT = 600;
    for (uint32 tick = 0; tick < TICK_COUNT; ++tick)
    {
        REQUIRE(fixture.World.Step().Result == Status::Success);
        const BodyState state = fixture.GetState(body);
        maximumEnergy = math::Max(maximumEnergy, state.KineticEnergyJoules);
        maximumMomentumDrift =
            math::Max(maximumMomentumDrift,
                      GetMaxDifference(state.AngularMomentumWorld, initial.AngularMomentumWorld) / initialMomentum);
        // The body's Y axis turns away from the momentum direction during a flip.
        const Vector3d bodyY = RotateVector(state.Pose.OrientationWorld, {0.0, 1.0, 0.0});
        hasFlipped = hasFlipped == true || bodyY.Y < 0.0;
    }
    const BodyState finished = fixture.GetState(body);
    const float64 energyDrift = (finished.KineticEnergyJoules - initialEnergy) / initialEnergy;
    const float64 momentumDrift = (math::Length(finished.AngularMomentumWorld) - initialMomentum) / initialMomentum;
    CHECK(hasFlipped == true);
    // The implicit midpoint gyroscopic step preserves kinetic energy and |L| up to its Newton tolerance; measured
    // 1.2e-12 and 5.8e-13 on macOS arm64. The direction of L drifts through the splitting of velocity and
    // orientation updates; measured 2.0% of |L|.
    CHECK(maximumEnergy <= initialEnergy * (1.0 + 1.0e-9));
    CHECK(math::Abs(energyDrift) <= 1.0e-9);
    CHECK(math::Abs(momentumDrift) <= 1.0e-9);
    CHECK(maximumMomentumDrift <= 0.05);
    // Record the drift of the 10 s scene at 60 Hz with 4 substeps in the test log.
    WARN("asymmetric top drift: energy " << energyDrift << ", |L| " << momentumDrift << ", L components "
                                         << maximumMomentumDrift);
}

TEST_CASE("Torque-free spin about a principal axis is steady", "[rigid][motion][gyroscopic]")
{
    WorldFixture fixture({ .GravityWorld = {} });
    BodyColliderDescription collider{ .Shape = fixture.Box };
    BodyHandle body;
    REQUIRE(fixture.World.TryCreateBody(
                {
                    .AngularVelocityWorld = {30.0, 0.0, 0.0},
                    .Colliders = BorrowedView<const BodyColliderDescription>(&collider, 1),
                },
                body) == Status::Success);
    fixture.StepTicks(120);
    const BodyState state = fixture.GetState(body);
    CHECK(GetMaxDifference(state.AngularVelocityWorld, {30.0, 0.0, 0.0}) <= 1.0e-9);
    CHECK(GetAngleBetween(state.Pose.OrientationWorld, MakeAxisAngle({1.0, 0.0, 0.0}, 60.0)) <= 1.0e-5);
}

TEST_CASE("Kinematic targets move along the shortest arc and land exactly", "[rigid][motion][kinematic]")
{
    WorldFixture fixture;
    const BodyHandle body = fixture.CreateKinematic({0.0, 1.0, 0.0});
    // Three quarters of a turn about +Y is reached by a quarter turn about -Y.
    const collision::RigidPose target
    {
        .PositionWorld = {1.2, 1.0, -0.6},
        .OrientationWorld = MakeAxisAngle({0.0, 1.0, 0.0}, 1.5 * math::kPi),
    };
    REQUIRE(fixture.World.TrySetKinematicTarget({ .Key = { .TargetTick = 0 }, .Body = body, .TargetPose = target }) ==
            Status::Success);
    fixture.StepTicks(1);
    const BodyState reached = fixture.GetState(body);
    CHECK(GetMaxDifference(reached.Pose.PositionWorld, target.PositionWorld) <= 1.0e-12);
    CHECK(GetAngleBetween(reached.Pose.OrientationWorld, target.OrientationWorld) <= 1.0e-6);
    CHECK(GetMaxDifference(reached.LinearVelocityWorld, Vector3d{1.2, 0.0, -0.6} * (1.0 / DEFAULT_TICK_SECONDS)) <=
          1.0e-9);
    CHECK(GetMaxDifference(reached.AngularVelocityWorld, {0.0, -0.5 * math::kPi / DEFAULT_TICK_SECONDS, 0.0}) <=
          1.0e-4);
    // Midway through the tick the body has turned a quarter of the way along the shorter arc.
    collision::RigidPose midway;
    REQUIRE(fixture.World.TryGetInterpolatedPose(body, 0.5, midway) == Status::Success);
    CHECK(GetAngleBetween(midway.OrientationWorld, MakeAxisAngle({0.0, 1.0, 0.0}, -0.25 * math::kPi)) <= 1.0e-6);

    // Without a new target the body rests at the target.
    fixture.StepTicks(1);
    const BodyState resting = fixture.GetState(body);
    CHECK(resting.LinearVelocityWorld == Vector3d{});
    CHECK(resting.AngularVelocityWorld == Vector3d{});
    CHECK(GetMaxDifference(resting.Pose.PositionWorld, target.PositionWorld) <= 1.0e-12);
    CHECK(resting.Pose.PositionWorld == resting.PreviousPose.PositionWorld);

    // Kinematic bodies ignore gravity and hold an explicit velocity.
    REQUIRE(fixture.World.TrySetBodyVelocity(body, {0.0, 0.0, 6.0}, {}) == Status::Success);
    fixture.StepTicks(10);
    CHECK(fixture.GetState(body).Pose.PositionWorld.Z == Approx(-0.6 + 1.0).epsilon(1.0e-12));
    CHECK(fixture.GetState(body).Pose.PositionWorld.Y == 1.0);
}
