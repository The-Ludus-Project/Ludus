#pragma once
#include "projected_solid_angle.slang"

// Linearly transformed cosines (LTCs) integrated over and sampled within convex polygons; the GPU port of
// FoundationMath's linearly_transformed_cosine.hpp with the same operations.
// Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt, "Real-Time Polygonal-Light Shading with
// Linearly Transformed Cosines", ACM Transactions on Graphics 35(4), Article 41, 2016 (SIGGRAPH 2016),
// https://doi.org/10.1145/2897824.2925895, section 3.1 (Equation 1), Equation 3 and Algorithm 1, and to Christoph
// Peters, "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics 40(4), Article 140, 2021,
// section 4.1, https://doi.org/10.1145/3450626.3459672. Implemented from the papers' text.
//
// Matrices are three float3 columns acting on column vectors, the layout of the C++ Matrix3, so the host can pass
// them without a row/column-major convention. Floats are classified on their bits because Ludus compiles Metal
// shaders with fast math.

// MIN_LINEARLY_TRANSFORMED_COSINE_DETERMINANT of the C++ header, 2^-32.
static const float MIN_LINEARLY_TRANSFORMED_COSINE_DETERMINANT = 2.3283064365386963e-10;
static const uint LINEARLY_TRANSFORMED_COSINE_MANTISSA_BIT_COUNT = 23;
static const uint LINEARLY_TRANSFORMED_COSINE_SMALLEST_NORMAL_BITS = 0x00800000u;
// A largest-magnitude entry with this biased exponent or above (2^127) has no normal power-of-two scale into [1, 2).
static const uint LINEARLY_TRANSFORMED_COSINE_MAX_SCALABLE_EXPONENT = 253;
// The scale 2^(127 - e), which moves biased exponent e to 127, has biased exponent 254 - e.
static const uint LINEARLY_TRANSFORMED_COSINE_TWICE_EXPONENT_BIAS = 254;

// The same fields as the C++ LinearlyTransformedCosine.
struct LinearlyTransformedCosine
{
    // M^-1 scaled by a power of two so that its largest-magnitude entry lies in [1, 2).
    float3 InverseTransform[3];
    // The adjugate of InverseTransform, a positive multiple of M.
    float3 Transform[3];
    float InverseDeterminant;
};

// The same fields as the C++ LinearlyTransformedCosinePolygon.
struct LinearlyTransformedCosinePolygon
{
    ProjectedSolidAnglePolygon TransformedPolygon;
    LinearlyTransformedCosine Distribution;
};

