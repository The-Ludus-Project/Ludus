#include "edge_diffraction_oracle.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/content/scene_source/scene_source.h>

namespace ludus::graphics::renderer::testing
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
/// Below this |u| the Fresnel tail comes from the power series; above it from the continued fraction. At 2.5 the
/// series' largest term is about 85, so it keeps about 14 digits, and the continued fraction converges within its
/// depth.
constexpr float64 FRESNEL_SERIES_LIMIT = 2.5;
/// Terms of the Fresnel power series; it stops earlier once a term no longer changes the sum.
constexpr uint32 FRESNEL_SERIES_TERMS = 200;
/// Depth of the backward-evaluated continued fraction for erfc.
constexpr uint32 CONTINUED_FRACTION_DEPTH = 400;
/// Within this distance (radians) of a shadow or reflection boundary the UTD coefficient uses its limit there, where
/// the cotangent and the transition function's zero would otherwise cancel in rounding.
constexpr float64 BOUNDARY_LIMIT_ANGLE = 1e-5;
/// Simpson panels of the Fresnel-Kirchhoff integral across the slit. Its phase changes by at most k (|sin| + |sin|) per
/// metre, about 63 radians across the design slit, so 800 panels sample it 13 times per radian.
constexpr uint32 KIRCHHOFF_PANELS = 800;

[[nodiscard]] DiffractionComplex Add(DiffractionComplex first, DiffractionComplex second) noexcept
{
    return {first.Real + second.Real, first.Imaginary + second.Imaginary};
}

[[nodiscard]] DiffractionComplex Subtract(DiffractionComplex first, DiffractionComplex second) noexcept
{
    return {first.Real - second.Real, first.Imaginary - second.Imaginary};
}

[[nodiscard]] DiffractionComplex Multiply(DiffractionComplex first, DiffractionComplex second) noexcept
{
    return {first.Real * second.Real - first.Imaginary * second.Imaginary,
            first.Real * second.Imaginary + first.Imaginary * second.Real};
}

[[nodiscard]] DiffractionComplex Scale(DiffractionComplex value, float64 factor) noexcept
{
    return {value.Real * factor, value.Imaginary * factor};
}

[[nodiscard]] DiffractionComplex Divide(DiffractionComplex numerator, DiffractionComplex denominator) noexcept
{
    const float64 squared = ComputeSquaredMagnitude(denominator);
    return {(numerator.Real * denominator.Real + numerator.Imaginary * denominator.Imaginary) / squared,
            (numerator.Imaginary * denominator.Real - numerator.Real * denominator.Imaginary) / squared};
}

/// exp(j angle).
[[nodiscard]] DiffractionComplex MakePhase(float64 angle) noexcept
{
    return {math::Cos(angle), math::Sin(angle)};
}

/// integral_0^infinity exp(-j t^2) dt = (sqrt(pi) / 2) exp(-j pi / 4).
[[nodiscard]] DiffractionComplex GetFresnelTailAtZero() noexcept
{
    const float64 half = math::Sqrt(math::kPi / 2) / 2;
    return {half, -half};
}

/// C(x) and S(x) by their power series (Abramowitz and Stegun, Section 7.3), accurate for |x| up to about 2.
[[nodiscard]] FresnelIntegrals ComputeFresnelSeries(float64 x) noexcept
{
    // term_m = (pi / 2)^m x^(2m + 1) / m!; C takes the even m with sign (-1)^(m / 2), S the odd m with sign
    // (-1)^((m - 1) / 2), each divided by 2m + 1.
    const float64 step = math::kPi / 2 * x * x;
    float64 term = x;
    float64 cosineIntegral = 0;
    float64 sineIntegral = 0;
    for (uint32 m = 0; m < FRESNEL_SERIES_TERMS; ++m)
    {
        if (m > 0)
        {
            term *= step / static_cast<float64>(m);
        }
        const float64 contribution = term / static_cast<float64>(2 * m + 1);
        const bool isNegative = (m / 2) % 2 == 1;
        const float64 signedContribution = isNegative == true ? -contribution : contribution;
        if (m % 2 == 0)
        {
            cosineIntegral += signedContribution;
        }
        else
        {
            sineIntegral += signedContribution;
        }
        if (m > 2 && math::Abs(contribution) < 1e-18 * (math::Abs(cosineIntegral) + math::Abs(sineIntegral)))
        {
            break;
        }
    }
    return {cosineIntegral, sineIntegral};
}

/// H(u) = exp(j u^2) G(u) for u >= FRESNEL_SERIES_LIMIT. With z = exp(j pi / 4) u, G(u) = (sqrt(pi) / 2) exp(-j pi / 4)
/// erfc(z), and erfc(z) = exp(-z^2) / sqrt(pi) / (z + (1/2) / (z + (2/2) / (z + (3/2) / ...))) for Re z > 0
/// (Abramowitz and Stegun, Section 7.1). exp(-z^2) = exp(-j u^2) cancels in H, so H keeps full precision for large u.
[[nodiscard]] DiffractionComplex ComputeScaledFresnelTail(float64 u) noexcept
{
    const DiffractionComplex z = Scale(MakePhase(math::kPi / 4), u);
    DiffractionComplex fraction = z;
    for (uint32 m = CONTINUED_FRACTION_DEPTH; m > 0; --m)
    {
        fraction = Add(z, Divide({static_cast<float64>(m) / 2, 0}, fraction));
    }
    return Divide(Scale(MakePhase(-math::kPi / 4), 0.5), fraction);
}

/// cot(x) for x away from multiples of pi.
[[nodiscard]] float64 ComputeCotangent(float64 x) noexcept
{
    return math::Cos(x) / math::Sin(x);
}

/// The integer N that most nearly satisfies 2 pi n N - beta = side * pi.
[[nodiscard]] float64 FindNearestBoundaryIndex(float64 beta, float64 wedgeN, float64 side) noexcept
{
    return math::Floor((beta + side * math::kPi) / (2 * math::kPi * wedgeN) + 0.5);
}

/// cot((pi + side beta) / (2n)) F(k L a^side(beta)), one of the coefficient's four terms. Near the boundary where the
/// cotangent diverges, the product's limit n [sqrt(2 pi k L) sgn(e) - 2 k L e exp(j pi / 4)] exp(j pi / 4) is used,
/// with e = pi + side beta - 2 pi n N the angular distance from that boundary.
[[nodiscard]] DiffractionComplex
ComputeCoefficientTerm(float64 beta, float64 side, const WedgeDiffraction& diffraction) noexcept
{
    const float64 n = diffraction.WedgeN;
    const float64 kl = diffraction.Wavenumber * diffraction.DistanceParameterWorld;
    const float64 index = FindNearestBoundaryIndex(beta, n, side);
    // a^side(beta) = 2 cos^2((2 pi n N - beta) / 2); 2 pi n N - beta = side pi - side e.
    const float64 boundaryDistance = side * (side * math::kPi - (2 * math::kPi * n * index - beta));
    if (math::Abs(boundaryDistance) < BOUNDARY_LIMIT_ANGLE)
    {
        const float64 sign = boundaryDistance < 0 ? -1.0 : 1.0;
        const DiffractionComplex inner = Subtract({sign * math::Sqrt(2 * math::kPi * kl), 0},
                                                  Scale(MakePhase(math::kPi / 4), 2 * kl * boundaryDistance));
        return Scale(Multiply(inner, MakePhase(math::kPi / 4)), n);
    }
    const float64 halfAngle = (2 * math::kPi * n * index - beta) / 2;
    const float64 a = 2 * math::Cos(halfAngle) * math::Cos(halfAngle);
    const float64 cotangent = ComputeCotangent((math::kPi + side * beta) / (2 * n));
    return Scale(ComputeUtdTransition(kl * a), cotangent);
}

