#pragma once

// Raycast validation shared by TriangleMesh and CollisionWorld, so world queries fail exactly as mesh queries do.

#include <ludus/foundation/base/core.h>

#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision
{
class TriangleMesh;
}

namespace ludus::physics::collision::internal
{
/// Validate a raycast: finiteness, then the unit direction, distance and enumerators, then the envelope.
[[nodiscard]] Status ValidateRaycast(const RaycastInput& input) noexcept;

/// Like TriangleMesh::RaycastClosest, but stops at the first hit the traversal finds: a hit, not the nearest.
[[nodiscard]] Status
RaycastAnyHit(const TriangleMesh& mesh, const RaycastInput& input, RaycastResult& outResult) noexcept;
} // namespace ludus::physics::collision::internal
