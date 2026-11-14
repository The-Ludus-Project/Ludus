// Mesh skin bindings: palette-to-skeleton maps and inverse bind transforms owned
// per mesh, kept distinct from the skeleton rest pose
// (docs/architecture/skeletal-animation.md, "Skeleton asset").

#include <ludus/animation/skin_binding.hpp>

#include "internal/storage.hpp"

#include <ludus/foundation/base/core.h>

#include <new>

namespace ludus::animation
{
using foundation::uint32;
using foundation::usize;
namespace math = foundation::math;

namespace
{
bool IsInverseBindValid(const math::Affine3& inverseBind) noexcept
{
    math::Affine3 bind{};
    return math::IsFinite(inverseBind) == true && math::TryInverse(inverseBind, bind) == math::MathStatus::Success;
}
} // namespace

SkinBinding::SkinBinding() noexcept = default;
SkinBinding::~SkinBinding() noexcept = default;
SkinBinding::SkinBinding(SkinBinding&& other) noexcept = default;
SkinBinding& SkinBinding::operator=(SkinBinding&& other) noexcept = default;

bool SkinBinding::IsPrepared() const noexcept
{
    return mStorage.Get() != nullptr;
}

uint32 SkinBinding::GetPaletteSize() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : static_cast<uint32>(mStorage->JointIndices.GetSize());
}

foundation::Fingerprint128 SkinBinding::GetCompatibility() const noexcept
{
    return mStorage.Get() == nullptr ? foundation::Fingerprint128{} : mStorage->Compatibility;
}

foundation::BorrowedView<const uint32> SkinBinding::GetJointIndices() const noexcept
{
    if (mStorage.Get() == nullptr)
    {
        return {};
    }
    return {mStorage->JointIndices.GetData(), mStorage->JointIndices.GetSize()};
}

foundation::BorrowedView<const math::Affine3> SkinBinding::GetInverseBindTransforms() const noexcept
{
    if (mStorage.Get() == nullptr)
    {
        return {};
    }
    return {mStorage->InverseBinds.GetData(), mStorage->InverseBinds.GetSize()};
}

AnimationStatus TryPrepareSkinBinding(const Skeleton& skeleton,
                                      const SkinBindingPackage& package,
                                      const SkinBindingLimits& limits,
                                      SkinBinding& outBinding) noexcept
{
    const foundation::BorrowedView<const SkinBindingJointRecord> palette = package.Palette;
    if (skeleton.IsPrepared() == false || palette.IsValid() == false || palette.IsEmpty() == true ||
        limits.MaxPaletteEntries == 0)
    {
        return AnimationStatus::InvalidArgument;
    }
    if (package.SkeletonCompatibility != skeleton.GetCompatibility())
    {
        return AnimationStatus::IncompatibleSkeleton;
    }
    if (palette.GetSize() > limits.MaxPaletteEntries || palette.GetSize() > MAX_SKELETON_JOINTS)
    {
        return AnimationStatus::BudgetExceeded;
    }
    for (const SkinBindingJointRecord& entry : palette)
    {
        if (IsInverseBindValid(entry.InverseBindComponent) == false)
        {
            return AnimationStatus::InvalidTransform;
        }
    }

    detail::SkinBindingStorage* storageOrNull = new (std::nothrow) detail::SkinBindingStorage;
    if (storageOrNull == nullptr)
    {
        return AnimationStatus::OutOfMemory;
    }
    foundation::UniquePtr<detail::SkinBindingStorage> candidate(storageOrNull);
    if (detail::TryAllocateArray(palette.GetSize(), candidate->JointIndices) == false ||
        detail::TryAllocateArray(palette.GetSize(), candidate->InverseBinds) == false)
    {
        return AnimationStatus::OutOfMemory;
    }
    for (usize index = 0; index < palette.GetSize(); ++index)
    {
        uint32 jointIndex = 0;
        if (skeleton.TryFindJointIndex(palette[index].JointId, jointIndex) != AnimationStatus::Success)
        {
            return AnimationStatus::InvalidJointId;
        }
        candidate->JointIndices[index] = jointIndex;
        candidate->InverseBinds[index] = palette[index].InverseBindComponent;
    }
    // A palette may not drive one joint twice. Preparation-time pairwise comparison is
    // bounded by the admitted palette limit and needs no scratch allocation.
    for (usize index = 1; index < palette.GetSize(); ++index)
    {
        for (usize previous = 0; previous < index; ++previous)
        {
            if (candidate->JointIndices[index] == candidate->JointIndices[previous])
            {
                return AnimationStatus::InvalidJointId;
            }
        }
    }
    candidate->Compatibility = skeleton.GetCompatibility();
    outBinding.mStorage = foundation::Move(candidate);
    return AnimationStatus::Success;
}

AnimationStatus TryComputeSkinningPalette(const SkinBinding& binding,
                                          const ComponentPose& pose,
                                          foundation::BorrowedView<math::Affine3> outPalette) noexcept
{
    if (binding.IsPrepared() == false || pose.IsPrepared() == false || outPalette.IsValid() == false ||
        outPalette.GetSize() != binding.GetPaletteSize())
    {
        return AnimationStatus::InvalidArgument;
    }
    if (binding.GetCompatibility() != pose.GetCompatibility())
    {
        return AnimationStatus::IncompatibleSkeleton;
    }
    if (pose.IsEvaluated() == false)
    {
        return AnimationStatus::NotEvaluated;
    }
    const foundation::BorrowedView<const math::Affine3> components = pose.GetJoints();
    const foundation::BorrowedView<const uint32> jointIndices = binding.GetJointIndices();
    const foundation::BorrowedView<const math::Affine3> inverseBinds = binding.GetInverseBindTransforms();
    // Check every entry before writing any, so failure leaves the caller's palette unchanged
    // without scratch storage.
    for (usize index = 0; index < jointIndices.GetSize(); ++index)
    {
        if (math::IsFinite(math::Compose(components[jointIndices[index]], inverseBinds[index])) == false)
        {
            return AnimationStatus::OutOfRange;
        }
    }
    for (usize index = 0; index < jointIndices.GetSize(); ++index)
    {
        outPalette[index] = math::Compose(components[jointIndices[index]], inverseBinds[index]);
    }
    return AnimationStatus::Success;
}
} // namespace ludus::animation
