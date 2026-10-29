// Projected solid angle sampling of convex polygons (Peters 2021, section 3).
//
// Compiled with precise FP flags (-ffp-contract=off, no fast math); every fused
// multiply-add below is an explicit std::fma that mirrors the author's GLSL, so
// the shader port in shaders/projected_solid_angle.slang evaluates the same
// operations. <cmath>/<limits> are permitted here (ADR 0003, ADR 0010).
//
// The functions from MakeEllipseFromEdge through the tangent-line iteration and
// the sorting networks are ported from polygon_sampling.glsl, distributed with
// the paper's supplemental material, which the author licenses under either the
// three-clause BSD license or the GPLv3. Ludus uses it under the BSD license:
//
//   Copyright (C) 2021, Christoph Peters, Karlsruhe Institute of Technology
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are met:
//
//   1. Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//   2. Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//   3. Neither the name of the copyright holder nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
//   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
//   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//   POSSIBILITY OF SUCH DAMAGE.
//
// Departures from that file, none of which changes the sampled distribution:
// - Ellipse kinds (outer, inner, degenerate) are stored explicitly instead of in
//   the sign bit of u.x and as an infinite u.x, and positive_atan(y / x) is
//   evaluated without dividing by zero, because Ludus' Metal and WGSL shader
//   targets do not guarantee signed zeros, infinities or NaN propagation. The
//   C++ code uses the same representation so both ports stay comparable.
// - Sector selection skips sectors with zero area, so rounding at the end of
//   the distribution can never select an empty sector (the original yields NaN).
// - The approximate exponent-flip reciprocal in the iteration is an exact
//   reciprocal, the variant the author's comment offers for ports.
// - Horizon clipping is an independent implementation.
// Thanks also to Claude-Pierre Jeannerod, Nicolas Louvet and Jean-Michel Muller,
// "Further analysis of Kahan's algorithm for the accurate computation of 2x2
// determinants", Mathematics of Computation 82(284), 2013,
// https://doi.org/10.1090/S0025-5718-2013-02679-8, for the error-free
// difference of products, and to James F. Blinn, "How to Solve a Quadratic
// Equation, Part 2", IEEE Computer Graphics and Applications 26(2), 2006,
// https://doi.org/10.1109/MCG.2006.35, for the homogeneous quadratic solver.

#include "internal/projected_solid_angle.hpp"

#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <cmath>

