#pragma once
#include "math.slang"

// Projected solid angle sampling of convex polygonal lights; the GPU port of
// FoundationMath's projected_solid_angle.hpp with the same operations and fused
// multiply-adds. Thanks to Christoph Peters, "BRDF Importance Sampling for
// Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021,
// section 3 and supplement sections A.1-A.7, https://doi.org/10.1145/3450626.3459672.
// Ported from the author's polygon_sampling.glsl under the three-clause BSD
// license (the author also offers the GPLv3):
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
// ellipse kinds are explicit instead of the sign bit and infinity encodings, the
// [0, pi] arctangent never divides by zero and finiteness is tested on bit
// patterns, because Ludus compiles Metal shaders with fast math and WGSL does not
// guarantee infinities, NaNs or signed zeros; that arctangent is math.slang's
// ComputeArctangent, not the backend's atan; sector selection skips sectors
// without area; the iteration uses an exact reciprocal; horizon clipping is an
// independent implementation. See the FoundationMath module page. Loops keep
// compile-time bounds and unroll, so array indices stay constant on GPUs (paper
// section 3.7). Kahan's difference of products relies on fma being fused;
// targets that evaluate fma unfused keep the algorithm but lose its 1.5 ulp bound.

static const uint MAX_PROJECTED_POLYGON_INPUT_VERTICES = 7;
static const uint MAX_PROJECTED_POLYGON_VERTICES = MAX_PROJECTED_POLYGON_INPUT_VERTICES + 1;
static const uint PROJECTED_POLYGON_ITERATION_COUNT = 2;
static const float PROJECTED_POLYGON_ITERATION_BOUNDARY_TOLERANCE = 1.0e-5;
// ProjectedEllipseKind values of the C++ header.
static const uint PROJECTED_ELLIPSE_OUTER = 0;
static const uint PROJECTED_ELLIPSE_INNER = 1;
static const uint PROJECTED_ELLIPSE_DEGENERATE = 2;
static const uint FLOAT_EXPONENT_BITS = 0x7f800000u;

// A convex planar polygon in the tangent space of a shading point (origin at the shading point, normal +z).
struct ProjectedPolygonInput
{
    float3 PositionsTangent[MAX_PROJECTED_POLYGON_INPUT_VERTICES];
    uint VertexCount;
};

// Per-polygon, per-shading-point state; the same fields as the C++ ProjectedSolidAnglePolygon.
struct ProjectedSolidAnglePolygon
{
    float2 VerticesXy[MAX_PROJECTED_POLYGON_VERTICES];
    float2 Ellipses[MAX_PROJECTED_POLYGON_VERTICES];
    uint EllipseKinds[MAX_PROJECTED_POLYGON_VERTICES];
    float2 InnerEllipse0;
    bool IsCentralCase;
    float SectorProjectedSolidAngles[MAX_PROJECTED_POLYGON_VERTICES];
    float ProjectedSolidAngle;
    uint VertexCount;
    bool IsMirrored;
};

// The ellipses bounding one sector: an inner one (or none in the central case) and an outer one.
struct SectorEllipses
{
    float2 Inner;
    float2 Outer;
    uint OuterKind;
};