/// The sum of the two terms of one argument beta = phi -+ phi'.
[[nodiscard]] DiffractionComplex ComputeCoefficientPair(float64 beta, const WedgeDiffraction& diffraction) noexcept
{
    return Add(ComputeCoefficientTerm(beta, 1, diffraction), ComputeCoefficientTerm(beta, -1, diffraction));
}

/// The sign of the reflected terms: -1 for a soft edge, +1 for a hard one and 0 for an absorbing one.
[[nodiscard]] float64 GetReflectionSign(EdgeCondition condition) noexcept
{
    switch (condition)
    {
        case EdgeCondition::Soft:
            return -1.0;
        case EdgeCondition::Hard:
            return 1.0;
        case EdgeCondition::Absorbing:
            return 0.0;
    }
    return 0.0;
}

/// An angle in [0, 2 pi).
[[nodiscard]] float64 WrapAngle(float64 angle) noexcept
{
    const float64 wrapped = angle - math::kTau * math::Floor(angle / math::kTau);
    return wrapped >= math::kTau ? 0.0 : wrapped;
}

/// exp(-j k r) / r.
[[nodiscard]] DiffractionComplex MakeSphericalWave(float64 wavenumber, float64 distance) noexcept
{
    return Scale(MakePhase(-wavenumber * distance), 1 / distance);
}

/// Distances below this (metres) are equal: corners closer than it are one corner, and a segment that only touches an
/// obstacle's boundary within it is not blocked.
constexpr float64 GEOMETRY_TOLERANCE_WORLD = 1e-9;
/// Corners of the light and the wall rectangles.
constexpr usize RECTANGLE_CORNERS = 4;

/// The cross product of (b - a) and (c - a) in the x-z plane: positive when c lies left of a -> b (x right, z up).
[[nodiscard]] float64 ComputePlanarCross(math::Vector3d a, math::Vector3d b, math::Vector3d c) noexcept
{
    return (b.X - a.X) * (c.Z - a.Z) - (b.Z - a.Z) * (c.X - a.X);
}

/// The distance between two points' projections on the x-z plane.
[[nodiscard]] float64 ComputePlanarDistance(math::Vector3d a, math::Vector3d b) noexcept
{
    return math::Sqrt((b.X - a.X) * (b.X - a.X) + (b.Z - a.Z) * (b.Z - a.Z));
}

/// The direction of b seen from a in the x-z plane, radians from +x towards +z.
[[nodiscard]] float64 ComputePlanarAngle(math::Vector3d a, math::Vector3d b) noexcept
{
    return math::Atan2(b.Z - a.Z, b.X - a.X);
}

/// The unit outward normal (Y = 0) of a counterclockwise cross-section's side from corner to next.
[[nodiscard]] math::Vector3d ComputeOutwardNormal(math::Vector3d corner, math::Vector3d next) noexcept
{
    const float64 length = ComputePlanarDistance(corner, next);
    return {(next.Z - corner.Z) / length, 0, -(next.X - corner.X) / length};
}

/// The signed distance of a point from a side's line in the x-z plane, positive outside.
[[nodiscard]] float64 ComputeSideDistance(math::Vector3d point, math::Vector3d corner, math::Vector3d normal) noexcept
{
    return (point.X - corner.X) * normal.X + (point.Z - corner.Z) * normal.Z;
}

/// Whether the segment between two points crosses an obstacle's interior. The prisms are infinitely long along y, so
/// only the segment's projection on the x-z plane matters. The segment is clipped against every side's half-plane.
// A segment's two ends.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] bool
IsSegmentBlocked(const EdgeDiffractionScene& scene, math::Vector3d fromWorld, math::Vector3d toWorld) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    for (usize o = 0; o < scene.ObstacleCount; ++o)
    {
        const DiffractionObstacle& obstacle = scene.Obstacles[o];
        float64 enter = 0;
        float64 leave = 1;
        bool isMissed = false;
        for (usize i = 0; i < obstacle.CornerCount && isMissed == false; ++i)
        {
            const math::Vector3d corner = obstacle.CornersWorld[i];
            const math::Vector3d normal =
                ComputeOutwardNormal(corner, obstacle.CornersWorld[(i + 1) % obstacle.CornerCount]);
            // Strictly inside means more than the tolerance inside every side.
            const float64 fromDistance = ComputeSideDistance(fromWorld, corner, normal) + GEOMETRY_TOLERANCE_WORLD;
            const float64 toDistance = ComputeSideDistance(toWorld, corner, normal) + GEOMETRY_TOLERANCE_WORLD;
            if (fromDistance >= 0 && toDistance >= 0)
            {
                isMissed = true;
            }
            else if (fromDistance >= 0 || toDistance >= 0)
            {
                const float64 crossing = fromDistance / (fromDistance - toDistance);
                if (fromDistance >= 0)
                {
                    enter = math::Max(enter, crossing);
                }
                else
                {
                    leave = math::Min(leave, crossing);
                }
            }
        }
        if (isMissed == false && enter < leave)
        {
            return true;
        }
    }
    return false;
}

/// One edge: the corner of a cross-section and the directions of its faces.
struct EdgeFrame final
{
    math::Vector3d CornerWorld{};
    float64 Face0Angle = 0; ///< Direction of face 0 (towards the previous corner), radians from +x towards +z.
    float64 WedgeN = 2;     ///< Exterior angle over pi.
};

[[nodiscard]] EdgeFrame MakeEdgeFrame(const DiffractionObstacle& obstacle, usize corner) noexcept
{
    const usize count = obstacle.CornerCount;
    const math::Vector3d at = obstacle.CornersWorld[corner];
    const math::Vector3d previous = obstacle.CornersWorld[(corner + count - 1) % count];
    const math::Vector3d next = obstacle.CornersWorld[(corner + 1) % count];
    EdgeFrame frame;
    frame.CornerWorld = at;
    // Counterclockwise, the interior lies between the direction to the next corner and, turning on, the direction to
    // the previous one; the exterior turns on from there back to the next corner.
    frame.Face0Angle = ComputePlanarAngle(at, previous);
    const float64 interior = WrapAngle(frame.Face0Angle - ComputePlanarAngle(at, next));
    frame.WedgeN = (math::kTau - interior) / math::kPi;
    return frame;
}

