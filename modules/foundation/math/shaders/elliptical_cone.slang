#pragma once
#include "math.slang"
#include "projected_solid_angle.slang"
// GPU counterpart of elliptical_cone.cpp, independently executing the same vertex/edge/plane extrema.
// Thanks to Emre, Kanak and Steinberg, "High-Performance Elliptical Cone Tracing", CGF 44(7), e70230,
// 2025, Section 3, https://doi.org/10.1111/cgf.70230 . See the C++ owner for the derivation and contract.

static const float CONE_FRAME_SQUARED_LENGTH_TOLERANCE = 0.000001;
static const float CONE_FRAME_ORTHOGONALITY_TOLERANCE = 0.000001;
static const float CONE_DIAGONAL_COMPONENT = 0.7071067811865475244;
static const float CONE_AXIAL_ROUNDOFF_SCALE = 0.00000095367431640625; // Eight binary32 epsilons.
static const uint CONE_SIDE_COUNT = 8;

struct EllipticalCone
{
    float3 OriginWorld;
    float3 DirectionWorld;
    float3 AxisXWorld;
    float RadiusWorld;
    float Slope;
    float MinorToMajor;
    float NearWorld;
    float FarWorld;
};
struct ConeTriangle
{
    float3 FirstWorld;
    float3 SecondWorld;
    float3 ThirdWorld;
};
struct ConeTriangleIntersection
{
    bool IsHit;
    float NearWorld;
    float FarWorld;
    float3 NearBarycentric;
    float3 FarBarycentric;
};

