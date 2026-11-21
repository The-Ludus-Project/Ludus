#pragma once

/// @file
/// @brief Rigid world: bodies over one owned collision world, a fixed tick with substeps, commands and state export.
///
/// See docs/architecture/physics-rigid.md for usage and "World phases, commands and failure contracts" in
/// docs/architecture/physics-systems.md for the failure table this world implements.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/identity/handle.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/commands.h>
#include <ludus/physics/rigid/mass_properties.h>
#include <ludus/physics/rigid/status.h>

namespace ludus::physics::rigid
{
/// Default tick duration, seconds: a starting point for tuning, not a recommendation.
inline constexpr float64 DEFAULT_TICK_SECONDS = 1.0 / 60.0;

/// Shortest admitted tick, seconds.
inline constexpr float64 MIN_TICK_SECONDS = 1.0e-6;

/// Longest admitted tick, seconds.
inline constexpr float64 MAX_TICK_SECONDS = 1.0;

/// Default number of substeps per tick.
inline constexpr uint32 DEFAULT_SUBSTEP_COUNT = 4;

/// Most substeps per tick.
inline constexpr uint32 MAX_SUBSTEP_COUNT = 64;

/// Standard gravity, meters per second squared.
inline constexpr float64 STANDARD_GRAVITY_METERS_PER_SECOND_SQUARED = 9.80665;

/// Largest admitted magnitude of a gravity component, meters per second squared.
inline constexpr float64 MAX_GRAVITY_COMPONENT_METERS_PER_SECOND_SQUARED = 1.0e3;

/// @brief Fixed capacity and clock of a RigidWorld, chosen at initialization.
///
/// Changing TickSeconds or SubstepCount changes integration results; both belong to replay configuration.
struct RigidWorldSettings final
{
    uint32 MaxBodies = 0;                        ///< Body slots, in [1, MAX_RIGID_WORLD_BODIES].
    uint32 MaxColliders = 0;                     ///< Collider slots of the owned collision world.
    uint32 MaxPendingCommands = 0;               ///< Pending command slots, in [1, MAX_PENDING_COMMANDS].
    float64 TickSeconds = DEFAULT_TICK_SECONDS;  ///< Tick duration in [MIN_TICK_SECONDS, MAX_TICK_SECONDS].
    uint32 SubstepCount = DEFAULT_SUBSTEP_COUNT; ///< Substeps per tick, in [1, MAX_SUBSTEP_COUNT].
    /// Gravity acceleration, components within MAX_GRAVITY_COMPONENT_METERS_PER_SECOND_SQUARED.
    foundation::math::Vector3d GravityWorld{0.0, -STANDARD_GRAVITY_METERS_PER_SECOND_SQUARED, 0.0};
};

/// @brief Phase of a RigidWorld. Callers observe Uninitialized, Idle or Faulted; the others exist during Step.
enum class WorldPhase : uint8
{
    Uninitialized, ///< TryInitialize has not succeeded.
    Idle,          ///< Between ticks: mutations, commands and read views are admitted.
    Commit,        ///< Step is applying the tick's commands.
    Simulate,      ///< Step is integrating substeps.
    Publish,       ///< Step is publishing poses, colliders and command results.
    Faulted,       ///< A tick failed after advancement began; only diagnostics, reads and destruction remain.
};

/// @brief Why a world entered WorldPhase::Faulted.
enum class FaultReason : uint8
{
    None,                   ///< The world has not faulted.
    NonFiniteState,         ///< A body's position, orientation or velocity became NaN or infinite.
    LeftCoordinateEnvelope, ///< A body's COM left collision::MAX_COORDINATE_METERS.
    ColliderUpdateFailed,   ///< The collision world refused an attached collider's new pose.
};

/// @brief Outcome of one RigidWorld::Step call.
///
/// Result is Success when the tick advanced exactly once. A preflight failure (Phase Idle) leaves the tick, the
/// pending commands and every published value unchanged. A fault (Result Faulted, Phase Faulted) leaves state
/// partially advanced and publishes no tick.
struct StepResult final
{
    Status Result = Status::Success;       ///< Success, InvalidPhase, or Faulted.
    WorldPhase Phase = WorldPhase::Idle;   ///< Idle after success or preflight failure; Faulted after a fault.
    uint64 Tick = 0;                       ///< The tick that was simulated, on success.
    uint32 AppliedCommandCount = 0;        ///< Commands with CommandOutcome::Applied.
    uint32 UnappliedCommandCount = 0;      ///< Commands whose body was destroyed or whose target was superseded.
    FaultReason Fault = FaultReason::None; ///< Why the world faulted.
    BodyHandle FaultBody{};                ///< The first body found in the fault state.
};

/// @brief Plain-value diagnostics of a RigidWorld.
struct RigidWorldDiagnostics final
{
    WorldPhase Phase = WorldPhase::Uninitialized; ///< Current phase.
    uint64 NextTick = 0;                          ///< Tick the next Step simulates; also the number of completed ticks.
    uint64 Epoch = 0;                             ///< Advanced by every successful mutation and tick.
    usize BodyCount = 0;                          ///< Live bodies.
    usize PendingCommandCount = 0;                ///< Admitted commands not yet applied.
    usize PendingCommandHighWater = 0;            ///< Largest PendingCommandCount since initialization.
    usize LastCommandResultCount = 0;             ///< Results published by the last successful tick.
    FaultReason Fault = FaultReason::None;        ///< Why the world faulted.
    BodyHandle FaultBody{};                       ///< The first body found in the fault state.
};

/// @brief Completion of a body-state export.
///
/// Records are written in body slot order until the buffer is full; IsComplete is true exactly when every live
/// body was written, and BodyCount tells a caller how many records a complete export needs.
struct BodyExportResult final
{
    usize BodyCount = 0;    ///< Live bodies.
    usize WrittenCount = 0; ///< Records written, a prefix of the buffer.
    bool IsComplete = true; ///< Whether WrittenCount equals BodyCount.
    uint64 NextTick = 0;    ///< The world's next tick when the export was taken.
};

/// @brief Static, kinematic and dynamic bodies over one owned CollisionWorld, advanced by a fixed tick.
///
/// The world reserves every body, collider, command and result slot when it is initialized, so creating bodies,
/// admitting commands and stepping never allocate. Bodies have no contacts in this gate: they fall, spin and follow
/// kinematic targets without touching each other. Each body's attached colliders follow it in the collision world.
///
/// One Step advances one tick: preflight, then Commit (apply that tick's commands in key order), Simulate
/// (integrate SubstepCount substeps) and Publish (poses, colliders, results). Dynamic bodies integrate their COM
/// pose with semi-implicit Euler and an implicit gyroscopic term; body poses are recovered through
/// BodyFromCenterOfMass.
///
/// Thread affinity: initialization, mutation, command admission and Step happen on one owner thread. Collision read
/// views may run queries on other threads; while one is active, Step and body mutations fail with
/// Status::InvalidPhase. The world is neither copyable nor movable, because its collision world keeps its address.
class RigidWorld final
{
public:
    /// Construct an uninitialized world. Does not allocate.
    RigidWorld() noexcept = default;
    /// Not copyable: bodies have one owner.
    RigidWorld(const RigidWorld&) = delete;
    /// Not copyable: bodies have one owner.
    RigidWorld& operator=(const RigidWorld&) = delete;
    /// Not movable: the collision world and its read views keep their addresses.
    RigidWorld(RigidWorld&&) = delete;
    /// Not movable: the collision world and its read views keep their addresses.
    RigidWorld& operator=(RigidWorld&&) = delete;
    /// Destroy every body and its colliders. Permitted in any phase, including Faulted.
    /// @pre No collision read view of this world is active, and the store is still alive; checked by assertion.
    ~RigidWorld() noexcept;

