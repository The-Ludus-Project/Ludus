#pragma once

// The general convex cast on an already posed shape, shared by TriangleMesh and CollisionWorld.
//
// CollisionWorld composes a collider's rotation with the query shape's in
// float64 and casts the result in the collider's local space; this entry point
// takes those rotation columns directly instead of a float32 quaternion.

#include "convex_support.h"
#include "mesh_arithmetic.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision
{
class TriangleMesh;
}

namespace ludus::physics::collision::internal
{
/// Largest translation component the posed entry points admit. A collider's rotation can grow the largest
/// component of an admitted world translation by up to sqrt(3); every separating-axis and support product stays
/// finite at twice the public limit.
inline constexpr float64 MAX_LOCAL_TRANSLATION_METERS = 2.0 * MAX_QUERY_DISTANCE_METERS;

/// Bounds of a posed shape: centre relative to its position, and half extents including the margin.
struct PosedBounds final
{
    math::Vector3d CenterMesh{};
    math::Vector3d HalfExtentsMesh{};
};

/// A validated cast request with its shape already posed in mesh axes.
struct PosedConvexCast final
{
    PosedConvex Shape{};
    math::Vector3d PositionMesh{};
    math::Vector3d TranslationMesh{};
    InitialOverlapPolicy OverlapPolicy = InitialOverlapPolicy::Report;
    QueryTraversal Traversal = QueryTraversal::Hierarchy;
    /// Stop at the first admitted hit instead of finding the closest. Ignored under InitialOverlapPolicy::Reject,
    /// which must visit every candidate to find any overlap.
    bool ShouldStopAtFirstHit = false;
};

/// Validate a convex cast request: unknown kind or enumerator, finiteness, admitted values, then the envelope.
[[nodiscard]] Status ValidateConvexCast(const ConvexCastInput& input) noexcept;

/// Pose a validated shape with the rotation columns of `rotation`.
[[nodiscard]] PosedConvex PoseConvexShape(const ConvexShape& shape, const PosedConvex& rotation) noexcept;

/// Axis-aligned bounds of a posed shape in its mesh axes.
[[nodiscard]] PosedBounds GetPosedBounds(const PosedConvex& shape) noexcept;

/// Triangles a posed shape overlaps in one mesh.
struct MeshOverlap final
{
    uint32 TriangleCount = 0;                            ///< Overlapping triangles.
    uint32 LowestTriangleIndex = INVALID_TRIANGLE_INDEX; ///< Lowest overlapping source triangle.
};

/// Cast a posed shape. Only the position and translation envelope is checked (OutOfRange); the shape must be valid.
[[nodiscard]] Status
CastPosedConvex(const TriangleMesh& mesh, const PosedConvexCast& request, ConvexCastResult& outResult) noexcept;

/// Cast a posed shape against a convex target posed at the origin of the same frame. TriangleIndex stays invalid.
[[nodiscard]] Status CastPosedConvexAgainstConvex(const PosedConvex& target,
                                                  const PosedConvexCast& request,
                                                  ConvexCastResult& outResult) noexcept;

/// Find the triangles a stationary posed shape overlaps, by the same rule as a cast's initial overlap.
/// Only request.PositionMesh, request.Shape and request.Traversal are read.
[[nodiscard]] Status
OverlapPosedConvexWithMesh(const TriangleMesh& mesh, const PosedConvexCast& request, MeshOverlap& outOverlap) noexcept;

/// Whether a stationary posed shape overlaps a convex target posed at the origin, by the same rule.
/// Only request.PositionMesh and request.Shape are read.
[[nodiscard]] Status
IsOverlappingConvex(const PosedConvex& target, const PosedConvexCast& request, bool& outIsOverlapping) noexcept;
} // namespace ludus::physics::collision::internal