/// The field that one edge diffracts from a point source to a wall point, or zero when either path is hidden.
[[nodiscard]] DiffractionComplex ComputeEdgeField(const EdgeDiffractionScene& scene,
                                                  const EdgeFrame& frame,
                                                  float64 wavenumber,
                                                  EdgeCondition condition,
                                                  math::Vector3d sourceWorld,
                                                  math::Vector3d wallWorld) noexcept
{
    const math::Vector3d corner = frame.CornerWorld;
    const float64 incidentAngle = WrapAngle(ComputePlanarAngle(corner, sourceWorld) - frame.Face0Angle);
    const float64 observationAngle = WrapAngle(ComputePlanarAngle(corner, wallWorld) - frame.Face0Angle);
    const float64 exterior = frame.WedgeN * math::kPi;
    if (incidentAngle >= exterior || observationAngle >= exterior)
    {
        return {};
    }
    const math::Vector3d cornerAtSource{corner.X, sourceWorld.Y, corner.Z};
    const math::Vector3d cornerAtWall{corner.X, wallWorld.Y, corner.Z};
    if (IsSegmentBlocked(scene, sourceWorld, cornerAtSource) == true ||
        IsSegmentBlocked(scene, cornerAtWall, wallWorld) == true)
    {
        return {};
    }
    // Keller's law places the diffraction point where the incident and diffracted rays make equal angles with the
    // edge: unfolding both rays about the edge puts it at the distance-weighted point between their y.
    const float64 sourceDistance = ComputePlanarDistance(corner, sourceWorld);
    const float64 wallDistance = ComputePlanarDistance(corner, wallWorld);
    const float64 edgeY =
        sourceWorld.Y + (wallWorld.Y - sourceWorld.Y) * sourceDistance / (sourceDistance + wallDistance);
    const float64 incidentLength =
        math::Sqrt(sourceDistance * sourceDistance + (edgeY - sourceWorld.Y) * (edgeY - sourceWorld.Y));
    const float64 diffractedLength =
        math::Sqrt(wallDistance * wallDistance + (wallWorld.Y - edgeY) * (wallWorld.Y - edgeY));
    const float64 edgeSine = sourceDistance / incidentLength;
    WedgeDiffraction diffraction;
    diffraction.ObservationAngle = observationAngle;
    diffraction.IncidentAngle = incidentAngle;
    diffraction.DistanceParameterWorld =
        diffractedLength * incidentLength * edgeSine * edgeSine / (diffractedLength + incidentLength);
    diffraction.EdgeSine = edgeSine;
    diffraction.WedgeN = frame.WedgeN;
    diffraction.Wavenumber = wavenumber;
    diffraction.Condition = condition;
    const DiffractionComplex coefficient = ComputeUtdCoefficient(diffraction);
    const float64 spreading = math::Sqrt(incidentLength / (diffractedLength * (incidentLength + diffractedLength)));
    const DiffractionComplex incident = MakeSphericalWave(wavenumber, incidentLength);
    return Scale(Multiply(Multiply(incident, coefficient), MakePhase(-wavenumber * diffractedLength)), spreading);
}

/// The field that a perfectly conducting side reflects from a point source to a wall point, or zero.
[[nodiscard]] DiffractionComplex ComputeReflectedField(const EdgeDiffractionScene& scene,
                                                       math::Vector3d corner,
                                                       math::Vector3d next,
                                                       float64 wavenumber,
                                                       EdgeCondition condition,
                                                       math::Vector3d sourceWorld,
                                                       math::Vector3d wallWorld) noexcept
{
    const math::Vector3d normal = ComputeOutwardNormal(corner, next);
    const float64 sourceSide = ComputeSideDistance(sourceWorld, corner, normal);
    const float64 wallSide = ComputeSideDistance(wallWorld, corner, normal);
    if (sourceSide <= GEOMETRY_TOLERANCE_WORLD || wallSide <= GEOMETRY_TOLERANCE_WORLD)
    {
        return {};
    }
    // The image source mirrors the source across the side's plane, which contains y.
    const math::Vector3d imageWorld{sourceWorld.X - 2 * sourceSide * normal.X,
                                    sourceWorld.Y,
                                    sourceWorld.Z - 2 * sourceSide * normal.Z};
    const float64 crossing = sourceSide / (sourceSide + wallSide);
    const math::Vector3d reflectionWorld = imageWorld + (wallWorld - imageWorld) * crossing;
    const float64 sideLengthSquared =
        (next.X - corner.X) * (next.X - corner.X) + (next.Z - corner.Z) * (next.Z - corner.Z);
    const float64 along =
        ((reflectionWorld.X - corner.X) * (next.X - corner.X) + (reflectionWorld.Z - corner.Z) * (next.Z - corner.Z)) /
        sideLengthSquared;
    if (along <= 0 || along >= 1 || IsSegmentBlocked(scene, sourceWorld, reflectionWorld) == true ||
        IsSegmentBlocked(scene, reflectionWorld, wallWorld) == true)
    {
        return {};
    }
    return Scale(MakeSphericalWave(wavenumber, math::Length(wallWorld - imageWorld)), GetReflectionSign(condition));
}

/// First-order UTD: geometric optics (direct and, for conducting faces, reflected) plus every edge's diffraction.
[[nodiscard]] float64 ComputeUtdTransmission(const EdgeDiffractionScene& scene,
                                             float64 wavenumber,
                                             EdgeCondition condition,
                                             math::Vector3d sourceWorld,
                                             math::Vector3d wallWorld) noexcept
{
    const float64 freeDistance = math::Length(wallWorld - sourceWorld);
    DiffractionComplex total{};
    if (IsSegmentBlocked(scene, sourceWorld, wallWorld) == false)
    {
        total = MakeSphericalWave(wavenumber, freeDistance);
    }
    for (usize o = 0; o < scene.ObstacleCount; ++o)
    {
        const DiffractionObstacle& obstacle = scene.Obstacles[o];
        for (usize i = 0; i < obstacle.CornerCount; ++i)
        {
            total =
                Add(total,
                    ComputeEdgeField(scene, MakeEdgeFrame(obstacle, i), wavenumber, condition, sourceWorld, wallWorld));
            if (condition != EdgeCondition::Absorbing)
            {
                total = Add(total,
                            ComputeReflectedField(scene,
                                                  obstacle.CornersWorld[i],
                                                  obstacle.CornersWorld[(i + 1) % obstacle.CornerCount],
                                                  wavenumber,
                                                  condition,
                                                  sourceWorld,
                                                  wallWorld));
            }
        }
    }
    return ComputeSquaredMagnitude(total) * freeDistance * freeDistance;
}

/// An aperture of the Kirchhoff model: the open interval [Start, End] of x in the plane z = Height.
struct Aperture final
{
    float64 Start = 0;
    float64 End = 0;
    float64 Height = 0;
};

