#include <ludus/animation/clip.hpp>
#include <ludus/animation/clip_player.hpp>
#include <ludus/animation/inspection.hpp>
#include <ludus/animation/pose.hpp>
#include <ludus/animation/skeleton.hpp>
#include <ludus/foundation/containers/static_array.hpp>

bool ExerciseInstalledAnimation() noexcept
{
    using namespace ludus::animation;
    const ludus::foundation::StaticArray<SkeletonJointRecord, 2> joints{{
        { .JointId = 1, .ParentIndex = NO_PARENT_JOINT, .SubtreeEnd = 2, .RestLocal = { .Translation = {0, 1, 0} } },
        { .JointId = 2, .ParentIndex = 0, .SubtreeEnd = 2, .RestLocal = { .Translation = {0, 0, 2} } },
    }};
    Skeleton skeleton;
    LocalPose local;
    ComponentPose component;
    if (TryPrepareSkeleton(
            { .Identity = { .AuthoringId = 1, .Revision = 1 }, .Joints = {joints.GetData(), joints.GetSize()} },
            {},
            skeleton) != AnimationStatus::Success ||
        TryPrepareLocalPose(skeleton, local) != AnimationStatus::Success ||
        TryPrepareComponentPose(skeleton, component) != AnimationStatus::Success ||
        TryEvaluateComponentPose(skeleton, local, component) != AnimationStatus::Success)
    {
        return false;
    }
    JointInspection inspection{};
    if (TryInspectJoint(skeleton, local, &component, 1, inspection) != AnimationStatus::Success ||
        inspection.Depth != 1 || inspection.IsComponentCurrent == false ||
        component.GetJoints()[1].Translation.Y != 1 || component.GetJoints()[1].Translation.Z != 2)
    {
        return false;
    }

    // A two-sample looping clip on joint 2 with one loop-start event and a straight trajectory.
    const ludus::foundation::uint64 trackJointId = 2;
    const ludus::foundation::StaticArray<ludus::foundation::math::TransformTRS, 2> samples{{
        { .Translation = {0, 0, 2} },
        { .Translation = {0, 0, 3} },
    }};
    const ClipEventRecord loopStart{ .EventId = 5, .TimeSeconds = 0, .IsReversible = false };
    const ludus::foundation::StaticArray<RootTransform, 2> root{{
        {},
        { .Translation = {0, 0, 1} },
    }};
    Clip clip;
    ClipPlayer player;
    ClipEvent event{};
    ClipAdvanceResult result{};
    if (TryPrepareClip(
            {
                .Identity = { .AuthoringId = 1, .Revision = 1 },
                .DurationSeconds = 1,
                .SampleRateHertz = 1,
                .SampleCount = 2,
                .WrapMode = ClipWrapMode::Loop,
                .TrackJointIds = {&trackJointId, 1},
                .Samples = {samples.GetData(), samples.GetSize()},
                .Events = {&loopStart, 1},
                .RootMode = RootMotionMode::Full,
                .RootSamples = {root.GetData(), root.GetSize()},
                .RootEnd = root[1],
                .RootReferenceOffset = {},
            },
            skeleton,
            {},
            clip) != AnimationStatus::Success ||
        TryActivateClipPlayer(clip,
                              {
                                  .InstanceId = 1,
                                  .ActivationId = 1,
                                  .StartTimeSeconds = 0.5,
                                  .Rate = 1,
                              },
                              player) != AnimationStatus::Success ||
        TryAdvanceClipPlayer(clip, 1, {&event, 1}, player, result) != AnimationStatus::Success ||
        TrySampleClip(clip, player.GetTimeSeconds(), local) != AnimationStatus::Success)
    {
        return false;
    }
    return result.EventCount == 1 && event.Cycle == 1 && player.GetCycle() == 1 &&
           result.RootDelta.Translation.Z == 1 && local.GetJoints()[1].Translation.Z == 2.5F;
}