namespace ludus::foundation::math
{
namespace
{
using core::usize;

// Relative target areas this close to a sector boundary skip the iteration: the
// initialization is accurate there and the tangent lines may become parallel
// (paper section 3.5).
constexpr float32 ITERATION_BOUNDARY_TOLERANCE = 1.0e-5F;
// The two sector boundaries and their half vector used by the initialization (paper section 3.4).
constexpr usize QUAD_DIRECTION_COUNT = 3;
// The largest sorting network: twelve comparisons sort seven or eight vertices (supplement A.6).
constexpr usize MAX_SORTING_NETWORK_COMPARISONS = 12;

/// One comparison of a sorting network: swap when the shorter path from Left to Right is clockwise.
struct SortingComparison final
{
    uint32 Left = 0;
    uint32 Right = 0;
};

/// A sorting network for bitonic sequences, ending with the comparisons the networks share.
struct SortingNetwork final
{
    uint32 ComparisonCount = 0;
    SortingComparison Comparisons[MAX_SORTING_NETWORK_COMPARISONS]{};
};

// The optimal networks of supplement A.6 (Fig. A.2), indexed by vertex count. The final (0, 2), (2, 3) and (0, 1)
// comparisons are shared; triangles omit (2, 3).
constexpr SortingNetwork SORTING_NETWORKS[MAX_PROJECTED_POLYGON_VERTICES + 1] = {
    {},
    {},
    {},
    { .ComparisonCount = 3, .Comparisons = {{1, 2}, {0, 2}, {0, 1}} },
    { .ComparisonCount = 4, .Comparisons = {{1, 3}, {0, 2}, {2, 3}, {0, 1}} },
    { .ComparisonCount = 8, .Comparisons = {{2, 4}, {1, 3}, {1, 2}, {0, 3}, {3, 4}, {0, 2}, {2, 3}, {0, 1}} },
    { .ComparisonCount = 9, .Comparisons = {{3, 5}, {2, 4}, {1, 5}, {0, 4}, {4, 5}, {1, 3}, {0, 2}, {2, 3}, {0, 1}} },
    {
        .ComparisonCount = 12,
        .Comparisons = {{2, 5}, {1, 6}, {5, 6}, {3, 4}, {0, 4}, {4, 6}, {1, 3}, {3, 5}, {4, 5}, {0, 2}, {2, 3}, {0, 1}},
    },
    {
        .ComparisonCount = 12,
        .Comparisons = {{2, 6}, {3, 7}, {1, 5}, {0, 4}, {4, 6}, {5, 7}, {6, 7}, {4, 5}, {1, 3}, {0, 2}, {2, 3}, {0, 1}},
    },
};

/// The entries of a 2x2 matrix whose quadratic form w^T M w is solved for w.
struct HomogeneousQuadratic final
{
    float32 Xx = 0.0F;
    float32 Xy = 0.0F;
    float32 Yx = 0.0F;
    float32 Yy = 0.0F;
};

/// The ellipses bounding one sector: an inner one (or none in the central case) and an outer one.
struct SectorEllipses final
{
    Vector2 Inner{};
    Vector2 Outer{};
    ProjectedEllipseKind OuterKind = ProjectedEllipseKind::Outer;
};

[[nodiscard]] float32 MultiplyAdd(float32 a, float32 b, float32 c) noexcept
{
    return std::fma(a, b, c);
}
[[nodiscard]] float32 GetReciprocalSquareRoot(float32 value) noexcept
{
    return 1.0F / std::sqrt(value);
}
/// a * b - c * d with at most 1.5 ulps of error (Kahan's algorithm).
// NOLINTBEGIN(bugprone-easily-swappable-parameters): a * b - c * d is the formula order.
[[nodiscard]] float32 ComputeDifferenceOfProducts(float32 a, float32 b, float32 c, float32 d) noexcept
{
    const float32 product = c * d;
    const float32 error = MultiplyAdd(c, d, -product);
    const float32 result = MultiplyAdd(a, b, -product);
    return result - error;
}
// NOLINTEND(bugprone-easily-swappable-parameters)
/// Cross product with Kahan's algorithm for every entry; the sign of z is exact.
[[nodiscard]] Vector3 ComputeStableCross(Vector3 left, Vector3 right) noexcept
{
    return Vector3{
        ComputeDifferenceOfProducts(left.Y, right.Z, left.Z, right.Y),
        ComputeDifferenceOfProducts(left.Z, right.X, left.X, right.Z),
        ComputeDifferenceOfProducts(left.X, right.Y, left.Y, right.X),
    };
}
[[nodiscard]] Vector2 Rotate90(Vector2 value) noexcept
{
    return Vector2{-value.Y, value.X};
}
[[nodiscard]] Vector2 GetXy(Vector3 value) noexcept
{
    return Vector2{value.X, value.Y};
}
/// Multiply point by the ellipse matrix I + u u^T.
[[nodiscard]] Vector2 TransformByEllipse(Vector2 ellipse, Vector2 point) noexcept
{
    const float32 projection = Dot(ellipse, point);
    return Vector2{MultiplyAdd(projection, ellipse.X, point.X), MultiplyAdd(projection, ellipse.Y, point.Y)};
}
[[nodiscard]] float32 GetEllipseDeterminant(Vector2 ellipse) noexcept
{
    return MultiplyAdd(ellipse.X, ellipse.X, MultiplyAdd(ellipse.Y, ellipse.Y, 1.0F));
}
[[nodiscard]] float32 GetEllipseReciprocalSquareRootDeterminant(Vector2 ellipse) noexcept
{
    return GetReciprocalSquareRoot(GetEllipseDeterminant(ellipse));
}
/// Reciprocal square of the factor that scales direction onto the ellipse.
[[nodiscard]] float32 GetEllipseDirectionFactorReciprocalSquare(Vector2 ellipse, Vector2 direction) noexcept
{
    const float32 ellipseDotDirection = Dot(ellipse, direction);
    const float32 directionDotDirection = Dot(direction, direction);
    return MultiplyAdd(ellipseDotDirection, ellipseDotDirection, directionDotDirection);
}
[[nodiscard]] float32 GetEllipseDirectionFactor(Vector2 ellipse, Vector2 direction) noexcept
{
    return GetReciprocalSquareRoot(GetEllipseDirectionFactorReciprocalSquare(ellipse, direction));
}
[[nodiscard]] float32 GetEllipseNormalizedDirectionFactor(Vector2 ellipse, Vector2 normalizedDirection) noexcept
{
    const float32 ellipseDotDirection = Dot(ellipse, normalizedDirection);
    return GetReciprocalSquareRoot(MultiplyAdd(ellipseDotDirection, ellipseDotDirection, 1.0F));
}
/// The angle in [0, pi] of the direction (x, y) with y >= 0: the paper's positive_atan(y / x), which supplement
/// A.7 prefers over atan2, written so that a zero x never divides.
[[nodiscard]] float32 ComputeSectorAngle(float32 y, float32 x) noexcept
{
    if (x > 0.0F)
    {
        return std::atan(y / x);
    }
    if (x < 0.0F)
    {
        return std::atan(y / x) + kPiF;
    }
    return y > 0.0F ? kHalfPiF : 0.0F;
}
// NOLINTBEGIN(bugprone-easily-swappable-parameters): inner then outer, as in Equation (1).
[[nodiscard]] float32 ComputeAreaBetweenEllipsesFromAngles(float32 innerReciprocalSquareRootDeterminant,
                                                           float32 innerAngle,
                                                           float32 outerReciprocalSquareRootDeterminant,
                                                           float32 outerAngle) noexcept
{
    const float32 innerArea = innerReciprocalSquareRootDeterminant * innerAngle;
    const float32 result = MultiplyAdd(outerReciprocalSquareRootDeterminant, outerAngle, -innerArea);
    // Sort out negative results from rounding.
    return result > 0.0F ? 0.5F * result : 0.0F;
}
// NOLINTEND(bugprone-easily-swappable-parameters)
[[nodiscard]] float32 ComputeAreaBetweenEllipsesInSector(Vector2 innerEllipse,
                                                         float32 innerReciprocalSquareRootDeterminant,
                                                         Vector2 outerEllipse,
                                                         float32 outerReciprocalSquareRootDeterminant,
                                                         Vector2 direction0,
                                                         Vector2 direction1) noexcept
{
    const float32 directionDeterminant = Max(+0.0F, Dot(direction1, Rotate90(direction0)));
    const float32 innerDot =
        innerReciprocalSquareRootDeterminant * Dot(direction0, TransformByEllipse(innerEllipse, direction1));
    const float32 outerDot =
        outerReciprocalSquareRootDeterminant * Dot(direction0, TransformByEllipse(outerEllipse, direction1));
    return ComputeAreaBetweenEllipsesFromAngles(innerReciprocalSquareRootDeterminant,
                                                ComputeSectorAngle(directionDeterminant, innerDot),
                                                outerReciprocalSquareRootDeterminant,
                                                ComputeSectorAngle(directionDeterminant, outerDot));
}
/// Linear interpolation from x to y written to stay exact at both ends (Fabian Giesen).
[[nodiscard]] float32 MixWithMultiplyAdd(float32 x, float32 y, float32 t) noexcept
{
    return MultiplyAdd(t, y, MultiplyAdd(-t, x, x));
}
[[nodiscard]] Vector2 Normalize(Vector2 value) noexcept
{
    return value * GetReciprocalSquareRoot(Dot(value, value));
}
[[nodiscard]] float32 ComputeDeterminant(Vector2 column0, Vector2 column1) noexcept
{
    return column0.X * column1.Y - column1.X * column0.Y;
}
[[nodiscard]] HomogeneousQuadratic MakeOuterProduct(Vector2 left, Vector2 right) noexcept
{
    return HomogeneousQuadratic
    {
        .Xx = left.X * right.X,
        .Xy = left.X * right.Y,
        .Yx = left.Y * right.X,
        .Yy = left.Y * right.Y,
    };
}
[[nodiscard]] HomogeneousQuadratic Subtract(HomogeneousQuadratic left, HomogeneousQuadratic right) noexcept
{
    return HomogeneousQuadratic
    {
        .Xx = left.Xx - right.Xx,
        .Xy = left.Xy - right.Xy,
        .Yx = left.Yx - right.Yx,
        .Yy = left.Yy - right.Yy,
    };
}
/// A nonzero root w of w^T M w = 0 that depends continuously on M (Blinn 2006, without the special case for a
/// vanishing off-diagonal sum).
[[nodiscard]] Vector2 SolveHomogeneousQuadratic(HomogeneousQuadratic quadratic) noexcept
{
    const float32 coefficient = 0.5F * (quadratic.Yx + quadratic.Xy);
    const float32 discriminantSquareRoot =
        std::sqrt(Max(0.0F, coefficient * coefficient - quadratic.Xx * quadratic.Yy));
    const float32 scaledRoot = Abs(coefficient) + discriminantSquareRoot;
    return coefficient >= 0.0F ? Vector2{scaledRoot, -quadratic.Xx} : Vector2{quadratic.Yy, scaledRoot};
}
/// A multiple of value with an L1 norm of one whose dot product with semiCircle is nonnegative; a zero value is
/// returned unchanged.
[[nodiscard]] Vector2 NormalizeApproximatelyAndFlip(Vector2 value, Vector2 semiCircle) noexcept
{
    const float32 norm = Abs(value.X) + Abs(value.Y);
    float32 scaling = norm > 0.0F ? 1.0F / norm : 1.0F;
    scaling = Dot(value, semiCircle) >= 0.0F ? scaling : -scaling;
    return value * scaling;
}
/// Swap two vertices with their ellipses when the shorter path from left to right is clockwise. Equal angles put a
/// degenerate ellipse first (supplement A.2, rule 3).
void CompareAndSwap(ProjectedSolidAnglePolygon& inoutPolygon, uint32 left, uint32 right) noexcept
{
    const Vector2 leftVertex = inoutPolygon.VerticesXy[left];
    const Vector2 rightVertex = inoutPolygon.VerticesXy[right];
    // Agrees with the z coordinate of ComputeStableCross, which classifies inner and outer ellipses.
    const float32 normalZ = ComputeDifferenceOfProducts(leftVertex.X, -rightVertex.Y, leftVertex.Y, -rightVertex.X);
    const bool shouldSwap =
        normalZ == 0.0F ? inoutPolygon.EllipseKinds[right] == ProjectedEllipseKind::Degenerate : normalZ > 0.0F;
    if (shouldSwap == true)
    {
        inoutPolygon.VerticesXy[left] = rightVertex;
        inoutPolygon.VerticesXy[right] = leftVertex;
        const Vector2 leftEllipse = inoutPolygon.Ellipses[left];
        inoutPolygon.Ellipses[left] = inoutPolygon.Ellipses[right];
        inoutPolygon.Ellipses[right] = leftEllipse;
        const ProjectedEllipseKind leftKind = inoutPolygon.EllipseKinds[left];
        inoutPolygon.EllipseKinds[left] = inoutPolygon.EllipseKinds[right];
        inoutPolygon.EllipseKinds[right] = leftKind;
    }
}
/// Sort the bitonic vertex sequence of a convex polygon counterclockwise with the optimal sorting networks of
/// supplement A.6.
void SortConvexPolygonVertices(ProjectedSolidAnglePolygon& inoutPolygon) noexcept
{
    const SortingNetwork& network = SORTING_NETWORKS[inoutPolygon.VertexCount];
    for (uint32 i = 0; i < network.ComparisonCount; ++i)
    {
        CompareAndSwap(inoutPolygon, network.Comparisons[i].Left, network.Comparisons[i].Right);
    }
}
/// Advance the inner/outer pairing to sector i (paper section 3.7): sector 0 takes the outer ellipse stored at
/// vertex 0 and the separately stored inner ellipse; later sectors replace one of the two.
void AdvanceSectorEllipses(const ProjectedSolidAnglePolygon& polygon, uint32 i, SectorEllipses& inoutEllipses) noexcept
{
    if (i == 0)
    {
        inoutEllipses.Inner = polygon.InnerEllipse0;
        inoutEllipses.Outer = polygon.Ellipses[0];
        inoutEllipses.OuterKind = polygon.EllipseKinds[0];
    }
    else if (polygon.EllipseKinds[i] == ProjectedEllipseKind::Inner)
    {
        inoutEllipses.Inner = polygon.Ellipses[i];
    }
    else
    {
        inoutEllipses.Outer = polygon.Ellipses[i];
        inoutEllipses.OuterKind = polygon.EllipseKinds[i];
    }
}
[[nodiscard]] bool IsUnitInterval(float32 value) noexcept
{
    return value >= 0.0F && value <= 1.0F;
}
} // namespace

namespace internal
{
Vector2 MakeEllipseFromEdge(Vector3 vertex0, Vector3 vertex1, ProjectedEllipseKind& outKind) noexcept
{
    const Vector3 normal = ComputeStableCross(vertex0, vertex1);
    if (normal.Z == 0.0F)
    {
        // The great circle passes through the zenith (supplement A.2, rule 1).
        outKind = ProjectedEllipseKind::Degenerate;
        return Vector2{};
    }
    // The ellipse matrix I + u u^T does not depend on the sign of u.
    outKind = normal.Z < 0.0F ? ProjectedEllipseKind::Inner : ProjectedEllipseKind::Outer;
    const float32 scaling = 1.0F / normal.Z;
    return Vector2{normal.X * scaling, normal.Y * scaling};
}

float32 ComputeEllipseAreaInSector(Vector2 ellipse, Vector2 direction0, Vector2 direction1) noexcept
{
    const float32 reciprocalSquareRootDeterminant = GetEllipseReciprocalSquareRootDeterminant(ellipse);
    const float32 directionDeterminant = Max(+0.0F, Dot(direction1, Rotate90(direction0)));
    const float32 ellipseDot =
        reciprocalSquareRootDeterminant * Dot(direction0, TransformByEllipse(ellipse, direction1));
    return 0.5F * reciprocalSquareRootDeterminant * ComputeSectorAngle(directionDeterminant, ellipseDot);
}

float32 ComputeAreaBetweenEllipsesInSector(Vector2 innerEllipse,
                                           Vector2 outerEllipse,
                                           Vector2 direction0,
                                           Vector2 direction1) noexcept
{
    return math::ComputeAreaBetweenEllipsesInSector(innerEllipse,
                                                    GetEllipseReciprocalSquareRootDeterminant(innerEllipse),
                                                    outerEllipse,
                                                    GetEllipseReciprocalSquareRootDeterminant(outerEllipse),
                                                    direction0,
                                                    direction1);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): the order of supplement A.7, Algorithm 3.
Vector2 SampleSectorBetweenEllipses(Vector2 randomNumbers,
                                    float32 targetArea,
                                    Vector2 innerEllipse,
                                    Vector2 outerEllipse,
                                    Vector2 direction0,
                                    Vector2 direction1,
                                    uint32 iterationCount) noexcept
{
    // Split the sector in half for the initialization (paper section 3.4).
    Vector2 quadDirections[QUAD_DIRECTION_COUNT] = {Normalize(direction0), Vector2{}, Normalize(direction1)};
    quadDirections[1] = quadDirections[0] + quadDirections[2];
    // Where these rays intersect the ellipses; the six points define two adjacent quads.
    float32 innerFactors[QUAD_DIRECTION_COUNT] = {
        GetEllipseNormalizedDirectionFactor(innerEllipse, quadDirections[0]),
        GetEllipseDirectionFactor(innerEllipse, quadDirections[1]),
        GetEllipseNormalizedDirectionFactor(innerEllipse, quadDirections[2]),
    };
    float32 outerFactors[QUAD_DIRECTION_COUNT] = {
        GetEllipseNormalizedDirectionFactor(outerEllipse, quadDirections[0]),
        GetEllipseDirectionFactor(outerEllipse, quadDirections[1]),
        GetEllipseNormalizedDirectionFactor(outerEllipse, quadDirections[2]),
    };
    // Relative areas of the two quads.
    const float32 quadAreas[2] = {
        outerFactors[0] * outerFactors[1] - innerFactors[0] * innerFactors[1],
        outerFactors[1] * outerFactors[2] - innerFactors[1] * innerFactors[2],
    };
    // Select the quad to initialize from; move its data to indices 1 and 2.
    float32 targetQuadArea = MixWithMultiplyAdd(-quadAreas[0], quadAreas[1], randomNumbers.X);
    const bool isFirstQuad = targetQuadArea <= 0.0F;
    quadDirections[2] = isFirstQuad == true ? quadDirections[0] : quadDirections[2];
    innerFactors[2] = isFirstQuad == true ? innerFactors[0] : innerFactors[2];
    outerFactors[2] = isFirstQuad == true ? outerFactors[0] : outerFactors[2];
    targetQuadArea += isFirstQuad == true ? quadAreas[0] : -quadAreas[1];
    // The remaining area factors; a factor of 0.5 cancels with a 2 below.
    targetQuadArea *= Abs(ComputeDeterminant(quadDirections[1], quadDirections[2]));
    // Edge normals of the selected quad, built like half vectors to avoid cancellation (supplement A.4).
    Vector2 innerNormal = quadDirections[1] * innerFactors[1] + quadDirections[2] * innerFactors[2];
    Vector2 outerNormal = quadDirections[1] * outerFactors[1] + quadDirections[2] * outerFactors[2];
    innerNormal = TransformByEllipse(innerEllipse, innerNormal);
    outerNormal = TransformByEllipse(outerEllipse, outerNormal);
    const float32 innerOffset = Dot(innerNormal, quadDirections[1]) * innerFactors[1];
    const float32 outerOffset = Dot(outerNormal, quadDirections[1]) * outerFactors[1];
    // The initialization solves the quad's homogeneous quadratic Q (paper section 3.4).
    HomogeneousQuadratic quadratic =
        MakeOuterProduct(Rotate90(quadDirections[2]) * (outerOffset * outerFactors[2]), innerNormal);
    quadratic = Subtract(
        quadratic,
        MakeOuterProduct(Rotate90(quadDirections[2]) * (innerOffset * innerFactors[2]) + innerNormal * targetQuadArea,
                         outerNormal));
    Vector2 currentDirection = SolveHomogeneousQuadratic(quadratic);

    // Near the sector boundaries the initialization is exact but the iteration may be unstable.
    iterationCount = Abs(randomNumbers.X - 0.5F) <= 0.5F - ITERATION_BOUNDARY_TOLERANCE ? iterationCount : uint32{0};
    const float32 innerReciprocalSquareRootDeterminant = GetEllipseReciprocalSquareRootDeterminant(innerEllipse);
    const float32 outerReciprocalSquareRootDeterminant = GetEllipseReciprocalSquareRootDeterminant(outerEllipse);
    for (uint32 i = 0; i != iterationCount; ++i)
    {
        // Avoid under- and overflow; flip the sign so that clamping to zero below is meaningful.
        currentDirection = NormalizeApproximatelyAndFlip(currentDirection, quadDirections[1]);
        const Vector2 innerDirection = TransformByEllipse(innerEllipse, currentDirection);
        const Vector2 outerDirection = TransformByEllipse(outerEllipse, currentDirection);
        // Residual error of the current direction (Equation 7).
        const float32 directionDeterminant = Max(+0.0F, Dot(currentDirection, Rotate90(quadDirections[0])));
        const float32 error =
            targetArea -
            ComputeAreaBetweenEllipsesFromAngles(
                innerReciprocalSquareRootDeterminant,
                ComputeSectorAngle(directionDeterminant,
                                   innerReciprocalSquareRootDeterminant * Dot(quadDirections[0], innerDirection)),
                outerReciprocalSquareRootDeterminant,
                ComputeSectorAngle(directionDeterminant,
                                   outerReciprocalSquareRootDeterminant * Dot(quadDirections[0], outerDirection)));
        // The tangent-line quadratic T whose root is the next direction (Equation 5).
        quadratic = Subtract(MakeOuterProduct(innerDirection - outerDirection, Rotate90(currentDirection)),
                             MakeOuterProduct(innerDirection * (2.0F * error), outerDirection));
        currentDirection = SolveHomogeneousQuadratic(quadratic);
    }

    // The halved sector spans at most 90 degrees, so the sample lies on the half vector's side.
    currentDirection = Dot(currentDirection, quadDirections[1]) >= 0.0F ? currentDirection : -currentDirection;
    // Sample the squared radius uniformly between the two ellipses.
    const float32 innerFactor = 1.0F / GetEllipseDirectionFactorReciprocalSquare(innerEllipse, currentDirection);
    const float32 outerFactor = 1.0F / GetEllipseDirectionFactorReciprocalSquare(outerEllipse, currentDirection);
    return currentDirection * std::sqrt(MixWithMultiplyAdd(innerFactor, outerFactor, randomNumbers.Y));
}
// NOLINTEND(bugprone-easily-swappable-parameters)

uint32 ClipPolygonToUpperHemisphere(const Vector3 (&positions)[MAX_PROJECTED_POLYGON_INPUT_VERTICES],
                                    uint32 vertexCount,
                                    Vector3 (&outPositions)[MAX_PROJECTED_POLYGON_VERTICES]) noexcept
{
    uint32 outCount = 0;
    for (uint32 i = 0; i < vertexCount; ++i)
    {
        const Vector3 start = positions[i];
        const Vector3 end = positions[(i + 1) % vertexCount];
        if (start.Z >= 0.0F)
        {
            outPositions[outCount] = start;
            ++outCount;
        }
        // Only a strict sign change creates a vertex, so a vertex on the horizon is never duplicated. A convex
        // polygon crosses the plane at most twice, so outCount never exceeds vertexCount + 1.
        if ((start.Z > 0.0F && end.Z < 0.0F) || (start.Z < 0.0F && end.Z > 0.0F))
        {
            const float32 t = start.Z / (start.Z - end.Z);
            outPositions[outCount] = Vector3{
                MixWithMultiplyAdd(start.X, end.X, t),
                MixWithMultiplyAdd(start.Y, end.Y, t),
                0.0F,
            };
            ++outCount;
        }
    }
    return outCount >= MIN_PROJECTED_POLYGON_VERTICES ? outCount : uint32{0};
}
} // namespace internal

MathStatus TryClipProjectedPolygonToUpperHemisphere(const ProjectedPolygonInput& polygon,
                                                    ProjectedPolygonInput& outPolygon) noexcept
{
    constexpr uint32 MINIMUM_VERTEX_COUNT = 3;
    if (polygon.VertexCount < MINIMUM_VERTEX_COUNT || polygon.VertexCount > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return MathStatus::InvalidArgument;
    }
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        if (IsFinite(polygon.PositionsTangent[i]) == false)
        {
            return MathStatus::NonFiniteInput;
        }
    }
    // A positive common scale preserves the polygon's angular domain and keeps
    // the clipping denominator and interpolation finite for any finite input.
    float32 maximumMagnitude = 0;
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        const Vector3 positionTangent = polygon.PositionsTangent[i];
        maximumMagnitude =
            Max(maximumMagnitude, Max(Abs(positionTangent.X), Max(Abs(positionTangent.Y), Abs(positionTangent.Z))));
    }
    if (maximumMagnitude == 0)
    {
        outPolygon = {};
        return MathStatus::Success;
    }
    Vector3 positions[MAX_PROJECTED_POLYGON_INPUT_VERTICES]{};
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        // Divide components directly: the reciprocal may overflow for tiny inputs.
        const Vector3 value = polygon.PositionsTangent[i];
        positions[i] = {value.X / maximumMagnitude, value.Y / maximumMagnitude, value.Z / maximumMagnitude};
    }
    Vector3 clipped[MAX_PROJECTED_POLYGON_VERTICES]{};
    const uint32 count = internal::ClipPolygonToUpperHemisphere(positions, polygon.VertexCount, clipped);
    if (count > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return MathStatus::InvalidArgument;
    }
    ProjectedPolygonInput candidate;
    candidate.VertexCount = count;
    for (uint32 i = 0; i < count; ++i)
    {
        candidate.PositionsTangent[i] = clipped[i];
    }
    outPolygon = candidate;
    return MathStatus::Success;
}

