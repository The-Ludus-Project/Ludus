#pragma once

// Validation of ConvexShape values, shared by convex casts and the geometry store.

#include "mesh_arithmetic.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision::internal
{
[[nodiscard]] LUDUS_INLINE bool IsKnownKind(ConvexShapeKind kind) noexcept
{
    return kind == ConvexShapeKind::Sphere || kind == ConvexShapeKind::Box || kind == ConvexShapeKind::Capsule ||
           kind == ConvexShapeKind::Hull;
}

[[nodiscard]] LUDUS_INLINE bool AreShapeFieldsFinite(const ConvexShape& shape) noexcept
{
    switch (shape.Kind)
    {
        case ConvexShapeKind::Sphere:
            return math::IsFinite(shape.RadiusMeters) == true;
        case ConvexShapeKind::Capsule:
            return math::IsFinite(shape.RadiusMeters) == true && math::IsFinite(shape.HalfHeightMeters) == true;
        case ConvexShapeKind::Box:
            return math::IsFinite(shape.HalfExtentsMeters) == true;
        case ConvexShapeKind::Hull:
            return true;
    }
    return false;
}

[[nodiscard]] LUDUS_INLINE bool AreShapeFieldsAdmitted(const ConvexShape& shape) noexcept
{
    switch (shape.Kind)
    {
        case ConvexShapeKind::Sphere:
            return shape.RadiusMeters > 0.0;
        case ConvexShapeKind::Capsule:
            return shape.RadiusMeters > 0.0 && shape.HalfHeightMeters >= 0.0;
        case ConvexShapeKind::Box:
            return shape.HalfExtentsMeters.X > 0.0 && shape.HalfExtentsMeters.Y > 0.0 &&
                   shape.HalfExtentsMeters.Z > 0.0;
        case ConvexShapeKind::Hull:
            return shape.HullOrNull != nullptr && shape.HullOrNull->IsCooked() == true;
    }
    return false;
}

[[nodiscard]] LUDUS_INLINE bool AreShapeFieldsInRange(const ConvexShape& shape) noexcept
{
    switch (shape.Kind)
    {
        case ConvexShapeKind::Sphere:
            return shape.RadiusMeters <= MAX_QUERY_DISTANCE_METERS;
        case ConvexShapeKind::Capsule:
            return shape.RadiusMeters <= MAX_QUERY_DISTANCE_METERS &&
                   shape.HalfHeightMeters <= MAX_QUERY_DISTANCE_METERS;
        case ConvexShapeKind::Box:
            return internal::IsWithinMagnitude(shape.HalfExtentsMeters, MAX_QUERY_DISTANCE_METERS) == true;
        case ConvexShapeKind::Hull:
            // Cooked hull points already lie within MAX_COORDINATE_METERS.
            return true;
    }
    return false;
}

/// Validate a shape value alone: unknown kind, then finiteness, then admitted values, then range.
[[nodiscard]] LUDUS_INLINE Status ValidateConvexShape(const ConvexShape& shape) noexcept
{
    if (IsKnownKind(shape.Kind) == false)
    {
        return Status::InvalidArgument;
    }
    if (AreShapeFieldsFinite(shape) == false)
    {
        return Status::NonFiniteInput;
    }
    if (AreShapeFieldsAdmitted(shape) == false)
    {
        return Status::InvalidArgument;
    }
    return AreShapeFieldsInRange(shape) == true ? Status::Success : Status::OutOfRange;
}
} // namespace ludus::physics::collision::internal