// Finite test on the representation, which fast-math compilation cannot assume away.
bool IsFiniteBits(float value)
{
    return (asuint(value) & FLOAT_EXPONENT_BITS) != FLOAT_EXPONENT_BITS;
}
bool IsFiniteBits(float3 value)
{
    return IsFiniteBits(value.x) == true && IsFiniteBits(value.y) == true && IsFiniteBits(value.z) == true;
}
// a * b - c * d with Kahan's algorithm.
float ComputeDifferenceOfProducts(float a, float b, float c, float d)
{
    float product = c * d;
    float error = fma(c, d, -product);
    float result = fma(a, b, -product);
    return result - error;
}
float3 ComputeStableCross(float3 left, float3 right)
{
    return float3(ComputeDifferenceOfProducts(left.y, right.z, left.z, right.y),
                  ComputeDifferenceOfProducts(left.z, right.x, left.x, right.z),
                  ComputeDifferenceOfProducts(left.x, right.y, left.y, right.x));
}
float2 Rotate90(float2 value)
{
    return float2(-value.y, value.x);
}
float2 TransformByEllipse(float2 ellipse, float2 point)
{
    float projection = dot(ellipse, point);
    return float2(fma(projection, ellipse.x, point.x), fma(projection, ellipse.y, point.y));
}
float GetEllipseDeterminant(float2 ellipse)
{
    return fma(ellipse.x, ellipse.x, fma(ellipse.y, ellipse.y, 1.0));
}
float GetEllipseReciprocalSquareRootDeterminant(float2 ellipse)
{
    return 1.0 / sqrt(GetEllipseDeterminant(ellipse));
}
float GetEllipseDirectionFactorReciprocalSquare(float2 ellipse, float2 direction)
{
    float ellipseDotDirection = dot(ellipse, direction);
    float directionDotDirection = dot(direction, direction);
    return fma(ellipseDotDirection, ellipseDotDirection, directionDotDirection);
}
float GetEllipseDirectionFactor(float2 ellipse, float2 direction)
{
    return 1.0 / sqrt(GetEllipseDirectionFactorReciprocalSquare(ellipse, direction));
}
float GetEllipseNormalizedDirectionFactor(float2 ellipse, float2 normalizedDirection)
{
    float ellipseDotDirection = dot(ellipse, normalizedDirection);
    return 1.0 / sqrt(fma(ellipseDotDirection, ellipseDotDirection, 1.0));
}
// The angle in [0, pi] of the direction (x, y) with y >= 0: positive_atan(y / x) without dividing by zero.
// Areas are differences of these angles, so the arctangent must not depend on the backend's atan precision.
float ComputeSectorAngle(float y, float x)
{
    if (x > 0.0)
    {
        return ComputeArctangent(y / x);
    }
    if (x < 0.0)
    {
        return ComputeArctangent(y / x) + PI;
    }
    return y > 0.0 ? 0.5 * PI : 0.0;
}
float ComputeAreaBetweenEllipsesFromAngles(float innerReciprocalSquareRootDeterminant,
                                           float innerAngle,
                                           float outerReciprocalSquareRootDeterminant,
                                           float outerAngle)
{
    float innerArea = innerReciprocalSquareRootDeterminant * innerAngle;
    float result = fma(outerReciprocalSquareRootDeterminant, outerAngle, -innerArea);
    return result > 0.0 ? 0.5 * result : 0.0;
}
float ComputeAreaBetweenEllipsesInSector(float2 innerEllipse, float2 outerEllipse, float2 direction0, float2 direction1)
{
    float innerReciprocalSquareRootDeterminant = GetEllipseReciprocalSquareRootDeterminant(innerEllipse);
    float outerReciprocalSquareRootDeterminant = GetEllipseReciprocalSquareRootDeterminant(outerEllipse);
    float directionDeterminant = max(0.0, dot(direction1, Rotate90(direction0)));
    float innerDot =
        innerReciprocalSquareRootDeterminant * dot(direction0, TransformByEllipse(innerEllipse, direction1));
    float outerDot =
        outerReciprocalSquareRootDeterminant * dot(direction0, TransformByEllipse(outerEllipse, direction1));
    return ComputeAreaBetweenEllipsesFromAngles(innerReciprocalSquareRootDeterminant,
                                                ComputeSectorAngle(directionDeterminant, innerDot),
                                                outerReciprocalSquareRootDeterminant,
                                                ComputeSectorAngle(directionDeterminant, outerDot));
}
float ComputeEllipseAreaInSector(float2 ellipse, float2 direction0, float2 direction1)
{
    float reciprocalSquareRootDeterminant = GetEllipseReciprocalSquareRootDeterminant(ellipse);
    float directionDeterminant = max(0.0, dot(direction1, Rotate90(direction0)));
    float ellipseDot = reciprocalSquareRootDeterminant * dot(direction0, TransformByEllipse(ellipse, direction1));
    return 0.5 * reciprocalSquareRootDeterminant * ComputeSectorAngle(directionDeterminant, ellipseDot);
}
float MixWithMultiplyAdd(float x, float y, float t)
{
    return fma(t, y, fma(-t, x, x));
}
float ComputeDeterminant(float2 column0, float2 column1)
{
    return column0.x * column1.y - column1.x * column0.y;
}
// The entries Xx, Xy, Yx, Yy of a 2x2 matrix whose quadratic form w^T M w is solved for w.
float4 MakeOuterProduct(float2 left, float2 right)
{
    return float4(left.x * right.x, left.x * right.y, left.y * right.x, left.y * right.y);
}
// A nonzero root of w^T M w = 0 that depends continuously on M (Blinn 2006).
float2 SolveHomogeneousQuadratic(float4 quadratic)
{
    float coefficient = 0.5 * (quadratic.z + quadratic.y);
    float discriminantSquareRoot = sqrt(max(0.0, coefficient * coefficient - quadratic.x * quadratic.w));
    float scaledRoot = abs(coefficient) + discriminantSquareRoot;
    return coefficient >= 0.0 ? float2(scaledRoot, -quadratic.x) : float2(quadratic.w, scaledRoot);
}
float2 NormalizeApproximatelyAndFlip(float2 value, float2 semiCircle)
{
    float norm = abs(value.x) + abs(value.y);
    float scaling = norm > 0.0 ? 1.0 / norm : 1.0;
    scaling = dot(value, semiCircle) >= 0.0 ? scaling : -scaling;
    return value * scaling;
}
// Ellipse vector u = n_xy / n_z for the great circle through two vertices and its kind (supplement A.7,
// Algorithm 1); a degenerate edge returns a zero vector.
float2 MakeEllipseFromEdge(float3 vertex0, float3 vertex1, out uint outKind)
{
    float3 normal = ComputeStableCross(vertex0, vertex1);
    if (normal.z == 0.0)
    {
        outKind = PROJECTED_ELLIPSE_DEGENERATE;
        return float2(0.0);
    }
    outKind = normal.z < 0.0 ? PROJECTED_ELLIPSE_INNER : PROJECTED_ELLIPSE_OUTER;
    return normal.xy * (1.0 / normal.z);
}
// Swap two vertices with their ellipses when the shorter path from left to right is clockwise. Equal angles put a
// degenerate ellipse first (supplement A.2, rule 3). Callers pass compile-time indices.
void CompareAndSwap(inout ProjectedSolidAnglePolygon inoutPolygon, uint left, uint right)
{
    float2 leftVertex = inoutPolygon.VerticesXy[left];
    float2 rightVertex = inoutPolygon.VerticesXy[right];
    float normalZ = ComputeDifferenceOfProducts(leftVertex.x, -rightVertex.y, leftVertex.y, -rightVertex.x);
    bool shouldSwap = normalZ == 0.0 ? inoutPolygon.EllipseKinds[right] == PROJECTED_ELLIPSE_DEGENERATE : normalZ > 0.0;
    float2 leftEllipse = inoutPolygon.Ellipses[left];
    float2 rightEllipse = inoutPolygon.Ellipses[right];
    uint leftKind = inoutPolygon.EllipseKinds[left];
    uint rightKind = inoutPolygon.EllipseKinds[right];
    inoutPolygon.VerticesXy[left] = shouldSwap == true ? rightVertex : leftVertex;
    inoutPolygon.VerticesXy[right] = shouldSwap == true ? leftVertex : rightVertex;
    inoutPolygon.Ellipses[left] = shouldSwap == true ? rightEllipse : leftEllipse;
    inoutPolygon.Ellipses[right] = shouldSwap == true ? leftEllipse : rightEllipse;
    inoutPolygon.EllipseKinds[left] = shouldSwap == true ? rightKind : leftKind;
    inoutPolygon.EllipseKinds[right] = shouldSwap == true ? leftKind : rightKind;
}
// Counterclockwise sort of the bitonic vertex sequence with the sorting networks of supplement A.6.
void SortConvexPolygonVertices(inout ProjectedSolidAnglePolygon inoutPolygon)
{
    uint count = inoutPolygon.VertexCount;
    if (count == 3)
    {
        CompareAndSwap(inoutPolygon, 1, 2);
    }
    else if (count == 4)
    {
        CompareAndSwap(inoutPolygon, 1, 3);
    }
    else if (count == 5)
    {
        CompareAndSwap(inoutPolygon, 2, 4);
        CompareAndSwap(inoutPolygon, 1, 3);
        CompareAndSwap(inoutPolygon, 1, 2);
        CompareAndSwap(inoutPolygon, 0, 3);
        CompareAndSwap(inoutPolygon, 3, 4);
    }
    else if (count == 6)
    {
        CompareAndSwap(inoutPolygon, 3, 5);
        CompareAndSwap(inoutPolygon, 2, 4);
        CompareAndSwap(inoutPolygon, 1, 5);
        CompareAndSwap(inoutPolygon, 0, 4);
        CompareAndSwap(inoutPolygon, 4, 5);
        CompareAndSwap(inoutPolygon, 1, 3);
    }
    else if (count == 7)
    {
        CompareAndSwap(inoutPolygon, 2, 5);
        CompareAndSwap(inoutPolygon, 1, 6);
        CompareAndSwap(inoutPolygon, 5, 6);
        CompareAndSwap(inoutPolygon, 3, 4);
        CompareAndSwap(inoutPolygon, 0, 4);
        CompareAndSwap(inoutPolygon, 4, 6);
        CompareAndSwap(inoutPolygon, 1, 3);
        CompareAndSwap(inoutPolygon, 3, 5);
        CompareAndSwap(inoutPolygon, 4, 5);
    }
    else if (count == 8)
    {
        CompareAndSwap(inoutPolygon, 2, 6);
        CompareAndSwap(inoutPolygon, 3, 7);
        CompareAndSwap(inoutPolygon, 1, 5);
        CompareAndSwap(inoutPolygon, 0, 4);
        CompareAndSwap(inoutPolygon, 4, 6);
        CompareAndSwap(inoutPolygon, 5, 7);
        CompareAndSwap(inoutPolygon, 6, 7);
        CompareAndSwap(inoutPolygon, 4, 5);
        CompareAndSwap(inoutPolygon, 1, 3);
    }
    CompareAndSwap(inoutPolygon, 0, 2);
    if (count >= 4)
    {
        CompareAndSwap(inoutPolygon, 2, 3);
    }
    CompareAndSwap(inoutPolygon, 0, 1);
}
// Advance the inner/outer pairing to sector i (paper section 3.7).
void AdvanceSectorEllipses(ProjectedSolidAnglePolygon polygon, uint i, inout SectorEllipses inoutEllipses)
{
    if (i == 0)
    {
        inoutEllipses.Inner = polygon.InnerEllipse0;
        inoutEllipses.Outer = polygon.Ellipses[0];
        inoutEllipses.OuterKind = polygon.EllipseKinds[0];
    }
    else if (polygon.EllipseKinds[i] == PROJECTED_ELLIPSE_INNER)
    {
        inoutEllipses.Inner = polygon.Ellipses[i];
    }
    else
    {
        inoutEllipses.Outer = polygon.Ellipses[i];
        inoutEllipses.OuterKind = polygon.EllipseKinds[i];
    }
}
// Uniform sample of the area between two ellipses within one sector (supplement A.7, Algorithm 3).
float2 SampleSectorBetweenEllipses(float2 randomNumbers,
                                   float targetArea,
                                   float2 innerEllipse,
                                   float2 outerEllipse,
                                   float2 direction0,
                                   float2 direction1,
                                   uint iterationCount)
{
    // Split the sector in half for the initialization (paper section 3.4).
    float2 quadDirection0 = normalize(direction0);
    float2 quadDirection2 = normalize(direction1);
    float2 quadDirection1 = quadDirection0 + quadDirection2;
    float innerFactor0 = GetEllipseNormalizedDirectionFactor(innerEllipse, quadDirection0);
    float innerFactor1 = GetEllipseDirectionFactor(innerEllipse, quadDirection1);
    float innerFactor2 = GetEllipseNormalizedDirectionFactor(innerEllipse, quadDirection2);
    float outerFactor0 = GetEllipseNormalizedDirectionFactor(outerEllipse, quadDirection0);
    float outerFactor1 = GetEllipseDirectionFactor(outerEllipse, quadDirection1);
    float outerFactor2 = GetEllipseNormalizedDirectionFactor(outerEllipse, quadDirection2);
    float quadArea0 = outerFactor0 * outerFactor1 - innerFactor0 * innerFactor1;
    float quadArea1 = outerFactor1 * outerFactor2 - innerFactor1 * innerFactor2;
    // Select the quad to initialize from; move its data to index 2.
    float targetQuadArea = MixWithMultiplyAdd(-quadArea0, quadArea1, randomNumbers.x);
    bool isFirstQuad = targetQuadArea <= 0.0;
    quadDirection2 = isFirstQuad == true ? quadDirection0 : quadDirection2;
    innerFactor2 = isFirstQuad == true ? innerFactor0 : innerFactor2;
    outerFactor2 = isFirstQuad == true ? outerFactor0 : outerFactor2;
    targetQuadArea += isFirstQuad == true ? quadArea0 : -quadArea1;
    targetQuadArea *= abs(ComputeDeterminant(quadDirection1, quadDirection2));
    // Edge normals of the selected quad, built like half vectors (supplement A.4).
    float2 innerNormal = quadDirection1 * innerFactor1 + quadDirection2 * innerFactor2;
    float2 outerNormal = quadDirection1 * outerFactor1 + quadDirection2 * outerFactor2;
    innerNormal = TransformByEllipse(innerEllipse, innerNormal);
    outerNormal = TransformByEllipse(outerEllipse, outerNormal);
    float innerOffset = dot(innerNormal, quadDirection1) * innerFactor1;
    float outerOffset = dot(outerNormal, quadDirection1) * outerFactor1;
    float4 quadratic =
        MakeOuterProduct(Rotate90(quadDirection2) * (outerOffset * outerFactor2), innerNormal) -
        MakeOuterProduct(Rotate90(quadDirection2) * (innerOffset * innerFactor2) + innerNormal * targetQuadArea,
                         outerNormal);
    float2 currentDirection = SolveHomogeneousQuadratic(quadratic);

    // Near the sector boundaries the initialization is exact but the iteration may be unstable.
    iterationCount =
        abs(randomNumbers.x - 0.5) <= 0.5 - PROJECTED_POLYGON_ITERATION_BOUNDARY_TOLERANCE ? iterationCount : 0;
    float innerReciprocalSquareRootDeterminant = GetEllipseReciprocalSquareRootDeterminant(innerEllipse);
    float outerReciprocalSquareRootDeterminant = GetEllipseReciprocalSquareRootDeterminant(outerEllipse);
    [loop] for (uint i = 0; i != iterationCount; ++i)
    {
        currentDirection = NormalizeApproximatelyAndFlip(currentDirection, quadDirection1);
        float2 innerDirection = TransformByEllipse(innerEllipse, currentDirection);
        float2 outerDirection = TransformByEllipse(outerEllipse, currentDirection);
        // Residual error of the current direction (Equation 7).
        float directionDeterminant = max(0.0, dot(currentDirection, Rotate90(quadDirection0)));
        float error =
            targetArea -
            ComputeAreaBetweenEllipsesFromAngles(
                innerReciprocalSquareRootDeterminant,
                ComputeSectorAngle(directionDeterminant,
                                   innerReciprocalSquareRootDeterminant * dot(quadDirection0, innerDirection)),
                outerReciprocalSquareRootDeterminant,
                ComputeSectorAngle(directionDeterminant,
                                   outerReciprocalSquareRootDeterminant * dot(quadDirection0, outerDirection)));
        // The tangent-line quadratic T whose root is the next direction (Equation 5).
        quadratic = MakeOuterProduct(innerDirection - outerDirection, Rotate90(currentDirection)) -
                    MakeOuterProduct(innerDirection * (2.0 * error), outerDirection);
        currentDirection = SolveHomogeneousQuadratic(quadratic);
    }

    currentDirection = dot(currentDirection, quadDirection1) >= 0.0 ? currentDirection : -currentDirection;
    float innerFactor = 1.0 / GetEllipseDirectionFactorReciprocalSquare(innerEllipse, currentDirection);
    float outerFactor = 1.0 / GetEllipseDirectionFactorReciprocalSquare(outerEllipse, currentDirection);
    return currentDirection * sqrt(MixWithMultiplyAdd(innerFactor, outerFactor, randomNumbers.y));
}
// Clip to z >= 0, keeping vertices on the horizon once. Returns the vertex count, zero when fewer than three remain.
uint ClipPolygonToUpperHemisphere(float3 positions[MAX_PROJECTED_POLYGON_INPUT_VERTICES],
                                  uint vertexCount,
                                  out float3 outPositions[MAX_PROJECTED_POLYGON_VERTICES])
{
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_VERTICES; ++i)
    {
        outPositions[i] = float3(0.0);
    }
    uint outCount = 0;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        if (i >= vertexCount)
        {
            break;
        }
        float3 start = positions[i];
        float3 end = positions[i + 1 < vertexCount ? i + 1 : 0];
        if (start.z >= 0.0)
        {
            outPositions[outCount] = start;
            ++outCount;
        }
        if ((start.z > 0.0 && end.z < 0.0) || (start.z < 0.0 && end.z > 0.0))
        {
            float t = start.z / (start.z - end.z);
            outPositions[outCount] =
                float3(MixWithMultiplyAdd(start.x, end.x, t), MixWithMultiplyAdd(start.y, end.y, t), 0.0);
            ++outCount;
        }
    }
    return outCount >= 3 ? outCount : 0;
}
// Clip in the receiver frame before an LTC transform. Matches the C++ API; failure preserves output.
bool TryClipProjectedPolygonToUpperHemisphere(in ProjectedPolygonInput polygon,
                                              inout ProjectedPolygonInput inoutPolygon)
{
    static const uint MINIMUM_VERTEX_COUNT = 3;
    if (polygon.VertexCount < MINIMUM_VERTEX_COUNT || polygon.VertexCount > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return false;
    }
    for (uint i = 0; i < polygon.VertexCount; ++i)
    {
        if (IsFiniteBits(polygon.PositionsTangent[i]) == false)
        {
            return false;
        }
    }
    float maximumMagnitude = 0;
    for (uint i = 0; i < polygon.VertexCount; ++i)
    {
        float3 value = abs(polygon.PositionsTangent[i]);
        maximumMagnitude = max(maximumMagnitude, max(value.x, max(value.y, value.z)));
    }
    if (maximumMagnitude == 0)
    {
        inoutPolygon = (ProjectedPolygonInput)0;
        return true;
    }
    float3 positions[MAX_PROJECTED_POLYGON_INPUT_VERTICES];
    for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        positions[i] = i < polygon.VertexCount ? polygon.PositionsTangent[i] / maximumMagnitude : float3(0);
    }
    float3 clipped[MAX_PROJECTED_POLYGON_VERTICES];
    uint count = ClipPolygonToUpperHemisphere(positions, polygon.VertexCount, clipped);
    if (count > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return false;
    }
    ProjectedPolygonInput result = (ProjectedPolygonInput)0;
    result.VertexCount = count;
    for (uint i = 0; i < count; ++i)
    {
        result.PositionsTangent[i] = clipped[i];
    }
    inoutPolygon = result;
    return true;
}
// Integrate cosine over a convex polygon without constructing importance-sampling state.
// Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
// "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", TOG 35(4),
// Article 41 (2016), section 3 and Algorithm 1, https://doi.org/10.1145/2897824.2925895.
// The spherical edge vector area is half the sum of edge angle times unit edge normal.
// Clip first; abs preserves the sampling API's winding-independent polygon contract.
float3 NormalizeProjectedDirection(float3 directionTangent)
{
    float magnitude = max(abs(directionTangent.x), max(abs(directionTangent.y), abs(directionTangent.z)));
    return magnitude > 0 ? NormalizeOrZero(directionTangent / magnitude) : float3(0);
}
float ComputeProjectedSolidAngleEdge(float3 startTangent, float3 endTangent)
{
    float3 edgeNormalTangent = ComputeStableCross(startTangent, endTangent);
    float sine = length(edgeNormalTangent);
    if (sine == 0)
    {
        return 0;
    }
    return edgeNormalTangent.z * (atan2(sine, dot(startTangent, endTangent)) / sine);
}
// Returns steradians projected onto +z, in [0, PI]; invalid or empty inputs return zero.
float ComputeProjectedSolidAngleIntegral(ProjectedPolygonInput polygon)
{
    static const uint MINIMUM_VERTEX_COUNT = 3;
    if (polygon.VertexCount < MINIMUM_VERTEX_COUNT || polygon.VertexCount > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return 0;
    }
    float3 aTangent = NormalizeProjectedDirection(polygon.PositionsTangent[0]);
    float3 bTangent = NormalizeProjectedDirection(polygon.PositionsTangent[1]);
    float3 cTangent = NormalizeProjectedDirection(polygon.PositionsTangent[2]);
    if (dot(aTangent, ComputeStableCross(bTangent, cTangent)) == 0)
    {
        return 0;
    }
    float3 firstTangent = float3(0);
    float3 previousTangent = float3(0);
    uint count = 0;
    float integral = 0;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        if (i >= polygon.VertexCount)
        {
            break;
        }
        float3 startTangent = polygon.PositionsTangent[i];
        float3 endTangent = polygon.PositionsTangent[0];
        if (i + 1 < polygon.VertexCount)
        {
            endTangent = polygon.PositionsTangent[(i + 1) % MAX_PROJECTED_POLYGON_INPUT_VERTICES];
        }
        if (IsFiniteBits(startTangent) == false)
        {
            return 0;
        }
        // Positive per-vertex scaling preserves spherical edges and horizon intersections.
        startTangent = NormalizeProjectedDirection(startTangent);
        endTangent = NormalizeProjectedDirection(endTangent);
        if (startTangent.z >= 0)
        {
            integral += ComputeProjectedSolidAngleEdge(previousTangent, startTangent);
            if (count == 0)
            {
                firstTangent = startTangent;
            }
            previousTangent = startTangent;
            ++count;
        }
        if ((startTangent.z > 0 && endTangent.z < 0) || (startTangent.z < 0 && endTangent.z > 0))
        {
            float t = startTangent.z / (startTangent.z - endTangent.z);
            float3 intersectionTangent = NormalizeOrZero(float3(lerp(startTangent.xy, endTangent.xy, t), 0));
            integral += ComputeProjectedSolidAngleEdge(previousTangent, intersectionTangent);
            if (count == 0)
            {
                firstTangent = intersectionTangent;
            }
            previousTangent = intersectionTangent;
            ++count;
        }
    }
    integral += ComputeProjectedSolidAngleEdge(previousTangent, firstTangent);
    return count >= MINIMUM_VERTEX_COUNT ? min(abs(integral) * 0.5, PI) : 0;
}
// Fast triangle case for raster area lights. Horizon crossings use the same streamed clip.
float ComputeProjectedTriangleIntegral(float3 aTangent, float3 bTangent, float3 cTangent)
{
    if (IsFiniteBits(aTangent) == false || IsFiniteBits(bTangent) == false || IsFiniteBits(cTangent) == false)
    {
        return 0;
    }
    if (aTangent.z >= 0 && bTangent.z >= 0 && cTangent.z >= 0)
    {
        aTangent = NormalizeProjectedDirection(aTangent);
        bTangent = NormalizeProjectedDirection(bTangent);
        cTangent = NormalizeProjectedDirection(cTangent);
        if (dot(aTangent, ComputeStableCross(bTangent, cTangent)) == 0)
        {
            return 0;
        }
        float integral = ComputeProjectedSolidAngleEdge(aTangent, bTangent) +
                         ComputeProjectedSolidAngleEdge(bTangent, cTangent) +
                         ComputeProjectedSolidAngleEdge(cTangent, aTangent);
        return min(abs(integral) * 0.5, PI);
    }
    ProjectedPolygonInput polygon = (ProjectedPolygonInput)0;
    static const uint TRIANGLE_CORNER_COUNT = 3;
    polygon.VertexCount = TRIANGLE_CORNER_COUNT;
    polygon.PositionsTangent[0] = aTangent;
    polygon.PositionsTangent[1] = bTangent;
    polygon.PositionsTangent[2] = cTangent;
    return ComputeProjectedSolidAngleIntegral(polygon);
}
ProjectedSolidAnglePolygon MakeEmptyProjectedSolidAnglePolygon(bool isMirrored)
{
    ProjectedSolidAnglePolygon result;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_VERTICES; ++i)
    {
        result.VerticesXy[i] = float2(0.0);
        result.Ellipses[i] = float2(0.0);
        result.EllipseKinds[i] = PROJECTED_ELLIPSE_OUTER;
        result.SectorProjectedSolidAngles[i] = 0.0;
    }
    result.InnerEllipse0 = float2(0.0);
    result.IsCentralCase = true;
    result.ProjectedSolidAngle = 0.0;
    result.VertexCount = 0;
    result.IsMirrored = isMirrored;
    return result;
}
// Clip and prepare a polygon. Returns false for a vertex count outside [3, 7] or a nonfinite coordinate, the C++
// InvalidArgument and NonFiniteInput statuses; an empty or edge-on polygon succeeds with ProjectedSolidAngle == 0.
bool TryPrepareProjectedSolidAnglePolygon(ProjectedPolygonInput polygon, out ProjectedSolidAnglePolygon outPolygon)
{
    outPolygon = MakeEmptyProjectedSolidAnglePolygon(false);
    if (polygon.VertexCount < 3 || polygon.VertexCount > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return false;
    }
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        if (i < polygon.VertexCount && IsFiniteBits(polygon.PositionsTangent[i]) == false)
        {
            return false;
        }
    }
    float orientation =
        dot(polygon.PositionsTangent[0], ComputeStableCross(polygon.PositionsTangent[1], polygon.PositionsTangent[2]));
    if ((orientation > 0.0 || orientation < 0.0) == false)
    {
        return true;
    }
    bool isMirrored = orientation < 0.0;
    float3 positions[MAX_PROJECTED_POLYGON_INPUT_VERTICES];
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        positions[i] = polygon.PositionsTangent[i];
        positions[i].y = isMirrored == true ? -positions[i].y : positions[i].y;
    }
    float3 clipped[MAX_PROJECTED_POLYGON_VERTICES];
    uint count = ClipPolygonToUpperHemisphere(positions, polygon.VertexCount, clipped);
    ProjectedSolidAnglePolygon result = MakeEmptyProjectedSolidAnglePolygon(isMirrored);
    if (count == 0)
    {
        outPolygon = result;
        return true;
    }

    // Associate each vertex with the next ellipse counterclockwise (paper section 3.7).
    result.VertexCount = count;
    float2 ellipses[MAX_PROJECTED_POLYGON_VERTICES];
    uint kinds[MAX_PROJECTED_POLYGON_VERTICES];
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_VERTICES; ++i)
    {
        ellipses[i] = float2(0.0);
        kinds[i] = PROJECTED_ELLIPSE_OUTER;
        if (i < count)
        {
            result.VerticesXy[i] = clipped[i].xy;
            ellipses[i] = MakeEllipseFromEdge(clipped[i], clipped[i + 1 < count ? i + 1 : 0], kinds[i]);
        }
    }
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_VERTICES; ++i)
    {
        if (i >= count)
        {
            break;
        }
        uint previous = i > 0 ? i - 1 : count - 1;
        bool isInner = kinds[i] == PROJECTED_ELLIPSE_INNER;
        result.Ellipses[i] = isInner == true ? ellipses[previous] : ellipses[i];
        result.EllipseKinds[i] = isInner == true ? kinds[previous] : kinds[i];
        if (kinds[previous] == PROJECTED_ELLIPSE_INNER && isInner == false)
        {
            result.InnerEllipse0 = ellipses[previous];
            result.IsCentralCase = false;
        }
    }

    if (result.IsCentralCase == true)
    {
        // Central case: one outer ellipse bounds each sector; degenerate ones enclose nothing (supplement A.2).
        [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_VERTICES; ++i)
        {
            if (i >= count)
            {
                break;
            }
            result.SectorProjectedSolidAngles[i] =
                result.EllipseKinds[i] == PROJECTED_ELLIPSE_DEGENERATE
                    ? 0.0
                    : ComputeEllipseAreaInSector(result.Ellipses[i],
                                                 result.VerticesXy[i],
                                                 result.VerticesXy[i + 1 < count ? i + 1 : 0]);
            result.ProjectedSolidAngle += result.SectorProjectedSolidAngles[i];
        }
    }
    else
    {
        // Decentral case: VertexCount - 1 sectors, each between an inner and an outer ellipse.
        SortConvexPolygonVertices(result);
        SectorEllipses sector;
        sector.Inner = float2(0.0);
        sector.Outer = float2(0.0);
        sector.OuterKind = PROJECTED_ELLIPSE_OUTER;
        [unroll] for (uint i = 0; i + 1 < MAX_PROJECTED_POLYGON_VERTICES; ++i)
        {
            if (i + 1 >= count)
            {
                break;
            }
            AdvanceSectorEllipses(result, i, sector);
            result.SectorProjectedSolidAngles[i] = sector.OuterKind == PROJECTED_ELLIPSE_DEGENERATE
                                                       ? 0.0
                                                       : ComputeAreaBetweenEllipsesInSector(sector.Inner,
                                                                                            sector.Outer,
                                                                                            result.VerticesXy[i],
                                                                                            result.VerticesXy[i + 1]);
            result.ProjectedSolidAngle += result.SectorProjectedSolidAngles[i];
        }
    }
    outPolygon = result;
    return true;
}
// Map random numbers in [0, 1]^2 to a unit direction uniform in projected solid angle. Returns false, leaving
// outDirectionTangent zero, for random numbers outside [0, 1], an empty polygon or a nonfinite sample.
bool TrySampleProjectedSolidAnglePolygon(ProjectedSolidAnglePolygon polygon,
                                         float2 randomNumbers,
                                         out float3 outDirectionTangent)
{
    outDirectionTangent = float3(0.0);
    uint count = polygon.VertexCount;
    if ((all(randomNumbers >= 0.0) && all(randomNumbers <= 1.0)) == false ||
        (polygon.ProjectedSolidAngle > 0.0) == false || count < 3 || count > MAX_PROJECTED_POLYGON_VERTICES)
    {
        return false;
    }
    // Select the sector containing the first random number; sectors without area are never selected.
    float remaining = randomNumbers.x * polygon.ProjectedSolidAngle;
    uint sectorCount = polygon.IsCentralCase == true ? count : count - 1;
    uint selected = MAX_PROJECTED_POLYGON_VERTICES;
    float selectedTarget = 0.0;
    SectorEllipses ellipses;
    ellipses.Inner = float2(0.0);
    ellipses.Outer = float2(0.0);
    ellipses.OuterKind = PROJECTED_ELLIPSE_OUTER;
    SectorEllipses selectedEllipses = ellipses;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_VERTICES; ++i)
    {
        if (i >= sectorCount)
        {
            break;
        }
        float area = polygon.SectorProjectedSolidAngles[i];
        if (polygon.IsCentralCase == false)
        {
            AdvanceSectorEllipses(polygon, i, ellipses);
        }
        if (area > 0.0)
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
    if (selected == MAX_PROJECTED_POLYGON_VERTICES)
    {
        return false;
    }
    float sectorArea = polygon.SectorProjectedSolidAngles[selected];
    selectedTarget = min(selectedTarget, sectorArea);
    float2 direction0 = polygon.VerticesXy[selected];
    float2 sampled;
    if (polygon.IsCentralCase == true)
    {
        // Central case: invert the ellipse sector area in closed form (paper section 3.2 and supplement A.7).
        float2 outerEllipse = polygon.Ellipses[selected];
        float squareRootDeterminant = sqrt(GetEllipseDeterminant(outerEllipse));
        float angle = 2.0 * selectedTarget * squareRootDeterminant;
        sampled = direction0 * (cos(angle) * squareRootDeterminant) +
                  Rotate90(TransformByEllipse(outerEllipse, direction0)) * sin(angle);
        sampled = sampled * sqrt(randomNumbers.y / GetEllipseDirectionFactorReciprocalSquare(outerEllipse, sampled));
    }
    else
    {
        // Decentral case: the two-quad initialization refined by tangent-line iterations (paper section 3.3).
        sampled = SampleSectorBetweenEllipses(float2(selectedTarget / sectorArea, randomNumbers.y),
                                              selectedTarget,
                                              selectedEllipses.Inner,
                                              selectedEllipses.Outer,
                                              direction0,
                                              polygon.VerticesXy[selected + 1],
                                              PROJECTED_POLYGON_ITERATION_COUNT);
    }
    // Lift the sample to the hemisphere (Equation 2) and undo the mirroring.
    float z = sqrt(max(0.0, fma(-sampled.x, sampled.x, fma(-sampled.y, sampled.y, 1.0))));
    float3 direction = float3(sampled.x, polygon.IsMirrored == true ? -sampled.y : sampled.y, z);
    if (IsFiniteBits(direction) == false)
    {
        return false;
    }
    outDirectionTangent = direction;
    return true;
}
// cos(theta) / ProjectedSolidAngle for a direction inside the polygon's solid angle; zero below the horizon, for a
// nonfinite direction or an empty polygon. No containment test is performed.
float EvaluateProjectedSolidAnglePolygonDensity(ProjectedSolidAnglePolygon polygon, float3 directionTangent)
{
    if (IsFiniteBits(directionTangent) == false || (polygon.ProjectedSolidAngle > 0.0) == false ||
        directionTangent.z <= 0.0)
    {
        return 0.0;
    }
    return directionTangent.z / polygon.ProjectedSolidAngle;
}
