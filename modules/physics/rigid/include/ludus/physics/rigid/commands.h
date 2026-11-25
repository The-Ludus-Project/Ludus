#pragma once

/// @file
/// @brief Tick-addressed force, impulse and kinematic-target commands and their per-tick results.
///
/// See "Commands" in docs/architecture/physics-rigid.md. Commands are ordered by target tick, producer ID and
/// producer-local sequence, never by arrival order.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/status.h>

namespace ludus::physics::rigid
{
/// Largest number of commands a world may hold pending.
inline constexpr uint32 MAX_PENDING_COMMANDS = 1U << 20U;

/// @brief Deterministic order key of one command.
///
/// The game assigns producer IDs deterministically and each producer numbers its commands. Keys of pending
/// commands are unique; the world applies one tick's commands in ascending (ProducerId, Sequence) order.
struct CommandKey final
{
    uint64 TargetTick = 0; ///< Tick during which the command applies; at least RigidWorld::GetNextTick().
    uint32 ProducerId = 0; ///< Deterministic producer identity.
    uint32 Sequence = 0;   ///< Producer-local sequence number.

    /// Exact field-wise equality.
    [[nodiscard]] constexpr bool operator==(const CommandKey&) const noexcept = default;
};

/// Whether `first` orders strictly before `second`: by tick, then producer, then sequence.
[[nodiscard]] constexpr bool IsOrderedBefore(const CommandKey& first, const CommandKey& second) noexcept
{
    if (first.TargetTick != second.TargetTick)
    {
        return first.TargetTick < second.TargetTick;
    }
    if (first.ProducerId != second.ProducerId)
    {
        return first.ProducerId < second.ProducerId;
    }
    return first.Sequence < second.Sequence;
}

/// @brief Frame in which a command's vectors and application point are expressed.
///
/// Body-frame values are converted to world space at the body's pose when the tick applies the command.
enum class CommandFrame : uint8
{
    World, ///< World axes; the application point is a world position.
    Body,  ///< Body axes; the application point is a body-frame position.
};

/// @brief A force and torque held constant through every substep of one tick on a dynamic body.
///
/// A force at an application point also adds the torque `(point - COM) x force`. Components are finite and at
/// most MAX_COMMAND_COMPONENT; the point is within collision::MAX_COORDINATE_METERS.
struct ForceCommand final
{
    CommandKey Key{};                                     ///< Order key.
    BodyHandle Body{};                                    ///< Dynamic target body.
    CommandFrame Frame = CommandFrame::World;             ///< Frame of the vectors and point.
    foundation::math::Vector3d ForceInFrame{};            ///< Force, newtons.
    foundation::math::Vector3d TorqueInFrame{};           ///< Additional torque, newton meters.
    bool HasApplicationPoint = false;                     ///< False applies the force at the COM.
    foundation::math::Vector3d ApplicationPointInFrame{}; ///< Where the force acts, meters.
};

/// @brief A linear and angular impulse applied once, at the start of one tick, to a dynamic body.
///
/// An impulse at an application point also adds the angular impulse `(point - COM) x impulse`. Admitted values
/// follow ForceCommand.
struct ImpulseCommand final
{
    CommandKey Key{};                                     ///< Order key.
    BodyHandle Body{};                                    ///< Dynamic target body.
    CommandFrame Frame = CommandFrame::World;             ///< Frame of the vectors and point.
    foundation::math::Vector3d LinearImpulseInFrame{};    ///< Linear impulse, newton seconds.
    foundation::math::Vector3d AngularImpulseInFrame{};   ///< Additional angular impulse, newton meter seconds.
    bool HasApplicationPoint = false;                     ///< False applies the impulse at the COM.
    foundation::math::Vector3d ApplicationPointInFrame{}; ///< Where the impulse acts, meters.
};

/// @brief The body-frame pose a kinematic body reaches at the end of one tick.
///
/// The tick moves the body with the constant linear velocity and the shortest-arc angular velocity that reach the
/// target, then places it exactly on the target. From the next tick the body rests there unless another target
/// or velocity is set. Rotations of half a turn or more per tick need an explicit angular velocity instead.
struct KinematicTargetCommand final
{
    CommandKey Key{};                  ///< Order key.
    BodyHandle Body{};                 ///< Kinematic target body.
    collision::RigidPose TargetPose{}; ///< Body frame in world space at the end of the target tick.
};

/// @brief Kind of a command.
enum class CommandKind : uint8
{
    Force,           ///< A ForceCommand.
    Impulse,         ///< An ImpulseCommand.
    KinematicTarget, ///< A KinematicTargetCommand.
};

/// @brief What happened to an admitted command when its tick applied it.
enum class CommandOutcome : uint8
{
    Applied,       ///< The command changed its body's motion for the tick.
    BodyDestroyed, ///< The body was destroyed after admission; the command did nothing.
    Superseded,    ///< A later kinematic target for the same body and tick replaced this one.
};

/// @brief Per-command result of the last successful tick, keyed by the command's order key.
struct CommandResult final
{
    CommandKey Key{};                                 ///< The command's key.
    CommandKind Kind = CommandKind::Force;            ///< The command's kind.
    BodyHandle Body{};                                ///< The command's target body.
    CommandOutcome Outcome = CommandOutcome::Applied; ///< What the tick did with it.
};
} // namespace ludus::physics::rigid
