#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/animation/status.hpp>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/transform.hpp>

namespace ludus::animation
{
/// Parent index of the single root joint; never a valid joint index.
inline constexpr foundation::uint32 NO_PARENT_JOINT = 0xFFFFFFFFU;
/// Largest joint count representable with 32-bit joint indices and the NO_PARENT_JOINT sentinel.
inline constexpr foundation::usize MAX_SKELETON_JOINTS = NO_PARENT_JOINT;
/// Default admission count for one skeleton; callers may raise it up to MAX_SKELETON_JOINTS.
inline constexpr foundation::usize DEFAULT_SKELETON_MAX_JOINTS = 4096;
/// Largest absolute squared-norm error admitted before a joint rotation is normalized at a boundary.
inline constexpr foundation::float32 JOINT_ROTATION_UNIT_TOLERANCE = 1e-4F;
/// Smallest admitted joint scale component; zero, negative and near-singular scales are unsupported.
inline constexpr foundation::float32 MIN_JOINT_SCALE = 1e-4F;
/// Largest admitted joint scale component, keeping each local linear block well conditioned.
inline constexpr foundation::float32 MAX_JOINT_SCALE = 1e4F;
/// Version of the canonical byte layout hashed into a skeleton compatibility identity.
inline constexpr foundation::uint32 SKELETON_COMPATIBILITY_VERSION = 1;

/// @brief One cooked joint in flat depth-first, parent-before-child order.
///
/// Joint 0 is the single root; a multi-root import adds an explicit synthetic identity root.
/// RestLocal is the skeleton's rest pose: it is not a mesh bind pose, additive reference or
/// retarget pose. Transforms follow FoundationMath conventions (right-handed, +Y up, column
/// vectors, Hamilton (x, y, z, w) quaternions) in the package's units.
struct SkeletonJointRecord final
{
    foundation::uint64 JointId = 0;                   ///< Stable nonzero identity, unique within the skeleton.
    foundation::uint32 ParentIndex = NO_PARENT_JOINT; ///< Parent joint index, smaller than this index; root only: none.
    foundation::uint32 SubtreeEnd = 0;                ///< Exclusive end of this joint's contiguous subtree.
    foundation::math::TransformTRS RestLocal{};       ///< Parent-local rest transform: unit rotation, supported scale.
};

/// @brief Stable authoring identity and revision of one cooked skeleton.
///
/// The authoring identity survives recooks; Revision changes with content. Neither establishes
/// structural compatibility: use Skeleton::GetCompatibility for that.
struct SkeletonIdentity final
{
    foundation::uint64 AuthoringId = 0; ///< Nonzero stable authoring identity.
    foundation::uint64 Revision = 0;    ///< Content revision of this cook; any value.

    /// Compare both identity fields.
    [[nodiscard]] friend constexpr bool operator==(const SkeletonIdentity&, const SkeletonIdentity&) noexcept = default;
};

/// @brief Borrowed cooked skeleton package, validated by TryPrepareSkeleton.
///
/// The views need only remain valid until TryPrepareSkeleton returns.
struct SkeletonPackage final
{
    SkeletonIdentity Identity{};           ///< Authoring identity and revision recorded by the cook.
    foundation::float32 MetersPerUnit = 1; ///< Finite positive length of one translation unit in meters.
    /// Joints in depth-first, parent-before-child order.
    foundation::BorrowedView<const SkeletonJointRecord> Joints{nullptr, 0};
};

/// Per-skeleton preparation admission policy.
struct SkeletonLimits final
{
    foundation::usize MaxJoints = DEFAULT_SKELETON_MAX_JOINTS; ///< Positive joint count limit <= MAX_SKELETON_JOINTS.
};

namespace detail
{
struct SkeletonStorage;
}

/// @brief Immutable, move-only validated skeleton generation.
///
/// Owns a normalized copy of the joint records, a sorted joint-identity index and the skeleton
/// compatibility identity. Const reads are safe concurrently while the owner stays alive and
/// unmoved; replacement, move and destruction require external synchronization with readers.
class Skeleton final
{
public:
    Skeleton() noexcept;                            ///< Construct unprepared storage without allocation.
    ~Skeleton() noexcept;                           ///< Release the generation; no readers may remain.
    Skeleton(Skeleton&& other) noexcept;            ///< Transfer ownership; other becomes unprepared.
    Skeleton& operator=(Skeleton&& other) noexcept; ///< Release this generation and take other's.
    Skeleton(const Skeleton&) = delete;             ///< Generations are not implicitly copied.
    Skeleton& operator=(const Skeleton&) = delete;  ///< Generations are not implicitly copied.

