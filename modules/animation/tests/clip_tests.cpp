// Reference clip validation, canonical events, fingerprints and the reference samplers.

#include "clip_fixtures.hpp"

#include <ludus/animation/clip.hpp>
#include <ludus/animation/pose.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::animation;
using namespace ludus::animation::test;
namespace foundation = ludus::foundation;

namespace
{
constexpr float32 SAMPLE_TOLERANCE = 1e-6F;

Skeleton MakeChainSkeleton()
{
    return PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID);
}

AnimationStatus Prepare(const ClipPackage& package, const Skeleton& skeleton, Clip& outClip)
{
    return TryPrepareClip(package, skeleton, {}, outClip);
}

void RequireSameTransform(const math::TransformTRS& left, const math::TransformTRS& right)
{
    REQUIRE(IsNear(left.Translation, right.Translation, SAMPLE_TOLERANCE));
    REQUIRE(math::SameRotation(left.Rotation, right.Rotation, SAMPLE_TOLERANCE) == true);
    REQUIRE(IsNear(left.Scale, right.Scale, SAMPLE_TOLERANCE));
}
} // namespace

TEST_CASE("A valid reference clip publishes its timeline, tracks and trajectory", "[animation][clip]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    const ClipSource source = MakeClipSource();
    const Clip clip = PrepareClip(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton);
    REQUIRE(clip.IsPrepared() == true);
    REQUIRE(clip.GetDurationSeconds() == CLIP_DURATION_SECONDS);
    REQUIRE(clip.GetSampleCount() == CLIP_SAMPLE_COUNT);
    REQUIRE(clip.GetTrackCount() == CLIP_TRACK_COUNT);
    REQUIRE(clip.GetWrapMode() == ClipWrapMode::Clamp);
    REQUIRE(clip.GetRootMode() == RootMotionMode::Full);
    REQUIRE(clip.GetSkeletonCompatibility() == skeleton.GetCompatibility());
    REQUIRE(clip.GetRootReferenceOffset().Translation.Y == 0.9F);
    REQUIRE(clip.GetEvents().GetSize() == CLIP_EVENT_COUNT);
    REQUIRE(clip.GetFingerprint() != foundation::Fingerprint128{});
}

TEST_CASE("Clip preparation rejects invalid timelines and shapes and preserves the generation", "[animation][clip]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    const ClipSource source = MakeClipSource();
    const ClipPackage valid = MakeClipPackage(source, ClipWrapMode::Loop);
    Clip clip = PrepareClip(valid, skeleton);
    const foundation::Fingerprint128 published = clip.GetFingerprint();

    const auto requireRejected = [&](const ClipPackage& package, AnimationStatus expected) {
        REQUIRE(Prepare(package, skeleton, clip) == expected);
        REQUIRE(clip.GetFingerprint() == published);
    };

    ClipPackage package = valid;
    package.Identity.AuthoringId = 0;
    requireRejected(package, AnimationStatus::InvalidArgument);

    package = valid;
    package.DurationSeconds = std::numeric_limits<float64>::quiet_NaN();
    requireRejected(package, AnimationStatus::InvalidArgument);
    package.DurationSeconds = -1;
    requireRejected(package, AnimationStatus::InvalidArgument);
    package.DurationSeconds = MAX_CLIP_DURATION_SECONDS * 2;
    requireRejected(package, AnimationStatus::InvalidArgument);
    // The last sample (1 s) may not lie after the duration.
    package.DurationSeconds = 0.75;
    requireRejected(package, AnimationStatus::InvalidArgument);

    package = valid;
    package.SampleRateHertz = 0;
    requireRejected(package, AnimationStatus::InvalidArgument);
    package.SampleRateHertz = MAX_CLIP_SAMPLE_RATE_HERTZ * 2;
    requireRejected(package, AnimationStatus::InvalidArgument);

    package = valid;
    package.SampleCount = 0;
    requireRejected(package, AnimationStatus::InvalidArgument);

    package = valid;
    package.Samples = {source.Samples.GetData(), source.Samples.GetSize() - 1};
    requireRejected(package, AnimationStatus::InvalidArgument);

    package = valid;
    package.Samples = {nullptr, source.Samples.GetSize()};
    requireRejected(package, AnimationStatus::InvalidArgument);

    package = valid;
    package.WrapMode = static_cast<ClipWrapMode>(7);
    requireRejected(package, AnimationStatus::InvalidArgument);

    package = valid;
    package.RootSamples = {source.RootSamples.GetData(), source.RootSamples.GetSize() - 1};
    requireRejected(package, AnimationStatus::InvalidArgument);

    // Zero duration cannot loop, and cannot carry events.
    package = valid;
    package.DurationSeconds = 0;
    package.SampleCount = 1;
    package.Samples = {source.Samples.GetData(), CLIP_TRACK_COUNT};
    package.RootSamples = {source.RootSamples.GetData(), 1};
    package.RootEnd = {};
    requireRejected(package, AnimationStatus::InvalidArgument);
    package.WrapMode = ClipWrapMode::Clamp;
    requireRejected(package, AnimationStatus::InvalidArgument);
    package.Events = {};
    REQUIRE(Prepare(package, skeleton, clip) == AnimationStatus::Success);
    REQUIRE(clip.GetDurationSeconds() == 0);
}

