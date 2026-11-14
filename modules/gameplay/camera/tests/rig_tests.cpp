#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/camera/control.hpp>
#include <ludus/gameplay/camera/evaluation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::gameplay::camera;
namespace math = ludus::foundation::math;
using ludus::foundation::float32;
using ludus::foundation::float64;
using ludus::foundation::uint32;

namespace
{
constexpr uint32 RATES[] = {30, 60, 120, 240};

CameraRigDefinition MakeOrbit()
{
    return
    {
        .Recipe = CameraRecipe::Orbit,
        .Lens = { .Projection = CameraProjection::PerspectiveInfinite, .VerticalFov = 1.0F, .NearPlane = 0.1F },
        .Orbit = { .PivotOffset = {0, 1.5F, 0}, .ArmLength = 4 },
    };
}

bool IsNear(math::Vector3d first, math::Vector3d second, float64 tolerance)
{
    return math::Length(first - second) <= tolerance;
}
} // namespace

TEST_CASE("Look input applies pointer displacement once and integrates stick rate", "[camera][control]")
{
    const CameraLookSettings settings{ .PointerRadiansPerUnit = 0.01F, .StickRadiansPerSecond = 2.0F };
    CameraControlView view;
    // Pointer right/down turns right and looks down, independent of the tick duration.
    REQUIRE(TryAdvanceControlView({},
                                  {
                                      .PointerDelta = {10, 5},
                                  },
                                  settings,
                                  1.0 / 30.0,
                                  view) == CameraStatus::Success);
    CameraControlView fast;
    REQUIRE(TryAdvanceControlView({},
                                  {
                                      .PointerDelta = {10, 5},
                                  },
                                  settings,
                                  1.0 / 240.0,
                                  fast) == CameraStatus::Success);
    REQUIRE(math::Abs(view.Yaw + 0.1F) < 1e-6F);
    REQUIRE(math::Abs(view.Pitch + 0.05F) < 1e-6F);
    REQUIRE(fast.Yaw == view.Yaw);
    // Full stick for a quarter second turns half a radian at either rate.
    CameraControlView slow{};
    CameraControlView quick{};
    for (int i = 0; i < 15; ++i)
    {
        REQUIRE(TryAdvanceControlView(slow, { .StickDeflection = {-1, 1} }, settings, 1.0 / 60.0, slow) ==
                CameraStatus::Success);
    }
    for (int i = 0; i < 60; ++i)
    {
        REQUIRE(TryAdvanceControlView(quick, { .StickDeflection = {-1, 1} }, settings, 1.0 / 240.0, quick) ==
                CameraStatus::Success);
    }
    REQUIRE(math::Abs(slow.Yaw - 0.5F) < 1e-5F);
    REQUIRE(math::Abs(quick.Yaw - slow.Yaw) < 1e-5F);
    REQUIRE(math::Abs(quick.Pitch - slow.Pitch) < 1e-5F);
}

TEST_CASE("Control yaw wraps deterministically and pitch stays bounded", "[camera][control]")
{
    const CameraLookSettings settings{ .PointerRadiansPerUnit = 1.0F };
    CameraControlView view;
    // Half a turn wraps to the negative end of [-pi, pi). (float32 pi lies just above pi.)
    REQUIRE(TryAdvanceControlView({},
                                  {
                                      .PointerDelta = {-math::kPiF, 0},
                                  },
                                  settings,
                                  0,
                                  view) == CameraStatus::Success);
    REQUIRE(view.Yaw < 0);
    REQUIRE(math::Abs(view.Yaw + math::kPiF) < 1e-6F);
    REQUIRE(TryAdvanceControlView({}, { .PointerDelta = {math::kPiF, 0} }, settings, 0, view) == CameraStatus::Success);
    REQUIRE(view.Yaw >= -math::kPiF);
    REQUIRE(view.Yaw < math::kPiF);
    REQUIRE(TryAdvanceControlView({ .Yaw = 3.0F }, { .PointerDelta = {-1, 0} }, settings, 0, view) ==
            CameraStatus::Success);
    REQUIRE(math::Abs(view.Yaw - (4.0F - math::kTauF)) < 1e-6F);
    REQUIRE(TryAdvanceControlView({}, { .PointerDelta = {0, -100} }, settings, 0, view) == CameraStatus::Success);
    REQUIRE(view.Pitch == settings.Pitch.Maximum);
    // Invalid input preserves the accepted view.
    view = { .Yaw = 0.25F, .Pitch = 0.125F };
    REQUIRE(TryAdvanceControlView({}, { .StickDeflection = {2, 0} }, settings, 1, view) == CameraStatus::InvalidInput);
    REQUIRE(TryAdvanceControlView({ .Pitch = 2 }, {}, settings, 1, view) == CameraStatus::InvalidInput);
    REQUIRE(TryAdvanceControlView({}, {}, settings, -1, view) == CameraStatus::InvalidInput);
    REQUIRE(TryAdvanceControlView({}, {}, { .Pitch = { .Minimum = 1, .Maximum = 0 } }, 1, view) ==
            CameraStatus::InvalidInput);
    REQUIRE(view.Yaw == 0.25F);
    REQUIRE(view.Pitch == 0.125F);
}

