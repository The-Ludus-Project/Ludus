#pragma once

// Support mappings of posed convex query shapes, for the general convex cast.
//
// Every shape is a convex core plus a rounding margin: a sphere is a point and
// a capsule a segment, each rounded by its radius; boxes and hulls are their
// own cores with no margin. Cores are polytopes, so GJK on them terminates in
// finitely many steps instead of converging asymptotically on a curved
// surface. Positions are relative to the shape's starting position.

#include "mesh_arithmetic.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/convex_shape.h>

namespace ludus::physics::collision::internal
{
/// A convex shape with its rotation applied, in mesh axes and relative to its position.
struct PosedConvex final
{
    ConvexShapeKind Kind = ConvexShapeKind::Sphere;
    math::Vector3d AxisXMesh{1.0, 0.0, 0.0}; ///< Shape +X in mesh axes.
    math::Vector3d AxisYMesh{0.0, 1.0, 0.0}; ///< Shape +Y in mesh axes.
    math::Vector3d AxisZMesh{0.0, 0.0, 1.0}; ///< Shape +Z in mesh axes.
    math::Vector3d HalfExtentsShape{};       ///< Box half extents along the shape axes.
    float64 HalfHeightMeters = 0.0;          ///< Capsule half segment length along shape +Y.
    float64 MarginMeters = 0.0;              ///< Rounding radius of spheres and capsules; zero otherwise.
    const ConvexHull* HullOrNull = nullptr;  ///< Hull points for ConvexShapeKind::Hull.
    math::Vector3d InteriorMesh{};           ///< A point inside the core, relative to the position.
};

/// Express a mesh-axis direction in shape axes.
[[nodiscard]] LUDUS_INLINE math::Vector3d ToShapeAxes(const PosedConvex& shape, math::Vector3d directionMesh) noexcept
{
    return {math::Dot(directionMesh, shape.AxisXMesh),
            math::Dot(directionMesh, shape.AxisYMesh),
            math::Dot(directionMesh, shape.AxisZMesh)};
}

/// Express a shape-axis vector in mesh axes.
[[nodiscard]] LUDUS_INLINE math::Vector3d ToMeshAxes(const PosedConvex& shape, math::Vector3d valueShape) noexcept
{
    return shape.AxisXMesh * valueShape.X + shape.AxisYMesh * valueShape.Y + shape.AxisZMesh * valueShape.Z;
}

/// The core point farthest along directionMesh, relative to the position. Ties pick the positive half extent, the
/// upper capsule end or the lowest hull point index, so the result is deterministic.
[[nodiscard]] LUDUS_INLINE math::Vector3d GetCoreSupport(const PosedConvex& shape,
                                                         math::Vector3d directionMesh) noexcept
{
    switch (shape.Kind)
    {
        case ConvexShapeKind::Sphere:
            return {};
        case ConvexShapeKind::Capsule:
            return shape.AxisYMesh * (math::Dot(directionMesh, shape.AxisYMesh) >= 0.0 ? shape.HalfHeightMeters
                                                                                       : -shape.HalfHeightMeters);
        case ConvexShapeKind::Box: {
            const math::Vector3d directionShape = ToShapeAxes(shape, directionMesh);
            const math::Vector3d cornerShape{
                directionShape.X >= 0.0 ? shape.HalfExtentsShape.X : -shape.HalfExtentsShape.X,
                directionShape.Y >= 0.0 ? shape.HalfExtentsShape.Y : -shape.HalfExtentsShape.Y,
                directionShape.Z >= 0.0 ? shape.HalfExtentsShape.Z : -shape.HalfExtentsShape.Z};
            return ToMeshAxes(shape, cornerShape);
        }
        case ConvexShapeKind::Hull: {
            const math::Vector3d directionShape = ToShapeAxes(shape, directionMesh);
            const foundation::BorrowedView<const math::Vector3> pointsShape = shape.HullOrNull->GetPointsShape();
            math::Vector3d bestShape = Widen(pointsShape[0]);
            float64 bestProjection = math::Dot(bestShape, directionShape);
            for (usize index = 1; index < pointsShape.GetSize(); ++index)
            {
                const math::Vector3d pointShape = Widen(pointsShape[index]);
                const float64 projection = math::Dot(pointShape, directionShape);
                if (projection > bestProjection)
                {
                    bestProjection = projection;
                    bestShape = pointShape;
                }
            }
            return ToMeshAxes(shape, bestShape);
        }
    }
    return {};
}

/// Rotation columns of a quaternion renormalized in float64; false for a zero or non-finite quaternion.
[[nodiscard]] LUDUS_INLINE bool TrySetRotation(math::Quaternion orientation, PosedConvex& inoutShape) noexcept
{
    const float64 x = static_cast<float64>(orientation.X);
    const float64 y = static_cast<float64>(orientation.Y);
    const float64 z = static_cast<float64>(orientation.Z);
    const float64 w = static_cast<float64>(orientation.W);
    const float64 normSquared = x * x + y * y + z * z + w * w;
    if (normSquared <= 0.0 || math::IsFinite(normSquared) == false)
    {
        return false;
    }
    const float64 scale = 2.0 / normSquared;
    const float64 xx = x * x * scale;
    const float64 yy = y * y * scale;
    const float64 zz = z * z * scale;
    const float64 xy = x * y * scale;
    const float64 xz = x * z * scale;
    const float64 yz = y * z * scale;
    const float64 wx = w * x * scale;
    const float64 wy = w * y * scale;
    const float64 wz = w * z * scale;
    inoutShape.AxisXMesh = {1.0 - (yy + zz), xy + wz, xz - wy};
    inoutShape.AxisYMesh = {xy - wz, 1.0 - (xx + zz), yz + wx};
    inoutShape.AxisZMesh = {xz + wy, yz - wx, 1.0 - (xx + yy)};
    return true;
}
} // namespace ludus::physics::collision::internal