TEST_CASE("Clip preparation enforces its admission limits", "[animation][clip]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    const ClipSource source = MakeClipSource();
    const ClipPackage package = MakeClipPackage(source, ClipWrapMode::Clamp);
    Clip clip;
    REQUIRE(TryPrepareClip(package, skeleton, { .MaxTracks = 1 }, clip) == AnimationStatus::BudgetExceeded);
    REQUIRE(TryPrepareClip(package, skeleton, { .MaxSamples = 2 }, clip) == AnimationStatus::BudgetExceeded);
    REQUIRE(TryPrepareClip(package, skeleton, { .MaxEvents = 3 }, clip) == AnimationStatus::BudgetExceeded);
    REQUIRE(TryPrepareClip(package, skeleton, { .MaxTracks = 0 }, clip) == AnimationStatus::InvalidArgument);
    REQUIRE(clip.IsPrepared() == false);
}

TEST_CASE("Clip tracks must name distinct skeleton joints with valid samples", "[animation][clip]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    Clip clip;

    ClipSource source = MakeClipSource();
    source.TrackJointIds = {{100, 999}};
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton, clip) == AnimationStatus::InvalidJointId);
    source.TrackJointIds = {{300, 300}};
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton, clip) == AnimationStatus::InvalidJointId);

    source = MakeClipSource();
    source.Samples[3].Rotation = {0, 0, 0, 2};
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton, clip) == AnimationStatus::InvalidTransform);

    source = MakeClipSource();
    source.Samples[4].Scale = {1, 0, 1};
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton, clip) == AnimationStatus::UnsupportedScale);

    // A clip for another skeleton never binds: the chain IDs are absent from the branch skeleton.
    const Skeleton branch = PrepareSkeleton(MakeBranchJoints(), BRANCH_AUTHORING_ID);
    source = MakeClipSource();
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Clamp), branch, clip) == AnimationStatus::InvalidJointId);
    REQUIRE(clip.IsPrepared() == false);
}

TEST_CASE("Clip events need unique identities in nondecreasing timeline order", "[animation][clip]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    Clip clip;
    ClipSource source = MakeClipSource();
    source.Events[1].EventId = 0;
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton, clip) == AnimationStatus::InvalidArgument);

    source = MakeClipSource();
    source.Events[2].EventId = EVENT_START;
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton, clip) == AnimationStatus::InvalidArgument);

    source = MakeClipSource();
    source.Events[1].TimeSeconds = 0.75;
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton, clip) == AnimationStatus::InvalidArgument);

    source = MakeClipSource();
    source.Events[3].TimeSeconds = CLIP_DURATION_SECONDS + 0.5;
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton, clip) == AnimationStatus::InvalidArgument);

    source = MakeClipSource();
    source.Events[3].TimeSeconds = std::numeric_limits<float64>::infinity();
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton, clip) == AnimationStatus::InvalidArgument);
    REQUIRE(clip.IsPrepared() == false);
}

TEST_CASE("A looping clip canonicalizes an endpoint event to a loop-start event", "[animation][clip][events]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    const ClipSource source = MakeClipSource();
    const Clip clamped = PrepareClip(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton);
    const Clip looping = PrepareClip(MakeClipPackage(source, ClipWrapMode::Loop), skeleton);

    // Clamped clips keep the endpoint; one-shot endpoint events occur at the duration.
    REQUIRE(clamped.GetEvents()[3].EventId == EVENT_END);
    REQUIRE(clamped.GetEvents()[3].TimeSeconds == CLIP_DURATION_SECONDS);

    // Looping clips order canonical events by (time, identity): both loop-start aliases sit at zero.
    const foundation::BorrowedView<const ClipEventRecord> events = looping.GetEvents();
    REQUIRE(events.GetSize() == CLIP_EVENT_COUNT);
    REQUIRE(events[0].EventId == EVENT_START);
    REQUIRE(events[1].EventId == EVENT_END);
    REQUIRE(events[1].TimeSeconds == 0);
    REQUIRE(events[2].EventId == EVENT_QUARTER);
    REQUIRE(events[3].EventId == EVENT_HALF);
}