TEST_CASE("Control orientation and movement heading agree on the ground plane", "[camera][control]")
{
    for (const float32 yaw : {0.0F, 0.7F, -2.5F, 3.0F})
    {
        for (const float32 pitch : {0.0F, 0.6F, -1.5F})
        {
            math::Quaternion orientation;
            math::Vector3 heading;
            REQUIRE(TryComputeControlOrientation({ .Yaw = yaw, .Pitch = pitch }, orientation) == CameraStatus::Success);
            REQUIRE(TryComputeMovementForward({ .Yaw = yaw, .Pitch = pitch }, heading) == CameraStatus::Success);
            const math::Vector3 forward = math::Rotate(orientation, {0, 0, -1});
            REQUIRE(math::Abs(forward.Y - math::Sin(pitch)) < 1e-5F);
            // Near-vertical pitch still yields a unit heading, along the horizontal part of forward.
            REQUIRE(heading.Y == 0);
            REQUIRE(math::Abs(math::Length(heading) - 1.0F) < 1e-6F);
            REQUIRE(math::Abs(heading.X * math::Cos(pitch) - forward.X) < 1e-5F);
            REQUIRE(math::Abs(heading.Z * math::Cos(pitch) - forward.Z) < 1e-5F);
        }
    }
    // Positive yaw turns left (towards -X); positive pitch looks up.
    math::Vector3 heading;
    REQUIRE(TryComputeMovementForward({ .Yaw = math::kHalfPiF }, heading) == CameraStatus::Success);
    REQUIRE(math::Abs(heading.X + 1) < 1e-6F);
}

TEST_CASE("Orbit places the eye behind the pivot along control aim", "[camera][orbit]")
{
    CameraRigDefinition definition = MakeOrbit();
    definition.Orbit.Pitch = { .Minimum = -0.5F, .Maximum = 0.5F };
    const CameraTargetSample target{ .Position = {10, 0, 0}, .BindingId = 4 };
    CameraSample sample;
    CameraTrace trace;
    REQUIRE(TryEvaluateCamera(definition, {}, &target, sample, &trace) == CameraStatus::Success);
    REQUIRE(IsNear(sample.Pose.Position, {10, 1.5, 4}, 1e-6));
    REQUIRE(trace.Pivot == math::Vector3d{10, 1.5, 0});
    REQUIRE(trace.BindingId == 4);
    REQUIRE(TryEvaluateCamera(definition, { .Control = { .Yaw = math::kHalfPiF } }, &target, sample, &trace) ==
            CameraStatus::Success);
    REQUIRE(IsNear(sample.Pose.Position, {14, 1.5, 0}, 1e-6));
    REQUIRE(math::Rotate(sample.Pose.Orientation, {0, 0, -1}).X < -0.999F);
    // The rig clamps authoritative pitch to its own bounds; the pivot stays on the view axis.
    REQUIRE(TryEvaluateCamera(definition, { .Control = { .Pitch = 1.3F } }, &target, sample, &trace) ==
            CameraStatus::Success);
    const math::Vector3 forward = math::Rotate(sample.Pose.Orientation, {0, 0, -1});
    REQUIRE(math::Abs(forward.Y - math::Sin(0.5F)) < 1e-5F);
    const math::Vector3d toPivot = trace.Pivot - sample.Pose.Position;
    REQUIRE(math::Abs(math::Length(toPivot) - 4.0) < 1e-5);
    // Orbit needs a target and valid parameters.
    REQUIRE(TryEvaluateCamera(definition, {}, nullptr, sample) == CameraStatus::MissingTarget);
    definition.Orbit.ArmLength = -1;
    REQUIRE(ValidateCameraRigDefinition(definition) == CameraStatus::InvalidInput);
    REQUIRE(TryEvaluateCamera(definition, {}, &target, sample) == CameraStatus::InvalidInput);
    definition = MakeOrbit();
    definition.Orbit.Pitch.Maximum = 2;
    REQUIRE(ValidateCameraRigDefinition(definition) == CameraStatus::InvalidInput);
    definition = MakeOrbit();
    definition.Damping.PivotSmoothTimeSeconds = -1;
    REQUIRE(ValidateCameraRigDefinition(definition) == CameraStatus::InvalidInput);
}

