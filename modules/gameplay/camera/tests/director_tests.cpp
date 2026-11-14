#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/camera/director.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::gameplay::camera;
namespace math = ludus::foundation::math;
using ludus::foundation::float32;
using ludus::foundation::float64;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;

namespace
{
constexpr uint32 RATES[] = {30, 60, 120, 240};
constexpr uint64 PLAYER = 7;

CameraRigRequest MakeOrbitRequest(float32 arm, float32 fov, float32 shoulder, float32 duration)
{
    return
    {
        .Definition =
        {
            .Recipe = CameraRecipe::Orbit,
            .Lens = { .Projection = CameraProjection::PerspectiveInfinite, .VerticalFov = fov, .NearPlane = 0.1F },
            .Orbit = { .PivotOffset = {0, 1.5F, 0}, .ShoulderOffset = {shoulder, 0, 0}, .ArmLength = arm },
        },
        .BindingId = PLAYER,
        .Transition = { .DurationSeconds = duration, .Path = CameraBlendPath::AroundPivot },
    };
}

CameraRigRequest MakeExplore()
{
    return MakeOrbitRequest(6, 1.0F, 0, 1);
}

CameraRigRequest MakeAim()
{
    return MakeOrbitRequest(2.5F, 0.7F, 0.6F, 1);
}

CameraRigRequest MakeOverview()
{
    return
    {
        .Definition =
        {
            .Pose = { .Position = {0, 20, 0} },
            .Lens = { .VerticalSpan = 30, .NearPlane = 0, .FarPlane = 100 },
        },
        .Transition = { .DurationSeconds = 1 },
    };
}

struct Frame final
{
    CameraDirector& Director;
    CameraTargetSample Target{ .Position = {4, 0, -3}, .BindingId = PLAYER };
    uint64 Sequence = 0;
    uint64 World = 0;
    bool HasTarget = true;
    CameraControlView Control{ .Yaw = 0.4F, .Pitch = -0.2F };
    CameraSample Sample{};
    CameraDirectorTrace Trace{};

    CameraStatus Step(float64 dt)
    {
        ++Sequence;
        const CameraFrameInput input
        {
            .PreviousTick = Sequence,
            .CurrentTick = Sequence + 1,
            .RenderAlpha = 0.5F,
            .PresentationSequence = Sequence,
            .WorldRevision = World,
            .DeltaSeconds = dt,
            .Control = Control,
            .Targets = HasTarget == true ? ludus::foundation::BorrowedView<const CameraTargetSample>{&Target, 1}
                                         : ludus::foundation::BorrowedView<const CameraTargetSample>{},
        };
        return Director.TryUpdate(input, Sample, &Trace);
    }
};

/// Normalized device coordinates of a world point for a perspective sample (+Y up).
math::Vector2 Project(const CameraSample& sample, math::Vector3d point)
{
    const math::Vector3d relative = point - sample.Pose.Position;
    const math::Vector3 local = math::Rotate(
        math::Conjugate(sample.Pose.Orientation),
        {static_cast<float32>(relative.X), static_cast<float32>(relative.Y), static_cast<float32>(relative.Z)});
    const float32 scale = math::Cos(0.5F * sample.Lens.VerticalFov) / math::Sin(0.5F * sample.Lens.VerticalFov);
    return {local.X / -local.Z * scale, local.Y / -local.Z * scale};
}
} // namespace

TEST_CASE("Director publishes a first cut, then continuous samples", "[camera][director]")
{
    CameraDirector director;
    Frame frame{ .Director = director };
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::NoSelection);
    REQUIRE(director.HasPublished() == false);
    REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::FirstPublication);
    REQUIRE(frame.Sample.DiscontinuityRevision == 1);
    REQUIRE(frame.Sample.PresentationSequence == frame.Sequence);
    REQUIRE(frame.Sample.RenderAlpha == 0.5F);
    REQUIRE(director.GetPublished() == frame.Sample);
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
        REQUIRE(frame.Trace.CutReason == CameraCutReason::None);
        REQUIRE(frame.Sample.DiscontinuityRevision == 1);
    }
    // The pivot is on the view axis: the target projects to the image center.
    REQUIRE(math::Distance(Project(frame.Sample, {4, 1.5, -3}), {}) < 1e-5F);
}