    /// @brief Reserve every table, obtain the world's owner token and initialize the owned collision world.
    /// @param inoutStore Initialized store that must outlive this world.
    /// @param settings Capacities and clock; see RigidWorldSettings for admitted values.
    /// @return Success; InvalidPhase when already initialized or the store is uninitialized; InvalidArgument for a
    /// zero capacity; CapacityExceeded above a capacity limit; NonFiniteInput or OutOfRange for the clock or gravity;
    /// AllocationFailure; or IdentityExhausted. The world is unchanged on failure.
    [[nodiscard]] Status TryInitialize(collision::GeometryStore& inoutStore,
                                       const RigidWorldSettings& settings) noexcept;

    /// @brief Create a body and its colliders.
    ///
    /// Creation is synchronous: the body and its colliders are live when this returns, with interpolation reset.
    /// @param description Motion, pose, velocities, colliders, mass source, gravity scale and binding.
    /// @param outBody Receives the handle on success; unchanged on failure.
    /// @return Success; InvalidPhase when uninitialized or a collision read view is active; Faulted; InvalidArgument,
    /// NonFiniteInput or OutOfRange for the description; InvalidHandle for a collider shape that is not live in the
    /// store; InvalidMassProperties when a dynamic body's mass source is missing, a triangle mesh or invalid;
    /// DegenerateShape for a hull that encloses no volume; or CapacityExceeded when a body, collider or shape-lease
    /// slot is full. No body or collider is created on failure.
    [[nodiscard]] Status TryCreateBody(const BodyDescription& description, BodyHandle& outBody) noexcept;

