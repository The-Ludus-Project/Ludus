// Root-motion requests: rigid delta composition across loop crossings and reverse traversal,
// planar policy, and a blocking motor that never receives rejected motion again.

#include "clip_fixtures.hpp"

#include <ludus/animation/clip.hpp>
#include <ludus/animation/clip_player.hpp>
#include <ludus/animation/pose.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::animation;
using namespace ludus::animation::test;
namespace foundation = ludus::foundation;

namespace
{
constexpr float32 ROOT_TOLERANCE = 1e-5F;
constexpr usize ROOT_SAMPLE_COUNT = 2;

/// One-second looping trajectory clip with no pose tracks: each cycle moves `cycleEnd`.
struct TrajectoryClip final
{
    foundation::StaticArray<RootTransform, ROOT_SAMPLE_COUNT> RootSamples{};
    Clip LoopClip;
};

void PrepareTrajectory(const Skeleton& skeleton,
                       const RootTransform& cycleEnd,
                       RootMotionMode mode,
                       TrajectoryClip& outTrajectory)
{
    outTrajectory.RootSamples = {{RootTransform{}, cycleEnd}};
    const ClipPackage package
    {
        .Identity = { .AuthoringId = 0x2007, .Revision = 1 },
        .DurationSeconds = 1,
        .SampleRateHertz = 1,
        .SampleCount = ROOT_SAMPLE_COUNT,
        .WrapMode = ClipWrapMode::Loop,
        .TrackJointIds = {},
        .Samples = {},
        .Events = {},
        .RootMode = mode,
        .RootSamples = {outTrajectory.RootSamples.GetData(), ROOT_SAMPLE_COUNT},
        .RootEnd = cycleEnd,
        .RootReferenceOffset = {},
    };
    REQUIRE(TryPrepareClip(package, skeleton, {}, outTrajectory.LoopClip) == AnimationStatus::Success);
}

/// A square walk: one unit forward (+Z) and a quarter turn left (about +Y) per cycle.
RootTransform MakeSquareCycle()
{
    return { .Translation = {0, 0, 1}, .Rotation = MakeRotation({0, 1, 0}, QUARTER_TURN) };
}

RootTransform AdvanceRoot(const Clip& clip, float64 deltaSeconds, ClipPlayer& inoutPlayer)
{
    ClipAdvanceResult result{};
    REQUIRE(TryAdvanceClipPlayer(clip, deltaSeconds, {}, inoutPlayer, result) == AnimationStatus::Success);
    return result.RootDelta;
}

RootTransform Compose(const RootTransform& left, const RootTransform& right)
{
    RootTransform composed{};
    REQUIRE(TryComposeRootTransforms(left, right, composed) == AnimationStatus::Success);
    return composed;
}
} // namespace

TEST_CASE("A single-interval delta is inverse(R(old)) * R(new)", "[animation][root]")
{
    const Skeleton skeleton = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID);
    const ClipSource source = MakeClipSource();
    const Clip clip = PrepareClip(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton);
    ClipPlayer player = ActivatePlayer(clip, 0.25, 1);
    ClipAdvanceResult result{};
    foundation::Array<ClipEvent> buffer = MakeEventBuffer(CLIP_EVENT_COUNT);
    REQUIRE(TryAdvanceClipPlayer(clip, 0.5, MakeEventView(buffer), player, result) == AnimationStatus::Success);

    RootTransform start{};
    RootTransform end{};
    REQUIRE(TrySampleClipRoot(clip, 0.25, start) == AnimationStatus::Success);
    REQUIRE(TrySampleClipRoot(clip, 0.75, end) == AnimationStatus::Success);
    REQUIRE(IsNear(result.RootDelta, Compose(InvertRootTransform(start), end), ROOT_TOLERANCE));
}

