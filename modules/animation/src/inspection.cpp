// Headless skeleton and selected-joint inspection
// (docs/architecture/skeletal-animation.md, "Inspection capture and failure behavior").

#include <ludus/animation/inspection.hpp>

#include <ludus/foundation/base/core.h>

namespace ludus::animation
{
using foundation::uint32;
using foundation::usize;

AnimationStatus TryInspectSkeleton(const Skeleton& skeleton, SkeletonInspection& outInspection) noexcept
{
    if (skeleton.IsPrepared() == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    const foundation::BorrowedView<const SkeletonJointRecord> joints = skeleton.GetJoints();
    uint32 leafCount = 0;
    for (usize index = 0; index < joints.GetSize(); ++index)
    {
        if (joints[index].SubtreeEnd == index + 1)
        {
            ++leafCount;
        }
    }
    outInspection =
    {
        .Identity = skeleton.GetIdentity(),
        .Compatibility = skeleton.GetCompatibility(),
        .MetersPerUnit = skeleton.GetMetersPerUnit(),
        .JointCount = skeleton.GetJointCount(),
        .LeafCount = leafCount,
        .MaxDepth = skeleton.GetMaxDepth(),
        .PreparedBytes = skeleton.GetPreparedBytes(),
    };
    return AnimationStatus::Success;
}

AnimationStatus TryInspectJoint(const Skeleton& skeleton,
                                const LocalPose& localPose,
                                const ComponentPose* componentPoseOrNull,
                                uint32 jointIndex,
                                JointInspection& outInspection) noexcept
{
    if (skeleton.IsPrepared() == false || localPose.IsPrepared() == false ||
        (componentPoseOrNull != nullptr && componentPoseOrNull->IsPrepared() == false) ||
        jointIndex >= skeleton.GetJointCount())
    {
        return AnimationStatus::InvalidArgument;
    }
    if (localPose.GetCompatibility() != skeleton.GetCompatibility() ||
        (componentPoseOrNull != nullptr && componentPoseOrNull->GetCompatibility() != skeleton.GetCompatibility()))
    {
        return AnimationStatus::IncompatibleSkeleton;
    }
    const foundation::BorrowedView<const SkeletonJointRecord> joints = skeleton.GetJoints();
    const SkeletonJointRecord& joint = joints[jointIndex];
    JointInspection inspection
    {
        .JointIndex = jointIndex,
        .JointId = joint.JointId,
        .ParentIndex = joint.ParentIndex,
        .SubtreeEnd = joint.SubtreeEnd,
        .RestLocal = joint.RestLocal,
        .CurrentLocal = localPose.GetJoints()[jointIndex],
        .LocalRevision = localPose.GetRevision(),
    };
    if (joint.ParentIndex != NO_PARENT_JOINT)
    {
        inspection.ParentJointId = joints[joint.ParentIndex].JointId;
    }
    for (uint32 ancestor = joint.ParentIndex; ancestor != NO_PARENT_JOINT; ancestor = joints[ancestor].ParentIndex)
    {
        ++inspection.Depth;
    }
    // Direct children are the subtree joints whose parent is this joint; skipping each
    // child's own subtree visits only the children.
    for (uint32 child = jointIndex + 1; child < joint.SubtreeEnd; child = joints[child].SubtreeEnd)
    {
        ++inspection.ChildCount;
    }
    if (componentPoseOrNull != nullptr && componentPoseOrNull->IsEvaluated() == true)
    {
        inspection.HasComponent = true;
        inspection.Component = componentPoseOrNull->GetJoints()[jointIndex];
        inspection.ComponentSourceRevision = componentPoseOrNull->GetSourceRevision();
        inspection.IsComponentCurrent = inspection.ComponentSourceRevision == inspection.LocalRevision;
    }
    outInspection = inspection;
    return AnimationStatus::Success;
}
} // namespace ludus::animation