/// The single bounded gap that obstacles sharing one top plane leave open.
[[nodiscard]] bool TryFindAperture(const EdgeDiffractionScene& scene, Aperture& outAperture) noexcept
{
    if (scene.ObstacleCount < 2)
    {
        return false;
    }
    StaticArray<float64, MAX_DIFFRACTION_OBSTACLES> lefts{};
    StaticArray<float64, MAX_DIFFRACTION_OBSTACLES> rights{};
    float64 height = scene.Obstacles[0].CornersWorld[0].Z;
    for (usize o = 0; o < scene.ObstacleCount; ++o)
    {
        const DiffractionObstacle& obstacle = scene.Obstacles[o];
        for (usize i = 0; i < obstacle.CornerCount; ++i)
        {
            height = math::Max(height, obstacle.CornersWorld[i].Z);
        }
    }
    for (usize o = 0; o < scene.ObstacleCount; ++o)
    {
        const DiffractionObstacle& obstacle = scene.Obstacles[o];
        bool hasTop = false;
        for (usize i = 0; i < obstacle.CornerCount; ++i)
        {
            const math::Vector3d corner = obstacle.CornersWorld[i];
            if (math::Abs(corner.Z - height) > GEOMETRY_TOLERANCE_WORLD)
            {
                continue;
            }
            lefts[o] = hasTop == true ? math::Min(lefts[o], corner.X) : corner.X;
            rights[o] = hasTop == true ? math::Max(rights[o], corner.X) : corner.X;
            hasTop = true;
        }
        // Every obstacle must reach the plane with a flat top face.
        if (hasTop == false || rights[o] - lefts[o] <= GEOMETRY_TOLERANCE_WORLD)
        {
            return false;
        }
    }
    // A gap lies between an obstacle's right end and the nearest left end beyond it; exactly one may be bounded.
    usize gaps = 0;
    float64 start = 0;
    float64 end = 0;
    for (usize o = 0; o < scene.ObstacleCount; ++o)
    {
        bool hasNext = false;
        float64 nearest = 0;
        for (usize p = 0; p < scene.ObstacleCount; ++p)
        {
            if (lefts[p] >= rights[o] && (hasNext == false || lefts[p] < nearest))
            {
                nearest = lefts[p];
                hasNext = true;
            }
        }
        if (hasNext == true && nearest - rights[o] > GEOMETRY_TOLERANCE_WORLD)
        {
            ++gaps;
            start = rights[o];
            end = nearest;
        }
    }
    if (gaps != 1)
    {
        return false;
    }
    outAperture = { .Start = start, .End = end, .Height = height };
    return true;
}

/// Fresnel-Kirchhoff over an aperture [start, end] in the plane z = height, with U = (j / lambda) times the integral
/// of U_incident exp(-j k r) / r and the mean obliquity (cos + cos) / 2. Along the infinitely long aperture the
/// integral is evaluated by stationary phase, across it by Simpson's rule.
[[nodiscard]] float64 ComputeKirchhoffTransmission(float64 wavelengthWorld,
                                                   const Aperture& aperture,
                                                   math::Vector3d sourceWorld,
                                                   math::Vector3d wallWorld) noexcept
{
    const float64 start = aperture.Start;
    const float64 end = aperture.End;
    const float64 height = aperture.Height;
    const float64 wavenumber = 2 * math::kPi / wavelengthWorld;
    const float64 panelWidth = (end - start) / static_cast<float64>(KIRCHHOFF_PANELS);
    const float64 alongAperture = wallWorld.Y - sourceWorld.Y;
    const float64 sourceHeight = sourceWorld.Z - height;
    const float64 wallDepth = height - wallWorld.Z;
    DiffractionComplex sum{};
    for (uint32 i = 0; i <= KIRCHHOFF_PANELS; ++i)
    {
        const float64 x = start + panelWidth * static_cast<float64>(i);
        const float64 incidentPlanar =
            math::Sqrt((x - sourceWorld.X) * (x - sourceWorld.X) + sourceHeight * sourceHeight);
        const float64 diffractedPlanar = math::Sqrt((wallWorld.X - x) * (wallWorld.X - x) + wallDepth * wallDepth);
        // The stationary point along the aperture lies on the unfolded straight line; both distances grow by the same
        // factor there.
        const float64 slope = alongAperture / (incidentPlanar + diffractedPlanar);
        const float64 growth = math::Sqrt(1 + slope * slope);
        const float64 incidentLength = incidentPlanar * growth;
        const float64 diffractedLength = diffractedPlanar * growth;
        const float64 curvature = (1 / incidentPlanar + 1 / diffractedPlanar) / (growth * growth * growth);
        const float64 obliquity = (sourceHeight / incidentLength + wallDepth / diffractedLength) / 2;
        // integral exp(-j k f''(y) y^2 / 2) dy = sqrt(2 pi / (k f'')) exp(-j pi / 4).
        const float64 amplitude =
            obliquity / (incidentLength * diffractedLength) * math::Sqrt(2 * math::kPi / (wavenumber * curvature));
        const DiffractionComplex value =
            Scale(MakePhase(-wavenumber * (incidentLength + diffractedLength) - math::kPi / 4), amplitude);
        float64 weight = 2;
        if (i == 0 || i == KIRCHHOFF_PANELS)
        {
            weight = 1;
        }
        else if (i % 2 == 1)
        {
            weight = 4;
        }
        sum = Add(sum, Scale(value, weight));
    }
    const DiffractionComplex field = Multiply({0, 1 / wavelengthWorld}, Scale(sum, panelWidth / 3));
    const float64 freeDistance = math::Length(wallWorld - sourceWorld);
    return ComputeSquaredMagnitude(field) * freeDistance * freeDistance;
}

/// Light positions (x) where some path's visibility can change, at most this many per wall point.
constexpr usize MAX_LIGHT_BREAKS = 1024;
/// The light is integrated in at least this many pieces along x.
constexpr usize MINIMUM_LIGHT_PIECES = 2;

/// Gauss-Legendre nodes and weights on [-1, 1] (Abramowitz and Stegun, Section 25.4): the roots of P_n by Newton's
/// method from Chebyshev estimates, and w = 2 / ((1 - x^2) P_n'(x)^2).
/// A Gauss-Legendre rule's nodes and weights on [-1, 1].
struct GaussLegendreRule final
{
    StaticArray<float64, MAX_LIGHT_QUADRATURE_NODES> Nodes{};
    StaticArray<float64, MAX_LIGHT_QUADRATURE_NODES> Weights{};
};

[[nodiscard]] GaussLegendreRule ComputeGaussLegendre(usize count) noexcept
{
    constexpr uint32 NEWTON_STEPS = 100;
    GaussLegendreRule rule;
    const float64 n = static_cast<float64>(count);
    for (usize i = 0; i < (count + 1) / 2; ++i)
    {
        float64 x = math::Cos(math::kPi * (static_cast<float64>(i) + 0.75) / (n + 0.5));
        float64 derivative = 1;
        for (uint32 step = 0; step < NEWTON_STEPS; ++step)
        {
            float64 previous = 1;
            float64 current = x;
            for (usize j = 2; j <= count; ++j)
            {
                const float64 next =
                    ((2 * static_cast<float64>(j) - 1) * x * current - (static_cast<float64>(j) - 1) * previous) /
                    static_cast<float64>(j);
                previous = current;
                current = next;
            }
            derivative = n * (x * current - previous) / (x * x - 1);
            const float64 change = current / derivative;
            x -= change;
            if (math::Abs(change) < 1e-16)
            {
                break;
            }
        }
        const float64 weight = 2 / ((1 - x * x) * derivative * derivative);
        rule.Nodes[i] = -x;
        rule.Weights[i] = weight;
        rule.Nodes[count - 1 - i] = x;
        rule.Weights[count - 1 - i] = weight;
    }
    return rule;
}