TEST_CASE("Blends share a pivot and complete at the same time at every rate", "[camera][director][transition]")
{
    math::Vector2 halfway{};
    CameraSample finished{};
    for (const uint32 rate : RATES)
    {
        CameraDirector director;
        Frame frame{ .Director = director };
        REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
        REQUIRE(frame.Step(0) == CameraStatus::Success);
        REQUIRE(director.SetBaseRig(MakeAim()) == CameraStatus::Success);
        const float64 dt = 1.0 / rate;
        math::Vector2 previous = Project(frame.Sample, {4, 1.5, -3});
        for (uint32 i = 0; i < rate / 2; ++i)
        {
            REQUIRE(frame.Step(dt) == CameraStatus::Success);
            REQUIRE(frame.Trace.IsBlending);
            REQUIRE(frame.Trace.IsOutgoingFrozen == false);
            REQUIRE(frame.Trace.CutReason == CameraCutReason::None);
            // The shared pivot drifts smoothly toward the aim composition; no single-frame jump.
            const math::Vector2 current = Project(frame.Sample, {4, 1.5, -3});
            REQUIRE(math::Distance(current, previous) < 2.0F / static_cast<float32>(rate));
            previous = current;
        }
        REQUIRE(math::Abs(frame.Trace.BlendWeight - 0.5F) < 1e-5F);
        if (rate == RATES[0])
        {
            halfway = previous;
        }
        REQUIRE(math::Distance(previous, halfway) < 1e-4F);
        for (uint32 i = 0; i < rate / 2; ++i)
        {
            REQUIRE(frame.Step(dt) == CameraStatus::Success);
        }
        if (frame.Trace.IsBlending == true)
        {
            // Accumulated 1/rate steps may fall one rounding short of the duration.
            REQUIRE(frame.Step(dt) == CameraStatus::Success);
        }
        REQUIRE(frame.Trace.IsBlending == false);
        REQUIRE(frame.Sample.DiscontinuityRevision == 1);
        REQUIRE(frame.Sample.Lens.VerticalFov == 0.7F);
        if (rate == RATES[0])
        {
            finished = frame.Sample;
        }
        REQUIRE(math::Length(frame.Sample.Pose.Position - finished.Pose.Position) < 1e-9);
    }
}

TEST_CASE("Reversing mid-blend starts from the last published pose and lens", "[camera][director][transition]")
{
    for (const uint32 rate : RATES)
    {
        CameraDirector director;
        Frame frame{ .Director = director };
        REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
        REQUIRE(frame.Step(0) == CameraStatus::Success);
        REQUIRE(director.SetBaseRig(MakeAim()) == CameraStatus::Success);
        const float64 dt = 1.0 / rate;
        for (uint32 i = 0; i < rate / 2; ++i)
        {
            REQUIRE(frame.Step(dt) == CameraStatus::Success);
        }
        const CameraSample shown = frame.Sample;
        const math::Vector2 shownTarget = Project(shown, {4, 1.5, -3});
        // Reverse: the explore rig is requested again while the blend is half done.
        REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
        REQUIRE(frame.Step(dt) == CameraStatus::Success);
        REQUIRE(frame.Trace.CutReason == CameraCutReason::None);
        REQUIRE(frame.Trace.IsBlending);
        REQUIRE(frame.Trace.IsOutgoingFrozen);
        REQUIRE(frame.Trace.Outgoing.Pose == shown.Pose);
        REQUIRE(frame.Trace.Outgoing.Lens == shown.Lens);
        // One frame of a one-second smoothstep moves the pose and lens by at most a few percent.
        const float64 tolerance = 0.05 * 3.5;
        REQUIRE(math::Length(frame.Sample.Pose.Position - shown.Pose.Position) < tolerance);
        REQUIRE(math::Abs(frame.Sample.Lens.VerticalFov - shown.Lens.VerticalFov) < 0.01F);
        REQUIRE(math::SameRotation(frame.Sample.Pose.Orientation, shown.Pose.Orientation, 0.02F));
        REQUIRE(math::Distance(Project(frame.Sample, {4, 1.5, -3}), shownTarget) < 0.01F);
        REQUIRE(frame.Sample.DiscontinuityRevision == 1);
        // The reversed blend ends exactly on the explore rig.
        for (uint32 i = 0; i <= rate; ++i)
        {
            REQUIRE(frame.Step(dt) == CameraStatus::Success);
        }
        REQUIRE(frame.Trace.IsBlending == false);
        REQUIRE(frame.Sample.Lens.VerticalFov == 1.0F);
        REQUIRE(math::Distance(Project(frame.Sample, {4, 1.5, -3}), {}) < 1e-5F);
    }
}

