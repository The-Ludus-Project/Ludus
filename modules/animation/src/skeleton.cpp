// Skeleton package validation and immutable generation preparation.
//
// The flat depth-first, parent-before-child layout with stored subtree ends,
// stable joint identities and a separate compatibility identity follows the
// Ludus skeletal animation architecture (docs/architecture/skeletal-animation.md,
// "Skeleton asset" and "Local pose and component pose").

#include <ludus/animation/skeleton.hpp>

#include "internal/storage.hpp"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <bit>
#include <new>
#include <span>

namespace ludus::animation
{
using foundation::float32;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;
namespace math = foundation::math;

namespace
{
/// Translation (3), rotation (4) and scale (3) components of one canonical rest transform.
constexpr usize TRANSFORM_COMPONENT_COUNT = 10;
/// Canonical header: version, joint count and units.
constexpr usize COMPATIBILITY_HEADER_BYTES = sizeof(uint32) + sizeof(uint32) + sizeof(float32);
/// Canonical joint: identity, parent, subtree end and rest transform components.
constexpr usize COMPATIBILITY_JOINT_BYTES =
    sizeof(uint64) + sizeof(uint32) + sizeof(uint32) + TRANSFORM_COMPONENT_COUNT * sizeof(float32);

/// Little-endian writer over a buffer sized exactly for the canonical compatibility bytes.
class CanonicalByteWriter final
{
public:
    explicit CanonicalByteWriter(std::span<uint8> bytes) noexcept : mBytes(bytes) {}

    void WriteUint32(uint32 value) noexcept
    {
        [[maybe_unused]] const bool isWritten = foundation::TryWriteLittleEndian(value, mBytes.subspan(mOffset));
        LUDUS_ASSERT(isWritten == true);
        mOffset += sizeof(uint32);
    }

    void WriteUint64(uint64 value) noexcept
    {
        [[maybe_unused]] const bool isWritten = foundation::TryWriteLittleEndian(value, mBytes.subspan(mOffset));
        LUDUS_ASSERT(isWritten == true);
        mOffset += sizeof(uint64);
    }

    void WriteFloat32(float32 value) noexcept
    {
        // Positive and negative zero describe the same transform; hash one spelling.
        const float32 canonical = value == 0 ? 0.0F : value;
        WriteUint32(std::bit_cast<uint32>(canonical));
    }