    [[nodiscard]] bool IsPrepared() const noexcept;                  ///< Whether a validated generation is owned.
    [[nodiscard]] foundation::uint32 GetJointCount() const noexcept; ///< Joint count, or zero when unprepared.
    [[nodiscard]] SkeletonIdentity GetIdentity() const noexcept;     ///< Package identity, or zeros when unprepared.
    [[nodiscard]] foundation::float32 GetMetersPerUnit() const noexcept; ///< Package units, or zero when unprepared.
    /// Maximum root-to-leaf edge count (zero for a single joint), or zero when unprepared.
    [[nodiscard]] foundation::uint32 GetMaxDepth() const noexcept;
    /// Owned joint payload bytes, excluding allocator metadata; zero when unprepared.
    [[nodiscard]] foundation::usize GetPreparedBytes() const noexcept;

    /// @brief Structural compatibility identity.
    ///
    /// XXH3-128 (FoundationHash StableFingerprint128) over SKELETON_COMPATIBILITY_VERSION, units,
    /// joint order, identities, hierarchy and normalized rest transforms. The authoring identity
    /// and revision are excluded. Equal fingerprints are treated as compatible; a matching joint
    /// count alone never is. Zero when unprepared.
    [[nodiscard]] foundation::Fingerprint128 GetCompatibility() const noexcept;

    /// @brief Normalized joint records in skeleton order; empty when unprepared.
    ///
    /// Borrowed from this owner: valid until it is moved, replaced or destroyed.
    [[nodiscard]] foundation::BorrowedView<const SkeletonJointRecord> GetJoints() const noexcept;

    /// @brief Resolve a stable joint identity to its index without string or linear lookups.
    /// @param jointId Joint identity to find.
    /// @param outJointIndex Replaced only on Success.
    /// @return Success, InvalidArgument when unprepared, or InvalidJointId when absent.
    [[nodiscard]] AnimationStatus TryFindJointIndex(foundation::uint64 jointId,
                                                    foundation::uint32& outJointIndex) const noexcept;

private:
    friend AnimationStatus
    TryPrepareSkeleton(const SkeletonPackage& package, const SkeletonLimits& limits, Skeleton& outSkeleton) noexcept;
    foundation::UniquePtr<detail::SkeletonStorage> mStorage;
};

/// @brief Validate a cooked skeleton package and publish an immutable generation.
///
/// Rejects an empty or zero-identity package, nonpositive units, a hierarchy that is not one
/// depth-first parent-before-child tree with matching subtree ends, zero or duplicate joint
/// identities, nonfinite or nonunit rest values, and scales outside
/// [MIN_JOINT_SCALE, MAX_JOINT_SCALE]. Rotations within JOINT_ROTATION_UNIT_TOLERANCE are
/// normalized in the owned copy; the package is unchanged. Preparation may allocate.
/// @param package Borrowed package; its views need not survive a successful return.
/// @param limits Joint-count admission policy.
/// @param outSkeleton Replaced only on Success, preserving any previous generation on every failure.
/// @return Success, InvalidArgument, InvalidHierarchy, InvalidJointId, InvalidTransform,
///         UnsupportedScale, BudgetExceeded or OutOfMemory. No thread affinity.
[[nodiscard]] AnimationStatus
TryPrepareSkeleton(const SkeletonPackage& package, const SkeletonLimits& limits, Skeleton& outSkeleton) noexcept;
} // namespace ludus::animation