TEST_CASE("A clip fingerprint follows content, not the authoring identity", "[animation][clip]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    ClipSource source = MakeClipSource();
    ClipPackage package = MakeClipPackage(source, ClipWrapMode::Loop);
    const Clip first = PrepareClip(package, skeleton);
    package.Identity = { .AuthoringId = 0xBEEF, .Revision = 9 };
    const Clip recooked = PrepareClip(package, skeleton);
    REQUIRE(recooked.GetFingerprint() == first.GetFingerprint());
    REQUIRE(recooked.GetIdentity().AuthoringId == 0xBEEF);

    const Clip clamped = PrepareClip(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton);
    REQUIRE(clamped.GetFingerprint() != first.GetFingerprint());

    source.Samples[5].Translation.X = 1.5F;
    const Clip edited = PrepareClip(MakeClipPackage(source, ClipWrapMode::Loop), skeleton);
    REQUIRE(edited.GetFingerprint() != first.GetFingerprint());
}

TEST_CASE("The root trajectory starts at the identity and keeps its endpoint", "[animation][clip][root]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    Clip clip;
    ClipSource source = MakeClipSource();
    source.RootSamples[0].Translation = {0, 0, 0.1F};
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Loop), skeleton, clip) == AnimationStatus::InvalidTransform);

    source = MakeClipSource();
    source.RootSamples[1].Rotation = {0, 0, 0, 0};
    REQUIRE(Prepare(MakeClipPackage(source, ClipWrapMode::Loop), skeleton, clip) == AnimationStatus::InvalidTransform);

    // Without an endpoint gap R(duration) is the last sample; a disagreeing endpoint is rejected.
    source = MakeClipSource();
    ClipPackage package = MakeClipPackage(source, ClipWrapMode::Loop);
    package.RootEnd.Translation.Z += 1;
    REQUIRE(Prepare(package, skeleton, clip) == AnimationStatus::InvalidTransform);

    // Planar trajectories rotate about +Y and stay in the XZ plane exactly.
    package = MakeClipPackage(source, ClipWrapMode::Loop);
    package.RootMode = RootMotionMode::Planar;
    REQUIRE(Prepare(package, skeleton, clip) == AnimationStatus::Success);
    source.RootSamples[1].Translation.Y = 0.01F;
    package = MakeClipPackage(source, ClipWrapMode::Loop);
    package.RootMode = RootMotionMode::Planar;
    REQUIRE(Prepare(package, skeleton, clip) == AnimationStatus::InvalidTransform);

    // Without a trajectory no root samples are accepted.
    source = MakeClipSource();
    package = MakeClipPackage(source, ClipWrapMode::Loop);
    package.RootMode = RootMotionMode::None;
    REQUIRE(Prepare(package, skeleton, clip) == AnimationStatus::InvalidArgument);
    package.RootSamples = {};
    REQUIRE(Prepare(package, skeleton, clip) == AnimationStatus::Success);
    RootTransform root{};
    REQUIRE(TrySampleClipRoot(clip, 0.5, root) == AnimationStatus::Success);
    REQUIRE(IsNear(root, RootTransform{}, 0));
}

TEST_CASE("The reference sampler reproduces samples and interpolates between them", "[animation][clip][sampler]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    const ClipSource source = MakeClipSource();
    const Clip clip = PrepareClip(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton);
    LocalPose pose;
    REQUIRE(TryPrepareLocalPose(skeleton, pose) == AnimationStatus::Success);

    for (usize sample = 0; sample < CLIP_SAMPLE_COUNT; ++sample)
    {
        const float64 time = static_cast<float64>(sample) / CLIP_SAMPLE_RATE_HERTZ;
        REQUIRE(TrySampleClip(clip, time, pose) == AnimationStatus::Success);
        RequireSameTransform(pose.GetJoints()[0], MakeChainSample(ChainTrack::Root, sample));
        RequireSameTransform(pose.GetJoints()[2], MakeChainSample(ChainTrack::Elbow, sample));
        // Untracked joints receive their rest value.
        RequireSameTransform(pose.GetJoints()[1], skeleton.GetJoints()[1].RestLocal);
        RequireSameTransform(pose.GetJoints()[3], skeleton.GetJoints()[3].RestLocal);
    }

    // A quarter of the way from sample 1 to sample 2.
    const uint64 revision = pose.GetRevision();
    REQUIRE(TrySampleClip(clip, 0.625, pose) == AnimationStatus::Success);
    REQUIRE(pose.GetRevision() == revision + 1);
    const math::TransformTRS first = MakeChainSample(ChainTrack::Elbow, 1);
    const math::TransformTRS second = MakeChainSample(ChainTrack::Elbow, 2);
    const math::TransformTRS expected
    {
        .Translation = math::Lerp(first.Translation, second.Translation, 0.25F),
        .Rotation = math::SlerpShortest(first.Rotation, second.Rotation, 0.25F),
        .Scale = math::Lerp(first.Scale, second.Scale, 0.25F),
    };
    RequireSameTransform(pose.GetJoints()[2], expected);
}