    /// @brief Destroy a body and its colliders. Pending commands for it report CommandOutcome::BodyDestroyed.
    /// @return Success; InvalidPhase; Faulted; or InvalidHandle.
    [[nodiscard]] Status TryDestroyBody(BodyHandle body) noexcept;

    /// @brief Place a body discontinuously; resets its interpolation and keeps its velocities.
    ///
    /// A teleport is not a fast kinematic move or a sweep, and it does not test initial overlap in this gate.
    /// @return Success; InvalidPhase; Faulted; InvalidHandle; or the pose validation failure. Unchanged on failure.
    [[nodiscard]] Status TryTeleportBody(BodyHandle body, const collision::RigidPose& pose) noexcept;

    /// @brief Replace a kinematic or dynamic body's COM velocity and angular velocity.
    ///
    /// A kinematic body then keeps this velocity until another velocity or target is set.
    /// @return Success; InvalidPhase; Faulted; InvalidHandle; InvalidArgument for a static body; or NonFiniteInput or
    /// OutOfRange for a velocity. Unchanged on failure.
    [[nodiscard]] Status TrySetBodyVelocity(BodyHandle body,
                                            foundation::math::Vector3d linearVelocityWorld,
                                            foundation::math::Vector3d angularVelocityWorld) noexcept;

    /// @brief Admit a force command.
    /// @return Success; InvalidPhase when uninitialized; Faulted; PastTick for a tick already simulated;
    /// InvalidHandle; InvalidArgument for a body that is not dynamic or an unknown frame; NonFiniteInput or
    /// OutOfRange for a vector or point; DuplicateCommand; or CapacityExceeded. Nothing is admitted on failure.
    [[nodiscard]] Status TryEnqueueForce(const ForceCommand& command) noexcept;

    /// @brief Admit an impulse command. @return As for TryEnqueueForce.
    [[nodiscard]] Status TryEnqueueImpulse(const ImpulseCommand& command) noexcept;

    /// @brief Admit a kinematic target.
    /// @return As for TryEnqueueForce, with InvalidArgument for a body that is not kinematic, and the pose validation
    /// failures for the target.
    [[nodiscard]] Status TrySetKinematicTarget(const KinematicTargetCommand& command) noexcept;

    /// @brief Advance exactly one tick, or report why not.
    /// @return See StepResult. Preflight fails with InvalidPhase when uninitialized or a collision read view is
    /// active, and with Faulted for a faulted world.
    [[nodiscard]] StepResult Step() noexcept;

    /// @brief Read one body's plain-value state. Permitted while Faulted, for diagnostics.
    /// @param body Body to resolve.
    /// @param outState Receives the state on success; unchanged on failure.
    /// @return Success, or InvalidHandle.
    [[nodiscard]] Status TryGetBodyState(BodyHandle body, BodyState& outState) const noexcept;

    /// @brief Read one body's mass frame.
    /// @param body Body to resolve.
    /// @param outInfo Receives the mass frame on success; unchanged on failure.
    /// @return Success, or InvalidHandle.
    [[nodiscard]] Status TryGetBodyMass(BodyHandle body, BodyMassInfo& outInfo) const noexcept;

    /// @brief List a body's attached colliders in creation order.
    /// @param body Body to resolve.
    /// @param colliderBuffer Caller storage, borrowed for this call; at most MAX_BODY_COLLIDERS records are written.
    /// @param outColliderCount Receives the body's collider count on success, which may exceed the buffer size;
    /// unchanged on failure.
    /// @return Success; InvalidHandle; or InvalidArgument for an invalid buffer view.
    [[nodiscard]] Status TryGetBodyColliders(BodyHandle body,
                                             foundation::BorrowedView<collision::ColliderHandle> colliderBuffer,
                                             usize& outColliderCount) const noexcept;