MathStatus TryPrepareProjectedSolidAnglePolygon(const ProjectedPolygonInput& polygon,
                                                ProjectedSolidAnglePolygon& outPolygon) noexcept
{
    if (polygon.VertexCount < MIN_PROJECTED_POLYGON_VERTICES ||
        polygon.VertexCount > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return MathStatus::InvalidArgument;
    }
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        if (IsFinite(polygon.PositionsTangent[i]) == false)
        {
            return MathStatus::NonFiniteInput;
        }
    }
    ProjectedSolidAnglePolygon result;
    // A polygon seen clockwise from the origin has a positive triple product. Mirroring y fixes the other winding
    // (paper section 3.1); a zero product means the shading point lies in the polygon's plane.
    const float32 orientation =
        Dot(polygon.PositionsTangent[0], ComputeStableCross(polygon.PositionsTangent[1], polygon.PositionsTangent[2]));
    if ((orientation > 0.0F || orientation < 0.0F) == false)
    {
        outPolygon = result;
        return MathStatus::Success;
    }
    result.IsMirrored = orientation < 0.0F;
    Vector3 positions[MAX_PROJECTED_POLYGON_INPUT_VERTICES]{};
    for (uint32 i = 0; i < polygon.VertexCount; ++i)
    {
        positions[i] = polygon.PositionsTangent[i];
        positions[i].Y = result.IsMirrored == true ? -positions[i].Y : positions[i].Y;
    }
    Vector3 clipped[MAX_PROJECTED_POLYGON_VERTICES]{};
    const uint32 count = internal::ClipPolygonToUpperHemisphere(positions, polygon.VertexCount, clipped);
    if (count == 0)
    {
        outPolygon = result;
        return MathStatus::Success;
    }

    // Associate each vertex with the next ellipse counterclockwise (paper section 3.7): its own edge if that edge
    // is outer, otherwise the previous edge. The inner ellipse adjacent to the clockwise end is kept separately.
    result.VertexCount = count;
    ProjectedEllipseKind kinds[MAX_PROJECTED_POLYGON_VERTICES]{};
    Vector2 ellipses[MAX_PROJECTED_POLYGON_VERTICES]{};
    for (uint32 i = 0; i < count; ++i)
    {
        result.VerticesXy[i] = GetXy(clipped[i]);
        ellipses[i] = internal::MakeEllipseFromEdge(clipped[i], clipped[(i + 1) % count], kinds[i]);
    }
    result.IsCentralCase = true;
    for (uint32 i = 0; i < count; ++i)
    {
        const uint32 previous = (i + count - 1) % count;
        const bool isInner = kinds[i] == ProjectedEllipseKind::Inner;
        result.Ellipses[i] = isInner == true ? ellipses[previous] : ellipses[i];
        result.EllipseKinds[i] = isInner == true ? kinds[previous] : kinds[i];
        if (kinds[previous] == ProjectedEllipseKind::Inner && isInner == false)
        {
            result.InnerEllipse0 = ellipses[previous];
            result.IsCentralCase = false;
        }
    }

    result.ProjectedSolidAngle = 0.0F;
    if (result.IsCentralCase == true)
    {
        // Central case: one outer ellipse bounds each of the VertexCount sectors; degenerate ones enclose nothing
        // (supplement A.2, rule 2).
        for (uint32 i = 0; i < count; ++i)
        {
            result.SectorProjectedSolidAngles[i] =
                result.EllipseKinds[i] == ProjectedEllipseKind::Degenerate
                    ? 0.0F
                    : internal::ComputeEllipseAreaInSector(result.Ellipses[i],
                                                           result.VerticesXy[i],
                                                           result.VerticesXy[(i + 1) % count]);
            result.ProjectedSolidAngle += result.SectorProjectedSolidAngles[i];
        }
    }
    else
    {
        // Decentral case: VertexCount - 1 sectors, each between an inner and an outer ellipse.
        SortConvexPolygonVertices(result);
        SectorEllipses sector;
        for (uint32 i = 0; i + 1 < count; ++i)
        {
            AdvanceSectorEllipses(result, i, sector);
            result.SectorProjectedSolidAngles[i] =
                sector.OuterKind == ProjectedEllipseKind::Degenerate
                    ? 0.0F
                    : internal::ComputeAreaBetweenEllipsesInSector(sector.Inner,
                                                                   sector.Outer,
                                                                   result.VerticesXy[i],
                                                                   result.VerticesXy[i + 1]);
            result.ProjectedSolidAngle += result.SectorProjectedSolidAngles[i];
        }
    }
    outPolygon = result;
    return MathStatus::Success;
}

