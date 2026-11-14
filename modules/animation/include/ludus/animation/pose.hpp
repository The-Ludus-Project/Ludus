#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/animation/skeleton.hpp>
#include <ludus/animation/status.hpp>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/transform.hpp>

namespace ludus::animation
{
namespace detail
{
struct LocalPoseStorage;
struct ComponentPoseStorage;
} // namespace detail

/// @brief Dense, canonical parent-local pose in skeleton joint order.
///
/// Values are translation, unit rotation and supported positive scale; edits are validated at
/// TrySetJoint, the controlled boundary that normalizes rotations. Each accepted edit increments
/// GetRevision so derived component poses can report staleness. Move-only; mutation needs one owner.
class LocalPose final
{
public:
    LocalPose() noexcept;                             ///< Construct unprepared storage without allocation.
    ~LocalPose() noexcept;                            ///< Release the joint storage.
    LocalPose(LocalPose&& other) noexcept;            ///< Transfer ownership; other becomes unprepared.
    LocalPose& operator=(LocalPose&& other) noexcept; ///< Release this pose and take other's.
    LocalPose(const LocalPose&) = delete;             ///< Poses are copied explicitly by their owner.
    LocalPose& operator=(const LocalPose&) = delete;  ///< Poses are copied explicitly by their owner.

    [[nodiscard]] bool IsPrepared() const noexcept;                  ///< Whether joint storage is owned.
    [[nodiscard]] foundation::uint32 GetJointCount() const noexcept; ///< Joint count, or zero when unprepared.
    /// Compatibility identity of the skeleton this pose was prepared for; zero when unprepared.
    [[nodiscard]] foundation::Fingerprint128 GetCompatibility() const noexcept;
    /// Accepted-edit counter: 1 after preparation, incremented by each successful edit/reset.
    [[nodiscard]] foundation::uint64 GetRevision() const noexcept;
    /// Local joint values in skeleton order; borrowed until the next edit, move or destruction.
    [[nodiscard]] foundation::BorrowedView<const foundation::math::TransformTRS> GetJoints() const noexcept;

    /// @brief Replace one joint's parent-local value.
    /// @param jointIndex Index below GetJointCount.
    /// @param value Finite translation, rotation within JOINT_ROTATION_UNIT_TOLERANCE (normalized on
    ///              acceptance) and componentwise scale in [MIN_JOINT_SCALE, MAX_JOINT_SCALE].
    /// @return Success, InvalidArgument (unprepared or index), InvalidTransform or UnsupportedScale.
    ///         The pose and revision are unchanged on failure. Allocation-free.
    [[nodiscard]] AnimationStatus TrySetJoint(foundation::uint32 jointIndex,
                                              const foundation::math::TransformTRS& value) noexcept;

    /// @brief Restore every joint to the skeleton's rest pose.
    /// @param skeleton Prepared skeleton with this pose's compatibility identity.
    /// @return Success, InvalidArgument or IncompatibleSkeleton; unchanged on failure. Allocation-free.
    [[nodiscard]] AnimationStatus TryResetToRest(const Skeleton& skeleton) noexcept;

private:
    friend AnimationStatus TryPrepareLocalPose(const Skeleton& skeleton, LocalPose& outPose) noexcept;
    foundation::UniquePtr<detail::LocalPoseStorage> mStorage;
};

/// @brief Allocate a local pose for a skeleton and initialize it to the skeleton's rest pose.
/// @param skeleton Prepared skeleton; only its compatibility identity is retained, not a reference.
/// @param outPose Replaced only on Success.
/// @return Success, InvalidArgument (unprepared skeleton) or OutOfMemory.
[[nodiscard]] AnimationStatus TryPrepareLocalPose(const Skeleton& skeleton, LocalPose& outPose) noexcept;

/// @brief Derived component-space pose: each joint's transform from joint space to the skeleton root's parent.
///
/// Component values are FoundationMath Affine3 transforms, so nonuniform parent scale composed
/// with child rotation keeps its shear; they are never decomposed back to TRS. Storage holds a
/// published buffer and a private candidate of equal size, both allocated at preparation:
/// evaluation writes the candidate and publishes it only after the whole pose is finite.
class ComponentPose final
{
public:
    ComponentPose() noexcept;                                 ///< Construct unprepared storage without allocation.
    ~ComponentPose() noexcept;                                ///< Release both buffers.
    ComponentPose(ComponentPose&& other) noexcept;            ///< Transfer ownership; other becomes unprepared.
    ComponentPose& operator=(ComponentPose&& other) noexcept; ///< Release this pose and take other's.
    ComponentPose(const ComponentPose&) = delete;             ///< Poses are copied explicitly by their owner.
    ComponentPose& operator=(const ComponentPose&) = delete;  ///< Poses are copied explicitly by their owner.

    [[nodiscard]] bool IsPrepared() const noexcept;                  ///< Whether buffers are owned.
    [[nodiscard]] bool IsEvaluated() const noexcept;                 ///< Whether a published evaluation exists.
    [[nodiscard]] foundation::uint32 GetJointCount() const noexcept; ///< Joint count, or zero when unprepared.
    /// Compatibility identity of the skeleton this pose was prepared for; zero when unprepared.
    [[nodiscard]] foundation::Fingerprint128 GetCompatibility() const noexcept;
    /// LocalPose::GetRevision of the published evaluation's source; zero when not evaluated.
    [[nodiscard]] foundation::uint64 GetSourceRevision() const noexcept;

    /// @brief Published component transforms in skeleton order.
    ///
    /// Empty until a successful evaluation; uncomputed values are never exposed as current joints.
    /// Borrowed until the next successful evaluation, move or destruction.
    [[nodiscard]] foundation::BorrowedView<const foundation::math::Affine3> GetJoints() const noexcept;

private:
    friend AnimationStatus TryPrepareComponentPose(const Skeleton& skeleton, ComponentPose& outPose) noexcept;
    friend AnimationStatus
    TryEvaluateComponentPose(const Skeleton& skeleton, const LocalPose& localPose, ComponentPose& inoutPose) noexcept;
    foundation::UniquePtr<detail::ComponentPoseStorage> mStorage;
};

/// @brief Allocate published and candidate component buffers for a skeleton.
/// @param skeleton Prepared skeleton; only its compatibility identity and joint count are retained.
/// @param outPose Replaced only on Success; the new pose is not evaluated.
/// @return Success, InvalidArgument (unprepared skeleton) or OutOfMemory.
[[nodiscard]] AnimationStatus TryPrepareComponentPose(const Skeleton& skeleton, ComponentPose& outPose) noexcept;

/// @brief Scalar forward kinematics from a local pose.
///
/// For joint j: component[root] = ToAffine3(local[root]) and
/// component[j] = Compose(component[parent[j]], ToAffine3(local[j])), using FoundationMath
/// affine composition in skeleton order. The whole pose is evaluated, written to the private
/// candidate, checked for finiteness and then published.
/// @param skeleton Prepared skeleton providing the hierarchy.
/// @param localPose Prepared local pose for the same compatibility identity.
/// @param inoutPose Prepared component pose for the same compatibility identity. On failure the
///                  published transforms, evaluation state and source revision are unchanged; only
///                  the private candidate buffer may change.
/// @return Success, InvalidArgument, IncompatibleSkeleton, InvalidTransform, UnsupportedScale or
///         OutOfRange. Allocation-free and noexcept; no thread affinity.
[[nodiscard]] AnimationStatus
TryEvaluateComponentPose(const Skeleton& skeleton, const LocalPose& localPose, ComponentPose& inoutPose) noexcept;
} // namespace ludus::animation