    /// @brief Interpolate between a body's previous and current pose for presentation.
    /// @param body Body to resolve.
    /// @param alpha Fraction in [0, 1] of the last tick: zero is PreviousPose, one is Pose.
    /// @param outPose Receives the linear position and shortest-arc normalized orientation blend; unchanged on failure.
    /// @return Success; InvalidHandle; NonFiniteInput or OutOfRange for alpha.
    [[nodiscard]] Status
    TryGetInterpolatedPose(BodyHandle body, float64 alpha, collision::RigidPose& outPose) const noexcept;

    /// @brief Export every live body's state as plain values, in slot order.
    /// @param stateBuffer Caller storage, borrowed for this call; may be empty.
    /// @param outResult Receives counts and completion on success; unchanged on failure.
    /// @return Success; InvalidPhase when uninitialized; or InvalidArgument for an invalid buffer view.
    [[nodiscard]] Status TryExportBodyStates(foundation::BorrowedView<BodyState> stateBuffer,
                                             BodyExportResult& outResult) const noexcept;

    /// Results of the last successful tick's commands, in application order; valid until the next mutation or Step.
    [[nodiscard]] foundation::BorrowedView<const CommandResult> GetCommandResults() const noexcept;

    /// @brief Begin a collision read phase on the owned collision world. See collision::CollisionReadView.
    /// @return Success; InvalidPhase when uninitialized; or InvalidArgument when outView is already active.
    [[nodiscard]] Status TryAcquireCollisionReadView(collision::CollisionReadView& outView) noexcept;

    /// The owned collision world, for inspection. Its colliders change only through this world.
    [[nodiscard]] LUDUS_INLINE const collision::CollisionWorld& GetCollisionWorld() const noexcept
    {
        return mCollision;
    }

    /// Plain-value diagnostics, including the fault reason. Permitted in every phase.
    [[nodiscard]] RigidWorldDiagnostics GetDiagnostics() const noexcept;

    /// Whether `body` names a live body of this world.
    [[nodiscard]] bool IsAlive(BodyHandle body) const noexcept;

    /// Whether the world has been initialized.
    [[nodiscard]] LUDUS_INLINE bool IsInitialized() const noexcept
    {
        return mOwner != 0;
    }
    /// Current phase: Uninitialized, Idle or Faulted between calls.
    [[nodiscard]] LUDUS_INLINE WorldPhase GetPhase() const noexcept
    {
        return mPhase;
    }
    /// Tick the next Step simulates; commands must target it or a later tick.
    [[nodiscard]] LUDUS_INLINE uint64 GetNextTick() const noexcept
    {
        return mNextTick;
    }
    /// Number of live bodies.
    [[nodiscard]] LUDUS_INLINE usize GetBodyCount() const noexcept
    {
        return mLiveCount;
    }
    /// The settings the world was initialized with.
    [[nodiscard]] LUDUS_INLINE const RigidWorldSettings& GetSettings() const noexcept
    {
        return mSettings;
    }

private:
    /// Unit quaternion in float64; motion state keeps float64 between substeps.
    struct RotationValue final
    {
        float64 X = 0.0; ///< Vector part, X.
        float64 Y = 0.0; ///< Vector part, Y.
        float64 Z = 0.0; ///< Vector part, Z.
        float64 W = 1.0; ///< Scalar part.
    };