TEST_CASE("Projection changes cut and reset view history", "[camera][director][transition]")
{
    CameraDirector director;
    Frame frame{ .Director = director };
    REQUIRE(director.SetBaseRig(MakeOverview()) == CameraStatus::Success);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Sample.Lens.Projection == CameraProjection::OrthographicFinite);
    const uint64 before = frame.Sample.DiscontinuityRevision;
    REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::IncompatibleLens);
    REQUIRE(frame.Trace.IsBlending == false);
    REQUIRE(frame.Sample.Lens.Projection == CameraProjection::PerspectiveInfinite);
    REQUIRE(frame.Sample.DiscontinuityRevision == before + 1);
    // Same projection with different clip planes cannot blend either.
    CameraRigRequest farther = MakeAim();
    farther.Definition.Lens.NearPlane = 0.5F;
    REQUIRE(director.SetBaseRig(farther) == CameraStatus::Success);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::IncompatibleLens);
    // A zero-duration transition is an explicit cut.
    CameraRigRequest instant = MakeAim();
    instant.Transition.DurationSeconds = 0;
    REQUIRE(director.SetBaseRig(instant) == CameraStatus::Success);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::ZeroDuration);
    REQUIRE(frame.Sample.DiscontinuityRevision == before + 3);
}

TEST_CASE("Orthographic magnification blends geometrically", "[camera][director][transition]")
{
    CameraDirector director;
    Frame frame{ .Director = director };
    REQUIRE(director.SetBaseRig(MakeOverview()) == CameraStatus::Success);
    REQUIRE(frame.Step(0) == CameraStatus::Success);
    CameraRigRequest closer = MakeOverview();
    closer.Definition.Lens.VerticalSpan = 7.5F;
    closer.Transition.Easing = CameraEasing::Linear;
    REQUIRE(director.SetBaseRig(closer) == CameraStatus::Success);
    for (int i = 0; i < 5; ++i)
    {
        REQUIRE(frame.Step(0.1) == CameraStatus::Success);
    }
    REQUIRE(frame.Trace.IsBlending);
    REQUIRE(math::Abs(frame.Sample.Lens.VerticalSpan - 15.0F) < 1e-4F);
}