MathStatus TrySampleProjectedSolidAnglePolygon(const ProjectedSolidAnglePolygon& polygon,
                                               Vector2 randomNumbers,
                                               Vector3& outDirectionTangent) noexcept
{
    if (IsFinite(randomNumbers) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (IsUnitInterval(randomNumbers.X) == false || IsUnitInterval(randomNumbers.Y) == false)
    {
        return MathStatus::InvalidArgument;
    }
    const uint32 count = polygon.VertexCount;
    if ((polygon.ProjectedSolidAngle > 0.0F) == false || count < MIN_PROJECTED_POLYGON_VERTICES ||
        count > MAX_PROJECTED_POLYGON_VERTICES)
    {
        return MathStatus::Degenerate;
    }
    // Select the sector whose share of the distribution contains the first random number. Sectors without area
    // are never selected; when rounding exhausts the sum, the last sector with area receives its end.
    float32 remaining = randomNumbers.X * polygon.ProjectedSolidAngle;
    const uint32 sectorCount = polygon.IsCentralCase == true ? count : count - 1;
    uint32 selected = sectorCount;
    float32 selectedTarget = 0.0F;
    SectorEllipses ellipses;
    SectorEllipses selectedEllipses;
    for (uint32 i = 0; i < sectorCount; ++i)
    {
        const float32 area = polygon.SectorProjectedSolidAngles[i];
        if (polygon.IsCentralCase == false)
        {
            AdvanceSectorEllipses(polygon, i, ellipses);
        }
        if (area > 0.0F)
        {
            selected = i;
            selectedTarget = remaining;
            selectedEllipses = ellipses;
        }
        if (remaining < area)
        {
            break;
        }
        remaining -= area;
    }
    if (selected == sectorCount)
    {
        return MathStatus::Degenerate;
    }
    const float32 sectorArea = polygon.SectorProjectedSolidAngles[selected];
    selectedTarget = Min(selectedTarget, sectorArea);
    const Vector2 direction0 = polygon.VerticesXy[selected];
    Vector2 sampled{};
    if (polygon.IsCentralCase == true)
    {
        // Central case: invert the ellipse sector area in closed form (paper section 3.2 and supplement A.7).
        const Vector2 outerEllipse = polygon.Ellipses[selected];
        const float32 squareRootDeterminant = std::sqrt(GetEllipseDeterminant(outerEllipse));
        const float32 angle = 2.0F * selectedTarget * squareRootDeterminant;
        sampled = direction0 * (std::cos(angle) * squareRootDeterminant) +
                  Rotate90(TransformByEllipse(outerEllipse, direction0)) * std::sin(angle);
        // Sample the squared radius uniformly within the ellipse.
        sampled =
            sampled * std::sqrt(randomNumbers.Y / GetEllipseDirectionFactorReciprocalSquare(outerEllipse, sampled));
    }
    else
    {
        // Decentral case: the two-quad initialization refined by tangent-line iterations (paper section 3.3).
        const Vector2 sectorRandomNumbers{selectedTarget / sectorArea, randomNumbers.Y};
        sampled = internal::SampleSectorBetweenEllipses(sectorRandomNumbers,
                                                        selectedTarget,
                                                        selectedEllipses.Inner,
                                                        selectedEllipses.Outer,
                                                        direction0,
                                                        polygon.VerticesXy[selected + 1],
                                                        PROJECTED_POLYGON_ITERATION_COUNT);
    }
    // Lift the sample to the hemisphere (Equation 2) and undo the mirroring.
    const float32 z =
        std::sqrt(Max(0.0F, MultiplyAdd(-sampled.X, sampled.X, MultiplyAdd(-sampled.Y, sampled.Y, 1.0F))));
    const Vector3 direction{sampled.X, polygon.IsMirrored == true ? -sampled.Y : sampled.Y, z};
    // Single precision can fail for extreme, tiny and distant polygons (paper section 3.7, Fig. 7b).
    if (IsFinite(direction) == false)
    {
        return MathStatus::IllConditioned;
    }
    outDirectionTangent = direction;
    return MathStatus::Success;
}

float32 EvaluateProjectedSolidAnglePolygonDensity(const ProjectedSolidAnglePolygon& polygon,
                                                  Vector3 directionTangent) noexcept
{
    if (IsFinite(directionTangent) == false || (polygon.ProjectedSolidAngle > 0.0F) == false ||
        directionTangent.Z <= 0.0F)
    {
        return 0.0F;
    }
    return directionTangent.Z / polygon.ProjectedSolidAngle;
}
} // namespace ludus::foundation::math
