#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::math
{
/// Columns of a lower triangular covariance square root, in the input variable's units.
/// Their outer products sum to the positive semidefinite covariance (up to rounding).
struct GaussianAxes final
{
    Vector3 Axis0{}; ///< First correlated displacement from the Gaussian mean.
    Vector3 Axis1{}; ///< Second correlated displacement; zero for a rank-one covariance.
    Vector3 Axis2{}; ///< Third correlated displacement; zero for a rank-two covariance.
};
/// Factor a symmetric 3D covariance, including singular planar/linear distributions.
/// @param diagonal Variances XX, YY, ZZ in squared input units.
/// @param cross Covariances XY, XZ, YZ in squared input units.
/// @param outAxes Written only on success; unchanged for nonfinite or indefinite inputs.
/// @return True if the covariance is positive semidefinite within relative float32 roundoff.
/// @note Negative variances/Schur pivots within 1e-5 times the largest absolute diagonal are clamped to zero.
/// Unresolved off-diagonals at a zero pivot must be within that same tolerance.
/// Uses a Cholesky square root, not principal eigenvectors. All operations are allocation-free.
[[nodiscard]] bool TryComputeGaussianAxes(Vector3 diagonal, Vector3 cross, GaussianAxes& outAxes) noexcept;
} // namespace ludus::foundation::math