TEST_CASE("Hitches clamp consumed time and pauses freeze blends", "[camera][director][timing]")
{
    CameraDirector director;
    REQUIRE(director.TryConfigure({ .OutputId = 3, .MaximumDeltaSeconds = 0.1F }) == CameraStatus::Success);
    Frame frame{ .Director = director };
    REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
    REQUIRE(frame.Step(0) == CameraStatus::Success);
    REQUIRE(frame.Sample.OutputId == 3);
    CameraRigRequest aim = MakeAim();
    aim.Transition.Easing = CameraEasing::Linear;
    REQUIRE(director.SetBaseRig(aim) == CameraStatus::Success);
    REQUIRE(frame.Step(2.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.AppliedDeltaSeconds == static_cast<float64>(0.1F));
    REQUIRE(math::Abs(frame.Trace.DiscardedDeltaSeconds - (2.0 - static_cast<float64>(0.1F))) < 1e-12);
    REQUIRE(math::Abs(frame.Trace.BlendWeight - 0.1F) < 1e-6F);
    const CameraSample beforePause = frame.Sample;
    for (int i = 0; i < 5; ++i)
    {
        REQUIRE(frame.Step(0) == CameraStatus::Success);
        REQUIRE(math::Abs(frame.Trace.BlendWeight - 0.1F) < 1e-6F);
        REQUIRE(frame.Sample.Pose == beforePause.Pose);
        REQUIRE(frame.Sample.Lens == beforePause.Lens);
    }
    REQUIRE(director.TryConfigure({ .OutputId = 0 }) == CameraStatus::InvalidInput);
    REQUIRE(director.TryConfigure({ .MaximumDeltaSeconds = 0 }) == CameraStatus::InvalidInput);
    REQUIRE(director.GetPublished().OutputId == 3);
}

TEST_CASE("Target loss fails the selected rig and recovers with a cut", "[camera][director][target]")
{
    CameraDirector director;
    Frame frame{ .Director = director };
    REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    const CameraSample accepted = frame.Sample;
    const CameraDirectorTrace acceptedTrace = frame.Trace;
    frame.HasTarget = false;
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::MissingTarget);
    REQUIRE(frame.Sample == accepted);
    REQUIRE(frame.Trace.Published == acceptedTrace.Published);
    REQUIRE(director.GetPublished() == accepted);
    // The game selects a validated fallback; the gap is re-primed as a cut rather than blended over.
    REQUIRE(director.SetBaseRig(MakeOverview()) == CameraStatus::Success);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::Recovery);
    REQUIRE(frame.Sample.DiscontinuityRevision == accepted.DiscontinuityRevision + 1);
}

TEST_CASE("A lost outgoing target freezes the outgoing endpoint", "[camera][director][target]")
{
    CameraDirector director;
    Frame frame{ .Director = director };
    CameraRigRequest overview = MakeOverview();
    overview.Definition.Lens = MakeExplore().Definition.Lens;
    overview.Definition.Pose.Position = {0, 10, 10};
    REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
    REQUIRE(frame.Step(0) == CameraStatus::Success);
    REQUIRE(director.SetBaseRig(overview) == CameraStatus::Success);
    REQUIRE(frame.Step(0.1) == CameraStatus::Success);
    REQUIRE(frame.Trace.IsBlending);
    REQUIRE(frame.Trace.IsOutgoingFrozen == false);
    frame.HasTarget = false;
    REQUIRE(frame.Step(0.1) == CameraStatus::Success);
    REQUIRE(frame.Trace.IsBlending);
    REQUIRE(frame.Trace.IsOutgoingFrozen);
}

TEST_CASE("Target teleports and world replacement cut", "[camera][director][target]")
{
    CameraDirector director;
    Frame frame{ .Director = director };
    CameraRigRequest lagging = MakeExplore();
    lagging.Definition.Damping.PivotSmoothTimeSeconds = 0.5F;
    REQUIRE(director.SetBaseRig(lagging) == CameraStatus::Success);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    frame.Target.Position = {500, 0, 0};
    frame.Target.DiscontinuityRevision = 1;
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::TargetDiscontinuity);
    REQUIRE(frame.Sample.DiscontinuityRevision == 2);
    REQUIRE(math::Distance(Project(frame.Sample, {500, 1.5, 0}), {}) < 1e-5F);
    frame.World = 9;
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::WorldDiscontinuity);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::None);
    // A requested cut survives a failed update and is consumed by the next accepted one.
    director.RequestCut();
    REQUIRE(frame.Step(-1) == CameraStatus::InvalidInput);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::Recovery);
    director.RequestCut();
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::Requested);
    REQUIRE(frame.Step(1.0 / 60.0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::None);
}

