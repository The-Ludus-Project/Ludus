#pragma once

// Private arithmetic shared by triangle-mesh cooking and queries. The module
// compiles with -ffp-contract=off: the watertight ray test needs the exact
// antisymmetry of a*b - c*d between neighbouring triangles, which a fused
// multiply-add would break, and the hierarchy and brute-force paths must run
// identical arithmetic.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision::internal
{
namespace math = foundation::math;

inline constexpr uint32 AXIS_COUNT = 3;
inline constexpr uint32 Z_AXIS = 2;

/// Relative slack added to every traversal bound. Rounding in the slab and
/// separating-axis arithmetic is a few float64 ulps (about 2^-50 relative);
/// 2^-40 relative to the query's scale keeps pruning conservative, so a pruned
/// node can never hold a nearer or tied hit, while visiting no measurable
/// number of extra nodes. Thanks to Thiago Ize, "Robust BVH Ray Traversal",
/// Journal of Computer Graphics Techniques 2(2), 2013, pages 12-27,
/// https://jcgt.org/published/0002/02/02/, for showing that slab bounds must
/// be widened by their rounding error; Ludus uses a looser, scale-aware bound.
inline constexpr float64 TRAVERSAL_RELATIVE_ALLOWANCE = 0x1p-40;

/// The three corners of one triangle in mesh space, in winding order.
struct TriangleCorners final
{
    math::Vector3 FirstMesh;
    math::Vector3 SecondMesh;
    math::Vector3 ThirdMesh;
};

[[nodiscard]] LUDUS_INLINE math::Vector3d Widen(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

[[nodiscard]] LUDUS_INLINE float64 GetComponent(math::Vector3d value, uint32 axis) noexcept
{
    return axis == 0 ? value.X : (axis == 1 ? value.Y : value.Z);
}

[[nodiscard]] LUDUS_INLINE float64 GetComponent(math::Vector3 value, uint32 axis) noexcept
{
    return static_cast<float64>(axis == 0 ? value.X : (axis == 1 ? value.Y : value.Z));
}

[[nodiscard]] LUDUS_INLINE float64 GetMaxMagnitude(math::Vector3d value) noexcept
{
    return math::Max(math::Abs(value.X), math::Max(math::Abs(value.Y), math::Abs(value.Z)));
}

/// Largest coordinate magnitude of a box; the scale its traversal slack is relative to.
[[nodiscard]] LUDUS_INLINE float64 GetMaxMagnitude(const math::Aabb3& bounds) noexcept
{
    return math::Max(GetMaxMagnitude(Widen(bounds.Min)), GetMaxMagnitude(Widen(bounds.Max)));
}

/// Whether every component is finite and within the coordinate envelope.
[[nodiscard]] LUDUS_INLINE bool IsWithinMagnitude(math::Vector3d value, float64 limit) noexcept
{
    return GetMaxMagnitude(value) <= limit;
}

/// Unit vector along a finite nonzero direction; false leaves the output unchanged.
[[nodiscard]] LUDUS_INLINE bool TryNormalize(math::Vector3d direction, math::Vector3d& outUnit) noexcept
{
    const float64 length = math::Length(direction);
    if (length <= 0 || math::IsFinite(length) == false)
    {
        return false;
    }
    outUnit = direction * (1 / length);
    return true;
}
} // namespace ludus::physics::collision::internal
