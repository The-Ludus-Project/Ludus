#include "edge_diffraction_oracle.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

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

/// -1 for a soft edge and +1 for a hard one: the sign of the reflected terms.
[[nodiscard]] float64 GetReflectionSign(EdgeCondition condition) noexcept
{
    return condition == EdgeCondition::Hard ? 1.0 : -1.0;
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

/// Whether the straight segment from the source to the wall point passes between the edges at z = 0.
[[nodiscard]] bool
IsGeometricallyVisible(const SingleSlitScene& scene, math::Vector3d sourceWorld, math::Vector3d wallWorld) noexcept
{
    const float64 t = sourceWorld.Z / (sourceWorld.Z - wallWorld.Z);
    const float64 crossingX = sourceWorld.X + (wallWorld.X - sourceWorld.X) * t;
    return math::Abs(crossingX) <= scene.SlitHalfWidthWorld;
}

/// UTD diffraction by one jaw's edge, at x = side * SlitHalfWidthWorld, of a point source's spherical wave.
[[nodiscard]] DiffractionComplex ComputeEdgeDiffractedField(const SingleSlitScene& scene,
                                                            float64 side,
                                                            math::Vector3d sourceWorld,
                                                            math::Vector3d wallWorld,
                                                            EdgeCondition condition) noexcept
{
    const float64 wavenumber = 2 * math::kPi / scene.WavelengthWorld;
    const float64 edgeX = side * scene.SlitHalfWidthWorld;
    // In the plane perpendicular to the edge, u points from the edge into its jaw (along face 0) and z towards the
    // light, so the exterior runs from face 0 (angle 0) through the light, the slit and the wall to the bevel.
    const float64 sourceU = side * (sourceWorld.X - edgeX);
    const float64 wallU = side * (wallWorld.X - edgeX);
    const float64 incidentAngle = WrapAngle(math::Atan2(sourceWorld.Z, sourceU));
    const float64 observationAngle = WrapAngle(math::Atan2(wallWorld.Z, wallU));
    if (observationAngle > scene.WedgeN * math::kPi || incidentAngle > scene.WedgeN * math::kPi)
    {
        return {};
    }
    // The diffraction point on the edge satisfies Keller's law: the incident and diffracted rays make equal angles
    // with the edge. Unfolding the two rays about the edge places it at the distance-weighted point.
    const float64 sourceDistance = math::Sqrt(sourceU * sourceU + sourceWorld.Z * sourceWorld.Z);
    const float64 wallDistance = math::Sqrt(wallU * wallU + wallWorld.Z * wallWorld.Z);
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
    diffraction.WedgeN = scene.WedgeN;
    diffraction.Wavenumber = wavenumber;
    diffraction.Condition = condition;
    const DiffractionComplex coefficient = ComputeUtdCoefficient(diffraction);
    const float64 spreading = math::Sqrt(incidentLength / (diffractedLength * (incidentLength + diffractedLength)));
    const DiffractionComplex incident = MakeSphericalWave(wavenumber, incidentLength);
    return Scale(Multiply(Multiply(incident, coefficient), MakePhase(-wavenumber * diffractedLength)), spreading);
}

[[nodiscard]] float64 ComputeUtdTransmission(const SingleSlitScene& scene,
                                             math::Vector3d sourceWorld,
                                             math::Vector3d wallWorld,
                                             EdgeCondition condition) noexcept
{
    const float64 wavenumber = 2 * math::kPi / scene.WavelengthWorld;
    const float64 freeDistance = math::Length(wallWorld - sourceWorld);
    DiffractionComplex total{};
    if (IsGeometricallyVisible(scene, sourceWorld, wallWorld) == true)
    {
        total = MakeSphericalWave(wavenumber, freeDistance);
    }
    total = Add(total, ComputeEdgeDiffractedField(scene, 1, sourceWorld, wallWorld, condition));
    total = Add(total, ComputeEdgeDiffractedField(scene, -1, sourceWorld, wallWorld, condition));
    return ComputeSquaredMagnitude(total) * freeDistance * freeDistance;
}

/// Fresnel-Kirchhoff over the slit opening |x| <= SlitHalfWidthWorld at z = 0, with U = (j / lambda) integral of
/// U_incident exp(-j k r) / r times the mean obliquity (cos + cos) / 2. Along the infinitely long slit the integral is
/// evaluated by stationary phase; across it by Simpson's rule.
[[nodiscard]] float64 ComputeKirchhoffTransmission(const SingleSlitScene& scene,
                                                   math::Vector3d sourceWorld,
                                                   math::Vector3d wallWorld) noexcept
{
    const float64 wavenumber = 2 * math::kPi / scene.WavelengthWorld;
    const float64 width = 2 * scene.SlitHalfWidthWorld;
    const float64 panelWidth = width / static_cast<float64>(KIRCHHOFF_PANELS);
    const float64 alongSlit = wallWorld.Y - sourceWorld.Y;
    DiffractionComplex sum{};
    for (uint32 i = 0; i <= KIRCHHOFF_PANELS; ++i)
    {
        const float64 x = -scene.SlitHalfWidthWorld + panelWidth * static_cast<float64>(i);
        const float64 incidentPlanar =
            math::Sqrt((x - sourceWorld.X) * (x - sourceWorld.X) + sourceWorld.Z * sourceWorld.Z);
        const float64 diffractedPlanar = math::Sqrt((wallWorld.X - x) * (wallWorld.X - x) + wallWorld.Z * wallWorld.Z);
        // The stationary point along the slit lies on the unfolded straight line; both distances grow by the same
        // factor there.
        const float64 slope = alongSlit / (incidentPlanar + diffractedPlanar);
        const float64 growth = math::Sqrt(1 + slope * slope);
        const float64 incidentLength = incidentPlanar * growth;
        const float64 diffractedLength = diffractedPlanar * growth;
        const float64 curvature = (1 / incidentPlanar + 1 / diffractedPlanar) / (growth * growth * growth);
        const float64 obliquity = (sourceWorld.Z / incidentLength - wallWorld.Z / diffractedLength) / 2;
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
    const DiffractionComplex field = Multiply({0, 1 / scene.WavelengthWorld}, Scale(sum, panelWidth / 3));
    const float64 freeDistance = math::Length(wallWorld - sourceWorld);
    return ComputeSquaredMagnitude(field) * freeDistance * freeDistance;
}

/// Sums weight(cell) * transmission(cell) over the light's midpoint-rule cells, where weight is the cell's
/// contribution to the wall's irradiance without the jaws.
template <typename TransmissionFunction>
[[nodiscard]] float64 IntegrateOverLight(const SingleSlitScene& scene,
                                         math::Vector3d wallWorld,
                                         usize subdivisions,
                                         const TransmissionFunction& transmission) noexcept
{
    const float64 cellSize = 2 * scene.LightHalfSizeWorld / static_cast<float64>(subdivisions);
    float64 irradiance = 0;
    for (usize row = 0; row < subdivisions; ++row)
    {
        for (usize column = 0; column < subdivisions; ++column)
        {
            const math::Vector3d sourceWorld{-scene.LightHalfSizeWorld +
                                                 cellSize * (static_cast<float64>(column) + 0.5),
                                             -scene.LightHalfSizeWorld + cellSize * (static_cast<float64>(row) + 0.5),
                                             scene.LightHeightWorld};
            const float64 height = sourceWorld.Z - wallWorld.Z;
            const float64 distanceSquared = math::LengthSquared(sourceWorld - wallWorld);
            // cos at the light times cos at the wall over d^2; both normals lie along z.
            const float64 geometry = height * height / (distanceSquared * distanceSquared);
            irradiance += scene.LightRadiance * geometry * cellSize * cellSize * transmission(sourceWorld);
        }
    }
    return irradiance * scene.WallAlbedo / math::kPi;
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

float64 ComputeSlitTransmission(const SingleSlitScene& scene,
                                SlitModel model,
                                math::Vector3d sourceWorld,
                                math::Vector3d wallWorld) noexcept
{
    switch (model)
    {
        case SlitModel::Geometric:
            return IsGeometricallyVisible(scene, sourceWorld, wallWorld) == true ? 1.0 : 0.0;
        case SlitModel::Kirchhoff:
            return ComputeKirchhoffTransmission(scene, sourceWorld, wallWorld);
        case SlitModel::UtdSoft:
            return ComputeUtdTransmission(scene, sourceWorld, wallWorld, EdgeCondition::Soft);
        case SlitModel::UtdHard:
            return ComputeUtdTransmission(scene, sourceWorld, wallWorld, EdgeCondition::Hard);
        case SlitModel::UtdUnpolarized:
            return (ComputeUtdTransmission(scene, sourceWorld, wallWorld, EdgeCondition::Soft) +
                    ComputeUtdTransmission(scene, sourceWorld, wallWorld, EdgeCondition::Hard)) /
                   2;
    }
    return 0;
}

float64 ComputeSlitWallRadiance(const SingleSlitScene& scene,
                                SlitModel model,
                                math::Vector3d wallWorld,
                                usize subdivisions) noexcept
{
    return IntegrateOverLight(scene, wallWorld, subdivisions, [&](math::Vector3d sourceWorld) {
        return ComputeSlitTransmission(scene, model, sourceWorld, wallWorld);
    });
}

float64 ComputeSlitFreeWallRadiance(const SingleSlitScene& scene, math::Vector3d wallWorld, usize subdivisions) noexcept
{
    return IntegrateOverLight(scene, wallWorld, subdivisions, [](math::Vector3d) { return 1.0; });
}
} // namespace ludus::graphics::renderer::testing