/// Adds the light-plane x where the line from a viewpoint through a corner meets the light, when it lies on the light.
[[nodiscard]] bool TryAddLightBreak(const EdgeDiffractionScene& scene,
                                    math::Vector3d viewpoint,
                                    math::Vector3d corner,
                                    StaticArray<float64, MAX_LIGHT_BREAKS>& inoutBreaks,
                                    usize& inoutCount) noexcept
{
    if (corner.Z <= viewpoint.Z + GEOMETRY_TOLERANCE_WORLD)
    {
        return true;
    }
    const float64 scale = (scene.LightCenterWorld.Z - viewpoint.Z) / (corner.Z - viewpoint.Z);
    const float64 x = viewpoint.X + (corner.X - viewpoint.X) * scale;
    if (x <= scene.LightCenterWorld.X - scene.LightHalfWidthWorld ||
        x >= scene.LightCenterWorld.X + scene.LightHalfWidthWorld)
    {
        return true;
    }
    if (inoutCount == MAX_LIGHT_BREAKS)
    {
        return false;
    }
    inoutBreaks[inoutCount] = x;
    ++inoutCount;
    return true;
}

/// The sorted light-plane x of every visibility change for a wall point: where an obstacle corner starts to hide the
/// direct path, and where one starts to hide the path from the light to an edge. The obstacles are prisms along y, so
/// visibility depends on the light point's x alone; between breaks the transmission is smooth (the uniform
/// coefficient keeps the field continuous across an edge's own shadow and reflection boundaries).
[[nodiscard]] bool TryCollectLightBreaks(const EdgeDiffractionScene& scene,
                                         math::Vector3d wallWorld,
                                         StaticArray<float64, MAX_LIGHT_BREAKS>& outBreaks,
                                         usize& outCount) noexcept
{
    usize count = 0;
    outBreaks[count] = scene.LightCenterWorld.X - scene.LightHalfWidthWorld;
    ++count;
    outBreaks[count] = scene.LightCenterWorld.X + scene.LightHalfWidthWorld;
    ++count;
    for (usize o = 0; o < scene.ObstacleCount; ++o)
    {
        const DiffractionObstacle& obstacle = scene.Obstacles[o];
        for (usize i = 0; i < obstacle.CornerCount; ++i)
        {
            const math::Vector3d edge = obstacle.CornersWorld[i];
            if (TryAddLightBreak(scene, wallWorld, edge, outBreaks, count) == false)
            {
                return false;
            }
            // The edge's own faces, extended towards the light.
            for (const usize neighbour :
                 {(i + 1) % obstacle.CornerCount, (i + obstacle.CornerCount - 1) % obstacle.CornerCount})
            {
                if (TryAddLightBreak(scene, obstacle.CornersWorld[neighbour], edge, outBreaks, count) == false)
                {
                    return false;
                }
            }
            for (usize p = 0; p < scene.ObstacleCount; ++p)
            {
                const DiffractionObstacle& other = scene.Obstacles[p];
                for (usize j = 0; j < other.CornerCount; ++j)
                {
                    if ((p != o || j != i) &&
                        TryAddLightBreak(scene, edge, other.CornersWorld[j], outBreaks, count) == false)
                    {
                        return false;
                    }
                }
            }
        }
    }
    // Insertion sort: the counts are small.
    for (usize i = 1; i < count; ++i)
    {
        const float64 value = outBreaks[i];
        usize j = i;
        while (j > 0 && outBreaks[j - 1] > value)
        {
            outBreaks[j] = outBreaks[j - 1];
            --j;
        }
        outBreaks[j] = value;
    }
    outCount = count;
    return true;
}

/// Integrates weight * transmission over the light, where weight is a point's unobstructed contribution to the
/// wall's irradiance, and returns the wall's radiance. Along x the light is split at every visibility change and each
/// piece takes Gauss-Legendre nodes; along y, where nothing changes visibility, the whole light takes them. Stops at
/// the first failed transmission.
template <typename TransmissionFunction>
[[nodiscard]] bool IntegrateOverLight(const EdgeDiffractionScene& scene,
                                      math::Vector3d wallWorld,
                                      usize nodeCount,
                                      const TransmissionFunction& transmission,
                                      float64& outRadiance) noexcept
{
    if (nodeCount == 0 || nodeCount > MAX_LIGHT_QUADRATURE_NODES)
    {
        return false;
    }
    const GaussLegendreRule rule = ComputeGaussLegendre(nodeCount);
    StaticArray<float64, MAX_LIGHT_BREAKS> breaks{};
    usize breakCount = 0;
    if (TryCollectLightBreaks(scene, wallWorld, breaks, breakCount) == false)
    {
        return false;
    }
    const float64 lightWidth = 2 * scene.LightHalfWidthWorld;
    const float64 halfHeight = scene.LightHalfHeightWorld;
    float64 irradiance = 0;
    for (usize b = 0; b + 1 < breakCount; ++b)
    {
        const float64 gap = breaks[b + 1] - breaks[b];
        if (gap <= GEOMETRY_TOLERANCE_WORLD)
        {
            continue;
        }
        // Long pieces split further, so that every piece spans at most 1 / MINIMUM_LIGHT_PIECES of the light.
        const usize pieces = static_cast<usize>(
            math::Ceil(gap * static_cast<float64>(MINIMUM_LIGHT_PIECES) / lightWidth - GEOMETRY_TOLERANCE_WORLD));
        const float64 pieceWidth = gap / static_cast<float64>(pieces == 0 ? 1 : pieces);
        for (usize piece = 0; piece < (pieces == 0 ? 1 : pieces); ++piece)
        {
            const float64 pieceCentre = breaks[b] + pieceWidth * (static_cast<float64>(piece) + 0.5);
            for (usize i = 0; i < nodeCount; ++i)
            {
                for (usize j = 0; j < nodeCount; ++j)
                {
                    const math::Vector3d sourceWorld{pieceCentre + pieceWidth / 2 * rule.Nodes[i],
                                                     scene.LightCenterWorld.Y + halfHeight * rule.Nodes[j],
                                                     scene.LightCenterWorld.Z};
                    float64 pointTransmission = 0;
                    if (transmission(sourceWorld, pointTransmission) == false)
                    {
                        return false;
                    }
                    const float64 height = sourceWorld.Z - wallWorld.Z;
                    const float64 distanceSquared = math::LengthSquared(sourceWorld - wallWorld);
                    // cos at the light times cos at the wall over d^2; both normals lie along z.
                    const float64 geometry = height * height / (distanceSquared * distanceSquared);
                    const float64 area = pieceWidth / 2 * rule.Weights[i] * halfHeight * rule.Weights[j];
                    irradiance += scene.LightRadiance * geometry * area * pointTransmission;
                }
            }
        }
    }
    outRadiance = irradiance * scene.WallAlbedo / math::kPi;
    return true;
}

/// Whether two values agree within the geometry tolerance.
[[nodiscard]] bool IsNear(float64 first, float64 second) noexcept
{
    return math::Abs(first - second) <= GEOMETRY_TOLERANCE_WORLD;
}

