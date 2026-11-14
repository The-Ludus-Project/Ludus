#pragma once

// Private owned storage for Animation's public owners, shared by the module's
// translation units. Never installed.

#include <ludus/foundation/base/types.h>

#include <ludus/animation/skeleton.hpp>
#include <ludus/animation/status.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/transform.hpp>

namespace ludus::animation::detail
{
/// One entry of the skeleton's identity index, sorted by JointId.
struct JointIdEntry final
{
    foundation::uint64 JointId = 0;
    foundation::uint32 JointIndex = 0;
};

struct SkeletonStorage final
{
    foundation::Array<SkeletonJointRecord> Joints;
    foundation::Array<JointIdEntry> SortedIds;
    SkeletonIdentity Identity{};
    foundation::Fingerprint128 Compatibility{};
    foundation::float32 MetersPerUnit = 0;
    foundation::uint32 MaxDepth = 0;
    foundation::usize PreparedBytes = 0;
};

struct LocalPoseStorage final
{
    foundation::Array<foundation::math::TransformTRS> Joints;
    foundation::Fingerprint128 Compatibility{};
    foundation::uint64 Revision = 0;
};

struct ComponentPoseStorage final
{
    foundation::Array<foundation::math::Affine3> Published;
    foundation::Array<foundation::math::Affine3> Candidate;
    foundation::Fingerprint128 Compatibility{};
    foundation::uint64 SourceRevision = 0;
    bool IsEvaluated = false;
};

struct SkinBindingStorage final
{
    foundation::Array<foundation::uint32> JointIndices;
    foundation::Array<foundation::math::Affine3> InverseBinds;
    foundation::Fingerprint128 Compatibility{};
};

/// Validate a parent-local joint value: finite translation, rotation within
/// JOINT_ROTATION_UNIT_TOLERANCE, componentwise scale in [MIN_JOINT_SCALE, MAX_JOINT_SCALE].
[[nodiscard]] AnimationStatus ValidateLocalTransform(const foundation::math::TransformTRS& value) noexcept;

/// Normalize the rotation of a value accepted by ValidateLocalTransform.
[[nodiscard]] foundation::math::TransformTRS
NormalizeLocalTransform(const foundation::math::TransformTRS& value) noexcept;

/// Exact-capacity fallible array allocation; leaves an empty array on failure.
template <typename ElementType>
[[nodiscard]] bool TryAllocateArray(foundation::usize count, foundation::Array<ElementType>& outElements) noexcept
{
    // Reserving first uses exact capacity, avoiding geometric growth beyond the request.
    return outElements.TryEnsureCapacity(count) == true && outElements.TryResize(count) == true;
}
} // namespace ludus::animation::detail
