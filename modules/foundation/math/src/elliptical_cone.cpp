#include <ludus/foundation/math/elliptical_cone.hpp>

#include <ludus/foundation/math/scalar.hpp>

#include <cmath>

// Thanks to Umut Emre, Aryan Kanak and Shlomi Steinberg, "High-Performance Elliptical Cone Tracing",
// Computer Graphics Forum 44(7), e70230, 2025, Section 3, Eqs. (2), (4)-(6),
// https://doi.org/10.1111/cgf.70230 . Original implementation from the geometric equations:
// stretch the minor axis to a circular frustum, solve its plane range as two linear inequalities,
// and combine plane-section, clip, edge and vertex extrema. BVH bounds use conservative support planes.
// No code from the authors' noncommercial implementation is incorporated.
namespace ludus::foundation::math
{
namespace
{
constexpr float64 FRAME_SQUARED_LENGTH_TOLERANCE = 1e-6;
constexpr float64 FRAME_ORTHOGONALITY_TOLERANCE = 1e-6;
constexpr float64 AXIAL_ROUNDOFF_SCALE =
    1.7763568394002505e-15; // Eight binary64 epsilons per normalized working length.
constexpr float64 DIAGONAL_COMPONENT = 0.7071067811865475244;

bool IsValidCone(const EllipticalCone& cone) noexcept
{
    return IsFinite(cone.OriginWorld) == true && IsFinite(cone.DirectionWorld) == true &&
           IsFinite(cone.AxisXWorld) == true && IsFinite(cone.RadiusWorld) == true && cone.RadiusWorld >= 0 &&
           IsFinite(cone.Slope) == true && cone.Slope >= 0 && IsFinite(cone.MinorToMajor) == true &&
           cone.MinorToMajor > 0 && cone.MinorToMajor <= 1 && IsFinite(cone.NearWorld) == true && cone.NearWorld >= 0 &&
           IsFinite(cone.FarWorld) == true && cone.FarWorld >= cone.NearWorld &&
           IsFinite(cone.RadiusWorld + cone.Slope * cone.FarWorld) == true &&
           Abs(Dot(cone.DirectionWorld, cone.DirectionWorld) - 1) <= FRAME_SQUARED_LENGTH_TOLERANCE &&
           Abs(Dot(cone.AxisXWorld, cone.AxisXWorld) - 1) <= FRAME_SQUARED_LENGTH_TOLERANCE &&
           Abs(Dot(cone.AxisXWorld, cone.DirectionWorld)) <= FRAME_ORTHOGONALITY_TOLERANCE;
}

Vector3d TransformToCone(const EllipticalCone& cone, Vector3d pointWorld) noexcept
{
    const Vector3d offsetWorld = pointWorld - cone.OriginWorld;
    return {Dot(offsetWorld, cone.AxisXWorld),
            Dot(offsetWorld, Cross(cone.DirectionWorld, cone.AxisXWorld)) / cone.MinorToMajor,
            Dot(offsetWorld, cone.DirectionWorld)};
}

float64 ComputeMaximumComponent(Vector3d value) noexcept
{
    return Max(Abs(value.X), Max(Abs(value.Y), Abs(value.Z)));
}

/// Clip a scalar interval to coefficient * t >= value. Rejection never divides by zero.
bool ClipLowerBound(float64 coefficient, float64 value, float64& inoutNear, float64& inoutFar) noexcept
{
    if (coefficient > 0)
    {
        inoutNear = Max(inoutNear, value / coefficient);
    }
    else if (coefficient < 0)
    {
        inoutFar = Min(inoutFar, value / coefficient);
    }
    else if (value > 0)
    {
        return false;
    }
    return inoutNear <= inoutFar;
}

/// Barycentrics in the projected triangle plane; the point is already known to lie on that plane.
bool TryComputeBarycentric(Vector3d first,
                           Vector3d second,
                           Vector3d third,
                           Vector3d point,
                           Vector3d& outBarycentric) noexcept
{
    const Vector3d normal = Cross(second - first, third - first);
    const float64 denominator = Dot(normal, normal);
    if (denominator <= 0 || IsFinite(denominator) == false)
    {
        return false;
    }
    const float64 secondWeight = Dot(Cross(point - first, third - first), normal) / denominator;
    const float64 thirdWeight = Dot(Cross(second - first, point - first), normal) / denominator;
    if (IsFinite(secondWeight) == false || IsFinite(thirdWeight) == false || secondWeight < 0 || thirdWeight < 0 ||
        secondWeight + thirdWeight > 1)
    {
        return false;
    }
    outBarycentric = {1 - secondWeight - thirdWeight, secondWeight, thirdWeight};
    return true;
}

void AddCandidate(float64 distance, Vector3d barycentric, ConeTriangleIntersection& inoutIntersection) noexcept
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

void AddEdgeParameter(float64 parameter,
                      Vector3d first,
                      Vector3d delta,
                      Vector3d firstBarycentric,
                      Vector3d deltaBarycentric,
                      ConeTriangleIntersection& inoutIntersection) noexcept
{
    AddCandidate(first.Z + parameter * delta.Z, firstBarycentric + deltaBarycentric * parameter, inoutIntersection);
}

bool IntersectEdge(const EllipticalCone& cone,
                   Vector3d first,
                   Vector3d second,
                   Vector3d firstBarycentric,
                   Vector3d secondBarycentric,
                   ConeTriangleIntersection& inoutIntersection) noexcept
{
    const Vector3d delta = second - first;
    const Vector3d deltaBarycentric = secondBarycentric - firstBarycentric;
    float64 nearParameter = 0;
    float64 farParameter = 1;
    if (ClipLowerBound(delta.Z, cone.NearWorld - first.Z, nearParameter, farParameter) == false ||
        ClipLowerBound(-delta.Z, first.Z - cone.FarWorld, nearParameter, farParameter) == false)
    {
        return true;
    }
    const float64 radius = cone.RadiusWorld + cone.Slope * first.Z;
    const float64 radiusDelta = cone.Slope * delta.Z;
    const float64 a = delta.X * delta.X + delta.Y * delta.Y - radiusDelta * radiusDelta;
    const float64 b = first.X * delta.X + first.Y * delta.Y - radius * radiusDelta;
    const float64 c = first.X * first.X + first.Y * first.Y - radius * radius;
    if (IsFinite(a) == false || IsFinite(b) == false || IsFinite(c) == false)
    {
        return false;
    }
    const float64 nearValue = (a * nearParameter + 2 * b) * nearParameter + c;
    const float64 farValue = (a * farParameter + 2 * b) * farParameter + c;
    if (nearValue <= 0)
    {
        AddEdgeParameter(nearParameter, first, delta, firstBarycentric, deltaBarycentric, inoutIntersection);
    }
    if (farValue <= 0)
    {
        AddEdgeParameter(farParameter, first, delta, firstBarycentric, deltaBarycentric, inoutIntersection);
    }
    if (a == 0)
    {
        if (b != 0)
        {
            const float64 root = -c / (2 * b);
            if (root >= nearParameter && root <= farParameter)
            {
                AddEdgeParameter(root, first, delta, firstBarycentric, deltaBarycentric, inoutIntersection);
            }
        }
        return true;
    }
    // Compensated b*b-a*c, followed by the cancellation-avoiding q formulation of the quadratic roots.
    const float64 product = a * c;
    const float64 discriminant = std::fma(b, b, -product) - std::fma(a, c, -product);
    if (IsFinite(discriminant) == false)
    {
        return false;
    }
    if (discriminant < 0)
    {
        return true;
    }
    const float64 rootDiscriminant = Sqrt(discriminant);
    const float64 q = -b - (b < 0 ? -rootDiscriminant : rootDiscriminant);
    const float64 firstRoot = q / a;
    const float64 secondRoot = q == 0 ? firstRoot : c / q;
    if (firstRoot >= nearParameter && firstRoot <= farParameter)
    {
        AddEdgeParameter(firstRoot, first, delta, firstBarycentric, deltaBarycentric, inoutIntersection);
    }
    if (secondRoot >= nearParameter && secondRoot <= farParameter)
    {
        AddEdgeParameter(secondRoot, first, delta, firstBarycentric, deltaBarycentric, inoutIntersection);
    }
    return true;
}

void AddPlaneCandidate(float64 distance,
                       Vector3d normal,
                       float64 planeDistance,
                       Vector3d first,
                       Vector3d second,
                       Vector3d third,
                       ConeTriangleIntersection& inoutIntersection) noexcept
{
    const float64 transverseSquared = normal.X * normal.X + normal.Y * normal.Y;
    Vector3d point{0, 0, distance};
    if (transverseSquared > 0)
    {
        const float64 offset = (planeDistance - normal.Z * distance) / transverseSquared;
        point.X = normal.X * offset;
        point.Y = normal.Y * offset;
    }
    Vector3d barycentric;
    if (TryComputeBarycentric(first, second, third, point, barycentric) == true)
    {
        AddCandidate(distance, barycentric, inoutIntersection);
    }
}
} // namespace

bool TryIntersectEllipticalConeTriangle(const EllipticalCone& cone,
                                        const ConeTriangle& triangle,
                                        ConeTriangleIntersection& outIntersection) noexcept
{
    if (IsValidCone(cone) == false || IsFinite(triangle.FirstWorld) == false ||
        IsFinite(triangle.SecondWorld) == false || IsFinite(triangle.ThirdWorld) == false)
    {
        return false;
    }
    Vector3d first = TransformToCone(cone, triangle.FirstWorld);
    Vector3d second = TransformToCone(cone, triangle.SecondWorld);
    Vector3d third = TransformToCone(cone, triangle.ThirdWorld);
    if (IsFinite(first) == false || IsFinite(second) == false || IsFinite(third) == false)
    {
        return false;
    }
    // Normalize lengths before products; the cone slope and ellipse aspect are dimensionless.
    const float64 scale = Max(
        1.0,
        Max(cone.RadiusWorld + cone.FarWorld * cone.Slope,
            Max(ComputeMaximumComponent(first), Max(ComputeMaximumComponent(second), ComputeMaximumComponent(third)))));
    first = first * (1 / scale);
    second = second * (1 / scale);
    third = third * (1 / scale);
    EllipticalCone normalized = cone;
    normalized.RadiusWorld /= scale;
    normalized.NearWorld /= scale;
    normalized.FarWorld /= scale;
    ConeTriangleIntersection intersection;
    const Vector3d normal = Cross(second - first, third - first);
    if (Dot(normal, normal) > 0)
    {
        const float64 planeDistance = Dot(normal, first);
        const float64 transverse = Sqrt(normal.X * normal.X + normal.Y * normal.Y);
        if (IsFinite(transverse * cone.Slope) == false)
        {
            return false;
        }
        float64 nearDistance = normalized.NearWorld;
        float64 farDistance = normalized.FarWorld;
        if (ClipLowerBound(normal.Z + transverse * cone.Slope,
                           planeDistance - transverse * normalized.RadiusWorld,
                           nearDistance,
                           farDistance) == true &&
            ClipLowerBound(-normal.Z + transverse * cone.Slope,
                           -planeDistance - transverse * normalized.RadiusWorld,
                           nearDistance,
                           farDistance) == true)
        {
            AddPlaneCandidate(nearDistance, normal, planeDistance, first, second, third, intersection);
            AddPlaneCandidate(farDistance, normal, planeDistance, first, second, third, intersection);
            if (IntersectEdge(normalized, first, second, {1, 0, 0}, {0, 1, 0}, intersection) == false ||
                IntersectEdge(normalized, second, third, {0, 1, 0}, {0, 0, 1}, intersection) == false ||
                IntersectEdge(normalized, third, first, {0, 0, 1}, {1, 0, 0}, intersection) == false)
            {
                return false;
            }
        }
    }
    intersection.NearWorld =
        intersection.IsHit == true
            ? Max(cone.NearWorld, Min(cone.FarWorld, intersection.NearWorld * scale - AXIAL_ROUNDOFF_SCALE * scale))
            : 0;
    intersection.FarWorld = intersection.IsHit == true
                                ? Max(intersection.NearWorld,
                                      Min(cone.FarWorld, intersection.FarWorld * scale + AXIAL_ROUNDOFF_SCALE * scale))
                                : 0;
    outIntersection = intersection;
    return true;
}

bool TryTestEllipticalConeBounds(const EllipticalCone& cone,
                                 Vector3d minimumWorld,
                                 Vector3d maximumWorld,
                                 bool& outMayIntersect) noexcept
{
    if (IsValidCone(cone) == false || IsFinite(minimumWorld) == false || IsFinite(maximumWorld) == false)
    {
        return false;
    }
    bool canIntersect =
        minimumWorld.X <= maximumWorld.X && minimumWorld.Y <= maximumWorld.Y && minimumWorld.Z <= maximumWorld.Z;
    const Vector3d centre = minimumWorld * 0.5 + maximumWorld * 0.5 - cone.OriginWorld;
    const Vector3d halfExtent = maximumWorld * 0.5 - minimumWorld * 0.5;
    const Vector3d axisYWorld = Cross(cone.DirectionWorld, cone.AxisXWorld) * (1 / cone.MinorToMajor);
    const Vector3d direction = cone.DirectionWorld;
    const Vector3d absoluteDirection{Abs(direction.X), Abs(direction.Y), Abs(direction.Z)};
    const float64 centreDistance = Dot(centre, direction);
    const float64 halfDistance = Dot(halfExtent, absoluteDirection);
    if (centreDistance + halfDistance < cone.NearWorld || centreDistance - halfDistance > cone.FarWorld)
    {
        canIntersect = false;
    }
    constexpr foundation::uint32 SIDE_COUNT = 8;
    for (foundation::uint32 side = 0; canIntersect == true && side < SIDE_COUNT; ++side)
    {
        const float64 x = side < SIDE_COUNT / 2 ? (side % 2 == 0 ? 1.0 : -1.0)
                                                : (side % 2 == 0 ? DIAGONAL_COMPONENT : -DIAGONAL_COMPONENT);
        const float64 y =
            side < SIDE_COUNT / 2 ? 0 : (side < SIDE_COUNT - 2 ? DIAGONAL_COMPONENT : -DIAGONAL_COMPONENT);
        const Vector3d sideNormal =
            side < 2 ? cone.AxisXWorld * x - direction * cone.Slope
                     : (side < SIDE_COUNT / 2 ? axisYWorld * (side % 2 == 0 ? 1.0 : -1.0) - direction * cone.Slope
                                              : cone.AxisXWorld * x + axisYWorld * y - direction * cone.Slope);
        const Vector3d absoluteNormal{Abs(sideNormal.X), Abs(sideNormal.Y), Abs(sideNormal.Z)};
        if (Dot(centre, sideNormal) - Dot(halfExtent, absoluteNormal) > cone.RadiusWorld)
        {
            canIntersect = false;
        }
    }
    outMayIntersect = canIntersect;
    return true;
}
} // namespace ludus::foundation::math
