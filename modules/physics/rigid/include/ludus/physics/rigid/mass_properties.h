#pragma once

/// @file
/// @brief Mass, center of mass and inertia: analytic primitives, hull volume integrals, composition and validation.
///
/// Mass is in kilograms and inertia in kilogram square meters about the center of mass (COM). See "Mass
/// properties" in docs/architecture/physics-rigid.md and "Shapes, cooking and mass properties" in
/// docs/architecture/physics-systems.md.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/rigid/status.h>

namespace ludus::physics::rigid
{
/// Most parts TryComposeMassProperties combines, and the most colliders one body may attach.
inline constexpr uint32 MAX_MASS_PARTS = 64;

/// Largest admitted density, kilograms per cubic meter. Osmium, the densest element, is about 22 590.
inline constexpr float64 MAX_DENSITY_KILOGRAMS_PER_CUBIC_METER = 1.0e6;

/// Smallest admitted mass of one part or body, kilograms: one milligram. Lighter bodies amplify impulses beyond the
/// admitted velocity envelope.
inline constexpr float64 MIN_MASS_KILOGRAMS = 1.0e-6;

/// Largest admitted mass of one part or body, kilograms.
inline constexpr float64 MAX_MASS_KILOGRAMS = 1.0e12;

/// @brief Smallest admitted ratio of the smallest to the largest principal moment of inertia.
///
/// A thinner tensor is rejected as ill-conditioned: its inverse would amplify rounding by more than the reciprocal.
inline constexpr float64 MIN_INERTIA_CONDITION_RATIO = 1.0e-6;

/// @brief Relative slack of the triangle inequality between principal moments.
///
/// A realizable rigid body has each principal moment at most the sum of the other two; equality is a flat body.
/// Rounding of an exact flat or rod-like source may exceed the sum by this fraction of it.
inline constexpr float64 INERTIA_TRIANGLE_RELATIVE_TOLERANCE = 1.0e-9;

/// @brief Symmetric inertia tensor in kilogram square meters, in stated axes about a stated point.
///
/// The diagonal holds the moments of inertia; the off-diagonal fields hold the tensor elements, which are the
/// negated products of inertia: `Xy = -integral(x * y dm)`.
struct InertiaTensor final
{
    float64 Xx = 0.0; ///< Moment about the X axis.
    float64 Yy = 0.0; ///< Moment about the Y axis.
    float64 Zz = 0.0; ///< Moment about the Z axis.
    float64 Xy = 0.0; ///< Tensor element (X, Y), equal to (Y, X).
    float64 Xz = 0.0; ///< Tensor element (X, Z), equal to (Z, X).
    float64 Yz = 0.0; ///< Tensor element (Y, Z), equal to (Z, Y).
};

/// @brief Mass properties of an object in its own frame (a shape's space, a part's frame or a body frame).
///
/// Admitted by TryValidateMassProperties: a finite mass in [MIN_MASS_KILOGRAMS, MAX_MASS_KILOGRAMS]; a finite COM
/// within collision::MAX_COORDINATE_METERS; and a finite inertia whose principal moments are positive, obey the
/// triangle inequality within INERTIA_TRIANGLE_RELATIVE_TOLERANCE and stay above MIN_INERTIA_CONDITION_RATIO of the
/// largest.
struct MassProperties final
{
    float64 MassKilograms = 0.0;                    ///< Total mass, kilograms.
    foundation::math::Vector3d CenterOfMassLocal{}; ///< COM in the object's frame, meters.
    InertiaTensor InertiaAboutCenterOfMass{};       ///< Inertia about the COM, in the object's axes.
};

/// @brief Rigid placement of a part or collider frame in its body frame: rotate, then translate.
///
/// Admitted values: finite position components within collision::MAX_COORDINATE_METERS; a finite orientation
/// whose squared norm is within collision::UNIT_QUATERNION_SQUARED_TOLERANCE of one (renormalized in float64).
struct BodyLocalPose final
{
    foundation::math::Vector3d PositionBody{};      ///< Origin of the local frame in the body frame, meters.
    foundation::math::Quaternion OrientationBody{}; ///< Rotation from local axes to body axes.
};

/// @brief One part of a compound: its mass properties in its own frame and that frame's placement in the body.
struct MassPart final
{
    MassProperties Properties{}; ///< Admitted mass properties in the part frame.
    BodyLocalPose Pose{};        ///< Placement of the part frame in the body frame.
};

/// @brief Principal decomposition of admitted mass properties.
///
/// `BodyFromCenterOfMass` maps the principal COM frame into the body frame: its position is the COM and its
/// rotation's columns are the principal axes, right-handed. The inertia about the COM is diagonal in that frame
/// with the moments in PrincipalMoments, ordered by axis X, Y, Z of the principal frame.
struct PrincipalMassFrame final
{
    float64 MassKilograms = 0.0;                                        ///< Total mass, kilograms.
    BodyLocalPose BodyFromCenterOfMass{};                               ///< COM frame in the body frame.
    foundation::math::Vector3d PrincipalMomentsKilogramMetersSquared{}; ///< Diagonal inertia in the COM frame.
};

/// @brief Compute the mass properties of a solid convex shape of uniform density, in the shape's space.
///
/// Spheres, boxes and capsules are integrated analytically. A hull is triangulated from its points by an
/// incremental convex hull and integrated exactly over tetrahedra in float64; see docs/architecture/physics-rigid.md.
/// @param shape A sphere, box, capsule or cooked hull with admitted ConvexShape dimensions; a hull is read only
/// during this call.
/// @param densityKilogramsPerCubicMeter Density in (0, MAX_DENSITY_KILOGRAMS_PER_CUBIC_METER].
/// @param outProperties Receives the properties on Success; unchanged on failure.
/// @return Success; NonFiniteInput or OutOfRange for a dimension or density; InvalidArgument for a null or
/// uncooked hull, an unknown kind or a nonpositive density; DegenerateShape when the hull triangulation encloses no
/// volume; or InvalidMassProperties when the result fails TryValidateMassProperties.
[[nodiscard]] Status TryComputeShapeMassProperties(const collision::ConvexShape& shape,
                                                   float64 densityKilogramsPerCubicMeter,
                                                   MassProperties& outProperties) noexcept;

/// @brief Compose disjoint parts into one body's mass properties with rotated tensors and the parallel-axis theorem.
///
/// Parts are summed additively: overlapping parts count their shared volume twice. Supply explicit mass properties
/// from a separate mass proxy when parts overlap.
/// @param parts One to MAX_MASS_PARTS admitted parts, borrowed for this call.
/// @param outProperties Receives the body-frame properties on Success; unchanged on failure.
/// @return Success; InvalidArgument for an invalid or empty view; CapacityExceeded above MAX_MASS_PARTS;
/// NonFiniteInput, OutOfRange or InvalidArgument for a part pose; or InvalidMassProperties for a part or result that
/// fails TryValidateMassProperties.
[[nodiscard]] Status TryComposeMassProperties(foundation::BorrowedView<const MassPart> parts,
                                              MassProperties& outProperties) noexcept;

/// @brief Check mass properties against the admitted values documented on MassProperties.
/// @return Success; NonFiniteInput for a nonfinite field; OutOfRange for a COM outside the coordinate envelope; or
/// InvalidMassProperties for a mass or tensor outside the admitted set.
[[nodiscard]] Status TryValidateMassProperties(const MassProperties& properties) noexcept;

/// @brief Validate mass properties and diagonalize their inertia.
/// @param properties Mass properties in a body frame.
/// @param outFrame Receives the principal frame on Success; unchanged on failure.
/// @return Success, or the TryValidateMassProperties failure.
[[nodiscard]] Status TryComputePrincipalMassFrame(const MassProperties& properties,
                                                  PrincipalMassFrame& outFrame) noexcept;
} // namespace ludus::physics::rigid
