#pragma once

// Raycast validation shared by TriangleMesh and CollisionWorld, so world queries fail exactly as mesh queries do.

#include <ludus/foundation/base/core.h>

#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision::internal
{
/// Validate a raycast: finiteness, then the unit direction, distance and enumerators, then the envelope.
[[nodiscard]] Status ValidateRaycast(const RaycastInput& input) noexcept;
} // namespace ludus::physics::collision::internal
