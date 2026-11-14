// Local poses and scalar forward kinematics onto FoundationMath Affine3.
//
// component[root] = ToAffine3(local[root]); component[j] = Compose(component[parent[j]],
// ToAffine3(local[j])), following docs/architecture/skeletal-animation.md "Local pose and
// component pose". Composition uses full affine transforms so that nonuniform parent scale
// keeps its shear, and only a complete finite candidate is published.

#include <ludus/animation/pose.hpp>

#include "internal/storage.hpp"

#include <ludus/foundation/base/core.h>

#include <new>

namespace ludus::animation
{
using foundation::uint32;
using foundation::uint64;
using foundation::usize;
namespace math = foundation::math;

namespace
{
bool IsCompatible(const Skeleton& skeleton, const foundation::Fingerprint128& compatibility, usize jointCount) noexcept
{
    return skeleton.GetCompatibility() == compatibility && skeleton.GetJointCount() == jointCount;
}
} // namespace

LocalPose::LocalPose() noexcept = default;
LocalPose::~LocalPose() noexcept = default;
LocalPose::LocalPose(LocalPose&& other) noexcept = default;
LocalPose& LocalPose::operator=(LocalPose&& other) noexcept = default;

bool LocalPose::IsPrepared() const noexcept
{
    return mStorage.Get() != nullptr;
}

uint32 LocalPose::GetJointCount() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : static_cast<uint32>(mStorage->Joints.GetSize());
}

foundation::Fingerprint128 LocalPose::GetCompatibility() const noexcept
{
    return mStorage.Get() == nullptr ? foundation::Fingerprint128{} : mStorage->Compatibility;
}

uint64 LocalPose::GetRevision() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->Revision;
}

foundation::BorrowedView<const math::TransformTRS> LocalPose::GetJoints() const noexcept
{
    if (mStorage.Get() == nullptr)
    {
        return {};
    }
    return {mStorage->Joints.GetData(), mStorage->Joints.GetSize()};
}

AnimationStatus LocalPose::TrySetJoint(uint32 jointIndex, const math::TransformTRS& value) noexcept
{
    if (mStorage.Get() == nullptr || jointIndex >= mStorage->Joints.GetSize())
    {
        return AnimationStatus::InvalidArgument;
    }
    const AnimationStatus status = detail::ValidateLocalTransform(value);
    if (status != AnimationStatus::Success)
    {
        return status;
    }
    mStorage->Joints[jointIndex] = detail::NormalizeLocalTransform(value);
    ++mStorage->Revision;
    return AnimationStatus::Success;
}

