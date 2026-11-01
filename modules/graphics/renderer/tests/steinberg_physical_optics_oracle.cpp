#include "steinberg_physical_optics_oracle.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

// Every function follows its namesake in wave_optics.slang step by step, in float64; see that file and the header for
// the paper's equations and the template's departures.
namespace ludus::graphics::renderer::testing
{
namespace
{
using foundation::float64;
using foundation::int32;
using foundation::uint32;
using foundation::math::Vector3d;
namespace math = foundation::math;

constexpr float64 NANOMETERS_PER_WORLD_UNIT = 1e9;
constexpr float64 REFERENCE_WAVELENGTH_NANOMETERS = 550;
constexpr float64 MINIMUM_COSINE = 1e-3;
constexpr float64 MINIMUM_AXIS_LENGTH_SQUARED = 1e-6;
constexpr float64 HESSIAN_STEP = .05;
constexpr float64 HESSIAN_RESOLUTION = 1e-4;
constexpr float64 MAXIMUM_DEFORMATION_ANISOTROPY = 100;
constexpr float64 BESSEL_SMALL_ARGUMENT = 1e-4;
constexpr float64 BESSEL_ASYMPTOTIC_ARGUMENT = 64;
constexpr float64 BESSEL_MILLER_ACCURACY = 40;
constexpr float64 BESSEL_RESCALE_THRESHOLD = 1e10;
constexpr float64 BESSEL_RESCALE_FACTOR = 1e-10;
constexpr uint32 GRATING_MAXIMUM_ORDER = 8;
/// Helper axes of MakePerpendicularWorld.
constexpr float64 PERPENDICULAR_HELPER_LIMIT = .9;
/// The Hessians' sample directions: the centre, +/- the first axis, +/- the second axis and both.
constexpr foundation::usize HESSIAN_DIRECTION_COUNT = 6;
using HessianValues = foundation::StaticArray<float64, HESSIAN_DIRECTION_COUNT>;

Vector3d Normalize(Vector3d value) noexcept
{
    return value * (1 / math::Length(value));
}

OpticsComplex Add(OpticsComplex first, OpticsComplex second) noexcept
{
    return {first.Real + second.Real, first.Imaginary + second.Imaginary};
}

OpticsComplex Subtract(OpticsComplex first, OpticsComplex second) noexcept
{
    return {first.Real - second.Real, first.Imaginary - second.Imaginary};
}

OpticsComplex Multiply(OpticsComplex first, OpticsComplex second) noexcept
{
    return {first.Real * second.Real - first.Imaginary * second.Imaginary,
            first.Real * second.Imaginary + first.Imaginary * second.Real};
}

OpticsComplex Conjugate(OpticsComplex value) noexcept
{
    return {value.Real, -value.Imaginary};
}

float64 SquaredMagnitude(OpticsComplex value) noexcept
{
    return value.Real * value.Real + value.Imaginary * value.Imaginary;
}

OpticsComplex Divide(OpticsComplex numerator, OpticsComplex denominator) noexcept
{
    const float64 squaredMagnitude = SquaredMagnitude(denominator);
    if (squaredMagnitude <= 0)
    {
        return {};
    }
    const OpticsComplex product = Multiply(numerator, Conjugate(denominator));
    return {product.Real / squaredMagnitude, product.Imaginary / squaredMagnitude};
}

OpticsComplex SquareRoot(OpticsComplex value) noexcept
{
    const float64 magnitude = math::Sqrt(SquaredMagnitude(value));
    const float64 real = math::Sqrt(math::Max((magnitude + value.Real) * .5, 0.0));
    const float64 imaginary = math::Sqrt(math::Max((magnitude - value.Real) * .5, 0.0));
    return {real, value.Imaginary < 0 ? -imaginary : imaginary};
}

OpticsStokes
ApplyFresnelMueller(OpticsComplex perpendicular, OpticsComplex parallel, const OpticsStokes& stokes) noexcept
{
    const float64 parallelSquared = SquaredMagnitude(parallel);
    const float64 perpendicularSquared = SquaredMagnitude(perpendicular);
    const float64 m00 = .5 * (parallelSquared + perpendicularSquared);
    const float64 m01 = .5 * (parallelSquared - perpendicularSquared);
    const OpticsComplex cross = Multiply(perpendicular, Conjugate(parallel));
    return {m00 * stokes.S0 + m01 * stokes.S1,
            m01 * stokes.S0 + m00 * stokes.S1,
            cross.Real * stokes.S2 + cross.Imaginary * stokes.S3,
            -cross.Imaginary * stokes.S2 + cross.Real * stokes.S3};
}

OpticsStokes RotateStokesFrame(const OpticsStokes& stokes, float64 angle) noexcept
{
    const float64 cosine = math::Cos(2 * angle);
    const float64 sine = math::Sin(2 * angle);
    return {stokes.S0, cosine * stokes.S1 + sine * stokes.S2, -sine * stokes.S1 + cosine * stokes.S2, stokes.S3};
}

OpticsSymmetric Invert(const OpticsSymmetric& matrix) noexcept
{
    const float64 determinant = ComputeOpticsDeterminant(matrix);
    if (determinant <= 0)
    {
        return {};
    }
    return {matrix.Yy / determinant, -matrix.Xy / determinant, matrix.Xx / determinant};
}

OpticsSymmetric Scale(const OpticsSymmetric& matrix, float64 factor) noexcept
{
    return {matrix.Xx * factor, matrix.Xy * factor, matrix.Yy * factor};
}

OpticsSymmetric Sum(const OpticsSymmetric& first, const OpticsSymmetric& second) noexcept
{
    return {first.Xx + second.Xx, first.Xy + second.Xy, first.Yy + second.Yy};
}

OpticsSymmetric Rotate(const OpticsSymmetric& matrix, float64 angle) noexcept
{
    const float64 cosine = math::Cos(angle);
    const float64 sine = math::Sin(angle);
    const float64 cosineSquared = cosine * cosine;
    const float64 sineSquared = sine * sine;
    const float64 product = cosine * sine;
    return {cosineSquared * matrix.Xx + 2 * product * matrix.Xy + sineSquared * matrix.Yy,
            -product * matrix.Xx + (cosineSquared - sineSquared) * matrix.Xy + product * matrix.Yy,
            sineSquared * matrix.Xx - 2 * product * matrix.Xy + cosineSquared * matrix.Yy};
}

/// U m U^T with U symmetric (u00, u01 = u10, u11).
OpticsSymmetric Transform(const OpticsSymmetric& matrix, const OpticsSymmetric& transform) noexcept
{
    const float64 firstX = transform.Xx * matrix.Xx + transform.Xy * matrix.Xy;
    const float64 firstY = transform.Xx * matrix.Xy + transform.Xy * matrix.Yy;
    const float64 secondX = transform.Xy * matrix.Xx + transform.Yy * matrix.Xy;
    const float64 secondY = transform.Xy * matrix.Xy + transform.Yy * matrix.Yy;
    return {firstX * transform.Xx + firstY * transform.Xy,
            firstX * transform.Xy + firstY * transform.Yy,
            secondX * transform.Xy + secondY * transform.Yy};
}

float64 ComputeSmallerEigenvalue(const OpticsSymmetric& matrix) noexcept
{
    const float64 mean = .5 * (matrix.Xx + matrix.Yy);
    const float64 halfDifference = .5 * (matrix.Xx - matrix.Yy);
    return mean - math::Sqrt(halfDifference * halfDifference + matrix.Xy * matrix.Xy);
}

float64 EvaluateBivariateNormal(float64 offsetX, float64 offsetY, const OpticsSymmetric& covariance) noexcept
{
    const float64 determinant = ComputeOpticsDeterminant(covariance);
    if (determinant <= 0 || covariance.Xx <= 0)
    {
        return 0;
    }
    const OpticsSymmetric inverse = Invert(covariance);
    const float64 exponent =
        inverse.Xx * offsetX * offsetX + 2 * inverse.Xy * offsetX * offsetY + inverse.Yy * offsetY * offsetY;
    return math::Exp(-.5 * exponent) / (2 * math::kPi * math::Sqrt(determinant));
}

Vector3d MakePerpendicular(Vector3d direction) noexcept
{
    const Vector3d helper = math::Abs(direction.X) < PERPENDICULAR_HELPER_LIMIT ? Vector3d{1, 0, 0} : Vector3d{0, 1, 0};
    return Normalize(math::Cross(helper, direction));
}

Vector3d GetTransverseY(const OpticsBeam& beam) noexcept
{
    return math::Cross(beam.PropagationWorld, beam.TransverseXWorld);
}

float64 ComputeWavenumber(float64 wavelengthNanometers) noexcept
{
    return 2 * math::kPi * NANOMETERS_PER_WORLD_UNIT / wavelengthNanometers;
}

/// wave_optics.slang's SurfaceCoherence: Sigma~^-1 in the basis whose first axis is the projected propagation.
struct SurfaceCoherence final
{
    float64 AxisX = 1; ///< Unit first axis in (tangent, bitangent) coordinates.
    float64 AxisY = 0;
    OpticsSymmetric Inverse{};
};

void RotateIntoBasis(const SurfaceCoherence& coherence,
                     float64 x,
                     float64 y,
                     float64& outFirst,
                     float64& outSecond) noexcept
{
    outFirst = x * coherence.AxisX + y * coherence.AxisY;
    outSecond = -x * coherence.AxisY + y * coherence.AxisX;
}

/// wave_optics.slang's ComputeSurfaceCoherence.
SurfaceCoherence ComputeSurfaceCoherence(const OpticsBeam& beam,
                                         const OpticsSymmetric& shape,
                                         Vector3d normal,
                                         Vector3d tangent,
                                         Vector3d bitangent,
                                         float64 wavenumber,
                                         float64 longitudinalCoherence) noexcept
{
    SurfaceCoherence result;
    const float64 projectedX = math::Dot(tangent, beam.PropagationWorld);
    const float64 projectedY = math::Dot(bitangent, beam.PropagationWorld);
    const float64 projectedLength = math::Sqrt(projectedX * projectedX + projectedY * projectedY);
    if (projectedLength > MINIMUM_AXIS_LENGTH_SQUARED)
    {
        result.AxisX = projectedX / projectedLength;
        result.AxisY = projectedY / projectedLength;
    }
    const Vector3d axisWorld = tangent * result.AxisX + bitangent * result.AxisY;
    const Vector3d perpendicularWorld = math::Cross(normal, axisWorld);
    const Vector3d y = GetTransverseY(beam);
    const float64 axisX = math::Dot(axisWorld, beam.TransverseXWorld);
    const float64 axisY = math::Dot(axisWorld, y);
    const float64 perpendicularX = math::Dot(perpendicularWorld, beam.TransverseXWorld);
    const float64 perpendicularY = math::Dot(perpendicularWorld, y);
    const OpticsSymmetric transverse = Scale(shape, beam.DistanceWorld * beam.DistanceWorld);
    const float64 axisAxis =
        axisX * axisX * transverse.Xx + 2 * axisX * axisY * transverse.Xy + axisY * axisY * transverse.Yy;
    const float64 axisPerpendicular = axisX * perpendicularX * transverse.Xx +
                                      (axisX * perpendicularY + axisY * perpendicularX) * transverse.Xy +
                                      axisY * perpendicularY * transverse.Yy;
    const float64 perpendicularPerpendicular = perpendicularX * perpendicularX * transverse.Xx +
                                               2 * perpendicularX * perpendicularY * transverse.Xy +
                                               perpendicularY * perpendicularY * transverse.Yy;
    const float64 longitudinal = wavenumber * longitudinalCoherence * projectedLength;
    const float64 projectionDeterminant = axisX * perpendicularY - axisY * perpendicularX;
    const float64 determinant = longitudinal * longitudinal * perpendicularPerpendicular +
                                ComputeOpticsDeterminant(transverse) * projectionDeterminant * projectionDeterminant;
    if (determinant <= 0)
    {
        return result;
    }
    result.Inverse = {perpendicularPerpendicular / determinant,
                      -axisPerpendicular / determinant,
                      (axisAxis + longitudinal * longitudinal) / determinant};
    return result;
}

void ComputeFresnelIntensities(Vector3d incident,
                               Vector3d outgoing,
                               OpticsComplex eta,
                               float64& outParallel,
                               float64& outPerpendicular) noexcept
{
    const Vector3d half = Normalize(incident + outgoing);
    OpticsComplex perpendicular;
    OpticsComplex parallel;
    ComputeOpticsFresnel(math::Dot(incident, half), eta, perpendicular, parallel);
    outParallel = SquaredMagnitude(parallel);
    outPerpendicular = SquaredMagnitude(perpendicular);
}

OpticsSymmetric ComputeShapeDeformation(const OpticsSymmetric& hessian, float64 centerIntensity) noexcept
{
    const float64 mean = .5 * (hessian.Xx + hessian.Yy);
    const float64 radius =
        math::Sqrt(.25 * (hessian.Xx - hessian.Yy) * (hessian.Xx - hessian.Yy) + hessian.Xy * hessian.Xy);
    const float64 first = mean + radius;
    const float64 second = mean - radius;
    const float64 largest = math::Max(math::Abs(first), math::Abs(second));
    const float64 resolvable = HESSIAN_RESOLUTION * math::Abs(centerIntensity) / (HESSIAN_STEP * HESSIAN_STEP);
    if (largest <= resolvable)
    {
        return {1, 0, 1};
    }
    const float64 smallest = math::Max(largest / MAXIMUM_DEFORMATION_ANISOTROPY, resolvable);
    if (first * second < 0 && math::Min(math::Abs(first), math::Abs(second)) > smallest)
    {
        return {1, 0, 1};
    }
    const float64 firstMagnitude = math::Max(math::Abs(first), smallest);
    const float64 secondMagnitude = math::Max(math::Abs(second), smallest);
    const float64 firstScale = math::Sqrt(secondMagnitude / firstMagnitude);
    const float64 secondScale = math::Sqrt(firstMagnitude / secondMagnitude);
    const float64 angle = .5 * math::Atan2(2 * hessian.Xy, hessian.Xx - hessian.Yy);
    const float64 cosine = math::Cos(angle);
    const float64 sine = math::Sin(angle);
    return {firstScale * cosine * cosine + secondScale * sine * sine,
            (firstScale - secondScale) * cosine * sine,
            firstScale * sine * sine + secondScale * cosine * cosine};
}

float64 EvaluateGratingDiffraction(const OpticsCase& opticsCase,
                                   float64 halfX,
                                   float64 halfY,
                                   const SurfaceCoherence& coherence,
                                   float64 wavelengthOverPeriod,
                                   float64 phaseAmplitude) noexcept
{
    // The grating vector is the surface frame's bitangent, as in shade.slang.
    const int32 maximumOrder = static_cast<int32>(math::Min(opticsCase.GratingMaximumOrder, GRATING_MAXIMUM_ORDER));
    float64 result = 0;
    for (int32 order = -maximumOrder; order <= maximumOrder; ++order)
    {
        const float64 bessel = EvaluateOpticsBessel(static_cast<uint32>(order < 0 ? -order : order), phaseAmplitude);
        float64 first = 0;
        float64 second = 0;
        RotateIntoBasis(coherence, halfX, halfY - static_cast<float64>(order) * wavelengthOverPeriod, first, second);
        result += bessel * bessel * EvaluateBivariateNormal(first, second, coherence.Inverse);
    }
    return result;
}
} // namespace

void ComputeOpticsFresnel(float64 cosine,
                          OpticsComplex eta,
                          OpticsComplex& outPerpendicular,
                          OpticsComplex& outParallel) noexcept
{
    const float64 clampedCosine = math::Clamp(cosine, 0.0, 1.0);
    const OpticsComplex incidentCosine{clampedCosine, 0};
    const OpticsComplex sineOverEtaSquared = Divide({1 - clampedCosine * clampedCosine, 0}, Multiply(eta, eta));
    const OpticsComplex transmittedCosine = SquareRoot(Subtract({1, 0}, sineOverEtaSquared));
    const OpticsComplex etaTransmittedCosine = Multiply(eta, transmittedCosine);
    const OpticsComplex etaIncidentCosine = Multiply(eta, incidentCosine);
    outPerpendicular =
        Divide(Subtract(incidentCosine, etaTransmittedCosine), Add(incidentCosine, etaTransmittedCosine));
    outParallel = Divide(Subtract(etaIncidentCosine, transmittedCosine), Add(etaIncidentCosine, transmittedCosine));
}

float64 ComputeOpticsUnpolarizedReflectance(float64 cosine, OpticsComplex eta) noexcept
{
    OpticsComplex perpendicular;
    OpticsComplex parallel;
    ComputeOpticsFresnel(cosine, eta, perpendicular, parallel);
    return .5 * (SquaredMagnitude(perpendicular) + SquaredMagnitude(parallel));
}

float64 ApplyOpticsPolarizer(const OpticsStokes& stokes, float64 angle) noexcept
{
    return math::Max(.5 * (stokes.S0 + math::Cos(2 * angle) * stokes.S1 + math::Sin(2 * angle) * stokes.S2), 0.0);
}

float64 EvaluateOpticsBessel(uint32 order, float64 x) noexcept
{
    if (x < BESSEL_SMALL_ARGUMENT)
    {
        return order == 0 ? 1 - .25 * x * x : (order == 1 ? .5 * x : 0);
    }
    if (x > BESSEL_ASYMPTOTIC_ARGUMENT)
    {
        return math::Sqrt(2 / (math::kPi * x)) *
               math::Cos(x - .5 * math::kPi * static_cast<float64>(order) - .25 * math::kPi);
    }
    const float64 largest = math::Max(static_cast<float64>(order), x);
    const uint32 start = 2 * ((static_cast<uint32>(largest + math::Sqrt(BESSEL_MILLER_ACCURACY * largest)) + 2) / 2);
    const float64 twoOverArgument = 2 / x;
    float64 higher = 0;
    float64 current = 1;
    float64 evenSum = 0;
    float64 result = 0;
    for (uint32 n = start; n > 0; --n)
    {
        const float64 lower = static_cast<float64>(n) * twoOverArgument * current - higher;
        higher = current;
        current = lower;
        if (math::Abs(current) > BESSEL_RESCALE_THRESHOLD)
        {
            current *= BESSEL_RESCALE_FACTOR;
            higher *= BESSEL_RESCALE_FACTOR;
            result *= BESSEL_RESCALE_FACTOR;
            evenSum *= BESSEL_RESCALE_FACTOR;
        }
        const uint32 lowerOrder = n - 1;
        if (lowerOrder == order)
        {
            result = current;
        }
        if (lowerOrder > 0 && (lowerOrder & 1U) == 0)
        {
            evenSum += current;
        }
    }
    return result / (current + 2 * evenSum);
}

OpticsBeam
MakeOpticsNaturalBeam(Vector3d propagationWorld, float64 radiance, float64 shape, float64 distanceWorld) noexcept
{
    OpticsBeam result;
    result.PropagationWorld = propagationWorld;
    result.TransverseXWorld = MakePerpendicular(propagationWorld);
    result.Stokes = {radiance, 0, 0, 0};
    result.ShapeX = {shape, 0, shape};
    result.ShapeY = result.ShapeX;
    result.DistanceWorld = distanceWorld;
    return result;
}

float64 ComputeOpticsRadiatorShape(float64 radiatorAreaWorld, float64 beamSolidAngle) noexcept
{
    return 2 * math::kPi * beamSolidAngle / radiatorAreaWorld;
}

float64 ComputeOpticsSunShape(float64 sunCoherenceWorld) noexcept
{
    const float64 ratio =
        ComputeWavenumber(REFERENCE_WAVELENGTH_NANOMETERS) * sunCoherenceWorld / OPTICS_SUN_DISTANCE_WORLD;
    return ratio * ratio;
}

float64 ComputeOpticsCoherenceLength(const OpticsBeam& beam, float64 wavelengthNanometers) noexcept
{
    const float64 smallest = math::Min(ComputeSmallerEigenvalue(beam.ShapeX), ComputeSmallerEigenvalue(beam.ShapeY));
    return beam.DistanceWorld * math::Sqrt(math::Max(smallest, 0.0)) / ComputeWavenumber(wavelengthNanometers);
}

float64 ComputeOpticsDeterminant(const OpticsSymmetric& matrix) noexcept
{
    return matrix.Xx * matrix.Yy - matrix.Xy * matrix.Xy;
}

OpticsResult EvaluateOpticsCase(const OpticsCase& opticsCase) noexcept
{
    OpticsResult result;
    const OpticsBeam& incident = opticsCase.Incident;
    const Vector3d incidentDirection = -incident.PropagationWorld;
    const Vector3d normal = opticsCase.NormalWorld;
    const Vector3d tangent = opticsCase.TangentWorld;
    const Vector3d bitangent = math::Cross(normal, tangent);
    const Vector3d outgoing = opticsCase.OutgoingWorld;
    const float64 wavenumber = ComputeWavenumber(opticsCase.WavelengthNanometers);
    const float64 incidentCosine = math::Dot(incidentDirection, normal);
    const float64 outgoingCosine = math::Dot(outgoing, normal);
    const float64 phase = wavenumber * incidentCosine * opticsCase.RmsHeightWorld;
    result.DirectFraction = math::Exp(-phase * phase);
    result.CoherenceLengthWorld = ComputeOpticsCoherenceLength(incident, opticsCase.WavelengthNanometers);
    const Vector3d sum = incidentDirection + outgoing;
    const float64 halfX = math::Dot(sum, tangent);
    const float64 halfY = math::Dot(sum, bitangent);
    const OpticsSymmetric envelope{opticsCase.EnvelopeVariance, 0, opticsCase.EnvelopeVariance};
    result.EnvelopeDensity = EvaluateBivariateNormal(halfX, halfY, envelope) * math::Max(incidentCosine, 0.0);

    // Interaction (shade.slang's MakeInteraction, without the scene).
    bool hasFresnel = true;
    bool hasShapeDeformation = false;
    float64 weight = opticsCase.Weight;
    if (opticsCase.Lobe == OpticsLobe::Scattered || opticsCase.Lobe == OpticsLobe::Grating)
    {
        SurfaceCoherence coherenceX;
        SurfaceCoherence coherenceY;
        SurfaceCoherence coherenceHalf;
        if (opticsCase.IsWave == true)
        {
            const float64 longitudinal = opticsCase.LongitudinalCoherenceWorld;
            coherenceX = ComputeSurfaceCoherence(incident,
                                                 incident.ShapeX,
                                                 normal,
                                                 tangent,
                                                 bitangent,
                                                 wavenumber,
                                                 longitudinal);
            coherenceY = ComputeSurfaceCoherence(incident,
                                                 incident.ShapeY,
                                                 normal,
                                                 tangent,
                                                 bitangent,
                                                 wavenumber,
                                                 longitudinal);
            coherenceHalf = ComputeSurfaceCoherence(incident,
                                                    Scale(Sum(incident.ShapeX, incident.ShapeY), .5),
                                                    normal,
                                                    tangent,
                                                    bitangent,
                                                    wavenumber,
                                                    longitudinal);
        }
        if (opticsCase.Lobe == OpticsLobe::Grating)
        {
            const float64 wavelengthOverPeriod =
                opticsCase.WavelengthNanometers / (opticsCase.GratingPeriodWorld * NANOMETERS_PER_WORLD_UNIT);
            const float64 cosineProduct =
                math::Max(incidentCosine, MINIMUM_COSINE) * math::Max(outgoingCosine, MINIMUM_COSINE);
            const float64 phaseAmplitude = wavenumber * opticsCase.GratingHeightWorld / (2 * cosineProduct);
            result.DiffractionX =
                EvaluateGratingDiffraction(opticsCase, halfX, halfY, coherenceX, wavelengthOverPeriod, phaseAmplitude);
            result.DiffractionY =
                EvaluateGratingDiffraction(opticsCase, halfX, halfY, coherenceY, wavelengthOverPeriod, phaseAmplitude);
            result.DiffractionHalf = EvaluateGratingDiffraction(opticsCase,
                                                                halfX,
                                                                halfY,
                                                                coherenceHalf,
                                                                wavelengthOverPeriod,
                                                                phaseAmplitude);
        }
        else
        {
            const float64 correlation = wavenumber * opticsCase.CorrelationLengthWorld;
            const float64 inverseCorrelationSquared = 1 / (correlation * correlation);
            const OpticsSymmetric correlationMatrix{inverseCorrelationSquared, 0, inverseCorrelationSquared};
            const auto evaluate = [halfX, halfY, &correlationMatrix](const SurfaceCoherence& coherence) {
                float64 first = 0;
                float64 second = 0;
                RotateIntoBasis(coherence, halfX, halfY, first, second);
                return EvaluateBivariateNormal(first, second, Sum(coherence.Inverse, correlationMatrix));
            };
            result.DiffractionX = evaluate(coherenceX);
            result.DiffractionY = evaluate(coherenceY);
            result.DiffractionHalf = evaluate(coherenceHalf);
            weight *= 1 - result.DirectFraction;
            hasShapeDeformation = opticsCase.IsWave;
        }
    }
    else if (opticsCase.Lobe == OpticsLobe::Diffuse)
    {
        hasFresnel = false;
    }
    else
    {
        weight *= result.DirectFraction;
    }

    // ScatterBeam.
    Vector3d perpendicular = math::Cross(incident.PropagationWorld, outgoing);
    if (math::LengthSquared(perpendicular) < MINIMUM_AXIS_LENGTH_SQUARED)
    {
        perpendicular = GetTransverseY(incident);
    }
    perpendicular = Normalize(perpendicular);
    const Vector3d incidentParallel = math::Cross(perpendicular, incident.PropagationWorld);
    const Vector3d outgoingParallel = math::Cross(perpendicular, outgoing);
    const float64 rotation = math::Atan2(math::Dot(incidentParallel, GetTransverseY(incident)),
                                         math::Dot(incidentParallel, incident.TransverseXWorld));
    const float64 irradianceX = .5 * (incident.Stokes.S0 + incident.Stokes.S1);
    const float64 irradianceY = .5 * (incident.Stokes.S0 - incident.Stokes.S1);
    const OpticsStokes diffracted =
        RotateStokesFrame({irradianceX * result.DiffractionX + irradianceY * result.DiffractionY,
                           irradianceX * result.DiffractionX - irradianceY * result.DiffractionY,
                           incident.Stokes.S2 * result.DiffractionHalf,
                           incident.Stokes.S3 * result.DiffractionHalf},
                          rotation);
    OpticsBeam& scattered = result.Scattered;
    scattered.PropagationWorld = outgoing;
    scattered.TransverseXWorld = outgoingParallel;
    scattered.DistanceWorld = incident.DistanceWorld;
    if (hasFresnel == true)
    {
        const Vector3d half = Normalize(incidentDirection + outgoing);
        OpticsComplex fresnelPerpendicular;
        OpticsComplex fresnelParallel;
        ComputeOpticsFresnel(math::Dot(incidentDirection, half), opticsCase.Eta, fresnelPerpendicular, fresnelParallel);
        scattered.Stokes = ApplyFresnelMueller(fresnelPerpendicular, fresnelParallel, diffracted);
    }
    else
    {
        scattered.Stokes = diffracted;
    }
    scattered.Stokes = {scattered.Stokes.S0 * weight,
                        scattered.Stokes.S1 * weight,
                        scattered.Stokes.S2 * weight,
                        scattered.Stokes.S3 * weight};
    // EnforceRealizableStokes: Eq. (9).
    const float64 intensity = math::Max(scattered.Stokes.S0, 0.0);
    const float64 polarized =
        math::Sqrt(scattered.Stokes.S1 * scattered.Stokes.S1 + scattered.Stokes.S2 * scattered.Stokes.S2 +
                   scattered.Stokes.S3 * scattered.Stokes.S3);
    const float64 shortening = polarized > intensity ? intensity / polarized : 1;
    scattered.Stokes = {intensity,
                        scattered.Stokes.S1 * shortening,
                        scattered.Stokes.S2 * shortening,
                        scattered.Stokes.S3 * shortening};
    const float64 cosineSquared = math::Cos(rotation) * math::Cos(rotation);
    const float64 sineSquared = 1 - cosineSquared;
    OpticsSymmetric shapeX =
        Rotate(Sum(Scale(incident.ShapeX, cosineSquared), Scale(incident.ShapeY, sineSquared)), rotation);
    OpticsSymmetric shapeY =
        Rotate(Sum(Scale(incident.ShapeX, sineSquared), Scale(incident.ShapeY, cosineSquared)), rotation);
    if (hasShapeDeformation == true)
    {
        const Vector3d firstAxis = outgoingParallel * HESSIAN_STEP;
        const Vector3d secondAxis = perpendicular * HESSIAN_STEP;
        const foundation::StaticArray<Vector3d, HESSIAN_DIRECTION_COUNT> directions{{
            outgoing,
            Normalize(outgoing + firstAxis),
            Normalize(outgoing - firstAxis),
            Normalize(outgoing + secondAxis),
            Normalize(outgoing - secondAxis),
            Normalize(outgoing + firstAxis + secondAxis),
        }};
        HessianValues parallelValues{};
        HessianValues perpendicularValues{};
        for (foundation::usize direction = 0; direction < HESSIAN_DIRECTION_COUNT; ++direction)
        {
            ComputeFresnelIntensities(incidentDirection,
                                      directions[direction],
                                      opticsCase.Eta,
                                      parallelValues[direction],
                                      perpendicularValues[direction]);
        }
        const float64 stepSquared = HESSIAN_STEP * HESSIAN_STEP;
        const auto makeHessian = [stepSquared](const HessianValues& values) {
            return OpticsSymmetric{(values[1] - 2 * values[0] + values[2]) / stepSquared,
                                   (values[5] - values[1] - values[3] + values[0]) / stepSquared,
                                   (values[3] - 2 * values[0] + values[4]) / stepSquared};
        };
        shapeX = Transform(shapeX, ComputeShapeDeformation(makeHessian(parallelValues), parallelValues[0]));
        shapeY = Transform(shapeY, ComputeShapeDeformation(makeHessian(perpendicularValues), perpendicularValues[0]));
    }
    scattered.ShapeX = shapeX;
    scattered.ShapeY = shapeY;
    return result;
}

float64 EvaluateOpticsScatteredRadiance(const OpticsCase& opticsCase) noexcept
{
    return EvaluateOpticsCase(opticsCase).Scattered.Stokes.S0;
}
} // namespace ludus::graphics::renderer::testing
