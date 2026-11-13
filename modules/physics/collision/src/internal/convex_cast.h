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
};

/// Validate a convex cast request: unknown kind or enumerator, finiteness, admitted values, then the envelope.
[[nodiscard]] Status ValidateConvexCast(const ConvexCastInput& input) noexcept;

/// Pose a validated shape with the rotation columns of `rotation`.
[[nodiscard]] PosedConvex PoseConvexShape(const ConvexShape& shape, const PosedConvex& rotation) noexcept;

/// Axis-aligned bounds of a posed shape in its mesh axes.
[[nodiscard]] PosedBounds GetPosedBounds(const PosedConvex& shape) noexcept;

/// Cast a posed shape. Only the position and translation envelope is checked (OutOfRange); the shape must be valid.
[[nodiscard]] Status
CastPosedConvex(const TriangleMesh& mesh, const PosedConvexCast& request, ConvexCastResult& outResult) noexcept;
} // namespace ludus::physics::collision::internal
