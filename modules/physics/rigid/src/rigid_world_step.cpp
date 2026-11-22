// One RigidWorld tick: preflight, Commit, Simulate and Publish.
//
// Dynamic bodies use semi-implicit Euler on the COM pose with an implicit gyroscopic term. Thanks to Erin Catto,
// "Numerical Methods", Game Developers Conference 2015, section "Gyroscopic Motion",
// https://box2d.org/files/ErinCatto_NumericalMethods_GDC2015.pdf, for solving the implicit gyroscopic equation in
// the principal frame with Newton's method. Ludus departs from the talk's single backward-Euler Newton step: it
// iterates the implicit midpoint form to convergence, because the backward-Euler step lost 31% of a fast asymmetric
// top's kinetic energy in 10 s, while the midpoint form preserves energy and |L|. Orientations turn by the exact
// rotation of the substep's constant angular velocity.

#include "internal/rigid_world_contacts.h"
#include "internal/rigid_world_motion.h"

#include "internal/rigid_math.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::rigid
{
namespace
{
namespace math = foundation::math;
using internal::Matrix3x3;
using internal::Rotation;

/// Most Newton iterations of the gyroscopic solve; each substep converges quadratically within a few.
constexpr uint32 MAX_GYROSCOPIC_ITERATIONS = 8;
/// Newton correction, relative to the angular speed, at which the gyroscopic solve stops.
constexpr float64 GYROSCOPIC_RELATIVE_TOLERANCE = 1.0e-14;
} // namespace

Rotation RigidWorldMotion::GetBodyRotation(const BodySlot& slot) noexcept
{
    return internal::Normalize(internal::Multiply(ToRotation(slot.CenterOfMassOrientationWorld),
                                                  internal::Conjugate(ToRotation(slot.BodyFromCenterOfMass))));
}

collision::RigidPose RigidWorldMotion::ComputeBodyPose(const BodySlot& slot) noexcept
{
    const Rotation body = GetBodyRotation(slot);
    return
    {
        .PositionWorld = slot.CenterOfMassWorld - internal::Rotate(body, slot.CenterOfMassBody),
        .OrientationWorld = internal::Narrow(body),
    };
}

void RigidWorldMotion::PlaceCenterOfMass(BodySlot& inoutSlot, const collision::RigidPose& pose) noexcept
{
    const Rotation body = internal::WidenNormalized(pose.OrientationWorld);
    inoutSlot.CenterOfMassWorld = pose.PositionWorld + internal::Rotate(body, inoutSlot.CenterOfMassBody);
    inoutSlot.CenterOfMassOrientationWorld =
        ToValue(internal::Normalize(internal::Multiply(body, ToRotation(inoutSlot.BodyFromCenterOfMass))));
}

math::Vector3d RigidWorldMotion::ApplyInverseInertia(const BodySlot& slot, math::Vector3d value) noexcept
{
    if (slot.Motion != MotionType::Dynamic)
    {
        return {};
    }
    const Rotation orientation = ToRotation(slot.CenterOfMassOrientationWorld);
    const math::Vector3d valueCom = internal::Rotate(internal::Conjugate(orientation), value);
    const math::Vector3d resultCom{valueCom.X / slot.PrincipalMoments.X,
                                   valueCom.Y / slot.PrincipalMoments.Y,
                                   valueCom.Z / slot.PrincipalMoments.Z};
    return internal::Rotate(orientation, resultCom);
}

BodyState RigidWorldMotion::MakeState(const RigidWorld& world, uint32 slotIndex) noexcept
{
    const BodySlot& slot = world.mBodies[slotIndex];
    BodyState state
    {
        .Body = { .Owner = world.mOwner, .Slot = slotIndex, .Generation = slot.Generation },
        .Binding = slot.Binding,
        .Motion = slot.Motion,
        .Pose = slot.Pose,
        .PreviousPose = slot.PreviousPose,
        .CenterOfMassWorld = slot.CenterOfMassWorld,
        .LinearVelocityWorld = slot.LinearVelocityWorld,
        .AngularVelocityWorld = slot.AngularVelocityWorld,
    };
    if (slot.Motion == MotionType::Dynamic)
    {
        const Rotation orientation = ToRotation(slot.CenterOfMassOrientationWorld);
        const math::Vector3d angularCom = internal::Rotate(internal::Conjugate(orientation), slot.AngularVelocityWorld);
        const math::Vector3d momentumCom = math::Hadamard(slot.PrincipalMoments, angularCom);
        state.KineticEnergyJoules = 0.5 * (slot.MassKilograms * math::LengthSquared(slot.LinearVelocityWorld) +
                                           math::Dot(angularCom, momentumCom));
        state.AngularMomentumWorld = internal::Rotate(orientation, momentumCom);
    }
    return state;
}

Status RigidWorldMotion::PoseColliders(RigidWorld& inoutWorld, const BodySlot& slot) noexcept
{
    const internal::Rotation body = internal::WidenNormalized(slot.Pose.OrientationWorld);
    uint32 colliderSlot = slot.FirstCollider;
    for (uint32 index = 0; index < slot.ColliderCount; ++index)
    {
        const RigidWorld::Attachment& attachment = inoutWorld.mAttachments[colliderSlot];
        const internal::Rotation local = internal::WidenNormalized(attachment.Pose.OrientationBody);
        const collision::RigidPose pose
        {
            .PositionWorld = slot.Pose.PositionWorld + internal::Rotate(body, attachment.Pose.PositionBody),
            .OrientationWorld = internal::Narrow(internal::Normalize(internal::Multiply(body, local))),
        };
        const collision::Status moved = inoutWorld.mCollision.TrySetColliderPose(attachment.Collider, pose);
        if (moved != collision::Status::Success)
        {
            return ToRigidStatus(moved);
        }
        colliderSlot = attachment.Next;
    }
    return Status::Success;
}

namespace
{
using BodySlot = RigidWorldMotion::BodySlot;

/// Express a command vector in world axes at the body's application pose.
math::Vector3d ToWorldAxes(CommandFrame frame, const collision::RigidPose& pose, math::Vector3d value) noexcept
{
    return frame == CommandFrame::World ? value
                                        : internal::Rotate(internal::WidenNormalized(pose.OrientationWorld), value);
}

/// Express a command point as a world position at the body's application pose.
math::Vector3d ToWorldPoint(CommandFrame frame, const collision::RigidPose& pose, math::Vector3d point) noexcept
{
    return frame == CommandFrame::World
               ? point
               : pose.PositionWorld + internal::Rotate(internal::WidenNormalized(pose.OrientationWorld), point);
}

/// Whether a slot's motion state is finite and its COM inside the coordinate envelope.
FaultReason CheckState(const BodySlot& slot) noexcept
{
    const RigidWorldMotion::RotationValue& orientation = slot.CenterOfMassOrientationWorld;
    const bool isFinite = math::IsFinite(slot.CenterOfMassWorld) == true &&
                          math::IsFinite(slot.LinearVelocityWorld) == true &&
                          math::IsFinite(slot.AngularVelocityWorld) == true && math::IsFinite(orientation.X) == true &&
                          math::IsFinite(orientation.Y) == true && math::IsFinite(orientation.Z) == true &&
                          math::IsFinite(orientation.W) == true;
    if (isFinite == false)
    {
        return FaultReason::NonFiniteState;
    }
    return internal::ValidatePosition(slot.CenterOfMassWorld) == Status::Success ? FaultReason::None
                                                                                 : FaultReason::LeftCoordinateEnvelope;
}

/// Angular velocity after one substep of explicit torque and the implicit-midpoint gyroscopic term, in the COM frame.
///
/// Solves `I (w2 - w1) + h wm x (I wm) = 0` with `wm = (w1 + w2) / 2` by Newton's method from `w2 = w1`. The
/// implicit midpoint rule preserves the quadratic invariants of the torque-free Euler equations, kinetic energy and
/// the magnitude of angular momentum, up to the Newton tolerance.
math::Vector3d IntegrateAngularVelocity(const BodySlot& slot,
                                        math::Vector3d angularCom,
                                        math::Vector3d torqueCom,
                                        float64 seconds) noexcept
{
    const math::Vector3d moments = slot.PrincipalMoments;
    const math::Vector3d driven =
        angularCom +
        math::Vector3d{torqueCom.X / moments.X, torqueCom.Y / moments.Y, torqueCom.Z / moments.Z} * seconds;
    const Matrix3x3 inertia = internal::MakeDiagonal(moments);
    const float64 tolerance = GYROSCOPIC_RELATIVE_TOLERANCE * internal::GetMaxMagnitude(driven);
    math::Vector3d next = driven;
    for (uint32 iteration = 0; iteration < MAX_GYROSCOPIC_ITERATIONS; ++iteration)
    {
        const math::Vector3d midpoint = (driven + next) * 0.5;
        const math::Vector3d momentum = math::Hadamard(moments, midpoint);
        const math::Vector3d residual =
            math::Hadamard(moments, next - driven) + math::Cross(midpoint, momentum) * seconds;
        // d/dw2 of the residual: I + h / 2 (skew(wm) I - skew(I wm)).
        const Matrix3x3 jacobian =
            internal::Add(inertia,
                          internal::Scale(internal::Add(internal::Multiply(internal::MakeSkew(midpoint), inertia),
                                                        internal::Scale(internal::MakeSkew(momentum), -1.0)),
                                          0.5 * seconds));
        math::Vector3d correction{};
        if (internal::TrySolve(jacobian, residual, correction) == false)
        {
            break;
        }
        next = next - correction;
        if (internal::GetMaxMagnitude(correction) <= tolerance)
        {
            break;
        }
    }
    return next;
}

void IntegrateDynamicSubstep(BodySlot& inoutSlot, math::Vector3d gravityWorld, float64 seconds) noexcept
{
    const math::Vector3d acceleration =
        gravityWorld * inoutSlot.GravityScale + inoutSlot.ForceWorld * inoutSlot.InverseMass;
    inoutSlot.LinearVelocityWorld = inoutSlot.LinearVelocityWorld + acceleration * seconds;
    const Rotation orientation = RigidWorldMotion::ToRotation(inoutSlot.CenterOfMassOrientationWorld);
    const Rotation inverse = internal::Conjugate(orientation);
    const math::Vector3d angularCom =
        IntegrateAngularVelocity(inoutSlot,
                                 internal::Rotate(inverse, inoutSlot.AngularVelocityWorld),
                                 internal::Rotate(inverse, inoutSlot.TorqueWorld),
                                 seconds);
    inoutSlot.AngularVelocityWorld = internal::Rotate(orientation, angularCom);
    inoutSlot.CenterOfMassWorld = inoutSlot.CenterOfMassWorld + inoutSlot.LinearVelocityWorld * seconds;
    inoutSlot.CenterOfMassOrientationWorld =
        RigidWorldMotion::ToValue(internal::IntegrateRotation(orientation, inoutSlot.AngularVelocityWorld, seconds));
}

void IntegrateKinematicSubstep(BodySlot& inoutSlot, float64 seconds) noexcept
{
    inoutSlot.CenterOfMassWorld = inoutSlot.CenterOfMassWorld + inoutSlot.LinearVelocityWorld * seconds;
    inoutSlot.CenterOfMassOrientationWorld = RigidWorldMotion::ToValue(
        internal::IntegrateRotation(RigidWorldMotion::ToRotation(inoutSlot.CenterOfMassOrientationWorld),
                                    inoutSlot.AngularVelocityWorld,
                                    seconds));
}
} // namespace

StepResult RigidWorldMotion::Step(RigidWorld& inoutWorld) noexcept
{
    RigidWorld& world = inoutWorld;
    StepResult result{ .Phase = world.mPhase };
    if (world.IsInitialized() == false)
    {
        result.Result = Status::InvalidPhase;
        return result;
    }
    if (world.mPhase == WorldPhase::Faulted)
    {
        result.Result = Status::Faulted;
        result.Fault = world.mFault;
        result.FaultBody = world.mFaultBody;
        return result;
    }
    if (world.mCollision.GetReaderCount() > 0)
    {
        result.Result = Status::InvalidPhase;
        return result;
    }
    // Every table was reserved at initialization with the admitted maxima: the results table holds as many entries
    // as the pending table, so nothing below can run out of storage.
    const uint64 tick = world.mNextTick;
    const float64 tickSeconds = world.mSettings.TickSeconds;

    // Detection comes before any mutation. It reads the poses the last tick published and writes only scratch, so
    // a tick whose contacts do not fit the reserved tables ends here with no pose, velocity, command or cached
    // contact changed. Commit changes velocities only, and contact geometry depends on poses alone.
    const bool hasContacts = world.mSettings.MaxContactPairs > 0;
    if (hasContacts == true)
    {
        world.mPhase = WorldPhase::Detect;
        const Status detection = RigidWorldContacts::Detect(world, tick);
        world.mPhase = WorldPhase::Idle;
        if (detection != Status::Success)
        {
            result.Result = detection;
            result.Phase = WorldPhase::Idle;
            if (detection == Status::CapacityExceeded)
            {
                result.ExhaustedTable = world.mContactDiagnostics.ExhaustedTable;
                result.RequiredCount = world.mContactDiagnostics.RequiredCount;
            }
            return result;
        }
    }

    world.mPhase = WorldPhase::Commit;
    for (BodySlot& slot : world.mBodies)
    {
        // A kinematic body that reached its target last tick rests there unless this tick moves it again.
        if (slot.IsLive == true && slot.IsFollowingTarget == true)
        {
            slot.LinearVelocityWorld = {};
            slot.AngularVelocityWorld = {};
            slot.IsFollowingTarget = false;
        }
    }
    usize committed = 0;
    usize resultCount = 0;
    while (committed < world.mPendingCount && world.mPending[committed].Key.TargetTick == tick)
    {
        const RigidWorld::PendingCommand& command = world.mPending[committed];
        ++committed;
        CommandResult& commandResult = world.mResults[resultCount];
        commandResult =
        {
            .Key = command.Key,
            .Kind = command.Kind,
            .Body = command.Body,
        };
        ++resultCount;
        BodySlot* slotOrNull = world.findLive(command.Body);
        if (slotOrNull == nullptr)
        {
            commandResult.Outcome = CommandOutcome::BodyDestroyed;
            ++result.UnappliedCommandCount;
            continue;
        }
        BodySlot& slot = *slotOrNull;
        ++result.AppliedCommandCount;
        switch (command.Kind)
        {
            case CommandKind::Force: {
                const math::Vector3d force = ToWorldAxes(command.Frame, slot.Pose, command.Linear);
                math::Vector3d torque = ToWorldAxes(command.Frame, slot.Pose, command.Angular);
                if (command.HasApplicationPoint == true)
                {
                    const math::Vector3d point = ToWorldPoint(command.Frame, slot.Pose, command.ApplicationPoint);
                    torque = torque + math::Cross(point - slot.CenterOfMassWorld, force);
                }
                slot.ForceWorld = slot.ForceWorld + force;
                slot.TorqueWorld = slot.TorqueWorld + torque;
                break;
            }
            case CommandKind::Impulse: {
                const math::Vector3d linear = ToWorldAxes(command.Frame, slot.Pose, command.Linear);
                math::Vector3d angular = ToWorldAxes(command.Frame, slot.Pose, command.Angular);
                if (command.HasApplicationPoint == true)
                {
                    const math::Vector3d point = ToWorldPoint(command.Frame, slot.Pose, command.ApplicationPoint);
                    angular = angular + math::Cross(point - slot.CenterOfMassWorld, linear);
                }
                slot.LinearVelocityWorld = slot.LinearVelocityWorld + linear * slot.InverseMass;
                slot.AngularVelocityWorld = slot.AngularVelocityWorld + ApplyInverseInertia(slot, angular);
                break;
            }
            case CommandKind::KinematicTarget: {
                if (slot.HasTarget == true)
                {
                    world.mResults[slot.TargetResult].Outcome = CommandOutcome::Superseded;
                    --result.AppliedCommandCount;
                    ++result.UnappliedCommandCount;
                }
                const Rotation target = internal::WidenNormalized(command.TargetPose.OrientationWorld);
                slot.TargetCenterOfMassWorld =
                    command.TargetPose.PositionWorld + internal::Rotate(target, slot.CenterOfMassBody);
                const Rotation targetCom =
                    internal::Normalize(internal::Multiply(target, ToRotation(slot.BodyFromCenterOfMass)));
                slot.TargetOrientationWorld = ToValue(targetCom);
                slot.LinearVelocityWorld =
                    (slot.TargetCenterOfMassWorld - slot.CenterOfMassWorld) * (1.0 / tickSeconds);
                slot.AngularVelocityWorld =
                    internal::GetShortestArcVelocity(ToRotation(slot.CenterOfMassOrientationWorld),
                                                     targetCom,
                                                     tickSeconds);
                slot.HasTarget = true;
                slot.TargetResult = static_cast<uint32>(resultCount - 1);
                break;
            }
        }
    }
    // Committed commands leave the pending table; the rest keep their order.
    for (usize index = committed; index < world.mPendingCount; ++index)
    {
        world.mPending[index - committed] = world.mPending[index];
    }
    world.mPendingCount -= committed;

    world.mPhase = WorldPhase::Simulate;
    const uint32 substepCount = world.mSettings.SubstepCount;
    const float64 substepSeconds = tickSeconds / static_cast<float64>(substepCount);
    for (uint32 slotIndex = 0; slotIndex < world.mBodies.GetSize(); ++slotIndex)
    {
        BodySlot& slot = world.mBodies[slotIndex];
        if (slot.IsLive == false || slot.Motion == MotionType::Static)
        {
            continue;
        }
        for (uint32 substep = 0; substep < substepCount; ++substep)
        {
            if (slot.Motion == MotionType::Dynamic)
            {
                IntegrateDynamicSubstep(slot, world.mSettings.GravityWorld, substepSeconds);
            }
            else
            {
                IntegrateKinematicSubstep(slot, substepSeconds);
            }
            const FaultReason fault = CheckState(slot);
            if (fault != FaultReason::None)
            {
                world.mPhase = WorldPhase::Faulted;
                world.mFault = fault;
                world.mFaultBody = { .Owner = world.mOwner, .Slot = slotIndex, .Generation = slot.Generation };
                world.mResultCount = 0;
                result.Result = Status::Faulted;
                result.Phase = WorldPhase::Faulted;
                result.Fault = fault;
                result.FaultBody = world.mFaultBody;
                return result;
            }
        }
    }

    world.mPhase = WorldPhase::Publish;
    for (uint32 slotIndex = 0; slotIndex < world.mBodies.GetSize(); ++slotIndex)
    {
        BodySlot& slot = world.mBodies[slotIndex];
        if (slot.IsLive == false)
        {
            continue;
        }
        slot.PreviousPose = slot.Pose;
        if (slot.Motion == MotionType::Static)
        {
            continue;
        }
        if (slot.HasTarget == true)
        {
            // The target defines the pose at the end of the tick; remove the substeps' rounding.
            slot.CenterOfMassWorld = slot.TargetCenterOfMassWorld;
            slot.CenterOfMassOrientationWorld = slot.TargetOrientationWorld;
            slot.HasTarget = false;
            slot.IsFollowingTarget = true;
        }
        slot.Pose = ComputeBodyPose(slot);
        slot.ForceWorld = {};
        slot.TorqueWorld = {};
        if (PoseColliders(world, slot) != Status::Success)
        {
            world.mPhase = WorldPhase::Faulted;
            world.mFault = FaultReason::ColliderUpdateFailed;
            world.mFaultBody = { .Owner = world.mOwner, .Slot = slotIndex, .Generation = slot.Generation };
            world.mResultCount = 0;
            result.Result = Status::Faulted;
            result.Phase = WorldPhase::Faulted;
            result.Fault = world.mFault;
            result.FaultBody = world.mFaultBody;
            return result;
        }
    }
    world.mResultCount = resultCount;
    if (hasContacts == true)
    {
        RigidWorldContacts::Publish(world, tick);
    }
    ++world.mNextTick;
    ++world.mEpoch;
    world.mPhase = WorldPhase::Idle;
    result.Result = Status::Success;
    result.Phase = WorldPhase::Idle;
    result.Tick = tick;
    return result;
}
} // namespace ludus::physics::rigid
