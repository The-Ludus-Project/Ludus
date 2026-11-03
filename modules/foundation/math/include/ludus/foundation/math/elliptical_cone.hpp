#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::math
{
/// Finite elliptical conic frustum, including cylinders and rays. All distances use metres.
/// At axial distance z, the principal radii are RadiusWorld + z * Slope and that radius * MinorToMajor.
/// DirectionWorld and AxisXWorld must be orthogonal unit vectors; +Y is Cross(DirectionWorld, AxisXWorld).
struct EllipticalCone final
{
    Vector3d OriginWorld{};           ///< Centre of the ellipse at axial distance zero.
    Vector3d DirectionWorld{0, 0, 1}; ///< Unit propagation direction.
    Vector3d AxisXWorld{1, 0, 0};     ///< Unit major axis, perpendicular to propagation.
    float64 RadiusWorld = 0;          ///< Nonnegative initial major radius; zero permits an apex at the origin.
    float64 Slope = 0;                ///< Nonnegative tangent of the major opening half-angle; zero permits a cylinder.
    float64 MinorToMajor = 1;         ///< Minor/major radius ratio, in (0,1]. Flat, zero-width ellipses are rejected.
    float64 NearWorld = 0;            ///< Inclusive nonnegative near axial clip distance.
    float64 FarWorld = 0;             ///< Inclusive finite far axial clip distance, at least NearWorld.
};

/// A triangle in world space, with no ownership, allocation or scene identity.
struct ConeTriangle final
{
    Vector3d FirstWorld{};  ///< First corner; barycentric (1,0,0).
    Vector3d SecondWorld{}; ///< Second corner; barycentric (0,1,0).
    Vector3d ThirdWorld{};  ///< Third corner; barycentric (0,0,1).
};

/// Geometric cone/triangle range with axial roundoff padding; zero fields for a miss.
struct ConeTriangleIntersection final
{
    bool IsHit = false;    ///< Whether the triangle and the closed conic frustum intersect.
    float64 NearWorld = 0; ///< Lower axial distance, in metres, padded by eight working-scale epsilons and clipped; not
                           ///< radial distance.
    float64 FarWorld = 0;  ///< Upper axial distance, in metres, with the same padding and clipping.
    Vector3d NearBarycentric{}; ///< Barycentric weights at the unpadded nearest candidate point, summing to one.
    Vector3d FarBarycentric{};  ///< Barycentric weights at the unpadded farthest candidate point, summing to one.
};

/// Intersect an elliptical conic frustum with a two-sided triangle, without allocation.
/// Computes candidates on the triangle vertices, edges, cone/plane section and clip planes.
/// @return False for invalid/nonfinite inputs or an unrepresentable intermediate; preserves outIntersection.
/// A valid degenerate triangle is a miss. A valid miss returns true and clears outIntersection.
/// @note CPU uses float64; the matching Slang primitive uses float32. Grazing/tangent inputs need tolerance-aware
/// comparisons.
[[nodiscard]] bool TryIntersectEllipticalConeTriangle(const EllipticalCone& cone,
                                                      const ConeTriangle& triangle,
                                                      ConeTriangleIntersection& outIntersection) noexcept;

/// Conservatively test a world-space AABB against the cone, for BVH pruning.
/// @return False for invalid cone/bounds; preserves outMayIntersect. Otherwise writes the overlap possibility.
/// Uses axial clips and eight supporting side planes. False positives are permitted; false negatives are not intended.
/// No scene traversal, allocation or shared state. Bounds with any minimum greater than maximum are empty.
[[nodiscard]] bool TryTestEllipticalConeBounds(const EllipticalCone& cone,
                                               Vector3d minimumWorld,
                                               Vector3d maximumWorld,
                                               bool& outMayIntersect) noexcept;
} // namespace ludus::foundation::math
