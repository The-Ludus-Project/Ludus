#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/rigid/mass_properties.h>
#include <ludus/physics/rigid/status.h>

namespace ludus::physics::rigid::internal
{
/// Unvalidated mass properties of the convex hull of `pointsShape` at unit density, in shape space.
struct UnitDensityMass final
{
    float64 VolumeCubicMeters = 0.0;
    foundation::math::Vector3d CenterOfMassShape{};
    /// Inertia tensor about the COM for a density of one kilogram per cubic meter.
    InertiaTensor UnitInertia{};
};

/// @brief Triangulate the hull of a cooked hull's points and integrate its volume, COM and inertia in float64.
///
/// Uses no heap storage: the triangulation lives in fixed stack scratch sized for MAX_CONVEX_HULL_POINTS.
/// @return Success, or DegenerateShape when the points enclose no volume or the triangulation loses its closed
/// surface through rounding. `outMass` is unchanged on failure.
[[nodiscard]] Status ComputeHullUnitMass(foundation::BorrowedView<const foundation::math::Vector3> pointsShape,
                                         UnitDensityMass& outMass) noexcept;
} // namespace ludus::physics::rigid::internal
