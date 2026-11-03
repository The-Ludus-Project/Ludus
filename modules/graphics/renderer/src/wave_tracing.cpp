#include <ludus/graphics/renderer/wave_tracing.hpp>

#include <ludus/foundation/math/scalar.hpp>

// Thanks to Shlomi Steinberg and Matt Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics",
// Computer Graphics Forum 45(2), e70322, 2026, https://doi.org/10.1111/cgf.70322 : Sections 3.1 and 4.2,
// Eqs. (2), (16), (20), (21). Propagation retains the induced correlation. The aperture specialization
// evaluates the bilinear integral as a squared coherent Fresnel field sum; it uses a single planar mask
// and numerical quadrature instead of the paper's multi-region cone traversal and scattering operators.
namespace ludus::graphics::renderer
{
namespace
{
using foundation::float64;
using foundation::uint32;
namespace math = foundation::math;

bool IsPositiveFinite(float64 value) noexcept
{
    return math::IsFinite(value) == true && value > 0;
}
} // namespace

bool TryPropagateWaveBeam(const WaveBeam& beam,
                          float64 distanceWorld,
                          WaveBeam& outBeam,
                          WaveMoments& outMoments) noexcept
{
    if (IsPositiveFinite(beam.WavelengthWorld) == false || IsPositiveFinite(beam.SigmaXWorldSquared) == false ||
        IsPositiveFinite(beam.SigmaYWorldSquared) == false || math::IsFinite(beam.CorrelationWorldSquared) == false ||
        beam.CorrelationWorldSquared < 0 || math::IsFinite(distanceWorld) == false || distanceWorld < 0)
    {
        return false;
    }
    WaveBeam propagated = beam;
    propagated.CorrelationWorldSquared += distanceWorld * beam.WavelengthWorld / math::kTau;
    const float64 correlation = propagated.CorrelationWorldSquared;
    const WaveMoments moments
    {
        .VarianceXWorldSquared = (beam.SigmaXWorldSquared + correlation * correlation / beam.SigmaXWorldSquared) * 0.5,
        .VarianceYWorldSquared = (beam.SigmaYWorldSquared + correlation * correlation / beam.SigmaYWorldSquared) * 0.5,
        .CovarianceX = correlation / (2 * beam.SigmaXWorldSquared),
        .CovarianceY = correlation / (2 * beam.SigmaYWorldSquared),
    };
    if (math::IsFinite(moments.VarianceXWorldSquared) == false ||
        math::IsFinite(moments.VarianceYWorldSquared) == false || math::IsFinite(moments.CovarianceX) == false ||
        math::IsFinite(moments.CovarianceY) == false)
    {
        return false;
    }
    outBeam = propagated;
    outMoments = moments;
    return true;
}

bool TryEvaluateWaveAperture(const WaveAperture& aperture,
                             foundation::BorrowedView<const float64> transmission,
                             math::Vector3d receiverApertureWorld,
                             WaveField& outField) noexcept
{
    if (transmission.IsValid() == false || IsPositiveFinite(aperture.WavelengthWorld) == false ||
        IsPositiveFinite(aperture.SigmaXWorldSquared) == false ||
        IsPositiveFinite(aperture.SigmaYWorldSquared) == false || IsPositiveFinite(aperture.HalfWidthWorld) == false ||
        IsPositiveFinite(aperture.HalfHeightWorld) == false || aperture.Resolution == 0 ||
        math::IsFinite(receiverApertureWorld) == false || receiverApertureWorld.Z <= 0 ||
        transmission.GetSize() / aperture.Resolution != aperture.Resolution ||
        transmission.GetSize() % aperture.Resolution != 0)
    {
        return false;
    }
    const float64 stepXWorld = 2 * aperture.HalfWidthWorld / aperture.Resolution;
    const float64 stepYWorld = 2 * aperture.HalfHeightWorld / aperture.Resolution;
    const float64 phaseScale = math::kPi / (aperture.WavelengthWorld * receiverApertureWorld.Z);
    // Bound the phase change across a cell on each axis by pi/2. Checking both axes avoids high-frequency
    // Fresnel patterns silently aliasing into plausible low-frequency images. Mask edges still need refinement.
    const float64 phaseStepX =
        phaseScale * stepXWorld * 2 * (aperture.HalfWidthWorld + math::Abs(receiverApertureWorld.X));
    const float64 phaseStepY =
        phaseScale * stepYWorld * 2 * (aperture.HalfHeightWorld + math::Abs(receiverApertureWorld.Y));
    if (math::IsFinite(phaseScale) == false || phaseStepX > math::kPi * 0.5 || phaseStepY > math::kPi * 0.5)
    {
        return false;
    }
    WaveField field;
    const float64 weight = stepXWorld * stepYWorld / (aperture.WavelengthWorld * receiverApertureWorld.Z);
    for (uint32 y = 0; y < aperture.Resolution; ++y)
    {
        const float64 positionYWorld = (static_cast<float64>(y) + 0.5) * stepYWorld - aperture.HalfHeightWorld;
        for (uint32 x = 0; x < aperture.Resolution; ++x)
        {
            const float64 coverage = transmission[static_cast<foundation::usize>(y) * aperture.Resolution + x];
            if (math::IsFinite(coverage) == false || coverage < 0 || coverage > 1)
            {
                return false;
            }
            const float64 positionXWorld = (static_cast<float64>(x) + 0.5) * stepXWorld - aperture.HalfWidthWorld;
            const float64 amplitude = coverage * weight *
                                      math::Exp(-0.5 * (positionXWorld * positionXWorld / aperture.SigmaXWorldSquared +
                                                        positionYWorld * positionYWorld / aperture.SigmaYWorldSquared));
            const float64 offsetXWorld = positionXWorld - receiverApertureWorld.X;
            const float64 offsetYWorld = positionYWorld - receiverApertureWorld.Y;
            const float64 phase = phaseScale * (offsetXWorld * offsetXWorld + offsetYWorld * offsetYWorld);
            // 1/i in the Fresnel kernel. Dropping the common exp(ikz) changes no irradiance.
            field.Real += amplitude * math::Sin(phase);
            field.Imaginary -= amplitude * math::Cos(phase);
        }
    }
    if (math::IsFinite(field.Real) == false || math::IsFinite(field.Imaginary) == false ||
        math::IsFinite(field.Real * field.Real + field.Imaginary * field.Imaginary) == false)
    {
        return false;
    }
    outField = field;
    return true;
}
} // namespace ludus::graphics::renderer
