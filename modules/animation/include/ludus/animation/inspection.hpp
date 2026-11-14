#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/animation/pose.hpp>
#include <ludus/animation/skeleton.hpp>
#include <ludus/animation/status.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/transform.hpp>

namespace ludus::animation
{
/// @brief Headless summary of a validated skeleton generation.
///
/// Copied values only; it borrows nothing and remains valid after the skeleton is released.
struct SkeletonInspection final
{
    SkeletonIdentity Identity{};                ///< Authoring identity and revision.
    foundation::Fingerprint128 Compatibility{}; ///< Structural compatibility identity.
    foundation::float32 MetersPerUnit = 0;      ///< Package translation units.
    foundation::uint32 JointCount = 0;          ///< Joint count, including any synthetic root.
    foundation::uint32 LeafCount = 0;           ///< Joints without children.
    foundation::uint32 MaxDepth = 0;            ///< Maximum root-to-leaf edge count.
    foundation::usize PreparedBytes = 0;        ///< Owned joint payload bytes, excluding allocator metadata.
};

/// @brief Headless selected-joint inspector record for a validated skeleton and its poses.
///
/// Shows the joint's place in the hierarchy, its rest value, the current local value and, when a
/// component pose is supplied and evaluated, the published component transform with its source
/// revision. Copied values only.
struct JointInspection final
{
    foundation::uint32 JointIndex = 0;                ///< Inspected joint index in skeleton order.
    foundation::uint64 JointId = 0;                   ///< Stable joint identity.
    foundation::uint32 ParentIndex = NO_PARENT_JOINT; ///< Parent index, or NO_PARENT_JOINT for the root.
    foundation::uint64 ParentJointId = 0;             ///< Parent identity, or zero for the root.
    foundation::uint32 SubtreeEnd = 0;                ///< Exclusive end of the contiguous subtree.
    foundation::uint32 Depth = 0;                     ///< Edge count from the root.
    foundation::uint32 ChildCount = 0;                ///< Direct children.
    foundation::math::TransformTRS RestLocal{};       ///< Skeleton rest value; never a mesh bind pose.
    foundation::math::TransformTRS CurrentLocal{};    ///< Local pose value.
    foundation::uint64 LocalRevision = 0;             ///< LocalPose::GetRevision at inspection.
    bool HasComponent = false;                        ///< Whether Component holds a published transform.
    /// Whether the component pose was evaluated from the inspected local revision; false when absent.
    bool IsComponentCurrent = false;
    foundation::math::Affine3 Component{};          ///< Published component transform when HasComponent.
    foundation::uint64 ComponentSourceRevision = 0; ///< Local revision the component pose was evaluated from.
};

/// @brief Summarize a validated skeleton without allocation.
/// @param skeleton Prepared skeleton.
/// @param outInspection Replaced only on Success.
/// @return Success or InvalidArgument (unprepared).
[[nodiscard]] AnimationStatus TryInspectSkeleton(const Skeleton& skeleton, SkeletonInspection& outInspection) noexcept;

/// @brief Inspect one joint of a skeleton, its local pose and an optional component pose.
///
/// Reading never evaluates or edits either pose, so inspection cannot advance pose history.
/// @param skeleton Prepared skeleton.
/// @param localPose Prepared local pose for the skeleton's compatibility identity.
/// @param componentPoseOrNull Optional component pose for the same identity; an unevaluated pose
///                            reports HasComponent false.
/// @param jointIndex Index below the skeleton joint count.
/// @param outInspection Replaced only on Success.
/// @return Success, InvalidArgument (unprepared input or index) or IncompatibleSkeleton. Allocation-free.
[[nodiscard]] AnimationStatus TryInspectJoint(const Skeleton& skeleton,
                                              const LocalPose& localPose,
                                              const ComponentPose* componentPoseOrNull,
                                              foundation::uint32 jointIndex,
                                              JointInspection& outInspection) noexcept;
} // namespace ludus::animation
