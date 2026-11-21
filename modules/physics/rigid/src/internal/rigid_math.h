#pragma once

// Private float64 rotation and 3x3 matrix arithmetic for mass frames and integration. Public values use
// FoundationMath's float32 Quaternion; motion state keeps float64 so that integration and mass composition do not
// round through float32 between substeps.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/rigid/mass_properties.h>
#include <ludus/physics/rigid/status.h>

namespace ludus::physics::rigid::internal
{
namespace math = foundation::math;

inline constexpr uint32 AXIS_COUNT = 3;
inline constexpr uint32 X_AXIS = 0;
inline constexpr uint32 Y_AXIS = 1;
inline constexpr uint32 Z_AXIS = 2;

/// Unit quaternion in float64, Hamilton convention and component order of math::Quaternion.
struct Rotation final
{
    float64 X = 0.0;
    float64 Y = 0.0;
    float64 Z = 0.0;
    float64 W = 1.0;
};

/// Column-major 3x3 matrix in float64, acting on column vectors.
struct Matrix3x3 final
{
    math::Vector3d ColumnX{};
    math::Vector3d ColumnY{};
    math::Vector3d ColumnZ{};
};

/// Eigen decomposition of a symmetric matrix: `M = Vectors * diag(Values) * Transpose(Vectors)`.
struct SymmetricEigen final
{
    math::Vector3d Values{};
    Matrix3x3 Vectors{}; ///< Orthonormal, right-handed columns.
};

[[nodiscard]] LUDUS_INLINE float64 GetComponent(math::Vector3d value, uint32 axis) noexcept
{
    return axis == X_AXIS ? value.X : (axis == Y_AXIS ? value.Y : value.Z);
}

[[nodiscard]] LUDUS_INLINE float64 GetMaxMagnitude(math::Vector3d value) noexcept
{
    return math::Max(math::Abs(value.X), math::Max(math::Abs(value.Y), math::Abs(value.Z)));
}

[[nodiscard]] LUDUS_INLINE Rotation Widen(math::Quaternion orientation) noexcept
{
    return {static_cast<float64>(orientation.X),
            static_cast<float64>(orientation.Y),
            static_cast<float64>(orientation.Z),
            static_cast<float64>(orientation.W)};
}

[[nodiscard]] LUDUS_INLINE math::Quaternion Narrow(Rotation rotation) noexcept
{
    return {static_cast<float32>(rotation.X),
            static_cast<float32>(rotation.Y),
            static_cast<float32>(rotation.Z),
            static_cast<float32>(rotation.W)};
}

/// Hamilton product: apply `second`, then `first`.
[[nodiscard]] LUDUS_INLINE Rotation Multiply(Rotation first, Rotation second) noexcept
{
    return {first.W * second.X + first.X * second.W + first.Y * second.Z - first.Z * second.Y,
            first.W * second.Y - first.X * second.Z + first.Y * second.W + first.Z * second.X,
            first.W * second.Z + first.X * second.Y - first.Y * second.X + first.Z * second.W,
            first.W * second.W - first.X * second.X - first.Y * second.Y - first.Z * second.Z};
}

[[nodiscard]] LUDUS_INLINE Rotation Conjugate(Rotation rotation) noexcept
{
    return {-rotation.X, -rotation.Y, -rotation.Z, rotation.W};
}

[[nodiscard]] LUDUS_INLINE float64 GetNormSquared(Rotation rotation) noexcept
{
    return rotation.X * rotation.X + rotation.Y * rotation.Y + rotation.Z * rotation.Z + rotation.W * rotation.W;
}

/// Rescale to unit norm. @pre The norm is positive and finite.
[[nodiscard]] Rotation Normalize(Rotation rotation) noexcept;

/// Rotate a vector by a unit rotation.
[[nodiscard]] math::Vector3d Rotate(Rotation rotation, math::Vector3d value) noexcept;

/// Rotation matrix of a unit rotation.
[[nodiscard]] Matrix3x3 ToMatrix(Rotation rotation) noexcept;

/// Unit rotation of an orthonormal right-handed matrix, by Shepperd's largest-component selection.
[[nodiscard]] Rotation FromMatrix(const Matrix3x3& matrix) noexcept;

[[nodiscard]] LUDUS_INLINE math::Vector3d Multiply(const Matrix3x3& matrix, math::Vector3d value) noexcept
{
    return matrix.ColumnX * value.X + matrix.ColumnY * value.Y + matrix.ColumnZ * value.Z;
}

[[nodiscard]] LUDUS_INLINE Matrix3x3 Multiply(const Matrix3x3& first, const Matrix3x3& second) noexcept
{
    return {Multiply(first, second.ColumnX), Multiply(first, second.ColumnY), Multiply(first, second.ColumnZ)};
}

[[nodiscard]] LUDUS_INLINE Matrix3x3 Transpose(const Matrix3x3& matrix) noexcept
{
    return {{matrix.ColumnX.X, matrix.ColumnY.X, matrix.ColumnZ.X},
            {matrix.ColumnX.Y, matrix.ColumnY.Y, matrix.ColumnZ.Y},
            {matrix.ColumnX.Z, matrix.ColumnY.Z, matrix.ColumnZ.Z}};
}

[[nodiscard]] LUDUS_INLINE Matrix3x3 Add(const Matrix3x3& first, const Matrix3x3& second) noexcept
{
    return {first.ColumnX + second.ColumnX, first.ColumnY + second.ColumnY, first.ColumnZ + second.ColumnZ};
}

[[nodiscard]] LUDUS_INLINE Matrix3x3 Scale(const Matrix3x3& matrix, float64 factor) noexcept
{
    return {matrix.ColumnX * factor, matrix.ColumnY * factor, matrix.ColumnZ * factor};
}

[[nodiscard]] LUDUS_INLINE Matrix3x3 MakeDiagonal(math::Vector3d diagonal) noexcept
{
    return {{diagonal.X, 0.0, 0.0}, {0.0, diagonal.Y, 0.0}, {0.0, 0.0, diagonal.Z}};
}

/// Outer product `first * Transpose(second)`.
[[nodiscard]] LUDUS_INLINE Matrix3x3 MakeOuterProduct(math::Vector3d first, math::Vector3d second) noexcept
{
    return {first * second.X, first * second.Y, first * second.Z};
}

/// Cross-product matrix: `MakeSkew(a) * b == Cross(a, b)`.
[[nodiscard]] LUDUS_INLINE Matrix3x3 MakeSkew(math::Vector3d value) noexcept
{
    return {{0.0, value.Z, -value.Y}, {-value.Z, 0.0, value.X}, {value.Y, -value.X, 0.0}};
}

/// Rotate `rotation` by the constant world angular velocity `angularVelocity` for `seconds`, exactly.
[[nodiscard]] Rotation IntegrateRotation(Rotation rotation, math::Vector3d angularVelocity, float64 seconds) noexcept;

/// Angular velocity, in world axes, that turns `from` into `to` along the shortest arc in `seconds`.
[[nodiscard]] math::Vector3d GetShortestArcVelocity(Rotation from, Rotation to, float64 seconds) noexcept;

[[nodiscard]] LUDUS_INLINE float64 GetTrace(const Matrix3x3& matrix) noexcept
{
    return matrix.ColumnX.X + matrix.ColumnY.Y + matrix.ColumnZ.Z;
}

/// `rotation * matrix * Transpose(rotation)`: a tensor expressed in rotated axes.
[[nodiscard]] LUDUS_INLINE Matrix3x3 RotateTensor(const Matrix3x3& rotation, const Matrix3x3& tensor) noexcept
{
    return Multiply(Multiply(rotation, tensor), Transpose(rotation));
}

/// Solve `matrix * solution = value` by Cramer's rule. Returns false for a determinant that is not a positive
/// fraction of the matrix scale, leaving `outSolution` unchanged.
[[nodiscard]] bool TrySolve(const Matrix3x3& matrix, math::Vector3d value, math::Vector3d& outSolution) noexcept;

[[nodiscard]] LUDUS_INLINE Matrix3x3 FromTensor(const InertiaTensor& tensor) noexcept
{
    return {{tensor.Xx, tensor.Xy, tensor.Xz}, {tensor.Xy, tensor.Yy, tensor.Yz}, {tensor.Xz, tensor.Yz, tensor.Zz}};
}

/// The symmetric part of `matrix` as a tensor.
[[nodiscard]] InertiaTensor ToTensor(const Matrix3x3& matrix) noexcept;

/// Diagonalize a finite symmetric matrix by cyclic Jacobi rotations. Returns false when the sweeps do not converge.
[[nodiscard]] bool TryDecomposeSymmetric(const Matrix3x3& matrix, SymmetricEigen& outEigen) noexcept;

/// Validate a public orientation: finite, with squared norm within collision::UNIT_QUATERNION_SQUARED_TOLERANCE.
[[nodiscard]] Status ValidateOrientation(math::Quaternion orientation) noexcept;

/// Validate a position: finite, every component within collision::MAX_COORDINATE_METERS.
[[nodiscard]] Status ValidatePosition(math::Vector3d position) noexcept;

/// Validate a body-local pose: ValidatePosition, then ValidateOrientation.
[[nodiscard]] Status ValidateLocalPose(const BodyLocalPose& pose) noexcept;

/// Validate a vector whose components must be finite and at most `limit` in magnitude.
[[nodiscard]] Status ValidateBounded(math::Vector3d value, float64 limit) noexcept;

/// Normalized float64 rotation of an admitted public orientation.
[[nodiscard]] LUDUS_INLINE Rotation WidenNormalized(math::Quaternion orientation) noexcept
{
    return Normalize(Widen(orientation));
}
} // namespace ludus::physics::rigid::internal