/// Whether a value is grey: three equal channels.
[[nodiscard]] bool IsGrey(math::Vector3d value) noexcept
{
    return value.X == value.Y && value.Y == value.Z;
}

/// An axis-aligned rectangle in a plane of constant z, with the z component of its winding's (unnormalized) normal.
struct SourceRectangle final
{
    math::Vector3d Minimum{};
    math::Vector3d Maximum{};
    float64 NormalZ = 0;
};

/// An object's single convex polygon as an axis-aligned rectangle in a plane of constant z. Returns false for anything
/// else.
[[nodiscard]] bool TryReadRectangle(const content::scene_source::SourceObject& object,
                                    SourceRectangle& outRectangle) noexcept
{
    if (object.PolygonVertexCounts.GetSize() != 1 || object.PolygonVertexCounts[0] != RECTANGLE_CORNERS)
    {
        return false;
    }
    math::Vector3d minimum = object.PositionsWorld[object.PolygonIndices[0]];
    math::Vector3d maximum = minimum;
    float64 normalZ = 0;
    for (usize i = 0; i < RECTANGLE_CORNERS; ++i)
    {
        const math::Vector3d point = object.PositionsWorld[object.PolygonIndices[i]];
        const math::Vector3d next = object.PositionsWorld[object.PolygonIndices[(i + 1) % RECTANGLE_CORNERS]];
        minimum = {math::Min(minimum.X, point.X), math::Min(minimum.Y, point.Y), math::Min(minimum.Z, point.Z)};
        maximum = {math::Max(maximum.X, point.X), math::Max(maximum.Y, point.Y), math::Max(maximum.Z, point.Z)};
        // Newell's method.
        normalZ += (point.X - next.X) * (point.Y + next.Y);
    }
    if (IsNear(minimum.Z, maximum.Z) == false)
    {
        return false;
    }
    for (usize i = 0; i < RECTANGLE_CORNERS; ++i)
    {
        const math::Vector3d point = object.PositionsWorld[object.PolygonIndices[i]];
        const bool isOnX = IsNear(point.X, minimum.X) == true || IsNear(point.X, maximum.X) == true;
        const bool isOnY = IsNear(point.Y, minimum.Y) == true || IsNear(point.Y, maximum.Y) == true;
        if (isOnX == false || isOnY == false)
        {
            return false;
        }
    }
    outRectangle = { .Minimum = minimum, .Maximum = maximum, .NormalZ = normalZ };
    return true;
}

/// An object's cross-section as a strictly convex counterclockwise prism along y, spanning y in [-halfLength,
/// halfLength] for the halfLength it reports. Returns false for anything else.
[[nodiscard]] bool TryReadPrism(const content::scene_source::SourceObject& object,
                                DiffractionObstacle& outObstacle,
                                float64& outHalfLength) noexcept
{
    const usize count = object.PositionsWorld.GetSize();
    if (count == 0)
    {
        return false;
    }
    float64 low = object.PositionsWorld[0].Y;
    float64 high = low;
    for (usize i = 0; i < count; ++i)
    {
        low = math::Min(low, object.PositionsWorld[i].Y);
        high = math::Max(high, object.PositionsWorld[i].Y);
    }
    if (high - low <= GEOMETRY_TOLERANCE_WORLD)
    {
        return false;
    }
    // Every position lies on one of the two end planes, and each end's cross-section has the same corners.
    StaticArray<math::Vector3d, MAX_CROSS_SECTION_CORNERS> points{};
    usize pointCount = 0;
    for (usize i = 0; i < count; ++i)
    {
        const math::Vector3d position = object.PositionsWorld[i];
        if (IsNear(position.Y, low) == false && IsNear(position.Y, high) == false)
        {
            return false;
        }
        const math::Vector3d planar{position.X, 0, position.Z};
        bool isKnown = false;
        for (usize p = 0; p < pointCount && isKnown == false; ++p)
        {
            isKnown = ComputePlanarDistance(points[p], planar) <= GEOMETRY_TOLERANCE_WORLD;
        }
        if (isKnown == false)
        {
            if (pointCount == MAX_CROSS_SECTION_CORNERS)
            {
                return false;
            }
            points[pointCount] = planar;
            ++pointCount;
        }
    }
    if (pointCount < 3 || count != 2 * pointCount)
    {
        return false;
    }
    // Gift wrapping from the lowest-x corner keeps every other corner on the left: counterclockwise.
    usize start = 0;
    for (usize p = 1; p < pointCount; ++p)
    {
        if (points[p].X < points[start].X || (points[p].X == points[start].X && points[p].Z < points[start].Z))
        {
            start = p;
        }
    }
    DiffractionObstacle obstacle;
    usize current = start;
    do
    {
        if (obstacle.CornerCount == pointCount)
        {
            return false;
        }
        obstacle.CornersWorld[obstacle.CornerCount] = points[current];
        ++obstacle.CornerCount;
        usize candidate = (current + 1) % pointCount;
        for (usize p = 0; p < pointCount; ++p)
        {
            if (p != current && ComputePlanarCross(points[current], points[candidate], points[p]) < 0)
            {
                candidate = p;
            }
        }
        current = candidate;
    } while (current != start);
    // Strict convexity: every corner is on the hull and turns left by more than the tolerance.
    if (obstacle.CornerCount != pointCount)
    {
        return false;
    }
    for (usize i = 0; i < obstacle.CornerCount; ++i)
    {
        const math::Vector3d previous = obstacle.CornersWorld[(i + obstacle.CornerCount - 1) % obstacle.CornerCount];
        const math::Vector3d next = obstacle.CornersWorld[(i + 1) % obstacle.CornerCount];
        if (ComputePlanarCross(previous, obstacle.CornersWorld[i], next) <= GEOMETRY_TOLERANCE_WORLD)
        {
            return false;
        }
    }
    outObstacle = obstacle;
    outHalfLength = (high - low) / 2;
    return true;
}
} // namespace

float64 ComputeSquaredMagnitude(DiffractionComplex value) noexcept
{
    return value.Real * value.Real + value.Imaginary * value.Imaginary;
}

FresnelIntegrals ComputeFresnelIntegrals(float64 x) noexcept
{
    const float64 magnitude = math::Abs(x);
    const float64 sign = x < 0 ? -1.0 : 1.0;
    // u = x sqrt(pi / 2) maps the pi t^2 / 2 convention onto G's t^2.
    const float64 u = magnitude * math::Sqrt(math::kPi / 2);
    if (u <= FRESNEL_SERIES_LIMIT)
    {
        const FresnelIntegrals series = ComputeFresnelSeries(magnitude);
        return {sign * series.Cosine, sign * series.Sine};
    }
    // G(u) = sqrt(pi / 2) [(1/2 - C) - j (1/2 - S)].
    const DiffractionComplex tail = ComputeFresnelTail(u);
    const float64 scale = math::Sqrt(math::kPi / 2);
    return {sign * (0.5 - tail.Real / scale), sign * (0.5 + tail.Imaginary / scale)};
}

