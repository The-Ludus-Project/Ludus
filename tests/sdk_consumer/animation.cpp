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
    return TryInspectJoint(skeleton, local, &component, 1, inspection) == AnimationStatus::Success &&
           inspection.Depth == 1 && inspection.IsComponentCurrent == true &&
           component.GetJoints()[1].Translation.Y == 1 && component.GetJoints()[1].Translation.Z == 2;
}