    [[nodiscard]] usize GetOffset() const noexcept
    {
        return mOffset;
    }

private:
    std::span<uint8> mBytes;
    usize mOffset = 0;
};

bool IsPackageHierarchyShapeValid(foundation::BorrowedView<const SkeletonJointRecord> joints,
                                  uint32& outMaxDepth) noexcept
{
    if (joints[0].ParentIndex != NO_PARENT_JOINT)
    {
        return false;
    }
    // In depth-first order the parent of joint j is joint j - 1 or one of its
    // ancestors. Each hop below closes a subtree that never reopens, so the walk is
    // linear in the joint count overall. Depth follows the same hops.
    uint32 depth = 0;
    uint32 maxDepth = 0;
    for (usize index = 1; index < joints.GetSize(); ++index)
    {
        const uint32 parent = joints[index].ParentIndex;
        if (parent == NO_PARENT_JOINT || parent >= index)
        {
            return false;
        }
        uint32 ancestor = static_cast<uint32>(index - 1);
        while (ancestor != parent)
        {
            ancestor = joints[ancestor].ParentIndex;
            if (ancestor == NO_PARENT_JOINT)
            {
                return false;
            }
            --depth;
        }
        ++depth;
        maxDepth = depth > maxDepth ? depth : maxDepth;
    }
    outMaxDepth = maxDepth;
    return true;
}

AnimationStatus ValidateJointValues(foundation::BorrowedView<const SkeletonJointRecord> joints) noexcept
{
    for (const SkeletonJointRecord& joint : joints)
    {
        if (joint.JointId == 0)
        {
            return AnimationStatus::InvalidJointId;
        }
        const AnimationStatus status = detail::ValidateLocalTransform(joint.RestLocal);
        if (status != AnimationStatus::Success)
        {
            return status;
        }
    }
    return AnimationStatus::Success;
}

/// Derive subtree ends from the validated parent order and compare the cooked values.
bool AreSubtreeEndsValid(foundation::BorrowedView<const SkeletonJointRecord> joints,
                         foundation::Array<SkeletonJointRecord>& inoutCopied) noexcept
{
    const usize count = joints.GetSize();
    for (usize index = 0; index < count; ++index)
    {
        inoutCopied[index].SubtreeEnd = static_cast<uint32>(index + 1);
    }
    for (usize index = count - 1; index > 0; --index)
    {
        SkeletonJointRecord& parent = inoutCopied[inoutCopied[index].ParentIndex];
        const uint32 end = inoutCopied[index].SubtreeEnd;
        parent.SubtreeEnd = end > parent.SubtreeEnd ? end : parent.SubtreeEnd;
    }
    for (usize index = 0; index < count; ++index)
    {
        if (inoutCopied[index].SubtreeEnd != joints[index].SubtreeEnd)
        {
            return false;
        }
    }
    return true;
}

bool HasDuplicateJointId(foundation::Array<detail::JointIdEntry>& inoutSortedIds) noexcept
{
    foundation::Sort(inoutSortedIds.GetData(),
                     inoutSortedIds.GetSize(),
                     [](const detail::JointIdEntry& left, const detail::JointIdEntry& right) noexcept {
                         return left.JointId < right.JointId;
                     });
    for (usize index = 1; index < inoutSortedIds.GetSize(); ++index)
    {
        if (inoutSortedIds[index].JointId == inoutSortedIds[index - 1].JointId)
        {
            return true;
        }
    }
    return false;
}

bool TryComputeCompatibility(const detail::SkeletonStorage& storage,
                             foundation::Fingerprint128& outFingerprint) noexcept
{
    const usize count = storage.Joints.GetSize();
    usize jointBytes = 0;
    usize totalBytes = 0;
    if (foundation::TryMultiply(count, COMPATIBILITY_JOINT_BYTES, jointBytes) == false ||
        foundation::TryAdd(jointBytes, COMPATIBILITY_HEADER_BYTES, totalBytes) == false)
    {
        return false;
    }
    foundation::Array<uint8> bytes;
    if (detail::TryAllocateArray(totalBytes, bytes) == false)
    {
        return false;
    }
    CanonicalByteWriter writer({bytes.GetData(), bytes.GetSize()});
    writer.WriteUint32(SKELETON_COMPATIBILITY_VERSION);
    writer.WriteUint32(static_cast<uint32>(count));
    writer.WriteFloat32(storage.MetersPerUnit);
    for (const SkeletonJointRecord& joint : storage.Joints)
    {
        writer.WriteUint64(joint.JointId);
        writer.WriteUint32(joint.ParentIndex);
        writer.WriteUint32(joint.SubtreeEnd);
        const math::TransformTRS& rest = joint.RestLocal;
        writer.WriteFloat32(rest.Translation.X);
        writer.WriteFloat32(rest.Translation.Y);
        writer.WriteFloat32(rest.Translation.Z);
        writer.WriteFloat32(rest.Rotation.X);
        writer.WriteFloat32(rest.Rotation.Y);
        writer.WriteFloat32(rest.Rotation.Z);
        writer.WriteFloat32(rest.Rotation.W);
        writer.WriteFloat32(rest.Scale.X);
        writer.WriteFloat32(rest.Scale.Y);
        writer.WriteFloat32(rest.Scale.Z);
    }
    LUDUS_ASSERT(writer.GetOffset() == totalBytes);
    outFingerprint = foundation::StableFingerprint128({bytes.GetData(), bytes.GetSize()});
    return true;
}
} // namespace

namespace detail
{
AnimationStatus ValidateLocalTransform(const math::TransformTRS& value) noexcept
{
    if (math::IsFinite(value.Translation) == false || math::IsFinite(value.Rotation) == false ||
        math::Abs(math::Dot(value.Rotation, value.Rotation) - 1.0F) > JOINT_ROTATION_UNIT_TOLERANCE ||
        math::IsFinite(value.Scale) == false)
    {
        return AnimationStatus::InvalidTransform;
    }
    const math::Vector3 scale = value.Scale;
    if (scale.X < MIN_JOINT_SCALE || scale.Y < MIN_JOINT_SCALE || scale.Z < MIN_JOINT_SCALE ||
        scale.X > MAX_JOINT_SCALE || scale.Y > MAX_JOINT_SCALE || scale.Z > MAX_JOINT_SCALE)
    {
        return AnimationStatus::UnsupportedScale;
    }
    return AnimationStatus::Success;
}

math::TransformTRS NormalizeLocalTransform(const math::TransformTRS& value) noexcept
{
    math::TransformTRS normalized = value;
    [[maybe_unused]] const math::MathStatus status = math::TryNormalize(value.Rotation, normalized.Rotation);
    LUDUS_ASSERT(status == math::MathStatus::Success);
    return normalized;
}
} // namespace detail

Skeleton::Skeleton() noexcept = default;
Skeleton::~Skeleton() noexcept = default;
Skeleton::Skeleton(Skeleton&& other) noexcept = default;
Skeleton& Skeleton::operator=(Skeleton&& other) noexcept = default;

bool Skeleton::IsPrepared() const noexcept
{
    return mStorage.Get() != nullptr;
}

uint32 Skeleton::GetJointCount() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : static_cast<uint32>(mStorage->Joints.GetSize());
}