TEST_CASE("Sampling the endpoint gap loops to the first sample or holds the last", "[animation][clip][sampler]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    const ClipSource source = MakeClipSource();
    // The cook dropped a duplicated endpoint: the last sample is at 1 s, the duration is 1.5 s.
    ClipPackage package = MakeClipPackage(source, ClipWrapMode::Loop);
    package.DurationSeconds = 1.5;
    package.Events = {source.Events.GetData(), 3};
    package.RootEnd = { .Translation = {0, 0, 1.5F}, .Rotation = MakeRotation({0, 1, 0}, 0.75F) };
    const Clip looping = PrepareClip(package, skeleton);
    package.WrapMode = ClipWrapMode::Clamp;
    const Clip clamped = PrepareClip(package, skeleton);

    LocalPose pose;
    REQUIRE(TryPrepareLocalPose(skeleton, pose) == AnimationStatus::Success);
    const math::TransformTRS last = MakeChainSample(ChainTrack::Elbow, 2);
    const math::TransformTRS first = MakeChainSample(ChainTrack::Elbow, 0);

    REQUIRE(TrySampleClip(looping, 1.25, pose) == AnimationStatus::Success);
    RequireSameTransform(pose.GetJoints()[2],
                         {
                             .Translation = math::Lerp(last.Translation, first.Translation, 0.5F),
                             .Rotation = math::SlerpShortest(last.Rotation, first.Rotation, 0.5F),
                             .Scale = math::Lerp(last.Scale, first.Scale, 0.5F),
                         });
    REQUIRE(TrySampleClip(looping, 1.5, pose) == AnimationStatus::Success);
    RequireSameTransform(pose.GetJoints()[2], first);

    REQUIRE(TrySampleClip(clamped, 1.25, pose) == AnimationStatus::Success);
    RequireSameTransform(pose.GetJoints()[2], last);

    // The trajectory interpolates towards its explicit endpoint and reaches it exactly.
    RootTransform root{};
    REQUIRE(TrySampleClipRoot(looping, 1.5, root) == AnimationStatus::Success);
    REQUIRE(root.Translation == package.RootEnd.Translation);
    REQUIRE(TrySampleClipRoot(looping, 1.25, root) == AnimationStatus::Success);
    REQUIRE(IsNear(root.Translation, {0, 0, 1.25F}, SAMPLE_TOLERANCE));
    REQUIRE(math::SameRotation(root.Rotation, MakeRotation({0, 1, 0}, 0.625F), SAMPLE_TOLERANCE) == true);
}

TEST_CASE("The sampler rejects out-of-range times and incompatible poses without writing", "[animation][clip][sampler]")
{
    const Skeleton skeleton = MakeChainSkeleton();
    const ClipSource source = MakeClipSource();
    const Clip clip = PrepareClip(MakeClipPackage(source, ClipWrapMode::Clamp), skeleton);
    LocalPose pose;
    REQUIRE(TryPrepareLocalPose(skeleton, pose) == AnimationStatus::Success);
    const uint64 revision = pose.GetRevision();
    REQUIRE(TrySampleClip(clip, -0.1, pose) == AnimationStatus::InvalidArgument);
    REQUIRE(TrySampleClip(clip, CLIP_DURATION_SECONDS + 0.1, pose) == AnimationStatus::InvalidArgument);
    REQUIRE(TrySampleClip(clip, std::numeric_limits<float64>::quiet_NaN(), pose) == AnimationStatus::InvalidArgument);
    REQUIRE(pose.GetRevision() == revision);

    const Skeleton branch = PrepareSkeleton(MakeBranchJoints(), BRANCH_AUTHORING_ID);
    LocalPose branchPose;
    REQUIRE(TryPrepareLocalPose(branch, branchPose) == AnimationStatus::Success);
    REQUIRE(TrySampleClip(clip, 0.5, branchPose) == AnimationStatus::IncompatibleSkeleton);
    REQUIRE(branchPose.GetRevision() == 1);

    LocalPose unprepared;
    REQUIRE(TrySampleClip(clip, 0.5, unprepared) == AnimationStatus::InvalidArgument);
    const Clip empty;
    REQUIRE(TrySampleClip(empty, 0, pose) == AnimationStatus::InvalidArgument);
    RootTransform root{};
    REQUIRE(TrySampleClipRoot(empty, 0, root) == AnimationStatus::InvalidArgument);
}