TEST_CASE("Loop crossings compose the cycle transform in order rather than adding positions", "[animation][root]")
{
    const Skeleton skeleton = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID);
    TrajectoryClip trajectory;
    PrepareTrajectory(skeleton, MakeSquareCycle(), RootMotionMode::Full, trajectory);
    const Clip& clip = trajectory.LoopClip;

    // Two cycles: forward, turn, forward again to the right. Adding translations would give (0, 0, 2).
    ClipPlayer player = ActivatePlayer(clip, 0, 1);
    const RootTransform twoCycles = AdvanceRoot(clip, 2, player);
    REQUIRE(player.GetCycle() == 2);
    REQUIRE(IsNear(twoCycles.Translation, {1, 0, 1}, ROOT_TOLERANCE));
    REQUIRE(math::SameRotation(twoCycles.Rotation, MakeRotation({0, 1, 0}, 2 * QUARTER_TURN), ROOT_TOLERANCE) == true);

    // Four cycles close the square.
    player = ActivatePlayer(clip, 0, 1);
    REQUIRE(IsNear(AdvanceRoot(clip, 4, player), RootTransform{}, ROOT_TOLERANCE));

    // One and a half cycles: the cycle end, then half of the next cycle in the turned frame.
    player = ActivatePlayer(clip, 0, 1);
    const RootTransform partial = AdvanceRoot(clip, 1.5, player);
    REQUIRE(IsNear(partial.Translation, {0.5F, 0, 1}, ROOT_TOLERANCE));
    REQUIRE(math::SameRotation(partial.Rotation, MakeRotation({0, 1, 0}, 1.5F * QUARTER_TURN), ROOT_TOLERANCE) == true);
}

TEST_CASE("Per-tick root deltas compose to the delta of the whole interval", "[animation][root]")
{
    const Skeleton skeleton = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID);
    TrajectoryClip trajectory;
    PrepareTrajectory(skeleton, MakeSquareCycle(), RootMotionMode::Full, trajectory);
    const Clip& clip = trajectory.LoopClip;

    constexpr usize TICK_COUNT = 37;
    constexpr float64 TICK_SECONDS = 0.1;
    ClipPlayer ticking = ActivatePlayer(clip, 0.35, 1);
    RootTransform composed{};
    for (usize tick = 0; tick < TICK_COUNT; ++tick)
    {
        composed = Compose(composed, AdvanceRoot(clip, TICK_SECONDS, ticking));
    }
    ClipPlayer whole = ActivatePlayer(clip, 0.35, 1);
    const RootTransform direct = AdvanceRoot(clip, static_cast<float64>(TICK_COUNT) * TICK_SECONDS, whole);
    REQUIRE(whole.GetCycle() == ticking.GetCycle());
    REQUIRE(IsNear(composed, direct, ROOT_TOLERANCE));
}

TEST_CASE("Reverse traversal requests the inverse of the forward motion", "[animation][root]")
{
    const Skeleton skeleton = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID);
    TrajectoryClip trajectory;
    PrepareTrajectory(skeleton, MakeSquareCycle(), RootMotionMode::Full, trajectory);
    const Clip& clip = trajectory.LoopClip;

    ClipPlayer player = ActivatePlayer(clip, 0.2, 1);
    const RootTransform forward = AdvanceRoot(clip, 2.3, player);
    REQUIRE(player.TrySetRate(-1) == AnimationStatus::Success);
    const RootTransform reverse = AdvanceRoot(clip, 2.3, player);
    REQUIRE(player.GetCycle() == 0);
    REQUIRE(math::Abs(player.GetTimeSeconds() - 0.2) <= 1e-9);
    REQUIRE(IsNear(reverse, InvertRootTransform(forward), ROOT_TOLERANCE));
    REQUIRE(IsNear(Compose(forward, reverse), RootTransform{}, ROOT_TOLERANCE));
}

TEST_CASE("Planar trajectories request planar motion exactly", "[animation][root]")
{
    const Skeleton skeleton = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID);
    TrajectoryClip trajectory;
    const RootTransform cycle{ .Translation = {0.3F, 0, 1.1F}, .Rotation = MakeRotation({0, 1, 0}, 0.7F) };
    PrepareTrajectory(skeleton, cycle, RootMotionMode::Planar, trajectory);
    ClipPlayer player = ActivatePlayer(trajectory.LoopClip, 0.1, 1.3);
    for (usize tick = 0; tick < 50; ++tick)
    {
        const RootTransform delta = AdvanceRoot(trajectory.LoopClip, 0.37, player);
        REQUIRE(delta.Translation.Y == 0);
        REQUIRE(delta.Rotation.X == 0);
        REQUIRE(delta.Rotation.Z == 0);
    }
}

