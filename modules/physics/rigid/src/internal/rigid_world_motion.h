#pragma once

#include "rigid_math.h"

#include <ludus/foundation/base/core.h>

#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/rigid_world.h>
#include <ludus/physics/rigid/status.h>

namespace ludus::physics::rigid
{
/// Private motion arithmetic and the Step phases of RigidWorld, kept out of the public header.
class RigidWorldMotion final
{
public:
    using BodySlot = RigidWorld::BodySlot;
    using RotationValue = RigidWorld::RotationValue;

    [[nodiscard]] static LUDUS_INLINE internal::Rotation ToRotation(const RotationValue& value) noexcept
    {
        return {value.X, value.Y, value.Z, value.W};
    }

    [[nodiscard]] static LUDUS_INLINE RotationValue ToValue(internal::Rotation rotation) noexcept
    {
        return {rotation.X, rotation.Y, rotation.Z, rotation.W};
    }

    /// Body-frame orientation of a slot in float64: COM orientation composed with the inverse BodyFromCenterOfMass.
    [[nodiscard]] static internal::Rotation GetBodyRotation(const BodySlot& slot) noexcept;

    /// Body frame of a slot, recovered from its COM state.
    [[nodiscard]] static collision::RigidPose ComputeBodyPose(const BodySlot& slot) noexcept;

    /// Set a slot's COM state from an admitted body-frame pose.
    static void PlaceCenterOfMass(BodySlot& inoutSlot, const collision::RigidPose& pose) noexcept;

    /// World inverse inertia applied to `value`; zero for bodies without mass.
    [[nodiscard]] static foundation::math::Vector3d ApplyInverseInertia(const BodySlot& slot,
                                                                        foundation::math::Vector3d value) noexcept;

    /// Plain-value state of a live slot.
    [[nodiscard]] static BodyState MakeState(const RigidWorld& world, uint32 slotIndex) noexcept;

    /// Move every attached collider of a slot to its body's published pose.
    [[nodiscard]] static Status PoseColliders(RigidWorld& inoutWorld, const BodySlot& slot) noexcept;

    /// One complete RigidWorld::Step.
    [[nodiscard]] static StepResult Step(RigidWorld& inoutWorld) noexcept;
};
} // namespace ludus::physics::rigid