bool IsValidEllipticalCone(EllipticalCone cone)
{
    return IsFiniteBits(cone.OriginWorld) == true && IsFiniteBits(cone.DirectionWorld) == true &&
           IsFiniteBits(cone.AxisXWorld) == true && IsFiniteBits(cone.RadiusWorld) == true && cone.RadiusWorld >= 0 &&
           IsFiniteBits(cone.Slope) == true && cone.Slope >= 0 && IsFiniteBits(cone.MinorToMajor) == true &&
           cone.MinorToMajor > 0 && cone.MinorToMajor <= 1 && IsFiniteBits(cone.NearWorld) == true &&
           cone.NearWorld >= 0 && IsFiniteBits(cone.FarWorld) == true && cone.FarWorld >= cone.NearWorld &&
           IsFiniteBits(cone.RadiusWorld + cone.Slope * cone.FarWorld) == true &&
           abs(dot(cone.DirectionWorld, cone.DirectionWorld) - 1) <= CONE_FRAME_SQUARED_LENGTH_TOLERANCE &&
           abs(dot(cone.AxisXWorld, cone.AxisXWorld) - 1) <= CONE_FRAME_SQUARED_LENGTH_TOLERANCE &&
           abs(dot(cone.AxisXWorld, cone.DirectionWorld)) <= CONE_FRAME_ORTHOGONALITY_TOLERANCE;
}
float3 TransformToEllipticalCone(EllipticalCone cone, float3 pointWorld)
{
    float3 offsetWorld = pointWorld - cone.OriginWorld;
    return float3(dot(offsetWorld, cone.AxisXWorld),
                  dot(offsetWorld, cross(cone.DirectionWorld, cone.AxisXWorld)) / cone.MinorToMajor,
                  dot(offsetWorld, cone.DirectionWorld));
}
float ComputeConeMaximumComponent(float3 value)
{
    return max(abs(value.x), max(abs(value.y), abs(value.z)));
}
bool ClipConeLowerBound(float coefficient, float value, inout float inoutNear, inout float inoutFar)
{
    if (coefficient > 0)
        inoutNear = max(inoutNear, value / coefficient);
    else if (coefficient < 0)
        inoutFar = min(inoutFar, value / coefficient);
    else if (value > 0)
        return false;
    return inoutNear <= inoutFar;
}
bool TryComputeConeBarycentric(float3 first, float3 second, float3 third, float3 point, out float3 outBarycentric)
{
    outBarycentric = float3(0);
    float3 normal = ComputeStableCross(second - first, third - first);
    float denominator = dot(normal, normal);
    if (denominator <= 0 || IsFiniteBits(denominator) == false)
        return false;
    float secondWeight = dot(ComputeStableCross(point - first, third - first), normal) / denominator;
    float thirdWeight = dot(ComputeStableCross(second - first, point - first), normal) / denominator;
    if (IsFiniteBits(secondWeight) == false || IsFiniteBits(thirdWeight) == false || secondWeight < 0 ||
        thirdWeight < 0 || secondWeight + thirdWeight > 1)
        return false;
    outBarycentric = float3(1 - secondWeight - thirdWeight, secondWeight, thirdWeight);
    return true;
}
void AddConeCandidate(float distance, float3 barycentric, inout ConeTriangleIntersection inoutIntersection)
{
    if (inoutIntersection.IsHit == false || distance < inoutIntersection.NearWorld)
    {
        inoutIntersection.NearWorld = distance;
        inoutIntersection.NearBarycentric = barycentric;
    }
    if (inoutIntersection.IsHit == false || distance > inoutIntersection.FarWorld)
    {
        inoutIntersection.FarWorld = distance;
        inoutIntersection.FarBarycentric = barycentric;
    }
    inoutIntersection.IsHit = true;
}
void AddConeEdgeParameter(float parameter,
                          float3 first,
                          float3 delta,
                          float3 firstBarycentric,
                          float3 deltaBarycentric,
                          inout ConeTriangleIntersection inoutIntersection)
{
    AddConeCandidate(first.z + parameter * delta.z, firstBarycentric + deltaBarycentric * parameter, inoutIntersection);
}
bool IntersectConeEdge(EllipticalCone cone,
                       float3 first,
                       float3 second,
                       float3 firstBarycentric,
                       float3 secondBarycentric,
                       inout ConeTriangleIntersection inoutIntersection)
{
    float3 delta = second - first;
    float3 deltaBarycentric = secondBarycentric - firstBarycentric;
    float nearParameter = 0;
    float farParameter = 1;
    if (ClipConeLowerBound(delta.z, cone.NearWorld - first.z, nearParameter, farParameter) == false ||
        ClipConeLowerBound(-delta.z, first.z - cone.FarWorld, nearParameter, farParameter) == false)
        return true;
    float radius = cone.RadiusWorld + cone.Slope * first.z;
    float radiusDelta = cone.Slope * delta.z;
    float a = dot(delta.xy, delta.xy) - radiusDelta * radiusDelta;
    float b = dot(first.xy, delta.xy) - radius * radiusDelta;
    float c = dot(first.xy, first.xy) - radius * radius;
    if (IsFiniteBits(a) == false || IsFiniteBits(b) == false || IsFiniteBits(c) == false)
        return false;
    float nearValue = (a * nearParameter + 2 * b) * nearParameter + c;
    float farValue = (a * farParameter + 2 * b) * farParameter + c;
    if (nearValue <= 0)
        AddConeEdgeParameter(nearParameter, first, delta, firstBarycentric, deltaBarycentric, inoutIntersection);
    if (farValue <= 0)
        AddConeEdgeParameter(farParameter, first, delta, firstBarycentric, deltaBarycentric, inoutIntersection);
    if (a == 0)
    {
        if (b != 0)
        {
            float root = -c / (2 * b);
            if (root >= nearParameter && root <= farParameter)
                AddConeEdgeParameter(root, first, delta, firstBarycentric, deltaBarycentric, inoutIntersection);
        }
        return true;
    }
    float discriminant = ComputeDifferenceOfProducts(b, b, a, c);
    if (IsFiniteBits(discriminant) == false)
        return false;
    if (discriminant < 0)
        return true;
    float rootDiscriminant = sqrt(discriminant);
    float q = -b - (b < 0 ? -rootDiscriminant : rootDiscriminant);
    float firstRoot = q / a;
    float secondRoot = q == 0 ? firstRoot : c / q;
    if (firstRoot >= nearParameter && firstRoot <= farParameter)
        AddConeEdgeParameter(firstRoot, first, delta, firstBarycentric, deltaBarycentric, inoutIntersection);
    if (secondRoot >= nearParameter && secondRoot <= farParameter)
        AddConeEdgeParameter(secondRoot, first, delta, firstBarycentric, deltaBarycentric, inoutIntersection);
    return true;
}