    /// One body slot. Mass fields are zero for static and kinematic bodies.
    struct BodySlot final
    {
        uint64 Binding = 0;                                         ///< The body's binding.
        MotionType Motion = MotionType::Static;                     ///< The body's motion type.
        float64 GravityScale = 1.0;                                 ///< Multiplier of the world's gravity.
        float64 MassKilograms = 0.0;                                ///< Mass of a dynamic body.
        float64 InverseMass = 0.0;                                  ///< Reciprocal mass; zero for infinite mass.
        foundation::math::Vector3d PrincipalMoments{};              ///< Diagonal inertia in the COM frame.
        foundation::math::Vector3d CenterOfMassBody{};              ///< BodyFromCenterOfMass translation.
        RotationValue BodyFromCenterOfMass{};                       ///< BodyFromCenterOfMass rotation.
        foundation::math::Vector3d CenterOfMassWorld{};             ///< Integrated COM position.
        RotationValue CenterOfMassOrientationWorld{};               ///< Integrated COM-frame orientation.
        foundation::math::Vector3d LinearVelocityWorld{};           ///< COM velocity.
        foundation::math::Vector3d AngularVelocityWorld{};          ///< Angular velocity in world axes.
        foundation::math::Vector3d ForceWorld{};                    ///< Force held through the current tick.
        foundation::math::Vector3d TorqueWorld{};                   ///< Torque held through the current tick.
        foundation::math::Vector3d TargetCenterOfMassWorld{};       ///< Kinematic target of the current tick.
        RotationValue TargetOrientationWorld{};                     ///< COM-frame orientation of the kinematic target.
        collision::RigidPose Pose{};                                ///< Published body frame.
        collision::RigidPose PreviousPose{};                        ///< Published body frame one tick earlier.
        uint32 FirstCollider = 0;                                   ///< Collision slot of the first attached collider.
        uint32 ColliderCount = 0;                                   ///< Attached colliders.
        uint32 Generation = foundation::identity::FIRST_GENERATION; ///< Current incarnation of the slot.
        uint32 TargetResult = 0;        ///< Result index of the target applied this tick, while HasTarget.
        bool HasTarget = false;         ///< A kinematic target applies to the tick being simulated.
        bool IsFollowingTarget = false; ///< The last tick followed a target; the body stops next tick.
        bool IsLive = false;            ///< Whether the slot holds a body.
    };

    /// One attached collider, indexed by its collision-world slot.
    struct Attachment final
    {
        collision::ColliderHandle Collider{}; ///< The collider in the owned collision world.
        BodyLocalPose Pose{};                 ///< Placement in the body frame.
        uint32 Next = 0;                      ///< Collision slot of the body's next collider.
    };

    /// One admitted command; the fields of other kinds stay zero.
    struct PendingCommand final
    {
        CommandKey Key{};                              ///< Order key.
        CommandKind Kind = CommandKind::Force;         ///< Which command this is.
        BodyHandle Body{};                             ///< Target body.
        CommandFrame Frame = CommandFrame::World;      ///< Frame of the vectors and point.
        foundation::math::Vector3d Linear{};           ///< Force or linear impulse.
        foundation::math::Vector3d Angular{};          ///< Torque or angular impulse.
        bool HasApplicationPoint = false;              ///< Whether ApplicationPoint applies.
        foundation::math::Vector3d ApplicationPoint{}; ///< Application point of a force or impulse.
        collision::RigidPose TargetPose{};             ///< Body-frame target of a kinematic target.
    };

    /// A body's attached colliders: the collision slot of the first and how many follow through Attachment::Next.
    struct ColliderChain final
    {
        uint32 FirstCollider = 0; ///< Collision slot of the first collider.
        uint32 ColliderCount = 0; ///< Colliders in the chain.
    };

    friend class RigidWorldMotion;

    [[nodiscard]] BodySlot* findLive(BodyHandle body) noexcept;
    [[nodiscard]] const BodySlot* findLive(BodyHandle body) const noexcept;
    [[nodiscard]] Status checkMutable() const noexcept;
    [[nodiscard]] Status checkAdmission(const CommandKey& key, BodyHandle body, MotionType motion) const noexcept;
    [[nodiscard]] Status admit(const PendingCommand& command) noexcept;
    void destroyColliders(ColliderChain chain) noexcept;

    collision::CollisionWorld mCollision;
    collision::GeometryStore* mStoreOrNull = nullptr;
    RigidWorldSettings mSettings{};
    foundation::Array<BodySlot> mBodies;
    foundation::Array<uint32> mFreeBodies; ///< LIFO stack of reusable body slots; its size is the free count.
    foundation::Array<Attachment> mAttachments;
    foundation::Array<PendingCommand> mPending; ///< Sorted by key; the first mPendingCount entries are live.
    foundation::Array<CommandResult> mResults;  ///< The first mResultCount entries are the last tick's results.
    usize mFreeBodyCount = 0;
    usize mPendingCount = 0;
    usize mPendingHighWater = 0;
    usize mResultCount = 0;
    usize mLiveCount = 0;
    uint64 mOwner = 0;
    uint64 mEpoch = 0;
    uint64 mNextTick = 0;
    WorldPhase mPhase = WorldPhase::Uninitialized;
    FaultReason mFault = FaultReason::None;
    BodyHandle mFaultBody{};
};
} // namespace ludus::physics::rigid