AnimationStatus LocalPose::TryResetToRest(const Skeleton& skeleton) noexcept
{
    if (mStorage.Get() == nullptr || skeleton.IsPrepared() == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    if (IsCompatible(skeleton, mStorage->Compatibility, mStorage->Joints.GetSize()) == false)
    {
        return AnimationStatus::IncompatibleSkeleton;
    }
    const foundation::BorrowedView<const SkeletonJointRecord> joints = skeleton.GetJoints();
    for (usize index = 0; index < joints.GetSize(); ++index)
    {
        mStorage->Joints[index] = joints[index].RestLocal;
    }
    ++mStorage->Revision;
    return AnimationStatus::Success;
}

AnimationStatus TryPrepareLocalPose(const Skeleton& skeleton, LocalPose& outPose) noexcept
{
    if (skeleton.IsPrepared() == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    detail::LocalPoseStorage* storageOrNull = new (std::nothrow) detail::LocalPoseStorage;
    if (storageOrNull == nullptr)
    {
        return AnimationStatus::OutOfMemory;
    }
    foundation::UniquePtr<detail::LocalPoseStorage> candidate(storageOrNull);
    const foundation::BorrowedView<const SkeletonJointRecord> joints = skeleton.GetJoints();
    if (detail::TryAllocateArray(joints.GetSize(), candidate->Joints) == false)
    {
        return AnimationStatus::OutOfMemory;
    }
    for (usize index = 0; index < joints.GetSize(); ++index)
    {
        candidate->Joints[index] = joints[index].RestLocal;
    }
    candidate->Compatibility = skeleton.GetCompatibility();
    candidate->Revision = 1;
    outPose.mStorage = foundation::Move(candidate);
    return AnimationStatus::Success;
}

ComponentPose::ComponentPose() noexcept = default;
ComponentPose::~ComponentPose() noexcept = default;
ComponentPose::ComponentPose(ComponentPose&& other) noexcept = default;
ComponentPose& ComponentPose::operator=(ComponentPose&& other) noexcept = default;

bool ComponentPose::IsPrepared() const noexcept
{
    return mStorage.Get() != nullptr;
}

bool ComponentPose::IsEvaluated() const noexcept
{
    return mStorage.Get() != nullptr && mStorage->IsEvaluated == true;
}

uint32 ComponentPose::GetJointCount() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : static_cast<uint32>(mStorage->Published.GetSize());
}

foundation::Fingerprint128 ComponentPose::GetCompatibility() const noexcept
{
    return mStorage.Get() == nullptr ? foundation::Fingerprint128{} : mStorage->Compatibility;
}

uint64 ComponentPose::GetSourceRevision() const noexcept
{
    return IsEvaluated() == true ? mStorage->SourceRevision : 0;
}

foundation::BorrowedView<const math::Affine3> ComponentPose::GetJoints() const noexcept
{
    if (IsEvaluated() == false)
    {
        return {};
    }
    return {mStorage->Published.GetData(), mStorage->Published.GetSize()};
}

AnimationStatus TryPrepareComponentPose(const Skeleton& skeleton, ComponentPose& outPose) noexcept
{
    if (skeleton.IsPrepared() == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    detail::ComponentPoseStorage* storageOrNull = new (std::nothrow) detail::ComponentPoseStorage;
    if (storageOrNull == nullptr)
    {
        return AnimationStatus::OutOfMemory;
    }
    foundation::UniquePtr<detail::ComponentPoseStorage> candidate(storageOrNull);
    const usize count = skeleton.GetJointCount();
    if (detail::TryAllocateArray(count, candidate->Published) == false ||
        detail::TryAllocateArray(count, candidate->Candidate) == false)
    {
        return AnimationStatus::OutOfMemory;
    }
    candidate->Compatibility = skeleton.GetCompatibility();
    outPose.mStorage = foundation::Move(candidate);
    return AnimationStatus::Success;
}

AnimationStatus
TryEvaluateComponentPose(const Skeleton& skeleton, const LocalPose& localPose, ComponentPose& inoutPose) noexcept
{
    if (skeleton.IsPrepared() == false || localPose.IsPrepared() == false || inoutPose.IsPrepared() == false)
    {
        return AnimationStatus::InvalidArgument;
    }
    detail::ComponentPoseStorage& storage = *inoutPose.mStorage;
    const foundation::BorrowedView<const math::TransformTRS> locals = localPose.GetJoints();
    if (IsCompatible(skeleton, localPose.GetCompatibility(), locals.GetSize()) == false ||
        IsCompatible(skeleton, storage.Compatibility, storage.Candidate.GetSize()) == false)
    {
        return AnimationStatus::IncompatibleSkeleton;
    }
    const foundation::BorrowedView<const SkeletonJointRecord> joints = skeleton.GetJoints();
    for (usize index = 0; index < joints.GetSize(); ++index)
    {
        // Local values are validated at their edit boundary; revalidating here keeps the
        // trusted ToAffine3 conversion sound for every producer of local poses.
        const AnimationStatus status = detail::ValidateLocalTransform(locals[index]);
        if (status != AnimationStatus::Success)
        {
            return status;
        }
        const math::Affine3 local = math::ToAffine3(locals[index]);
        const uint32 parent = joints[index].ParentIndex;
        const math::Affine3 component =
            parent == NO_PARENT_JOINT ? local : math::Compose(storage.Candidate[parent], local);
        if (math::IsFinite(component) == false)
        {
            return AnimationStatus::OutOfRange;
        }
        storage.Candidate[index] = component;
    }
    foundation::Array<math::Affine3> previous = foundation::Move(storage.Published);
    storage.Published = foundation::Move(storage.Candidate);
    storage.Candidate = foundation::Move(previous);
    storage.SourceRevision = localPose.GetRevision();
    storage.IsEvaluated = true;
    return AnimationStatus::Success;
}
} // namespace ludus::animation