TEST_CASE("Held-target pivot damping agrees across render rates", "[camera][orbit][damping]")
{
    CameraRigDefinition definition = MakeOrbit();
    definition.Damping.PivotSmoothTimeSeconds = 0.25F;
    const CameraTargetSample start{ .Position = {0, 0, 0}, .BindingId = 1 };
    const CameraTargetSample moved{ .Position = {3, 0, -1}, .BindingId = 1 };
    math::Vector3d reference{};
    for (const uint32 rate : RATES)
    {
        CameraRigState state;
        CameraSample sample;
        CameraTrace trace;
        REQUIRE(TryAdvanceCameraRig(definition, {}, &start, 0, state, sample, &trace) == CameraStatus::Success);
        REQUIRE(trace.IsReprimed);
        const float64 dt = 1.0 / rate;
        for (uint32 frame = 0; frame < rate / 2; ++frame)
        {
            REQUIRE(TryAdvanceCameraRig(definition, {}, &moved, dt, state, sample, &trace) == CameraStatus::Success);
            REQUIRE(trace.IsReprimed == false);
        }
        if (rate == RATES[0])
        {
            reference = state.Pivot;
            // A critical response has travelled most, but not all, of the way after two smooth times.
            const float64 remaining = math::Length(state.Pivot - math::Vector3d{3, 1.5, -1});
            REQUIRE(remaining > 0.1);
            REQUIRE(remaining < 1.0);
        }
        // Exact held-target steps subdivide: every rate reaches the same pivot at half a second.
        REQUIRE(IsNear(state.Pivot, reference, 1e-9));
    }
}

TEST_CASE("Moving-target damping keeps bounded lag at every rate", "[camera][orbit][damping]")
{
    CameraRigDefinition definition = MakeOrbit();
    definition.Damping.PivotSmoothTimeSeconds = 0.2F;
    constexpr float64 SPEED = 5.0;
    math::Vector3d reference{};
    for (const uint32 rate : RATES)
    {
        CameraRigState state;
        CameraSample sample;
        const float64 dt = 1.0 / rate;
        for (uint32 frame = 0; frame <= 3 * rate; ++frame)
        {
            const CameraTargetSample target{ .Position = {SPEED * frame * dt, 0, 0}, .BindingId = 1 };
            REQUIRE(TryAdvanceCameraRig(definition, {}, &target, frame == 0 ? 0 : dt, state, sample) ==
                    CameraStatus::Success);
        }
        // Steady critical-spring lag behind a constant velocity is velocity * smooth time, up to the
        // displacement of one sampling interval.
        const float64 lag = 3.0 * SPEED - state.Pivot.X;
        REQUIRE(math::Abs(lag - SPEED * 0.2) < SPEED * dt);
        if (rate == RATES[0])
        {
            reference = state.Pivot;
        }
        // Per-frame target sampling differs by rate; the error is bounded by the sampling interval.
        REQUIRE(IsNear(state.Pivot, reference, SPEED / RATES[0]));
    }
}

TEST_CASE("Look half-life halves an orientation error at any rate", "[camera][orbit][damping]")
{
    CameraRigDefinition definition = MakeOrbit();
    definition.Damping.LookHalfLifeSeconds = 0.1F;
    const CameraTargetSample target{ .BindingId = 1 };
    for (const uint32 rate : RATES)
    {
        CameraRigState state;
        CameraSample sample;
        REQUIRE(TryAdvanceCameraRig(definition, {}, &target, 0, state, sample) == CameraStatus::Success);
        const float64 dt = 1.0 / rate;
        for (uint32 frame = 0; frame < rate / 10; ++frame)
        {
            REQUIRE(TryAdvanceCameraRig(definition, { .Control = { .Yaw = 1.0F } }, &target, dt, state, sample) ==
                    CameraStatus::Success);
        }
        math::Quaternion half;
        REQUIRE(math::TryFromAxisAngle({0, 1, 0}, 0.5F, half) == math::MathStatus::Success);
        REQUIRE(math::SameRotation(state.Look, half, 1e-3F));
    }
}

