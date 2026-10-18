#pragma once

// Double-precision affine helpers over math::Matrix4d (column-major,
// Elements[4 * column + row]) for the pbrt graphics state.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <span>

namespace ludus::content::scene_cook::importers
{
namespace math = foundation::math;
using foundation::core::float64;

/// left x right (right applied first).
[[nodiscard]] math::Matrix4d MultiplyMatrices(const math::Matrix4d& left, const math::Matrix4d& right) noexcept;
[[nodiscard]] math::Matrix4d MakeTranslation(const math::Vector3d& offset) noexcept;
[[nodiscard]] math::Matrix4d MakeScale(const math::Vector3d& factors) noexcept;
/// Rotation by angleDegrees about axis (x, y, z); false for a zero axis.
[[nodiscard]] bool MakeRotation(float64 angleDegrees, const math::Vector3d& axis, math::Matrix4d& outMatrix) noexcept;
/// pbrt LookAt (eye, look, up): camera-from-world; false when degenerate.
[[nodiscard]] bool MakeLookAt(std::span<const float64> values, math::Matrix4d& outMatrix) noexcept;
/// Inverse of an affine matrix (last row 0 0 0 1 assumed); false when singular.
[[nodiscard]] bool TryInvertAffine(const math::Matrix4d& matrix, math::Matrix4d& outInverse) noexcept;
/// Determinant of the upper-left 3x3 block.
[[nodiscard]] float64 ComputeLinearDeterminant(const math::Matrix4d& matrix) noexcept;
[[nodiscard]] math::Vector3d TransformPoint(const math::Matrix4d& matrix, math::Vector3d point) noexcept;
[[nodiscard]] math::Vector3d TransformDirection(const math::Matrix4d& matrix, math::Vector3d direction) noexcept;
} // namespace ludus::content::scene_cook::importers
