#pragma once
#include "projected_solid_angle.slang"

// Linearly transformed cosines over convex polygonal lights; the GPU port of FoundationMath's
// linearly_transformed_cosine.hpp. Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David
// Neubelt, "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", ACM Transactions
// on Graphics 35(4), Article 41, 2016, sections 3-4, https://doi.org/10.1145/2897824.2925895, and to
// Christoph Peters, "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics
// 40(4), Article 140, 2021, section 4.1, https://doi.org/10.1145/3450626.3459672. Matrices are
// float3x3 rows acting on column vectors through mul(matrix, vector). The inverse is computed in
// single precision here and in double precision in C++.

// A convex polygon prepared for sampling in proportion to one LTC; the fields of the C++ LtcPolygon.
struct LtcPolygon
{
    ProjectedSolidAnglePolygon Polygon;
    float3x3 Transform;
    float3x3 InverseTransform;
    float InverseDeterminant;
};

bool IsFiniteBits(float3x3 value)
{
    return IsFiniteBits(value[0]) == true && IsFiniteBits(value[1]) == true && IsFiniteBits(value[2]) == true;
}
float ComputeDeterminant(float3x3 value)
{
    return dot(value[0], cross(value[1], value[2]));
}
// D(omega) = max(0, w_z) |det(M^-1)| / (pi |w|^4) with w = M^-1 omega (Heitz et al. Equation 1).
float EvaluateLtcDensity(float3x3 inverseTransform, float3 directionTangent)
{
    if (IsFiniteBits(inverseTransform) == false || IsFiniteBits(directionTangent) == false)
    {
        return 0.0;
    }
    float3 cosineDirection = mul(inverseTransform, directionTangent);
    float lengthSquared = dot(cosineDirection, cosineDirection);
    float inverseDeterminant = abs(ComputeDeterminant(inverseTransform));
    if (cosineDirection.z <= 0.0 || (lengthSquared > 0.0) == false)
    {
        return 0.0;
    }
    return cosineDirection.z * inverseDeterminant / (PI * lengthSquared * lengthSquared);
}
// Transform the polygon by M^-1, clip it to the cosine's horizon and prepare it. Returns false for an invalid
// polygon, a nonfinite matrix or a singular matrix, the C++ InvalidArgument, NonFiniteInput and Degenerate statuses.
bool TryPrepareLtcPolygon(ProjectedPolygonInput polygon, float3x3 inverseTransform, out LtcPolygon outPolygon)
{
    outPolygon.Polygon = MakeEmptyProjectedSolidAnglePolygon(false);
    outPolygon.Transform = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
    outPolygon.InverseTransform = inverseTransform;
    outPolygon.InverseDeterminant = 0.0;
    float determinant = ComputeDeterminant(inverseTransform);
    if (IsFiniteBits(inverseTransform) == false || (determinant > 0.0 || determinant < 0.0) == false)
    {
        return false;
    }
    // The columns of the inverse are cross products of row pairs divided by the determinant.
    float3x3 transform = transpose(float3x3(cross(inverseTransform[1], inverseTransform[2]),
                                            cross(inverseTransform[2], inverseTransform[0]),
                                            cross(inverseTransform[0], inverseTransform[1]))) *
                         (1.0 / determinant);
    if (IsFiniteBits(transform) == false)
    {
        return false;
    }
    ProjectedPolygonInput transformed = polygon;
    [unroll] for (uint i = 0; i < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++i)
    {
        transformed.PositionsTangent[i] = mul(inverseTransform, polygon.PositionsTangent[i]);
    }
    ProjectedSolidAnglePolygon prepared;
    if (TryPrepareProjectedSolidAnglePolygon(transformed, prepared) == false)
    {
        return false;
    }
    outPolygon.Polygon = prepared;
    outPolygon.Transform = transform;
    outPolygon.InverseDeterminant = abs(determinant);
    return true;
}
// The integral of the LTC over the polygon, in [0, 1] (Heitz et al. Equations 3 and 10).
float GetLtcPolygonIntegral(LtcPolygon polygon)
{
    return polygon.Polygon.ProjectedSolidAngle / PI;
}
// A direction in the polygon distributed in proportion to the LTC (Peters section 4.1). Returns false, leaving
// outDirectionTangent zero, when projected solid angle sampling fails or the polygon integral is zero.
bool TrySampleLtcPolygon(LtcPolygon polygon, float2 randomNumbers, out float3 outDirectionTangent)
{
    outDirectionTangent = float3(0.0);
    float3 cosineDirection;
    if (TrySampleProjectedSolidAnglePolygon(polygon.Polygon, randomNumbers, cosineDirection) == false)
    {
        return false;
    }
    float3 direction = mul(polygon.Transform, cosineDirection);
    float lengthSquared = dot(direction, direction);
    if ((lengthSquared > 0.0) == false)
    {
        return false;
    }
    direction = direction * (1.0 / sqrt(lengthSquared));
    if (IsFiniteBits(direction) == false)
    {
        return false;
    }
    outDirectionTangent = direction;
    return true;
}
// D(omega) divided by the polygon integral for a direction in the polygon; no containment test is performed.
float EvaluateLtcPolygonDensity(LtcPolygon polygon, float3 directionTangent)
{
    if (IsFiniteBits(directionTangent) == false || (polygon.Polygon.ProjectedSolidAngle > 0.0) == false)
    {
        return 0.0;
    }
    float3 cosineDirection = mul(polygon.InverseTransform, directionTangent);
    float lengthSquared = dot(cosineDirection, cosineDirection);
    if (cosineDirection.z <= 0.0 || (lengthSquared > 0.0) == false)
    {
        return 0.0;
    }
    return cosineDirection.z * polygon.InverseDeterminant /
           (polygon.Polygon.ProjectedSolidAngle * lengthSquared * lengthSquared);
}