float3 MultiplyColumns(float3 columns[3], float3 value)
{
    return columns[0] * value.x + columns[1] * value.y + columns[2] * value.z;
}
// True for a positive normal value: excludes zeros, subnormals, negatives and nonfinite values.
bool IsPositiveNormalBits(float value)
{
    uint bits = asuint(value);
    return bits >= LINEARLY_TRANSFORMED_COSINE_SMALLEST_NORMAL_BITS && bits < FLOAT_EXPONENT_BITS;
}
float GetLargestMagnitude(float3 value)
{
    return max(abs(value.x), max(abs(value.y), abs(value.z)));
}
LinearlyTransformedCosine MakeIdentityLinearlyTransformedCosine()
{
    LinearlyTransformedCosine result;
    result.InverseTransform[0] = float3(1.0, 0.0, 0.0);
    result.InverseTransform[1] = float3(0.0, 1.0, 0.0);
    result.InverseTransform[2] = float3(0.0, 0.0, 1.0);
    result.Transform = result.InverseTransform;
    result.InverseDeterminant = 1.0;
    return result;
}
// Make an LTC from the three columns of M^-1. Returns false, leaving the identity, for a nonfinite entry, a zero
// matrix, a largest-magnitude entry of at least 2^127, a determinant that is not positive or a scaled determinant below
// MIN_LINEARLY_TRANSFORMED_COSINE_DETERMINANT: the C++ NonFiniteInput, Degenerate and IllConditioned statuses.
bool TryMakeLinearlyTransformedCosine(float3 inverseTransform[3], out LinearlyTransformedCosine outDistribution)
{
    outDistribution = MakeIdentityLinearlyTransformedCosine();
    if (IsFiniteBits(inverseTransform[0]) == false || IsFiniteBits(inverseTransform[1]) == false ||
        IsFiniteBits(inverseTransform[2]) == false)
    {
        return false;
    }
    float largestMagnitude =
        max(GetLargestMagnitude(inverseTransform[0]),
            max(GetLargestMagnitude(inverseTransform[1]), GetLargestMagnitude(inverseTransform[2])));
    uint biasedExponent =
        (asuint(largestMagnitude) & FLOAT_EXPONENT_BITS) >> LINEARLY_TRANSFORMED_COSINE_MANTISSA_BIT_COUNT;
    if (biasedExponent == 0 || biasedExponent > LINEARLY_TRANSFORMED_COSINE_MAX_SCALABLE_EXPONENT)
    {
        return false;
    }
    float scale = asfloat((LINEARLY_TRANSFORMED_COSINE_TWICE_EXPONENT_BIAS - biasedExponent)
                          << LINEARLY_TRANSFORMED_COSINE_MANTISSA_BIT_COUNT);
    LinearlyTransformedCosine result;
    result.InverseTransform[0] = inverseTransform[0] * scale;
    result.InverseTransform[1] = inverseTransform[1] * scale;
    result.InverseTransform[2] = inverseTransform[2] * scale;
    float3 row0 = cross(result.InverseTransform[1], result.InverseTransform[2]);
    float3 row1 = cross(result.InverseTransform[2], result.InverseTransform[0]);
    float3 row2 = cross(result.InverseTransform[0], result.InverseTransform[1]);
    float determinant = dot(result.InverseTransform[0], row0);
    if ((determinant > 0.0) == false || determinant < MIN_LINEARLY_TRANSFORMED_COSINE_DETERMINANT)
    {
        return false;
    }
    // Columns of the adjugate, whose rows are row0, row1 and row2.
    result.Transform[0] = float3(row0.x, row1.x, row2.x);
    result.Transform[1] = float3(row0.y, row1.y, row2.y);
    result.Transform[2] = float3(row0.z, row1.z, row2.z);
    result.InverseDeterminant = determinant;
    outDistribution = result;
    return true;
}
// max(0, (M^-1 w).z) |M^-1| / |M^-1 w|^4, or zero.
float EvaluateLinearlyTransformedCosineJacobian(LinearlyTransformedCosine distribution, float3 directionTangent)
{
    if (IsFiniteBits(directionTangent) == false)
    {
        return 0.0;
    }
    float3 cosineDirection = MultiplyColumns(distribution.InverseTransform, directionTangent);
    float lengthSquared = dot(cosineDirection, cosineDirection);
    if ((cosineDirection.z > 0.0) == false || IsPositiveNormalBits(lengthSquared) == false)
    {
        return 0.0;
    }
    float inverseLength = 1.0 / sqrt(lengthSquared);
    float cosine = cosineDirection.z * inverseLength;
    float result = cosine * (distribution.InverseDeterminant * (inverseLength * inverseLength * inverseLength));
    return IsFiniteBits(result) == true ? result : 0.0;
}
// Density per steradian over the whole sphere; zero where (M^-1 w).z <= 0 or for an invalid direction.
float EvaluateLinearlyTransformedCosine(LinearlyTransformedCosine distribution, float3 directionTangent)
{
    return EvaluateLinearlyTransformedCosineJacobian(distribution, directionTangent) / PI;
}
// Transform a convex polygon by M^-1, clip it to z >= 0 and prepare it. Returns false for a vertex count outside
// [3, 7] or a nonfinite coordinate before or after the transform, the C++ InvalidArgument and NonFiniteInput statuses;
// an empty clip succeeds with TransformedPolygon.ProjectedSolidAngle == 0.
bool TryPrepareLinearlyTransformedCosinePolygon(ProjectedPolygonInput polygon,
                                                LinearlyTransformedCosine distribution,
                                                out LinearlyTransformedCosinePolygon outPolygon)
{
    outPolygon.TransformedPolygon = MakeEmptyProjectedSolidAnglePolygon(false);
    outPolygon.Distribution = distribution;
    if (polygon.VertexCount < 3 || polygon.VertexCount > MAX_PROJECTED_POLYGON_INPUT_VERTICES)
    {
        return false;
    }
    ProjectedPolygonInput transformed;
    transformed.VertexCount = polygon.VertexCount;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        if (i < polygon.VertexCount && IsFiniteBits(polygon.PositionsTangent[i]) == false)
        {
            return false;
        }
        transformed.PositionsTangent[i] = MultiplyColumns(distribution.InverseTransform, polygon.PositionsTangent[i]);
    }
    return TryPrepareProjectedSolidAnglePolygon(transformed, outPolygon.TransformedPolygon);
}
// The LTC's integral over the polygon, in [0, 1]; zero when the clip is empty.
float GetLinearlyTransformedCosinePolygonIntegral(LinearlyTransformedCosinePolygon polygon)
{
    return polygon.TransformedPolygon.ProjectedSolidAngle / PI;
}
// Map random numbers in [0, 1]^2 to a unit direction in the polygon distributed proportionally to the LTC. Returns
// false, leaving outDirectionTangent zero, for random numbers outside [0, 1], an empty clip or a nonfinite sample.
bool TrySampleLinearlyTransformedCosinePolygon(LinearlyTransformedCosinePolygon polygon,
                                               float2 randomNumbers,
                                               out float3 outDirectionTangent)
{
    outDirectionTangent = float3(0.0);
    float3 cosineDirection;
    if (TrySampleProjectedSolidAnglePolygon(polygon.TransformedPolygon, randomNumbers, cosineDirection) == false)
    {
        return false;
    }
    float3 direction = MultiplyColumns(polygon.Distribution.Transform, cosineDirection);
    float lengthSquared = dot(direction, direction);
    if (IsPositiveNormalBits(lengthSquared) == false)
    {
        return false;
    }
    float3 result = direction * (1.0 / sqrt(lengthSquared));
    if (IsFiniteBits(result) == false)
    {
        return false;
    }
    outDirectionTangent = result;
    return true;
}
// p(w) divided by the polygon integral for a direction inside the polygon's solid angle; zero where (M^-1 w).z <= 0,
// for an invalid direction or an empty clip. No containment test is performed.
float EvaluateLinearlyTransformedCosinePolygonDensity(LinearlyTransformedCosinePolygon polygon, float3 directionTangent)
{
    float projectedSolidAngle = polygon.TransformedPolygon.ProjectedSolidAngle;
    if ((projectedSolidAngle > 0.0) == false)
    {
        return 0.0;
    }
    float result =
        EvaluateLinearlyTransformedCosineJacobian(polygon.Distribution, directionTangent) / projectedSolidAngle;
    return IsFiniteBits(result) == true ? result : 0.0;
}