TEST_CASE("Root transform composition reports unrepresentable results", "[animation][root]")
{
    const RootTransform huge{ .Translation = {std::numeric_limits<float32>::max(), 0, 0}, .Rotation = {} };
    RootTransform composed{};
    REQUIRE(TryComposeRootTransforms(huge, huge, composed) == AnimationStatus::OutOfRange);
    const RootTransform nonfinite{ .Translation = {std::numeric_limits<float32>::infinity(), 0, 0}, .Rotation = {} };
    REQUIRE(TryComposeRootTransforms(nonfinite, {}, composed) == AnimationStatus::InvalidTransform);
    REQUIRE(IsNear(composed, RootTransform{}, 0));
}

TEST_CASE("Blocked root movement is never applied again by animation", "[animation][root][motor]")
{
    // A straight walk of one unit per second whose motion root was extracted at cook time: the
    // root joint's pose track keeps only a residual bob, so the visual pose never re-adds motion.
    const Skeleton skeleton = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID);
    ClipSource source = MakeClipSource();
    for (usize sample = 0; sample < CLIP_SAMPLE_COUNT; ++sample)
    {
        source.RootSamples[sample] = { .Translation = {0, 0, 0.5F * static_cast<float32>(sample)}, .Rotation = {} };
    }
    ClipPackage package = MakeClipPackage(source, ClipWrapMode::Loop);
    package.RootEnd = source.RootSamples[CLIP_SAMPLE_COUNT - 1];
    const Clip clip = PrepareClip(package, skeleton);
    LocalPose pose;
    REQUIRE(TryPrepareLocalPose(skeleton, pose) == AnimationStatus::Success);

    // The game motor owns the character transform and blocks at a wall.
    constexpr float32 WALL_Z = 1.6F;
    constexpr float64 TICK_SECONDS = 0.25;
    constexpr usize BLOCKED_TICKS = 16;
    RootTransform character{};
    ClipPlayer player = ActivatePlayer(clip, 0, 1);
    foundation::Array<ClipEvent> buffer = MakeEventBuffer(CLIP_EVENT_COUNT * 2);
    float32 previousResidualZ = 0;
    for (usize tick = 0; tick < BLOCKED_TICKS; ++tick)
    {
        ClipAdvanceResult result{};
        REQUIRE(TryAdvanceClipPlayer(clip, TICK_SECONDS, MakeEventView(buffer), player, result) ==
                AnimationStatus::Success);
        // Each request covers this tick's interval only.
        REQUIRE(IsNear(result.RootDelta.Translation, {0, 0, 0.25F}, ROOT_TOLERANCE));
        RootTransform desired = Compose(character, result.RootDelta);
        desired.Translation.Z = desired.Translation.Z > WALL_Z ? WALL_Z : desired.Translation.Z;
        character = desired;

        REQUIRE(TrySampleClip(clip, player.GetTimeSeconds(), pose) == AnimationStatus::Success);
        const math::TransformTRS& root = pose.GetJoints()[0];
        // The sampled root carries only the cooked residual, never the trajectory.
        REQUIRE(root.Translation.Z == previousResidualZ);
        previousResidualZ = root.Translation.Z;
        const math::Vector3 visualRoot = character.Translation + math::Rotate(character.Rotation, root.Translation);
        REQUIRE(visualRoot.Z <= WALL_Z + ROOT_TOLERANCE);
    }
    REQUIRE(character.Translation.Z == WALL_Z);
    REQUIRE(player.GetCycle() == 4);

    // With the wall removed the next request is one tick of motion, not the rejected backlog.
    ClipAdvanceResult result{};
    REQUIRE(TryAdvanceClipPlayer(clip, TICK_SECONDS, MakeEventView(buffer), player, result) ==
            AnimationStatus::Success);
    const RootTransform freed = Compose(character, result.RootDelta);
    REQUIRE(IsNear(freed.Translation.Z, WALL_Z + 0.25F, ROOT_TOLERANCE));
}