TEST_CASE("Rig pause, re-priming and failures", "[camera][orbit][damping]")
{
    CameraRigDefinition definition = MakeOrbit();
    definition.Damping.PivotSmoothTimeSeconds = 0.5F;
    CameraTargetSample target{ .Position = {0, 0, 0}, .BindingId = 1 };
    CameraRigState state;
    CameraSample sample;
    CameraTrace trace;
    REQUIRE(TryAdvanceCameraRig(definition, {}, &target, 0, state, sample, &trace) == CameraStatus::Success);
    target.Position = {10, 0, 0};
    REQUIRE(TryAdvanceCameraRig(definition, {}, &target, 1.0 / 60.0, state, sample, &trace) == CameraStatus::Success);
    const CameraRigState moving = state;
    // A zero delta (pause) advances no history.
    REQUIRE(TryAdvanceCameraRig(definition, {}, &target, 0, state, sample, &trace) == CameraStatus::Success);
    REQUIRE(state.Pivot == moving.Pivot);
    REQUIRE(state.PivotVelocity == moving.PivotVelocity);
    // Failures preserve state, sample and trace.
    const CameraSample accepted = sample;
    REQUIRE(TryAdvanceCameraRig(definition, {}, nullptr, 0.1, state, sample, &trace) == CameraStatus::MissingTarget);
    REQUIRE(TryAdvanceCameraRig(definition, {}, &target, -0.1, state, sample, &trace) == CameraStatus::InvalidInput);
    REQUIRE(
        TryAdvanceCameraRig(definition, {}, &target, std::numeric_limits<float64>::infinity(), state, sample, &trace) ==
        CameraStatus::InvalidInput);
    REQUIRE(state.Pivot == moving.Pivot);
    REQUIRE(sample == accepted);
    REQUIRE(trace.Published == accepted);
    // A teleport revision or a different binding snaps the history instead of sweeping across the world.
    target.DiscontinuityRevision = 1;
    REQUIRE(TryAdvanceCameraRig(definition, {}, &target, 1.0 / 60.0, state, sample, &trace) == CameraStatus::Success);
    REQUIRE(trace.IsReprimed);
    REQUIRE(state.Pivot == math::Vector3d{10, 1.5, 0});
    REQUIRE(state.PivotVelocity == math::Vector3d{});
    target.BindingId = 2;
    target.Position = {-50, 0, 0};
    REQUIRE(TryAdvanceCameraRig(definition, {}, &target, 1.0 / 60.0, state, sample, &trace) == CameraStatus::Success);
    REQUIRE(trace.IsReprimed);
    REQUIRE(state.Pivot == math::Vector3d{-50, 1.5, 0});
}

TEST_CASE("Follow damping uses the same pivot spring and keeps the eye plane", "[camera][damping]")
{
    const CameraRigDefinition definition
    {
        .Recipe = CameraRecipe::OrthographicFollow,
        .Pose = { .Position = {0, 0, 15} },
        .FollowOffset = {2, 0},
        .Damping = { .PivotSmoothTimeSeconds = 0.3F },
    };
    CameraRigState state;
    CameraSample sample;
    CameraTargetSample target{ .BindingId = 3 };
    REQUIRE(TryAdvanceCameraRig(definition, {}, &target, 0, state, sample) == CameraStatus::Success);
    REQUIRE(sample.Pose.Position == math::Vector3d{2, 0, 15});
    target.Position = {10, 4, 99};
    REQUIRE(TryAdvanceCameraRig(definition, {}, &target, 0.1, state, sample) == CameraStatus::Success);
    REQUIRE(sample.Pose.Position.X > 2);
    REQUIRE(sample.Pose.Position.X < 12);
    REQUIRE(sample.Pose.Position.Z == 15);
    // Fixed rigs publish their authored shot and never need a target.
    CameraRigState fixedState;
    REQUIRE(TryAdvanceCameraRig({}, {}, nullptr, 0.1, fixedState, sample) == CameraStatus::Success);
    REQUIRE(fixedState.IsPrimed);
}