TEST_CASE("Overrides select by priority and reveal the base with a transition", "[camera][director][selection]")
{
    CameraDirector director;
    Frame frame{ .Director = director };
    REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
    REQUIRE(frame.Step(0) == CameraStatus::Success);
    const uint64 base = frame.Trace.SelectedRequest;
    CameraOverrideToken aim;
    CameraOverrideToken cinematic;
    REQUIRE(director.AcquireOverride(MakeAim(), 1, 100, aim) == CameraStatus::Success);
    REQUIRE(frame.Step(0.1) == CameraStatus::Success);
    REQUIRE(frame.Trace.IsOverrideSelected);
    REQUIRE(frame.Trace.OverrideSlot == aim.Slot);
    const uint64 aimRequest = frame.Trace.SelectedRequest;
    // An equal-priority newcomer does not displace the incumbent; a higher priority does.
    CameraOverrideToken peer;
    REQUIRE(director.AcquireOverride(MakeOrbitRequest(3, 0.8F, -0.6F, 1), 1, 101, peer) == CameraStatus::Success);
    REQUIRE(frame.Step(0.1) == CameraStatus::Success);
    REQUIRE(frame.Trace.SelectedRequest == aimRequest);
    REQUIRE(director.AcquireOverride(MakeOrbitRequest(8, 1.0F, 0, 0.5F), 5, 200, cinematic) == CameraStatus::Success);
    REQUIRE(frame.Step(0.1) == CameraStatus::Success);
    REQUIRE(frame.Trace.OverrideSlot == cinematic.Slot);
    // A destroyed cinematic owner revokes its override. Neither remaining equal-priority override is the
    // incumbent, so the most recent acquisition wins, entered with the revocation's transition.
    REQUIRE(director.RevokeOverridesOwnedBy(200, { .DurationSeconds = 0.25F }) == 1);
    REQUIRE(frame.Step(0.1) == CameraStatus::Success);
    REQUIRE(frame.Trace.SelectedRequest == aimRequest + 1);
    REQUIRE(frame.Trace.OverrideSlot == peer.Slot);
    REQUIRE(frame.Trace.IsBlending);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::None);
    REQUIRE(director.ReleaseOverride(cinematic, {}) == CameraStatus::StaleHandle);
    REQUIRE(director.ReleaseOverride(peer, {}) == CameraStatus::Success);
    REQUIRE(frame.Step(0.1) == CameraStatus::Success);
    REQUIRE(frame.Trace.SelectedRequest == aimRequest);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::ZeroDuration);
    // Cancelling the aim reveals the still-current base with the release's blend.
    REQUIRE(director.ReleaseOverride(aim, { .DurationSeconds = 0.5F }) == CameraStatus::Success);
    REQUIRE(frame.Step(0.1) == CameraStatus::Success);
    REQUIRE(frame.Trace.SelectedRequest == base);
    REQUIRE(frame.Trace.IsBlending);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::None);
    REQUIRE(director.ReleaseOverride(aim, {}) == CameraStatus::StaleHandle);
    CameraOverrideToken foreign = aim;
    foreign.OutputId = 99;
    REQUIRE(director.ReleaseOverride(foreign, {}) == CameraStatus::StaleHandle);
}

TEST_CASE("Repeated requests keep their blend and invalid requests change nothing", "[camera][director][selection]")
{
    CameraDirector director;
    Frame frame{ .Director = director };
    REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
    REQUIRE(frame.Step(0) == CameraStatus::Success);
    REQUIRE(director.SetBaseRig(MakeAim()) == CameraStatus::Success);
    REQUIRE(frame.Step(0.25) == CameraStatus::Success);
    const float32 weight = frame.Trace.BlendWeight;
    REQUIRE(director.SetBaseRig(MakeAim()) == CameraStatus::Success);
    REQUIRE(frame.Step(0) == CameraStatus::Success);
    REQUIRE(frame.Trace.BlendWeight == weight);
    REQUIRE(frame.Trace.IsOutgoingFrozen == false);
    CameraRigRequest invalid = MakeAim();
    invalid.BindingId = 0;
    REQUIRE(director.SetBaseRig(invalid) == CameraStatus::InvalidInput);
    invalid = MakeAim();
    invalid.Transition.DurationSeconds = -1;
    REQUIRE(director.SetBaseRig(invalid) == CameraStatus::InvalidInput);
    CameraOverrideToken token{ .OutputId = 5 };
    REQUIRE(director.AcquireOverride(MakeAim(), 0, 0, token) == CameraStatus::InvalidInput);
    REQUIRE(token.OutputId == 5);
    REQUIRE(frame.Step(0) == CameraStatus::Success);
    REQUIRE(frame.Trace.BlendWeight == weight);
}

