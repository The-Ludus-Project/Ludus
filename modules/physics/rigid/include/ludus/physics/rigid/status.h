#pragma once

/// @file
/// @brief Result vocabulary and numeric envelope of PhysicsRigid.
///
/// See docs/architecture/physics-rigid.md for the contracts these values belong to and
/// "World phases, commands and failure contracts" in docs/architecture/physics-systems.md for the failure table.

#include <ludus/foundation/base/types.h>

#include <ludus/physics/collision/status.h>

namespace ludus::physics::rigid
{
using foundation::float32;
using foundation::float64;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// @brief Outcome of a PhysicsRigid operation.
///
/// Every failing value leaves the world, the caller's outputs and installed state unchanged, except where a Step
/// result states otherwise. Values shared with collision::Status keep their meaning; ToRigidStatus converts them.
enum class Status : uint8
{
    Success,               ///< The operation completed; its outputs were written.
    InvalidArgument,       ///< A view, count, enumerator, motion type or field combination violates the contract.
    NonFiniteInput,        ///< A position, orientation, velocity, force, mass or setting is NaN or infinite.
    OutOfRange,            ///< A finite value lies outside its documented envelope.
    InvalidHandle,         ///< A body or shape handle is null, malformed, stale, or from another world or store.
    InvalidPhase,          ///< The world is uninitialized, already initialized, or protected by a live read view.
    Unsupported,           ///< The request combines features this gate does not implement yet.
    CapacityExceeded,      ///< A body, collider, command or lease table admitted at initialization is full.
    AllocationFailure,     ///< Storage for the world's tables could not be allocated.
    IdentityExhausted,     ///< The process ran out of lifetime tokens for new worlds.
    InvalidMassProperties, ///< Mass, center of mass or inertia is nonpositive, unrealizable or ill-conditioned.
    DegenerateShape,       ///< A shape encloses no usable volume, so it cannot be a mass source.
    PastTick,              ///< A command targets a tick that has already been simulated.
    DuplicateCommand,      ///< A pending command already uses the same tick, producer and sequence key.
    Faulted,               ///< The world entered the Faulted phase; only diagnostics and destruction remain.
};

/// @brief Convert a PhysicsCollision status to the PhysicsRigid value of the same meaning.
///
/// Collision results that have no rigid counterpart (cooking and query outcomes) map to InvalidArgument.
[[nodiscard]] constexpr Status ToRigidStatus(collision::Status status) noexcept
{
    switch (status)
    {
        case collision::Status::Success:
            return Status::Success;
        case collision::Status::NonFiniteInput:
            return Status::NonFiniteInput;
        case collision::Status::OutOfRange:
            return Status::OutOfRange;
        case collision::Status::CapacityExceeded:
            return Status::CapacityExceeded;
        case collision::Status::AllocationFailure:
            return Status::AllocationFailure;
        case collision::Status::DegenerateShape:
            return Status::DegenerateShape;
        case collision::Status::InvalidHandle:
            return Status::InvalidHandle;
        case collision::Status::InvalidPhase:
            return Status::InvalidPhase;
        case collision::Status::Unsupported:
            return Status::Unsupported;
        case collision::Status::IdentityExhausted:
            return Status::IdentityExhausted;
        case collision::Status::InvalidArgument:
        case collision::Status::IndexOutOfRange:
        case collision::Status::DegenerateTriangle:
        case collision::Status::NoGeometry:
        case collision::Status::InitialOverlap:
        case collision::Status::NotConverged:
            return Status::InvalidArgument;
    }
    return Status::InvalidArgument;
}

/// Largest admitted magnitude of a linear velocity component, meters per second.
inline constexpr float64 MAX_LINEAR_SPEED_METERS_PER_SECOND = 1.0e5;

/// Largest admitted magnitude of an angular velocity component, radians per second.
inline constexpr float64 MAX_ANGULAR_SPEED_RADIANS_PER_SECOND = 1.0e5;

/// @brief Largest admitted magnitude of a force, torque or impulse component in a command.
///
/// Newtons, newton meters, newton seconds or newton meter seconds by field. The bound keeps every product of a
/// command with an admitted lever arm and inverse mass finite in float64.
inline constexpr float64 MAX_COMMAND_COMPONENT = 1.0e12;
} // namespace ludus::physics::rigid
