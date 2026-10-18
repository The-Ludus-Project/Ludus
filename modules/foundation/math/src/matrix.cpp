// Matrix conversions and checked inverse (design §7). Precise FP flags;
// <cmath>/<limits> permitted (ADR 0003/0010). All heavy numeric work is in
// float64 on fixed-size stack arrays; no allocation.

#include <ludus/foundation/math/matrix.hpp>

#include <cmath>
#include <limits>

namespace ludus::foundation::math
{
// Thanks to Jason Shankel, "Matrix-Quaternion Conversions", Game Programming
// Gems 1 (2000), section 2.8, pp. 200-204, for quaternion/matrix conversion and
// extracting a large component before division. Ludus checks a finite,
// approximately orthonormal, positive-determinant basis before extraction.
// Source scope and convention adaptations: docs/architecture/math-research.md.
Matrix3 ToMatrix3(Quaternion q) noexcept
{
    LUDUS_ASSERT(IsFinite(q));
    const float64 x = static_cast<float64>(q.X), y = static_cast<float64>(q.Y), z = static_cast<float64>(q.Z),
                  w = static_cast<float64>(q.W);
    const float64 xx = x * x, yy = y * y, zz = z * z;
    const float64 xy = x * y, xz = x * z, yz = y * z;
    const float64 wx = w * x, wy = w * y, wz = w * z;
    // Column-major: Columns[c] is the image of basis vector c.
    const Vector3 c0{static_cast<float32>(1.0 - 2.0 * (yy + zz)),
                     static_cast<float32>(2.0 * (xy + wz)),
                     static_cast<float32>(2.0 * (xz - wy))};
    const Vector3 c1{static_cast<float32>(2.0 * (xy - wz)),
                     static_cast<float32>(1.0 - 2.0 * (xx + zz)),
                     static_cast<float32>(2.0 * (yz + wx))};
    const Vector3 c2{static_cast<float32>(2.0 * (xz + wy)),
                     static_cast<float32>(2.0 * (yz - wx)),
                     static_cast<float32>(1.0 - 2.0 * (xx + yy))};
    return Matrix3{{c0, c1, c2}};
}

namespace
{
// Read element (row,col) of a column-major Matrix3 as double.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (row, column) index pair.
[[nodiscard]] float64 M3(const Matrix3& m, int r, int c) noexcept
{
    const Vector3& col = m.Columns[c];
    return r == 0 ? (float64)col.X : (r == 1 ? (float64)col.Y : (float64)col.Z);
}
} // namespace

MathStatus TryToRotation(const Matrix3& basis, RotationExtractionPolicy policy, Quaternion& out) noexcept
{
    if (IsFinite(basis) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    const float64 tol = static_cast<float64>(policy.OrthoTolerance);
    // Column lengths ~ 1.
    for (int c = 0; c < 3; ++c)
    {
        const float64 len2 =
            M3(basis, 0, c) * M3(basis, 0, c) + M3(basis, 1, c) * M3(basis, 1, c) + M3(basis, 2, c) * M3(basis, 2, c);
        if (std::fabs(len2 - 1.0) > tol * 2.0 + tol * tol)
        {
            return MathStatus::Degenerate;
        }
    }
    // Pairwise orthogonality.
    auto coldot = [&](int a, int b) {
        return M3(basis, 0, a) * M3(basis, 0, b) + M3(basis, 1, a) * M3(basis, 1, b) +
               M3(basis, 2, a) * M3(basis, 2, b);
    };
    if (std::fabs(coldot(0, 1)) > tol || std::fabs(coldot(0, 2)) > tol || std::fabs(coldot(1, 2)) > tol)
    {
        return MathStatus::Degenerate;
    }
    // Determinant near +1 (rejects reflection and scale).
    const float64 det = M3(basis, 0, 0) * (M3(basis, 1, 1) * M3(basis, 2, 2) - M3(basis, 1, 2) * M3(basis, 2, 1)) -
                        M3(basis, 0, 1) * (M3(basis, 1, 0) * M3(basis, 2, 2) - M3(basis, 1, 2) * M3(basis, 2, 0)) +
                        M3(basis, 0, 2) * (M3(basis, 1, 0) * M3(basis, 2, 1) - M3(basis, 1, 1) * M3(basis, 2, 0));
    if (std::fabs(det - 1.0) > tol * 4.0)
    {
        return MathStatus::Degenerate;
    }

    // Largest-component extraction. trace = m00+m11+m22.
    const float64 m00 = M3(basis, 0, 0), m11 = M3(basis, 1, 1), m22 = M3(basis, 2, 2);
    const float64 trace = m00 + m11 + m22;
    Quaternion q{};
    if (trace > 0.0)
    {
        const float64 sq = std::sqrt(trace + 1.0) * 2.0; // 4w
        q.W = static_cast<float32>(0.25 * sq);
        q.X = static_cast<float32>((M3(basis, 2, 1) - M3(basis, 1, 2)) / sq);
        q.Y = static_cast<float32>((M3(basis, 0, 2) - M3(basis, 2, 0)) / sq);
        q.Z = static_cast<float32>((M3(basis, 1, 0) - M3(basis, 0, 1)) / sq);
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float64 sq = std::sqrt(1.0 + m00 - m11 - m22) * 2.0; // 4x
        q.X = static_cast<float32>(0.25 * sq);
        q.Y = static_cast<float32>((M3(basis, 0, 1) + M3(basis, 1, 0)) / sq);
        q.Z = static_cast<float32>((M3(basis, 0, 2) + M3(basis, 2, 0)) / sq);
        q.W = static_cast<float32>((M3(basis, 2, 1) - M3(basis, 1, 2)) / sq);
    }
    else if (m11 > m22)
    {
        const float64 sq = std::sqrt(1.0 + m11 - m00 - m22) * 2.0; // 4y
        q.X = static_cast<float32>((M3(basis, 0, 1) + M3(basis, 1, 0)) / sq);
        q.Y = static_cast<float32>(0.25 * sq);
        q.Z = static_cast<float32>((M3(basis, 1, 2) + M3(basis, 2, 1)) / sq);
        q.W = static_cast<float32>((M3(basis, 0, 2) - M3(basis, 2, 0)) / sq);
    }
    else
    {
        const float64 sq = std::sqrt(1.0 + m22 - m00 - m11) * 2.0; // 4z
        q.X = static_cast<float32>((M3(basis, 0, 2) + M3(basis, 2, 0)) / sq);
        q.Y = static_cast<float32>((M3(basis, 1, 2) + M3(basis, 2, 1)) / sq);
        q.Z = static_cast<float32>(0.25 * sq);
        q.W = static_cast<float32>((M3(basis, 1, 0) - M3(basis, 0, 1)) / sq);
    }

    Quaternion unit{};
    if (TryNormalize(q, unit) != MathStatus::Success)
    {
        return MathStatus::Degenerate;
    }
    // Validate the rotation action reproduces the basis columns within tol.
    const Matrix3 reconstructed = ToMatrix3(unit);
    for (int c = 0; c < 3; ++c)
    {
        for (int r = 0; r < 3; ++r)
        {
            if (std::fabs(M3(reconstructed, r, c) - M3(basis, r, c)) > tol * 8.0 + 1e-5)
            {
                return MathStatus::Degenerate;
            }
        }
    }
    out = unit;
    return MathStatus::Success;
}

MathStatus TryToRotation(const Matrix3& basis, Quaternion& out) noexcept
{
    return TryToRotation(basis, RotationExtractionPolicy{}, out);
}

namespace
{
// Generic fixed-size scaled-pivot Gauss-Jordan inverse in float64. N is 3 or 4.
// Returns false if a zero pivot or non-finite intermediate is encountered;
// otherwise fills inv[N][N] and reports the reciprocal condition estimate
// rcond = 1/(normInf(A) * normInf(inv)).
template <int N>
[[nodiscard]] bool InvertDouble(const float64 src[N][N], float64 inv[N][N], float64& rcond) noexcept
{
    float64 a[N][N];
    // Scale by max abs to control range; track scale.
    float64 maxAbs = 0.0;
    for (int r = 0; r < N; ++r)
    {
        for (int c = 0; c < N; ++c)
        {
            maxAbs = std::fmax(maxAbs, std::fabs(src[r][c]));
            a[r][c] = src[r][c];
            inv[r][c] = (r == c) ? 1.0 : 0.0;
        }
    }
    if (maxAbs == 0.0 || std::isfinite(maxAbs) == false)
    {
        return false;
    }
    const float64 scale = maxAbs;
    for (int r = 0; r < N; ++r)
    {
        for (int c = 0; c < N; ++c)
        {
            a[r][c] /= scale;
        }
    }

    for (int col = 0; col < N; ++col)
    {
        // Partial pivot: largest |a[row][col]| for row >= col.
        int pivot = col;
        float64 best = std::fabs(a[col][col]);
        for (int r = col + 1; r < N; ++r)
        {
            const float64 v = std::fabs(a[r][col]);
            if (v > best)
            {
                best = v;
                pivot = r;
            }
        }
        if (best == 0.0 || std::isfinite(best) == false)
        {
            return false;
        }
        if (pivot != col)
        {
            for (int c = 0; c < N; ++c)
            {
                std::swap(a[col][c], a[pivot][c]);
                std::swap(inv[col][c], inv[pivot][c]);
            }
        }
        const float64 pivotValue = a[col][col];
        if (pivotValue == 0.0 || std::isfinite(pivotValue) == false)
        {
            return false;
        }
        const float64 invPivot = 1.0 / pivotValue;
        for (int c = 0; c < N; ++c)
        {
            a[col][c] *= invPivot;
            inv[col][c] *= invPivot;
        }
        for (int r = 0; r < N; ++r)
        {
            if (r == col)
            {
                continue;
            }
            const float64 factor = a[r][col];
            if (factor == 0.0)
            {
                continue;
            }
            for (int c = 0; c < N; ++c)
            {
                a[r][c] -= factor * a[col][c];
                inv[r][c] -= factor * inv[col][c];
            }
        }
    }

    // Undo the scaling: inverse of (A/scale) is scale * inverse(A), so the
    // inverse of A is inv/scale.
    for (int r = 0; r < N; ++r)
    {
        for (int c = 0; c < N; ++c)
        {
            inv[r][c] /= scale;
            if (std::isfinite(inv[r][c]) == false)
            {
                return false;
            }
        }
    }

    // rcond = 1 / (normInf(A) * normInf(inv)), using the ORIGINAL A.
    auto normInf = [](const float64 m[N][N]) {
        float64 best = 0.0;
        for (int r = 0; r < N; ++r)
        {
            float64 rowSum = 0.0;
            for (int c = 0; c < N; ++c)
            {
                rowSum += std::fabs(m[r][c]);
            }
            best = std::fmax(best, rowSum);
        }
        return best;
    };
    const float64 na = normInf(src);
    const float64 ni = normInf(inv);
    rcond = (na > 0.0 && ni > 0.0 && std::isfinite(na) && std::isfinite(ni)) ? 1.0 / (na * ni) : 0.0;
    return true;
}

// Two-sided residual: max(normInf(A*X - I), normInf(X*A - I)) /
// max(1, normInf(A)*normInf(X)). All in double for the narrowed float X.
template <int N>
[[nodiscard]] float64 TwoSidedResidual(const float64 A[N][N], const float64 X[N][N]) noexcept
{
    auto normInf = [](const float64 m[N][N]) {
        float64 best = 0.0;
        for (int r = 0; r < N; ++r)
        {
            float64 rowSum = 0.0;
            for (int c = 0; c < N; ++c)
            {
                rowSum += std::fabs(m[r][c]);
            }
            best = std::fmax(best, rowSum);
        }
        return best;
    };
    float64 ax[N][N];
    float64 xa[N][N];
    for (int r = 0; r < N; ++r)
    {
        for (int c = 0; c < N; ++c)
        {
            float64 sumAX = 0.0;
            float64 sumXA = 0.0;
            for (int k = 0; k < N; ++k)
            {
                sumAX += A[r][k] * X[k][c];
                sumXA += X[r][k] * A[k][c];
            }
            const float64 ident = (r == c) ? 1.0 : 0.0;
            ax[r][c] = sumAX - ident;
            xa[r][c] = sumXA - ident;
        }
    }
    const float64 denom = std::fmax(1.0, normInf(A) * normInf(X));
    return std::fmax(normInf(ax), normInf(xa)) / denom;
}

template <int N, typename MatrixType>
[[nodiscard]] MathStatus InvertChecked(const MatrixType& m,
                                       InversePolicy policy,
                                       MatrixType& out,
                                       void (*load)(const MatrixType&, float64[N][N]),
                                       MatrixType (*store)(const float64[N][N])) noexcept
{
    float64 A[N][N];
    load(m, A);
    for (int r = 0; r < N; ++r)
    {
        for (int c = 0; c < N; ++c)
        {
            if (std::isfinite(A[r][c]) == false)
            {
                return MathStatus::NonFiniteInput;
            }
        }
    }
    float64 inv[N][N];
    float64 rcond = 0.0;
    if (InvertDouble<N>(A, inv, rcond) == false)
    {
        return MathStatus::Degenerate;
    }
    if ((rcond >= static_cast<float64>(policy.MinReciprocalCondition)) == false)
    {
        return MathStatus::IllConditioned;
    }
    // Narrow to float32 then re-widen for the two-sided residual check.
    MatrixType candidate = store(inv);
    float64 X[N][N];
    load(candidate, X);
    for (int r = 0; r < N; ++r)
    {
        for (int c = 0; c < N; ++c)
        {
            if (std::isfinite(X[r][c]) == false)
            {
                return MathStatus::OutOfRange;
            }
        }
    }
    // Reject a narrowed all-zero inverse.
    bool allZero = true;
    for (int r = 0; r < N && allZero; ++r)
    {
        for (int c = 0; c < N; ++c)
        {
            if (X[r][c] != 0.0)
            {
                allZero = false;
                break;
            }
        }
    }
    if (allZero)
    {
        return MathStatus::OutOfRange;
    }
    const float64 residual = TwoSidedResidual<N>(A, X);
    if ((residual <= static_cast<float64>(policy.MaxResidual)) == false)
    {
        return MathStatus::IllConditioned;
    }
    out = candidate;
    return MathStatus::Success;
}

void Load3(const Matrix3& m, float64 a[3][3]) noexcept
{
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
        {
            a[r][c] = M3(m, r, c);
        }
    }
}
Matrix3 Store3(const float64 a[3][3]) noexcept
{
    Matrix3 m{};
    for (int c = 0; c < 3; ++c)
    {
        m.Columns[c] =
            Vector3{static_cast<float32>(a[0][c]), static_cast<float32>(a[1][c]), static_cast<float32>(a[2][c])};
    }
    return m;
}
void Load4(const Matrix4& m, float64 a[4][4]) noexcept
{
    for (int c = 0; c < 4; ++c)
    {
        const Vector4& col = m.Columns[c];
        a[0][c] = static_cast<float64>(col.X);
        a[1][c] = static_cast<float64>(col.Y);
        a[2][c] = static_cast<float64>(col.Z);
        a[3][c] = static_cast<float64>(col.W);
    }
}
Matrix4 Store4(const float64 a[4][4]) noexcept
{
    Matrix4 m{};
    for (int c = 0; c < 4; ++c)
    {
        m.Columns[c] = Vector4{static_cast<float32>(a[0][c]),
                               static_cast<float32>(a[1][c]),
                               static_cast<float32>(a[2][c]),
                               static_cast<float32>(a[3][c])};
    }
    return m;
}
} // namespace

MathStatus TryInverse(const Matrix3& m, InversePolicy policy, Matrix3& out) noexcept
{
    return InvertChecked<3, Matrix3>(m, policy, out, &Load3, &Store3);
}
MathStatus TryInverse(const Matrix4& m, InversePolicy policy, Matrix4& out) noexcept
{
    return InvertChecked<4, Matrix4>(m, policy, out, &Load4, &Store4);
}
MathStatus TryInverse(const Matrix3& m, Matrix3& out) noexcept
{
    return TryInverse(m, InversePolicy{}, out);
}
MathStatus TryInverse(const Matrix4& m, Matrix4& out) noexcept
{
    return TryInverse(m, InversePolicy{}, out);
}
} // namespace ludus::foundation::math