DiffractionComplex ComputeFresnelTail(float64 u) noexcept
{
    if (u < -FRESNEL_SERIES_LIMIT)
    {
        // G(u) = 2 G(0) - G(-u).
        return Subtract(Scale(GetFresnelTailAtZero(), 2), ComputeFresnelTail(-u));
    }
    if (u > FRESNEL_SERIES_LIMIT)
    {
        return Multiply(MakePhase(-u * u), ComputeScaledFresnelTail(u));
    }
    const float64 scale = math::Sqrt(math::kPi / 2);
    const FresnelIntegrals series = ComputeFresnelSeries(u / scale);
    return {scale * (0.5 - series.Cosine), -scale * (0.5 - series.Sine)};
}

DiffractionComplex ComputeUtdTransition(float64 x) noexcept
{
    if (x <= 0)
    {
        return {};
    }
    const float64 u = math::Sqrt(x);
    if (u > FRESNEL_SERIES_LIMIT)
    {
        // exp(j x) G(sqrt(x)) = H(sqrt(x)).
        return Scale(Multiply({0, 1}, ComputeScaledFresnelTail(u)), 2 * u);
    }
    return Scale(Multiply(Multiply({0, 1}, MakePhase(x)), ComputeFresnelTail(u)), 2 * u);
}

DiffractionComplex ComputeUtdCoefficient(const WedgeDiffraction& diffraction) noexcept
{
    const float64 n = diffraction.WedgeN;
    const float64 difference = diffraction.ObservationAngle - diffraction.IncidentAngle;
    const float64 sum = diffraction.ObservationAngle + diffraction.IncidentAngle;
    const DiffractionComplex bracket =
        Add(ComputeCoefficientPair(difference, diffraction),
            Scale(ComputeCoefficientPair(sum, diffraction), GetReflectionSign(diffraction.Condition)));
    // -exp(-j pi / 4) / (2 n sqrt(2 pi k) sin(beta0)).
    const float64 magnitude = 1 / (2 * n * math::Sqrt(2 * math::kPi * diffraction.Wavenumber) * diffraction.EdgeSine);
    return Multiply(Scale(MakePhase(-math::kPi / 4), -magnitude), bracket);
}

DiffractionComplex ComputeGtdCoefficient(const WedgeDiffraction& diffraction) noexcept
{
    const float64 n = diffraction.WedgeN;
    const float64 boundaryCosine = math::Cos(math::kPi / n);
    const float64 difference = diffraction.ObservationAngle - diffraction.IncidentAngle;
    const float64 sum = diffraction.ObservationAngle + diffraction.IncidentAngle;
    const float64 bracket = 1 / (boundaryCosine - math::Cos(difference / n)) +
                            GetReflectionSign(diffraction.Condition) / (boundaryCosine - math::Cos(sum / n));
    const float64 magnitude =
        math::Sin(math::kPi / n) / (n * math::Sqrt(2 * math::kPi * diffraction.Wavenumber) * diffraction.EdgeSine);
    return Scale(MakePhase(-math::kPi / 4), magnitude * bracket);
}

DiffractionComplex ComputeHalfPlaneField(const WedgeDiffraction& diffraction) noexcept
{
    const float64 k = diffraction.Wavenumber;
    const float64 rho = diffraction.DistanceParameterWorld;
    const float64 root = math::Sqrt(2 * k * rho);
    const float64 difference = diffraction.ObservationAngle - diffraction.IncidentAngle;
    const float64 sum = diffraction.ObservationAngle + diffraction.IncidentAngle;
    const DiffractionComplex direct =
        Multiply(MakePhase(k * rho * math::Cos(difference)), ComputeFresnelTail(-root * math::Cos(difference / 2)));
    const DiffractionComplex reflected =
        Multiply(MakePhase(k * rho * math::Cos(sum)), ComputeFresnelTail(-root * math::Cos(sum / 2)));
    const DiffractionComplex bracket = Add(direct, Scale(reflected, GetReflectionSign(diffraction.Condition)));
    return Scale(Multiply(MakePhase(math::kPi / 4), bracket), 1 / math::Sqrt(math::kPi));
}

DiffractionComplex ComputeUtdWedgeField(const WedgeDiffraction& diffraction) noexcept
{
    const float64 k = diffraction.Wavenumber;
    const float64 rho = diffraction.DistanceParameterWorld;
    const float64 n = diffraction.WedgeN;
    const float64 observation = diffraction.ObservationAngle;
    const float64 incident = diffraction.IncidentAngle;
    const float64 reflectionSign = GetReflectionSign(diffraction.Condition);
    DiffractionComplex total{};
    if (math::Abs(observation - incident) < math::kPi)
    {
        total = MakePhase(k * rho * math::Cos(observation - incident));
    }
    // Face 0 mirrors the source to -phi'; face n mirrors it to 2 n pi - phi'.
    if (observation + incident < math::kPi)
    {
        total = Add(total, Scale(MakePhase(k * rho * math::Cos(observation + incident)), reflectionSign));
    }
    if (observation + incident > (2 * n - 1) * math::kPi)
    {
        total = Add(total,
                    Scale(MakePhase(k * rho * math::Cos(observation + incident - 2 * n * math::kPi)), reflectionSign));
    }
    const DiffractionComplex diffracted =
        Scale(Multiply(ComputeUtdCoefficient(diffraction), MakePhase(-k * rho)), 1 / math::Sqrt(rho));
    return Add(total, diffracted);
}

DiffractionSceneStatus BuildEdgeDiffractionScene(const content::scene_source::SceneSource& source,
                                                 EdgeDiffractionScene& outScene) noexcept
{
    if (source.Camera.IsPresent == false)
    {
        return DiffractionSceneStatus::MissingCamera;
    }
    EdgeDiffractionScene scene;
    usize lights = 0;
    usize walls = 0;
    for (usize i = 0; i < source.Objects.GetSize(); ++i)
    {
        const content::scene_source::SourceObject& object = source.Objects[i];
        const content::scene_source::SourceMaterial& material = source.Materials[object.Material];
        const math::Vector3d emission = material.EmittedRadiance;
        const math::Vector3d color = material.BaseColor;
        SourceRectangle rectangle;
        if (emission.X > 0 || emission.Y > 0 || emission.Z > 0)
        {
            // The light: grey, one-sided, an axis-aligned rectangle whose winding faces -z.
            if (IsGrey(emission) == false || material.IsEmissionTwoSided == true ||
                TryReadRectangle(object, rectangle) == false || rectangle.NormalZ >= 0)
            {
                return DiffractionSceneStatus::UnsupportedLight;
            }
            scene.LightCenterWorld = (rectangle.Minimum + rectangle.Maximum) * 0.5;
            scene.LightHalfWidthWorld = (rectangle.Maximum.X - rectangle.Minimum.X) / 2;
            scene.LightHalfHeightWorld = (rectangle.Maximum.Y - rectangle.Minimum.Y) / 2;
            scene.LightRadiance = emission.X;
            ++lights;
        }
        else if (color.X > 0 || color.Y > 0 || color.Z > 0)
        {
            // The wall: grey and Lambertian, an axis-aligned rectangle facing +z.
            if (material.Model != content::scene_source::SourceMaterialModel::Diffuse || IsGrey(color) == false ||
                TryReadRectangle(object, rectangle) == false || rectangle.NormalZ <= 0)
            {
                return DiffractionSceneStatus::UnsupportedWall;
            }
            scene.WallHeightWorld = rectangle.Minimum.Z;
            scene.WallMinimumWorld = rectangle.Minimum;
            scene.WallMaximumWorld = rectangle.Maximum;
            scene.WallAlbedo = color.X;
            ++walls;
        }
        else
        {
            if (scene.ObstacleCount == MAX_DIFFRACTION_OBSTACLES)
            {
                return DiffractionSceneStatus::TooManyObstacles;
            }
            float64 halfLength = 0;
            if (TryReadPrism(object, scene.Obstacles[scene.ObstacleCount], halfLength) == false)
            {
                return DiffractionSceneStatus::UnsupportedObstacle;
            }
            ++scene.ObstacleCount;
        }
    }
    if (lights != 1)
    {
        return DiffractionSceneStatus::UnsupportedLight;
    }
    if (walls != 1 || scene.WallHeightWorld >= scene.LightCenterWorld.Z)
    {
        return DiffractionSceneStatus::UnsupportedWall;
    }
    // Every obstacle lies between the light and the wall.
    for (usize o = 0; o < scene.ObstacleCount; ++o)
    {
        const DiffractionObstacle& obstacle = scene.Obstacles[o];
        for (usize i = 0; i < obstacle.CornerCount; ++i)
        {
            const float64 z = obstacle.CornersWorld[i].Z;
            if (z <= scene.WallHeightWorld || z >= scene.LightCenterWorld.Z)
            {
                return DiffractionSceneStatus::UnsupportedObstacle;
            }
        }
    }
    scene.EyeWorld = source.Camera.EyeWorld;
    scene.TargetWorld = source.Camera.TargetWorld;
    scene.UpWorld = source.Camera.UpWorld;
    scene.VerticalFovRadians = source.Camera.VerticalFovRadians;
    outScene = scene;
    return DiffractionSceneStatus::Success;
}