SkeletonIdentity Skeleton::GetIdentity() const noexcept
{
    return mStorage.Get() == nullptr ? SkeletonIdentity{} : mStorage->Identity;
}

float32 Skeleton::GetMetersPerUnit() const noexcept
{
    return mStorage.Get() == nullptr ? 0.0F : mStorage->MetersPerUnit;
}

uint32 Skeleton::GetMaxDepth() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->MaxDepth;
}

usize Skeleton::GetPreparedBytes() const noexcept
{
    return mStorage.Get() == nullptr ? 0 : mStorage->PreparedBytes;
}

foundation::Fingerprint128 Skeleton::GetCompatibility() const noexcept
{
    return mStorage.Get() == nullptr ? foundation::Fingerprint128{} : mStorage->Compatibility;
}

foundation::BorrowedView<const SkeletonJointRecord> Skeleton::GetJoints() const noexcept
{
    if (mStorage.Get() == nullptr)
    {
        return {};
    }
    return {mStorage->Joints.GetData(), mStorage->Joints.GetSize()};
}

AnimationStatus Skeleton::TryFindJointIndex(uint64 jointId, uint32& outJointIndex) const noexcept
{
    if (mStorage.Get() == nullptr)
    {
        return AnimationStatus::InvalidArgument;
    }
    const foundation::Array<detail::JointIdEntry>& entries = mStorage->SortedIds;
    usize first = 0;
    usize end = entries.GetSize();
    while (first < end)
    {
        const usize middle = first + (end - first) / 2;
        if (entries[middle].JointId < jointId)
        {
            first = middle + 1;
        }
        else
        {
            end = middle;
        }
    }
    if (first == entries.GetSize() || entries[first].JointId != jointId)
    {
        return AnimationStatus::InvalidJointId;
    }
    outJointIndex = entries[first].JointIndex;
    return AnimationStatus::Success;
}

AnimationStatus
TryPrepareSkeleton(const SkeletonPackage& package, const SkeletonLimits& limits, Skeleton& outSkeleton) noexcept
{
    const foundation::BorrowedView<const SkeletonJointRecord> joints = package.Joints;
    if (joints.IsValid() == false || joints.IsEmpty() == true || limits.MaxJoints == 0 ||
        limits.MaxJoints > MAX_SKELETON_JOINTS || package.Identity.AuthoringId == 0 ||
        math::IsFinite(package.MetersPerUnit) == false || package.MetersPerUnit <= 0)
    {
        return AnimationStatus::InvalidArgument;
    }
    if (joints.GetSize() > limits.MaxJoints)
    {
        return AnimationStatus::BudgetExceeded;
    }
    uint32 maxDepth = 0;
    if (IsPackageHierarchyShapeValid(joints, maxDepth) == false)
    {
        return AnimationStatus::InvalidHierarchy;
    }
    const AnimationStatus valueStatus = ValidateJointValues(joints);
    if (valueStatus != AnimationStatus::Success)
    {
        return valueStatus;
    }

    detail::SkeletonStorage* storageOrNull = new (std::nothrow) detail::SkeletonStorage;
    if (storageOrNull == nullptr)
    {
        return AnimationStatus::OutOfMemory;
    }
    foundation::UniquePtr<detail::SkeletonStorage> candidate(storageOrNull);
    const usize count = joints.GetSize();
    if (detail::TryAllocateArray(count, candidate->Joints) == false ||
        detail::TryAllocateArray(count, candidate->SortedIds) == false)
    {
        return AnimationStatus::OutOfMemory;
    }
    for (usize index = 0; index < count; ++index)
    {
        SkeletonJointRecord& copied = candidate->Joints[index];
        copied = joints[index];
        copied.RestLocal = detail::NormalizeLocalTransform(joints[index].RestLocal);
        candidate->SortedIds[index] = { .JointId = joints[index].JointId, .JointIndex = static_cast<uint32>(index) };
    }
    if (AreSubtreeEndsValid(joints, candidate->Joints) == false)
    {
        return AnimationStatus::InvalidHierarchy;
    }
    if (HasDuplicateJointId(candidate->SortedIds) == true)
    {
        return AnimationStatus::InvalidJointId;
    }
    candidate->Identity = package.Identity;
    candidate->MetersPerUnit = package.MetersPerUnit;
    candidate->MaxDepth = maxDepth;
    // Joint storage cannot overflow: count <= MAX_SKELETON_JOINTS and both arrays were allocated.
    candidate->PreparedBytes =
        sizeof(detail::SkeletonStorage) + count * sizeof(SkeletonJointRecord) + count * sizeof(detail::JointIdEntry);
    if (TryComputeCompatibility(*candidate, candidate->Compatibility) == false)
    {
        return AnimationStatus::OutOfMemory;
    }
    outSkeleton.mStorage = foundation::Move(candidate);
    return AnimationStatus::Success;
}
} // namespace ludus::animation