void AddConePlaneCandidate(float distance,
                           float3 normal,
                           float planeDistance,
                           float3 first,
                           float3 second,
                           float3 third,
                           inout ConeTriangleIntersection inoutIntersection)
{
    float transverseSquared = dot(normal.xy, normal.xy);
    float3 point = float3(0, 0, distance);
    if (transverseSquared > 0)
        point.xy = normal.xy * ((planeDistance - normal.z * distance) / transverseSquared);
    float3 barycentric;
    if (TryComputeConeBarycentric(first, second, third, point, barycentric) == true)
        AddConeCandidate(distance, barycentric, inoutIntersection);
}
/// Invalid input preserves inoutIntersection. A valid miss clears it, matching the CPU API.
bool TryIntersectEllipticalConeTriangle(EllipticalCone cone,
                                        ConeTriangle triangle,
                                        inout ConeTriangleIntersection inoutIntersection)
{
    if (IsValidEllipticalCone(cone) == false || IsFiniteBits(triangle.FirstWorld) == false ||
        IsFiniteBits(triangle.SecondWorld) == false || IsFiniteBits(triangle.ThirdWorld) == false)
        return false;
    float3 first = TransformToEllipticalCone(cone, triangle.FirstWorld);
    float3 second = TransformToEllipticalCone(cone, triangle.SecondWorld);
    float3 third = TransformToEllipticalCone(cone, triangle.ThirdWorld);
    if (IsFiniteBits(first) == false || IsFiniteBits(second) == false || IsFiniteBits(third) == false)
        return false;
    float scale = max(1.0,
                      max(cone.RadiusWorld + cone.FarWorld * cone.Slope,
                          max(ComputeConeMaximumComponent(first),
                              max(ComputeConeMaximumComponent(second), ComputeConeMaximumComponent(third)))));
    first /= scale;
    second /= scale;
    third /= scale;
    EllipticalCone normalized = cone;
    normalized.RadiusWorld /= scale;
    normalized.NearWorld /= scale;
    normalized.FarWorld /= scale;
    ConeTriangleIntersection intersection = (ConeTriangleIntersection)0;
    float3 normal = ComputeStableCross(second - first, third - first);
    if (dot(normal, normal) > 0)
    {
        float planeDistance = dot(normal, first);
        float transverse = length(normal.xy);
        if (IsFiniteBits(transverse * cone.Slope) == false)
            return false;
        float nearDistance = normalized.NearWorld;
        float farDistance = normalized.FarWorld;
        if (ClipConeLowerBound(normal.z + transverse * cone.Slope,
                               planeDistance - transverse * normalized.RadiusWorld,
                               nearDistance,
                               farDistance) == true &&
            ClipConeLowerBound(-normal.z + transverse * cone.Slope,
                               -planeDistance - transverse * normalized.RadiusWorld,
                               nearDistance,
                               farDistance) == true)
        {
            AddConePlaneCandidate(nearDistance, normal, planeDistance, first, second, third, intersection);
            AddConePlaneCandidate(farDistance, normal, planeDistance, first, second, third, intersection);
            if (IntersectConeEdge(normalized, first, second, float3(1, 0, 0), float3(0, 1, 0), intersection) == false ||
                IntersectConeEdge(normalized, second, third, float3(0, 1, 0), float3(0, 0, 1), intersection) == false ||
                IntersectConeEdge(normalized, third, first, float3(0, 0, 1), float3(1, 0, 0), intersection) == false)
                return false;
        }
    }
    intersection.NearWorld =
        intersection.IsHit == true
            ? clamp(intersection.NearWorld * scale - CONE_AXIAL_ROUNDOFF_SCALE * scale, cone.NearWorld, cone.FarWorld)
            : 0;
    intersection.FarWorld = intersection.IsHit == true
                                ? clamp(intersection.FarWorld * scale + CONE_AXIAL_ROUNDOFF_SCALE * scale,
                                        intersection.NearWorld,
                                        cone.FarWorld)
                                : 0;
    inoutIntersection = intersection;
    return true;
}
/// Eight supporting side planes conservatively prune world-space AABBs.
bool TryTestEllipticalConeBounds(EllipticalCone cone,
                                 float3 minimumWorld,
                                 float3 maximumWorld,
                                 out bool outMayIntersect)
{
    outMayIntersect = false;
    if (IsValidEllipticalCone(cone) == false || IsFiniteBits(minimumWorld) == false ||
        IsFiniteBits(maximumWorld) == false)
        return false;
    bool canIntersect = all(minimumWorld <= maximumWorld) == true;
    float3 centre = minimumWorld * 0.5 + maximumWorld * 0.5 - cone.OriginWorld;
    float3 halfExtent = maximumWorld * 0.5 - minimumWorld * 0.5;
    float3 axisYWorld = cross(cone.DirectionWorld, cone.AxisXWorld) / cone.MinorToMajor;
    float3 direction = cone.DirectionWorld;
    float centreDistance = dot(centre, direction);
    float halfDistance = dot(halfExtent, abs(direction));
    if (centreDistance + halfDistance < cone.NearWorld || centreDistance - halfDistance > cone.FarWorld)
        canIntersect = false;
    for (uint side = 0; canIntersect == true && side < CONE_SIDE_COUNT; ++side)
    {
        float x = side < CONE_SIDE_COUNT / 2 ? (side % 2 == 0 ? 1.0 : -1.0)
                                             : (side % 2 == 0 ? CONE_DIAGONAL_COMPONENT : -CONE_DIAGONAL_COMPONENT);
        float y = side < CONE_SIDE_COUNT / 2
                      ? 0
                      : (side < CONE_SIDE_COUNT - 2 ? CONE_DIAGONAL_COMPONENT : -CONE_DIAGONAL_COMPONENT);
        float3 sideNormal =
            side < 2 ? cone.AxisXWorld * x - direction * cone.Slope
                     : (side < CONE_SIDE_COUNT / 2 ? axisYWorld * (side % 2 == 0 ? 1.0 : -1.0) - direction * cone.Slope
                                                   : cone.AxisXWorld * x + axisYWorld * y - direction * cone.Slope);
        if (dot(centre, sideNormal) - dot(halfExtent, abs(sideNormal)) > cone.RadiusWorld)
            canIntersect = false;
    }
    outMayIntersect = canIntersect;
    return true;
}