bool TryComputeDiffractionTransmission(const EdgeDiffractionScene& scene,
                                       DiffractionModel model,
                                       float64 wavelengthWorld,
                                       math::Vector3d sourceWorld,
                                       math::Vector3d wallWorld,
                                       float64& outTransmission) noexcept
{
    if (math::IsFinite(wavelengthWorld) == false || wavelengthWorld <= 0)
    {
        return false;
    }
    const float64 wavenumber = 2 * math::kPi / wavelengthWorld;
    switch (model)
    {
        case DiffractionModel::Geometric:
            outTransmission = IsSegmentBlocked(scene, sourceWorld, wallWorld) == true ? 0.0 : 1.0;
            return true;
        case DiffractionModel::Kirchhoff: {
            Aperture aperture;
            if (TryFindAperture(scene, aperture) == false)
            {
                return false;
            }
            outTransmission = ComputeKirchhoffTransmission(wavelengthWorld, aperture, sourceWorld, wallWorld);
            return true;
        }
        case DiffractionModel::UtdAbsorbing:
            outTransmission =
                ComputeUtdTransmission(scene, wavenumber, EdgeCondition::Absorbing, sourceWorld, wallWorld);
            return true;
        case DiffractionModel::UtdSoft:
            outTransmission = ComputeUtdTransmission(scene, wavenumber, EdgeCondition::Soft, sourceWorld, wallWorld);
            return true;
        case DiffractionModel::UtdHard:
            outTransmission = ComputeUtdTransmission(scene, wavenumber, EdgeCondition::Hard, sourceWorld, wallWorld);
            return true;
        case DiffractionModel::UtdUnpolarized:
            outTransmission = (ComputeUtdTransmission(scene, wavenumber, EdgeCondition::Soft, sourceWorld, wallWorld) +
                               ComputeUtdTransmission(scene, wavenumber, EdgeCondition::Hard, sourceWorld, wallWorld)) /
                              2;
            return true;
    }
    return false;
}

bool TryComputeDiffractionWallRadiance(const EdgeDiffractionScene& scene,
                                       const DiffractionSettings& settings,
                                       math::Vector3d wallWorld,
                                       float64& outRadiance) noexcept
{
    return IntegrateOverLight(
        scene,
        wallWorld,
        settings.LightQuadratureNodes,
        [&](math::Vector3d sourceWorld, float64& outTransmission) {
            return TryComputeDiffractionTransmission(scene,
                                                     settings.Model,
                                                     settings.WavelengthWorld,
                                                     sourceWorld,
                                                     wallWorld,
                                                     outTransmission);
        },
        outRadiance);
}

float64 ComputeDiffractionFreeWallRadiance(const EdgeDiffractionScene& scene,
                                           math::Vector3d wallWorld,
                                           usize lightQuadratureNodes) noexcept
{
    float64 radiance = 0;
    const bool isIntegrated = IntegrateOverLight(
        scene,
        wallWorld,
        lightQuadratureNodes,
        [](math::Vector3d, float64& outTransmission) {
            outTransmission = 1;
            return true;
        },
        radiance);
    return isIntegrated == true ? radiance : 0.0;
}

bool TryFindDiffractionWallPoint(const EdgeDiffractionScene& scene,
                                 uint32 column,
                                 uint32 row,
                                 uint32 width,
                                 uint32 height,
                                 math::Vector3d& outWallWorld) noexcept
{
    if (width == 0 || height == 0 || column >= width || row >= height)
    {
        return false;
    }
    // The camera of math::TryLookAt and a symmetric perspective: x right, y up, looking along the target direction.
    const math::Vector3d towardsTarget = scene.TargetWorld - scene.EyeWorld;
    const math::Vector3d forward = towardsTarget * (1 / math::Length(towardsTarget));
    const math::Vector3d rightUnscaled = math::Cross(forward, scene.UpWorld);
    const math::Vector3d right = rightUnscaled * (1 / math::Length(rightUnscaled));
    const math::Vector3d up = math::Cross(right, forward);
    const float64 tangent = math::Sin(scene.VerticalFovRadians / 2) / math::Cos(scene.VerticalFovRadians / 2);
    const float64 aspect = static_cast<float64>(width) / static_cast<float64>(height);
    const float64 ndcX = (static_cast<float64>(column) + 0.5) / static_cast<float64>(width) * 2 - 1;
    const float64 ndcY = 1 - (static_cast<float64>(row) + 0.5) / static_cast<float64>(height) * 2;
    const math::Vector3d direction = forward + right * (ndcX * tangent * aspect) + up * (ndcY * tangent);
    if (direction.Z >= 0)
    {
        return false;
    }
    const float64 distance = (scene.WallHeightWorld - scene.EyeWorld.Z) / direction.Z;
    const math::Vector3d wallWorld = scene.EyeWorld + direction * distance;
    if (distance <= 0 || wallWorld.X < scene.WallMinimumWorld.X || wallWorld.X > scene.WallMaximumWorld.X ||
        wallWorld.Y < scene.WallMinimumWorld.Y || wallWorld.Y > scene.WallMaximumWorld.Y ||
        IsSegmentBlocked(scene, scene.EyeWorld, wallWorld) == true)
    {
        return false;
    }
    outWallWorld = {wallWorld.X, wallWorld.Y, scene.WallHeightWorld};
    return true;
}
} // namespace ludus::graphics::renderer::testing
