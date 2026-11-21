#include <ludus/physics/rigid/rigid_world.h>

#include "internal/mass_frame.h"
#include "internal/rigid_math.h"
#include "internal/rigid_world_motion.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/identity/handle.hpp>
#include <ludus/foundation/identity/lifetime.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::rigid
{
namespace
{
namespace math = foundation::math;
using foundation::StaticArray;

Status ValidateWorldPose(const collision::RigidPose& pose) noexcept
{
    if (math::IsFinite(pose.PositionWorld) == false || math::IsFinite(pose.OrientationWorld) == false)
    {
        return Status::NonFiniteInput;
    }
    const Status position = internal::ValidatePosition(pose.PositionWorld);
    return position != Status::Success ? position : internal::ValidateOrientation(pose.OrientationWorld);
}

/// A COM velocity and an angular velocity, validated together.
struct BodyVelocity final
{
    math::Vector3d LinearWorld{};
    math::Vector3d AngularWorld{};
};

Status ValidateVelocities(const BodyVelocity& velocity) noexcept
{
    const Status linear = internal::ValidateBounded(velocity.LinearWorld, MAX_LINEAR_SPEED_METERS_PER_SECOND);
    return linear != Status::Success
               ? linear
               : internal::ValidateBounded(velocity.AngularWorld, MAX_ANGULAR_SPEED_RADIANS_PER_SECOND);
}

bool IsKnownMotion(MotionType motion) noexcept
{
    return motion == MotionType::Static || motion == MotionType::Kinematic || motion == MotionType::Dynamic;
}

bool IsKnownFrame(CommandFrame frame) noexcept
{
    return frame == CommandFrame::World || frame == CommandFrame::Body;
}

Status ValidateSettings(const RigidWorldSettings& settings) noexcept
{
    if (settings.MaxBodies == 0 || settings.MaxColliders == 0 || settings.MaxPendingCommands == 0 ||
        settings.SubstepCount == 0)
    {
        return Status::InvalidArgument;
    }
    if (settings.MaxBodies > MAX_RIGID_WORLD_BODIES ||
        settings.MaxColliders > collision::MAX_COLLISION_WORLD_COLLIDERS ||
        settings.MaxPendingCommands > MAX_PENDING_COMMANDS)
    {
        return Status::CapacityExceeded;
    }
    if (math::IsFinite(settings.TickSeconds) == false)
    {
        return Status::NonFiniteInput;
    }
    if (settings.TickSeconds < MIN_TICK_SECONDS || settings.TickSeconds > MAX_TICK_SECONDS ||
        settings.SubstepCount > MAX_SUBSTEP_COUNT)
    {
        return Status::OutOfRange;
    }
    return internal::ValidateBounded(settings.GravityWorld, MAX_GRAVITY_COMPONENT_METERS_PER_SECOND_SQUARED);
}

/// Whether a collider contributes to its dynamic body's composed mass.
bool IsMassSource(const BodyColliderDescription& collider) noexcept
{
    return collider.ShouldContributeMass == true && collider.Participation.IsSensor == false;
}

/// World placement of a collider attached at `localPose` to a body at `bodyPose`.
collision::RigidPose ComposePose(const collision::RigidPose& bodyPose, const BodyLocalPose& localPose) noexcept
{
    const internal::Rotation body = internal::WidenNormalized(bodyPose.OrientationWorld);
    const internal::Rotation local = internal::WidenNormalized(localPose.OrientationBody);
    return
    {
        .PositionWorld = bodyPose.PositionWorld + internal::Rotate(body, localPose.PositionBody),
        .OrientationWorld = internal::Narrow(internal::Normalize(internal::Multiply(body, local))),
    };
}

/// Validated colliders of a new body and, for a dynamic body composed from colliders, their mass parts.
struct ColliderPreparation final
{
    StaticArray<MassPart, MAX_BODY_COLLIDERS> Parts{};
    usize PartCount = 0;
};

Status PrepareColliders(const collision::GeometryStore& store,
                        const BodyDescription& description,
                        ColliderPreparation& outPreparation) noexcept
{
    const bool isComposed = description.Motion == MotionType::Dynamic && description.Mass == MassSource::Colliders;
    outPreparation.PartCount = 0;
    for (const BodyColliderDescription& collider : description.Colliders)
    {
        const Status pose = internal::ValidateLocalPose(collider.Pose);
        if (pose != Status::Success)
        {
            return pose;
        }
        collision::ShapeKind kind = collision::ShapeKind::TriangleMesh;
        if (store.TryGetKind(collider.Shape, kind) != collision::Status::Success)
        {
            return Status::InvalidHandle;
        }
        if ((collider.Participation.IsSolid == true && collider.Participation.IsSensor == true) ||
            collider.Filter.CategoryBits == 0)
        {
            return Status::InvalidArgument;
        }
        if (isComposed == false || IsMassSource(collider) == false)
        {
            continue;
        }
        // A triangle mesh is an open or unqualified surface, never a mass source.
        if (kind == collision::ShapeKind::TriangleMesh)
        {
            return Status::InvalidMassProperties;
        }
        collision::ConvexShape shape;
        const collision::Status resolved = store.TryGetConvexShape(collider.Shape, shape);
        LUDUS_ASSERT(resolved == collision::Status::Success);
        (void)resolved;
        MassPart& part = outPreparation.Parts[outPreparation.PartCount];
        const Status mass =
            TryComputeShapeMassProperties(shape, collider.DensityKilogramsPerCubicMeter, part.Properties);
        if (mass != Status::Success)
        {
            return mass;
        }
        part.Pose = collider.Pose;
        ++outPreparation.PartCount;
    }
    return Status::Success;
}
} // namespace

RigidWorld::~RigidWorld() noexcept
{
    // The owned collision world releases every collider's shape lease and asserts that no read view is active.
}

Status RigidWorld::TryInitialize(collision::GeometryStore& inoutStore, const RigidWorldSettings& settings) noexcept
{
    if (IsInitialized() == true || inoutStore.IsInitialized() == false)
    {
        return Status::InvalidPhase;
    }
    const Status validation = ValidateSettings(settings);
    if (validation != Status::Success)
    {
        return validation;
    }
    foundation::Array<BodySlot> bodies;
    foundation::Array<uint32> freeBodies;
    foundation::Array<Attachment> attachments;
    foundation::Array<PendingCommand> pending;
    foundation::Array<CommandResult> results;
    if (bodies.TryResize(settings.MaxBodies) == false || freeBodies.TryResize(settings.MaxBodies) == false ||
        attachments.TryResize(settings.MaxColliders) == false ||
        pending.TryResize(settings.MaxPendingCommands) == false ||
        results.TryResize(settings.MaxPendingCommands) == false)
    {
        return Status::AllocationFailure;
    }
    for (uint32 index = 0; index < settings.MaxBodies; ++index)
    {
        freeBodies[index] = settings.MaxBodies - 1 - index;
    }
    uint64 owner = 0;
    if (foundation::identity::TryIssueLifetimeId(owner) != foundation::identity::LifetimeIdStatus::Success)
    {
        return Status::IdentityExhausted;
    }
    // Initialize the collision world last: it cannot be uninitialized again, so nothing may fail after it.
    const collision::Status collision = mCollision.TryInitialize(inoutStore, { .MaxColliders = settings.MaxColliders });
    if (collision != collision::Status::Success)
    {
        return ToRigidStatus(collision);
    }
    mBodies = foundation::Move(bodies);
    mFreeBodies = foundation::Move(freeBodies);
    mAttachments = foundation::Move(attachments);
    mPending = foundation::Move(pending);
    mResults = foundation::Move(results);
    mFreeBodyCount = settings.MaxBodies;
    mStoreOrNull = &inoutStore;
    mSettings = settings;
    mOwner = owner;
    mPhase = WorldPhase::Idle;
    return Status::Success;
}

Status RigidWorld::checkMutable() const noexcept
{
    if (IsInitialized() == false)
    {
        return Status::InvalidPhase;
    }
    if (mPhase == WorldPhase::Faulted)
    {
        return Status::Faulted;
    }
    return mCollision.GetReaderCount() > 0 ? Status::InvalidPhase : Status::Success;
}

const RigidWorld::BodySlot* RigidWorld::findLive(BodyHandle body) const noexcept
{
    if (mOwner == 0 || body.Owner != mOwner || body.Generation == 0 || body.Slot >= mBodies.GetSize())
    {
        return nullptr;
    }
    const BodySlot& slot = mBodies[body.Slot];
    return slot.IsLive == true && slot.Generation == body.Generation ? &slot : nullptr;
}

RigidWorld::BodySlot* RigidWorld::findLive(BodyHandle body) noexcept
{
    return IsAlive(body) == true ? &mBodies[body.Slot] : nullptr;
}

bool RigidWorld::IsAlive(BodyHandle body) const noexcept
{
    return findLive(body) != nullptr;
}

void RigidWorld::destroyColliders(ColliderChain chain) noexcept
{
    uint32 colliderSlot = chain.FirstCollider;
    for (uint32 index = 0; index < chain.ColliderCount; ++index)
    {
        Attachment& attachment = mAttachments[colliderSlot];
        const collision::Status destroyed = mCollision.TryDestroyCollider(attachment.Collider);
        LUDUS_ASSERT(destroyed == collision::Status::Success);
        (void)destroyed;
        const uint32 next = attachment.Next;
        attachment = Attachment{};
        colliderSlot = next;
    }
}

Status RigidWorld::TryCreateBody(const BodyDescription& description, BodyHandle& outBody) noexcept
{
    const Status phase = checkMutable();
    if (phase != Status::Success)
    {
        return phase;
    }
    // Collider creation leases from the store, which a read view of any world on it protects.
    if (mStoreOrNull->GetReaderCount() > 0)
    {
        return Status::InvalidPhase;
    }
    if (IsKnownMotion(description.Motion) == false ||
        (description.Mass != MassSource::Colliders && description.Mass != MassSource::Explicit) ||
        description.Colliders.IsValid() == false)
    {
        return Status::InvalidArgument;
    }
    const Status pose = ValidateWorldPose(description.Pose);
    if (pose != Status::Success)
    {
        return pose;
    }
    const Status velocity = ValidateVelocities(
        { .LinearWorld = description.LinearVelocityWorld, .AngularWorld = description.AngularVelocityWorld });
    if (velocity != Status::Success)
    {
        return velocity;
    }
    const math::Vector3d zero{};
    if (description.Motion == MotionType::Static &&
        (description.LinearVelocityWorld != zero || description.AngularVelocityWorld != zero))
    {
        return Status::InvalidArgument;
    }
    if (math::IsFinite(description.GravityScale) == false)
    {
        return Status::NonFiniteInput;
    }
    if (math::Abs(description.GravityScale) > MAX_GRAVITY_SCALE)
    {
        return Status::OutOfRange;
    }
    if (description.Colliders.GetSize() > MAX_BODY_COLLIDERS)
    {
        return Status::CapacityExceeded;
    }
    ColliderPreparation preparation;
    const Status colliders = PrepareColliders(*mStoreOrNull, description, preparation);
    if (colliders != Status::Success)
    {
        return colliders;
    }
    internal::PrincipalFrame frame{};
    if (description.Motion == MotionType::Dynamic)
    {
        MassProperties properties = description.ExplicitMass;
        if (description.Mass == MassSource::Colliders)
        {
            if (preparation.PartCount == 0)
            {
                return Status::InvalidMassProperties;
            }
            const Status composed = TryComposeMassProperties(
                foundation::BorrowedView<const MassPart>(preparation.Parts.GetData(), preparation.PartCount),
                properties);
            if (composed != Status::Success)
            {
                return composed;
            }
        }
        const Status principal = internal::ComputePrincipalFrame(properties, frame);
        if (principal != Status::Success)
        {
            return principal;
        }
    }
    const usize colliderCount = description.Colliders.GetSize();
    if (mFreeBodyCount == 0 || mCollision.GetColliderCount() + colliderCount > mSettings.MaxColliders)
    {
        return Status::CapacityExceeded;
    }
    const collision::RigidPose bodyPose
    {
        .PositionWorld = description.Pose.PositionWorld,
        .OrientationWorld = internal::Narrow(internal::WidenNormalized(description.Pose.OrientationWorld)),
    };
    // Create every collider before publishing the body, and roll back on any failure.
    uint32 firstCollider = 0;
    uint32 previousCollider = 0;
    for (usize index = 0; index < colliderCount; ++index)
    {
        const BodyColliderDescription& collider = description.Colliders[index];
        collision::ColliderHandle handle;
        const collision::Status created = mCollision.TryCreateCollider(
            {
                .Shape = collider.Shape,
                .Pose = ComposePose(bodyPose, collider.Pose),
                .Participation = collider.Participation,
                .Filter = collider.Filter,
                .IsEnabled = collider.IsEnabled,
                .Binding = collider.Binding,
            },
            handle);
        if (created != collision::Status::Success)
        {
            destroyColliders({ .FirstCollider = firstCollider, .ColliderCount = static_cast<uint32>(index) });
            return ToRigidStatus(created);
        }
        mAttachments[handle.Slot] =
        {
            .Collider = handle,
            .Pose = collider.Pose,
        };
        if (index == 0)
        {
            firstCollider = handle.Slot;
        }
        else
        {
            mAttachments[previousCollider].Next = handle.Slot;
        }
        previousCollider = handle.Slot;
    }
    --mFreeBodyCount;
    const uint32 slotIndex = mFreeBodies[mFreeBodyCount];
    BodySlot& slot = mBodies[slotIndex];
    const uint32 generation = slot.Generation;
    slot = BodySlot{};
    slot.Generation = generation;
    slot.Binding = description.Binding;
    slot.Motion = description.Motion;
    slot.GravityScale = description.GravityScale;
    if (description.Motion == MotionType::Dynamic)
    {
        slot.MassKilograms = frame.MassKilograms;
        slot.InverseMass = 1.0 / frame.MassKilograms;
        slot.PrincipalMoments = frame.PrincipalMoments;
        slot.CenterOfMassBody = frame.CenterOfMassBody;
        slot.BodyFromCenterOfMass = RigidWorldMotion::ToValue(frame.BodyFromCenterOfMass);
    }
    RigidWorldMotion::PlaceCenterOfMass(slot, bodyPose);
    slot.LinearVelocityWorld = description.LinearVelocityWorld;
    slot.AngularVelocityWorld = description.AngularVelocityWorld;
    slot.Pose = bodyPose;
    slot.PreviousPose = bodyPose;
    slot.FirstCollider = firstCollider;
    slot.ColliderCount = static_cast<uint32>(colliderCount);
    slot.IsLive = true;
    ++mLiveCount;
    ++mEpoch;
    outBody = { .Owner = mOwner, .Slot = slotIndex, .Generation = slot.Generation };
    return Status::Success;
}

Status RigidWorld::TryDestroyBody(BodyHandle body) noexcept
{
    const Status phase = checkMutable();
    if (phase != Status::Success)
    {
        return phase;
    }
    if (mStoreOrNull->GetReaderCount() > 0)
    {
        return Status::InvalidPhase;
    }
    BodySlot* slotOrNull = findLive(body);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    destroyColliders({ .FirstCollider = slotOrNull->FirstCollider, .ColliderCount = slotOrNull->ColliderCount });
    uint32 generation = slotOrNull->Generation;
    *slotOrNull = BodySlot{};
    --mLiveCount;
    ++mEpoch;
    // A slot destroyed at the terminal generation is retired: it never returns to the free stack.
    if (foundation::identity::AdvanceGeneration(generation) == foundation::identity::GenerationStatus::Advanced)
    {
        mFreeBodies[mFreeBodyCount] = body.Slot;
        ++mFreeBodyCount;
    }
    slotOrNull->Generation = generation;
    return Status::Success;
}

Status RigidWorld::TryTeleportBody(BodyHandle body, const collision::RigidPose& pose) noexcept
{
    const Status phase = checkMutable();
    if (phase != Status::Success)
    {
        return phase;
    }
    BodySlot* slotOrNull = findLive(body);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    const Status validation = ValidateWorldPose(pose);
    if (validation != Status::Success)
    {
        return validation;
    }
    const collision::RigidPose placed
    {
        .PositionWorld = pose.PositionWorld,
        .OrientationWorld = internal::Narrow(internal::WidenNormalized(pose.OrientationWorld)),
    };
    BodySlot& slot = *slotOrNull;
    RigidWorldMotion::PlaceCenterOfMass(slot, placed);
    slot.Pose = placed;
    slot.PreviousPose = placed;
    const Status colliders = RigidWorldMotion::PoseColliders(*this, slot);
    // The pose was validated and no read view is active, so every collider accepts it.
    LUDUS_ASSERT(colliders == Status::Success);
    (void)colliders;
    ++mEpoch;
    return Status::Success;
}

Status RigidWorld::TrySetBodyVelocity(BodyHandle body,
                                      math::Vector3d linearVelocityWorld,
                                      math::Vector3d angularVelocityWorld) noexcept
{
    const Status phase = checkMutable();
    if (phase != Status::Success)
    {
        return phase;
    }
    BodySlot* slotOrNull = findLive(body);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (slotOrNull->Motion == MotionType::Static)
    {
        return Status::InvalidArgument;
    }
    const Status validation =
        ValidateVelocities({ .LinearWorld = linearVelocityWorld, .AngularWorld = angularVelocityWorld });
    if (validation != Status::Success)
    {
        return validation;
    }
    slotOrNull->LinearVelocityWorld = linearVelocityWorld;
    slotOrNull->AngularVelocityWorld = angularVelocityWorld;
    slotOrNull->IsFollowingTarget = false;
    ++mEpoch;
    return Status::Success;
}

Status RigidWorld::checkAdmission(const CommandKey& key, BodyHandle body, MotionType motion) const noexcept
{
    if (IsInitialized() == false)
    {
        return Status::InvalidPhase;
    }
    if (mPhase == WorldPhase::Faulted)
    {
        return Status::Faulted;
    }
    if (key.TargetTick < mNextTick)
    {
        return Status::PastTick;
    }
    const BodySlot* slotOrNull = findLive(body);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    return slotOrNull->Motion == motion ? Status::Success : Status::InvalidArgument;
}

Status RigidWorld::admit(const PendingCommand& command) noexcept
{
    // Binary search for the first pending key that does not order before the new one.
    usize low = 0;
    usize high = mPendingCount;
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (IsOrderedBefore(mPending[middle].Key, command.Key) == true)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    if (low < mPendingCount && mPending[low].Key == command.Key)
    {
        return Status::DuplicateCommand;
    }
    if (mPendingCount == mPending.GetSize())
    {
        return Status::CapacityExceeded;
    }
    for (usize index = mPendingCount; index > low; --index)
    {
        mPending[index] = mPending[index - 1];
    }
    mPending[low] = command;
    ++mPendingCount;
    mPendingHighWater = math::Max(mPendingHighWater, mPendingCount);
    return Status::Success;
}

namespace
{
/// The frame, two vectors and optional point shared by force and impulse commands.
struct CommandValues final
{
    CommandFrame Frame = CommandFrame::World;
    math::Vector3d Linear{};
    math::Vector3d Angular{};
    bool HasApplicationPoint = false;
    math::Vector3d ApplicationPoint{};
};

Status ValidateCommandValues(const CommandValues& values) noexcept
{
    if (IsKnownFrame(values.Frame) == false)
    {
        return Status::InvalidArgument;
    }
    const Status linear = internal::ValidateBounded(values.Linear, MAX_COMMAND_COMPONENT);
    if (linear != Status::Success)
    {
        return linear;
    }
    const Status angular = internal::ValidateBounded(values.Angular, MAX_COMMAND_COMPONENT);
    if (angular != Status::Success)
    {
        return angular;
    }
    return values.HasApplicationPoint == true ? internal::ValidatePosition(values.ApplicationPoint) : Status::Success;
}
} // namespace

Status RigidWorld::TryEnqueueForce(const ForceCommand& command) noexcept
{
    const Status admission = checkAdmission(command.Key, command.Body, MotionType::Dynamic);
    if (admission != Status::Success)
    {
        return admission;
    }
    const CommandValues commandValues
    {
        .Frame = command.Frame,
        .Linear = command.ForceInFrame,
        .Angular = command.TorqueInFrame,
        .HasApplicationPoint = command.HasApplicationPoint,
        .ApplicationPoint = command.ApplicationPointInFrame,
    };
    const Status values = ValidateCommandValues(commandValues);
    if (values != Status::Success)
    {
        return values;
    }
    const PendingCommand pending
    {
        .Key = command.Key,
        .Kind = CommandKind::Force,
        .Body = command.Body,
        .Frame = command.Frame,
        .Linear = command.ForceInFrame,
        .Angular = command.TorqueInFrame,
        .HasApplicationPoint = command.HasApplicationPoint,
        .ApplicationPoint = command.ApplicationPointInFrame,
    };
    return admit(pending);
}

Status RigidWorld::TryEnqueueImpulse(const ImpulseCommand& command) noexcept
{
    const Status admission = checkAdmission(command.Key, command.Body, MotionType::Dynamic);
    if (admission != Status::Success)
    {
        return admission;
    }
    const CommandValues commandValues
    {
        .Frame = command.Frame,
        .Linear = command.LinearImpulseInFrame,
        .Angular = command.AngularImpulseInFrame,
        .HasApplicationPoint = command.HasApplicationPoint,
        .ApplicationPoint = command.ApplicationPointInFrame,
    };
    const Status values = ValidateCommandValues(commandValues);
    if (values != Status::Success)
    {
        return values;
    }
    const PendingCommand pending
    {
        .Key = command.Key,
        .Kind = CommandKind::Impulse,
        .Body = command.Body,
        .Frame = command.Frame,
        .Linear = command.LinearImpulseInFrame,
        .Angular = command.AngularImpulseInFrame,
        .HasApplicationPoint = command.HasApplicationPoint,
        .ApplicationPoint = command.ApplicationPointInFrame,
    };
    return admit(pending);
}

Status RigidWorld::TrySetKinematicTarget(const KinematicTargetCommand& command) noexcept
{
    const Status admission = checkAdmission(command.Key, command.Body, MotionType::Kinematic);
    if (admission != Status::Success)
    {
        return admission;
    }
    const Status pose = ValidateWorldPose(command.TargetPose);
    if (pose != Status::Success)
    {
        return pose;
    }
    const PendingCommand pending
    {
        .Key = command.Key,
        .Kind = CommandKind::KinematicTarget,
        .Body = command.Body,
        .TargetPose = command.TargetPose,
    };
    return admit(pending);
}

StepResult RigidWorld::Step() noexcept
{
    return RigidWorldMotion::Step(*this);
}

Status RigidWorld::TryGetBodyState(BodyHandle body, BodyState& outState) const noexcept
{
    if (IsAlive(body) == false)
    {
        return Status::InvalidHandle;
    }
    outState = RigidWorldMotion::MakeState(*this, body.Slot);
    return Status::Success;
}

Status RigidWorld::TryGetBodyMass(BodyHandle body, BodyMassInfo& outInfo) const noexcept
{
    const BodySlot* slotOrNull = findLive(body);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    outInfo =
    {
        .Motion = slotOrNull->Motion,
        .Frame =
        {
            .MassKilograms = slotOrNull->MassKilograms,
            .BodyFromCenterOfMass =
            {
                .PositionBody = slotOrNull->CenterOfMassBody,
                .OrientationBody =
                    internal::Narrow(RigidWorldMotion::ToRotation(slotOrNull->BodyFromCenterOfMass)),
            },
            .PrincipalMomentsKilogramMetersSquared = slotOrNull->PrincipalMoments,
        },
    };
    return Status::Success;
}

Status RigidWorld::TryGetBodyColliders(BodyHandle body,
                                       foundation::BorrowedView<collision::ColliderHandle> colliderBuffer,
                                       usize& outColliderCount) const noexcept
{
    const BodySlot* slotOrNull = findLive(body);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (colliderBuffer.IsValid() == false)
    {
        return Status::InvalidArgument;
    }
    uint32 colliderSlot = slotOrNull->FirstCollider;
    for (uint32 index = 0; index < slotOrNull->ColliderCount && index < colliderBuffer.GetSize(); ++index)
    {
        colliderBuffer[index] = mAttachments[colliderSlot].Collider;
        colliderSlot = mAttachments[colliderSlot].Next;
    }
    outColliderCount = slotOrNull->ColliderCount;
    return Status::Success;
}

Status RigidWorld::TryGetInterpolatedPose(BodyHandle body, float64 alpha, collision::RigidPose& outPose) const noexcept
{
    const BodySlot* slotOrNull = findLive(body);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (math::IsFinite(alpha) == false)
    {
        return Status::NonFiniteInput;
    }
    if (alpha < 0.0 || alpha > 1.0)
    {
        return Status::OutOfRange;
    }
    const internal::Rotation previous = internal::WidenNormalized(slotOrNull->PreviousPose.OrientationWorld);
    internal::Rotation current = internal::WidenNormalized(slotOrNull->Pose.OrientationWorld);
    const float64 dot =
        previous.X * current.X + previous.Y * current.Y + previous.Z * current.Z + previous.W * current.W;
    if (dot < 0.0)
    {
        current = {-current.X, -current.Y, -current.Z, -current.W};
    }
    const float64 keep = 1.0 - alpha;
    const internal::Rotation blended{keep * previous.X + alpha * current.X,
                                     keep * previous.Y + alpha * current.Y,
                                     keep * previous.Z + alpha * current.Z,
                                     keep * previous.W + alpha * current.W};
    outPose =
    {
        .PositionWorld = slotOrNull->PreviousPose.PositionWorld * keep + slotOrNull->Pose.PositionWorld * alpha,
        .OrientationWorld = internal::Narrow(internal::Normalize(blended)),
    };
    return Status::Success;
}

Status RigidWorld::TryExportBodyStates(foundation::BorrowedView<BodyState> stateBuffer,
                                       BodyExportResult& outResult) const noexcept
{
    if (IsInitialized() == false)
    {
        return Status::InvalidPhase;
    }
    if (stateBuffer.IsValid() == false)
    {
        return Status::InvalidArgument;
    }
    usize written = 0;
    for (uint32 slotIndex = 0; slotIndex < mBodies.GetSize() && written < stateBuffer.GetSize(); ++slotIndex)
    {
        if (mBodies[slotIndex].IsLive == true)
        {
            stateBuffer[written] = RigidWorldMotion::MakeState(*this, slotIndex);
            ++written;
        }
    }
    outResult =
    {
        .BodyCount = mLiveCount,
        .WrittenCount = written,
        .IsComplete = written == mLiveCount,
        .NextTick = mNextTick,
    };
    return Status::Success;
}

foundation::BorrowedView<const CommandResult> RigidWorld::GetCommandResults() const noexcept
{
    return {mResults.GetData(), mResultCount};
}

Status RigidWorld::TryAcquireCollisionReadView(collision::CollisionReadView& outView) noexcept
{
    if (IsInitialized() == false)
    {
        return Status::InvalidPhase;
    }
    return ToRigidStatus(mCollision.TryAcquireReadView(outView));
}

RigidWorldDiagnostics RigidWorld::GetDiagnostics() const noexcept
{
    return
    {
        .Phase = mPhase,
        .NextTick = mNextTick,
        .Epoch = mEpoch,
        .BodyCount = mLiveCount,
        .PendingCommandCount = mPendingCount,
        .PendingCommandHighWater = mPendingHighWater,
        .LastCommandResultCount = mResultCount,
        .Fault = mFault,
        .FaultBody = mFaultBody,
    };
}
} // namespace ludus::physics::rigid
