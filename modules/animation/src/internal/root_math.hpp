#pragma once

// Private float64 rigid-transform arithmetic for root trajectories. Never installed.
//
// Root deltas compose inverse(R(old)) * cycleEnd^n * R(new), following
// docs/architecture/skeletal-animation.md "Root motion and locomotion quality". Rotation and
// translation do not commute, so deltas are composed as rigid transforms in order, never by
// subtracting positions across a wrap.

#include <ludus/foundation/base/types.h>

#include <ludus/animation/clip.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::animation::detail
{
/// Hamilton quaternion in float64, stored (x, y, z, w).
struct QuaternionD final
{
    foundation::float64 X = 0;
    foundation::float64 Y = 0;
    foundation::float64 Z = 0;
    foundation::float64 W = 1;
};

/// Rigid transform in float64: rotate by Rotation, then translate.
struct RigidTransformD final
{
    foundation::math::Vector3d Translation{};
    QuaternionD Rotation{};
};

[[nodiscard]] RigidTransformD ConvertToRigid(const RootTransform& value) noexcept;

/// Round a float64 rigid transform to float32, renormalizing the rotation.
/// @return false when a component is not representable as a finite float32 or the rotation degenerates.
[[nodiscard]] bool TryConvertToRootTransform(const RigidTransformD& value, RootTransform& outValue) noexcept;

/// left * right: apply right first, then left. Renormalizes the rotation.
[[nodiscard]] RigidTransformD Compose(const RigidTransformD& left, const RigidTransformD& right) noexcept;

/// Inverse of a rigid transform with a unit rotation.
[[nodiscard]] RigidTransformD Invert(const RigidTransformD& value) noexcept;

/// value^exponent by repeated squaring (negative exponents use the inverse); O(log |exponent|).
[[nodiscard]] RigidTransformD RaiseToPower(const RigidTransformD& value, foundation::int64 exponent) noexcept;

/// Whether every component is finite.
[[nodiscard]] bool IsFinite(const RigidTransformD& value) noexcept;
} // namespace ludus::animation::detail