TEST_CASE("Override capacity and generations are bounded", "[camera][director][selection]")
{
    CameraDirector director;
    CameraOverrideToken tokens[MAX_CAMERA_OVERRIDES];
    for (CameraOverrideToken& token : tokens)
    {
        REQUIRE(director.AcquireOverride(MakeAim(), 0, 1, token) == CameraStatus::Success);
    }
    CameraOverrideToken extra{ .Slot = 77 };
    REQUIRE(director.AcquireOverride(MakeAim(), 0, 1, extra) == CameraStatus::CapacityExceeded);
    REQUIRE(extra.Slot == 77);
    REQUIRE(director.RevokeOverridesOwnedBy(1, {}) == MAX_CAMERA_OVERRIDES);
    // Cycle every slot to its last generation; slots then retire instead of wrapping into old tokens.
    CameraOverrideToken token;
    usize acquisitions = 0;
    while (director.AcquireOverride(MakeAim(), 0, 1, token) == CameraStatus::Success)
    {
        REQUIRE(director.ReleaseOverride(token, {}) == CameraStatus::Success);
        ++acquisitions;
    }
    REQUIRE(acquisitions == MAX_CAMERA_OVERRIDES * (MAX_CAMERA_OVERRIDE_GENERATION - 1));
    REQUIRE(director.ReleaseOverride(tokens[0], {}) == CameraStatus::StaleHandle);
    // Reset (world replacement) revokes overrides and clears the base; publication restarts with a cut.
    CameraDirector replaced;
    Frame frame{ .Director = replaced };
    REQUIRE(replaced.SetBaseRig(MakeExplore()) == CameraStatus::Success);
    REQUIRE(replaced.AcquireOverride(MakeAim(), 0, 1, token) == CameraStatus::Success);
    REQUIRE(frame.Step(0) == CameraStatus::Success);
    replaced.Reset();
    REQUIRE(replaced.HasPublished() == false);
    REQUIRE(replaced.ReleaseOverride(token, {}) == CameraStatus::StaleHandle);
    REQUIRE(frame.Step(0) == CameraStatus::NoSelection);
    REQUIRE(replaced.SetBaseRig(MakeExplore()) == CameraStatus::Success);
    REQUIRE(frame.Step(0) == CameraStatus::Success);
    REQUIRE(frame.Trace.CutReason == CameraCutReason::FirstPublication);
    REQUIRE(frame.Sample.DiscontinuityRevision == 2);
}

TEST_CASE("Invalid director input preserves the publication", "[camera][director]")
{
    CameraDirector director;
    Frame frame{ .Director = director };
    REQUIRE(director.SetBaseRig(MakeExplore()) == CameraStatus::Success);
    REQUIRE(frame.Step(0) == CameraStatus::Success);
    const CameraSample accepted = frame.Sample;
    frame.Control.Pitch = 2;
    REQUIRE(frame.Step(0.1) == CameraStatus::InvalidInput);
    frame.Control.Pitch = 0;
    frame.Target.BindingId = 0;
    REQUIRE(frame.Step(0.1) == CameraStatus::InvalidInput);
    REQUIRE(frame.Sample == accepted);
    CameraSample untouched;
    const CameraFrameInput badTicks{ .PreviousTick = 3, .CurrentTick = 2 };
    REQUIRE(director.TryUpdate(badTicks, untouched) == CameraStatus::InvalidInput);
    const CameraFrameInput badView{ .Targets = {nullptr, 1} };
    REQUIRE(director.TryUpdate(badView, untouched) == CameraStatus::InvalidInput);
    REQUIRE(untouched == CameraSample{});
}
