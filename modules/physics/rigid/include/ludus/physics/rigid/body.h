#pragma once

/// @file
/// @brief Rigid-body identities, descriptions and the plain-value state a RigidWorld exports every tick.
///
/// See "Bodies" in docs/architecture/physics-rigid.md and "Bodies, forces and materials" in
/// docs/architecture/physics-systems.md. World-space values carry the suffix `World` and body-frame values `Body`.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/identity/handle.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/rigid/mass_properties.h>
#include <ludus/physics/rigid/status.h>

namespace ludus::physics::rigid
{
/// Handle domain marker for bodies in a RigidWorld; never instantiated.
struct BodyTag final
{
};

/// @brief Reference to one body incarnation in one RigidWorld.
///
/// Destroying a body advances its slot's generation, so every copy of the handle becomes stale.
using BodyHandle = foundation::identity::Handle<BodyTag>;

/// Largest number of body slots a world admits.
inline constexpr uint32 MAX_RIGID_WORLD_BODIES = 1U << 20U;

/// Most colliders one body attaches.
inline constexpr uint32 MAX_BODY_COLLIDERS = MAX_MASS_PARTS;

/// Default collider density, kilograms per cubic meter: water.
inline constexpr float64 DEFAULT_DENSITY_KILOGRAMS_PER_CUBIC_METER = 1000.0;

/// Largest admitted magnitude of a body's gravity scale.
inline constexpr float64 MAX_GRAVITY_SCALE = 1.0e3;

/// @brief How a body moves.
enum class MotionType : uint8
{
    Static,    ///< Never integrated; moves only by TryTeleportBody.
    Kinematic, ///< Follows its velocity or tick-addressed targets; infinite mass, no force response.
    Dynamic,   ///< Finite positive mass and inertia; integrates gravity, forces and impulses.
};

/// @brief Where a dynamic body's mass properties come from.
enum class MassSource : uint8
{
    /// Compose the attached colliders that contribute mass. Leaves are summed additively: overlapping colliders
    /// count their shared volume twice.
    Colliders,
    /// Use BodyDescription::ExplicitMass, validated, for art-directed bodies or a separate mass proxy.
    Explicit,
};

/// @brief One collider attached to a body, posed in the body frame.
///
/// A collider contributes mass when ShouldContributeMass is true and it is not a sensor; its shape must then be a
/// sphere, box, capsule or hull, because triangle meshes are not closed mass sources.
struct BodyColliderDescription final
{
    collision::ShapeHandle Shape{};                   ///< Live shape of the world's store.
    BodyLocalPose Pose{};                             ///< Placement of the shape's local space in the body frame.
    collision::ColliderParticipation Participation{}; ///< Simulation and query participation.
    collision::CollisionFilter Filter{};              ///< Simulation categories and mask.
    /// Uniform density in (0, MAX_DENSITY_KILOGRAMS_PER_CUBIC_METER] when the collider contributes mass.
    float64 DensityKilogramsPerCubicMeter = DEFAULT_DENSITY_KILOGRAMS_PER_CUBIC_METER;
    bool ShouldContributeMass = true; ///< False marks a massless collider.
    /// Opaque surface material identifier. Contacts of colliders with different materials never share a patch, and
    /// changing it (RigidWorld::TrySetColliderMaterial) retires the collider's cached contacts. Friction and
    /// restitution values arrive with the solver.
    uint32 Material = 0;
    bool IsEnabled = true; ///< Disabled colliders take part in nothing.
    uint64 Binding = 0;    ///< Opaque game value copied into query results.
};

/// @brief Description of a new body.
///
/// Admitted values: a known motion type; a pose as for collision::RigidPose; velocity components finite and within
/// MAX_LINEAR_SPEED_METERS_PER_SECOND and MAX_ANGULAR_SPEED_RADIANS_PER_SECOND, and zero for a static body; at most
/// MAX_BODY_COLLIDERS colliders; a gravity scale within MAX_GRAVITY_SCALE. A dynamic body needs positive mass from
/// its MassSource. Static and kinematic bodies ignore MassSource and ExplicitMass.
struct BodyDescription final
{
    MotionType Motion = MotionType::Dynamic; ///< How the body moves.
    collision::RigidPose Pose{};             ///< Body frame in world space.
    /// COM velocity, meters per second. A kinematic body's COM is its body origin.
    foundation::math::Vector3d LinearVelocityWorld{};
    foundation::math::Vector3d AngularVelocityWorld{}; ///< Angular velocity in world axes, radians per second.
    // Explicit so designated initializers may omit it without -Wmissing-designated-field-initializers.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    foundation::BorrowedView<const BodyColliderDescription> Colliders{}; ///< Colliders, borrowed for the call.
    MassSource Mass = MassSource::Colliders;                             ///< Where dynamic mass comes from.
    MassProperties ExplicitMass{}; ///< Body-frame mass properties for MassSource::Explicit.
    float64 GravityScale = 1.0;    ///< Multiplier of the world's gravity for a dynamic body.
    uint64 Binding = 0;            ///< Opaque game value copied into exported state, for example an entity.
};

/// @brief Plain-value state of one body after the last completed tick.
///
/// Pose and PreviousPose are the body frame at the end and start of that tick; they are equal after creation and
/// teleport, which reset interpolation. Energy and momentum are zero for static and kinematic bodies.
struct BodyState final
{
    BodyHandle Body{};                                 ///< The body.
    uint64 Binding = 0;                                ///< The body's binding.
    MotionType Motion = MotionType::Static;            ///< The body's motion type.
    collision::RigidPose Pose{};                       ///< Current body frame in world space.
    collision::RigidPose PreviousPose{};               ///< Body frame one tick earlier, for interpolation.
    foundation::math::Vector3d CenterOfMassWorld{};    ///< COM position, meters.
    foundation::math::Vector3d LinearVelocityWorld{};  ///< COM velocity, meters per second.
    foundation::math::Vector3d AngularVelocityWorld{}; ///< Angular velocity, radians per second.
    float64 KineticEnergyJoules = 0.0;                 ///< Translational plus rotational kinetic energy.
    /// Angular momentum about the COM in world axes, kilogram square meters per second.
    foundation::math::Vector3d AngularMomentumWorld{};
};

/// @brief Mass frame of one body, in its body frame.
///
/// Static and kinematic bodies report zero mass and moments with an identity BodyFromCenterOfMass: their solver
/// mass is infinite and their COM is the body origin.
struct BodyMassInfo final
{
    MotionType Motion = MotionType::Static; ///< The body's motion type.
    PrincipalMassFrame Frame{};             ///< Mass, BodyFromCenterOfMass and principal moments.
};
} // namespace ludus::physics::rigid
