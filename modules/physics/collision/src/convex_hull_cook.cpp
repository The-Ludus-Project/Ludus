#include <ludus/physics/collision/convex_shape.h>

#include "internal/mesh_arithmetic.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision
{
namespace
{
namespace math = foundation::math;
using internal::GetComponent;
using internal::Widen;

constexpr float64 INTERIOR_WEIGHT = 0.25;

/// The four points that span the hull's volume, by index into the source.
struct HullExtremes final
{
    usize First = 0;
    usize Second = 0;
    usize Third = 0;
    usize Fourth = 0;
};

Status CheckPoint(math::Vector3 pointShape) noexcept
{
    if (math::IsFinite(pointShape) == false)
    {
        return Status::NonFiniteInput;
    }
    return internal::IsWithinMagnitude(Widen(pointShape), static_cast<float64>(MAX_COORDINATE_METERS)) == true
               ? Status::Success
               : Status::OutOfRange;
}

/// Find a spanning tetrahedron and test it against MIN_CONVEX_HULL_THICKNESS_RATIO. Ties keep the lowest index.
bool TryFindExtremes(foundation::BorrowedView<const math::Vector3> pointsShape, HullExtremes& outExtremes) noexcept
{
    const usize count = pointsShape.GetSize();
    HullExtremes extremes;
    float64 widestSpread = -1.0;
    for (uint32 axis = 0; axis < internal::AXIS_COUNT; ++axis)
    {
        usize minimum = 0;
        usize maximum = 0;
        for (usize index = 1; index < count; ++index)
        {
            const float64 value = GetComponent(pointsShape[index], axis);
            if (value < GetComponent(pointsShape[minimum], axis))
            {
                minimum = index;
            }
            if (value > GetComponent(pointsShape[maximum], axis))
            {
                maximum = index;
            }
        }
        const float64 spread = GetComponent(pointsShape[maximum], axis) - GetComponent(pointsShape[minimum], axis);
        if (spread > widestSpread)
        {
            widestSpread = spread;
            extremes.First = minimum;
            extremes.Second = maximum;
        }
    }
    const math::Vector3d firstShape = Widen(pointsShape[extremes.First]);
    const math::Vector3d lineShape = Widen(pointsShape[extremes.Second]) - firstShape;
    const float64 length = math::Length(lineShape);
    if (length <= 0.0)
    {
        return false;
    }
    const float64 minimumThickness = MIN_CONVEX_HULL_THICKNESS_RATIO * length;
    float64 farthestFromLine = 0.0;
    for (usize index = 0; index < count; ++index)
    {
        const float64 distance = math::Length(math::Cross(Widen(pointsShape[index]) - firstShape, lineShape)) / length;
        if (distance > farthestFromLine)
        {
            farthestFromLine = distance;
            extremes.Third = index;
        }
    }
    if (farthestFromLine <= minimumThickness)
    {
        return false;
    }
    math::Vector3d normalShape;
    if (internal::TryNormalize(math::Cross(lineShape, Widen(pointsShape[extremes.Third]) - firstShape), normalShape) ==
        false)
    {
        return false;
    }
    float64 farthestFromPlane = 0.0;
    for (usize index = 0; index < count; ++index)
    {
        const float64 distance = math::Abs(math::Dot(Widen(pointsShape[index]) - firstShape, normalShape));
        if (distance > farthestFromPlane)
        {
            farthestFromPlane = distance;
            extremes.Fourth = index;
        }
    }
    if (farthestFromPlane <= minimumThickness)
    {
        return false;
    }
    outExtremes = extremes;
    return true;
}
} // namespace

ConvexHullCookResult ConvexHull::TryCook(const ConvexHullSource& source, ConvexHull& outHull) noexcept
{
    const foundation::BorrowedView<const math::Vector3> pointsShape = source.PointsShape;
    if (pointsShape.IsValid() == false)
    {
        return { .Result = Status::InvalidArgument };
    }
    const usize count = pointsShape.GetSize();
    if (count == 0)
    {
        return { .Result = Status::NoGeometry };
    }
    if (count < MIN_CONVEX_HULL_POINTS)
    {
        return { .Result = Status::DegenerateShape };
    }
    if (count > MAX_CONVEX_HULL_POINTS)
    {
        return { .Result = Status::CapacityExceeded };
    }
    for (usize index = 0; index < count; ++index)
    {
        const Status pointStatus = CheckPoint(pointsShape[index]);
        if (pointStatus != Status::Success)
        {
            return { .Result = pointStatus, .PointIndex = static_cast<uint32>(index) };
        }
    }
    HullExtremes extremes;
    if (TryFindExtremes(pointsShape, extremes) == false)
    {
        return { .Result = Status::DegenerateShape };
    }
    foundation::Array<math::Vector3> points;
    if (points.TryEnsureCapacity(count) == false)
    {
        return { .Result = Status::AllocationFailure };
    }
    math::Aabb3 boundsShape = math::EmptyAabb3();
    for (const math::Vector3 pointShape : pointsShape)
    {
        // Capacity is reserved, so this cannot allocate; the check keeps the contract explicit.
        if (points.TryAdd(pointShape) == false)
        {
            return { .Result = Status::AllocationFailure };
        }
        boundsShape.Min = {math::Min(boundsShape.Min.X, pointShape.X),
                           math::Min(boundsShape.Min.Y, pointShape.Y),
                           math::Min(boundsShape.Min.Z, pointShape.Z)};
        boundsShape.Max = {math::Max(boundsShape.Max.X, pointShape.X),
                           math::Max(boundsShape.Max.Y, pointShape.Y),
                           math::Max(boundsShape.Max.Z, pointShape.Z)};
    }
    outHull.mInteriorPointShape = (Widen(pointsShape[extremes.First]) + Widen(pointsShape[extremes.Second]) +
                                   Widen(pointsShape[extremes.Third]) + Widen(pointsShape[extremes.Fourth])) *
                                  INTERIOR_WEIGHT;
    outHull.mBoundsShape = boundsShape;
    outHull.mPoints = foundation::Move(points);
    return {};
}
} // namespace ludus::physics::collision
