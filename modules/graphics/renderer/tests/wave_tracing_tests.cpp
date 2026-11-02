#include <ludus/graphics/renderer/wave_tracing.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
using namespace ludus::graphics::renderer;
namespace math = ludus::foundation::math;

float64 ComputePower(WaveField field)
{
    return field.Real * field.Real + field.Imaginary * field.Imaginary;
}

Array<float64> MakeTransmission(uint32 resolution, float64 value)
{
    Array<float64> result;
    REQUIRE(result.TryResize(usize{resolution} * resolution, value) == true);
    return result;
}

WaveField
Evaluate(const WaveAperture& aperture, const Array<float64>& transmission, math::Vector3d receiverApertureWorld)
{
    WaveField field;
    REQUIRE(TryEvaluateWaveAperture(aperture,
                                    {transmission.GetData(), transmission.GetSize()},
                                    receiverApertureWorld,
                                    field) == true);
    return field;
}
} // namespace

TEST_CASE("Gaussian beam propagation retains anisotropic variance and induced correlation", "[renderer][wave-tracing]")
{
    const WaveBeam beam{0.02, 0.01, 0.04, 0};
    WaveBeam propagated;
    WaveMoments moments;
    REQUIRE(TryPropagateWaveBeam(beam, 2, propagated, moments) == true);
    CHECK(math::Abs(propagated.CorrelationWorldSquared - 0.04 / math::kTau) < 1e-15);
    CHECK(moments.VarianceXWorldSquared > 0.005);
    CHECK(moments.VarianceYWorldSquared > 0.02);
    CHECK(moments.CovarianceX > moments.CovarianceY);
    WaveBeam composed;
    WaveMoments composedMoments;
    REQUIRE(TryPropagateWaveBeam(propagated, 3, composed, composedMoments) == true);
    WaveBeam direct;
    WaveMoments directMoments;
    REQUIRE(TryPropagateWaveBeam(beam, 5, direct, directMoments) == true);
    CHECK(math::Abs(composed.CorrelationWorldSquared - direct.CorrelationWorldSquared) < 1e-15);
    CHECK(math::Abs(composedMoments.VarianceXWorldSquared - directMoments.VarianceXWorldSquared) < 1e-15);
    // Eq. (21): det of each position/wavevector covariance stays at the minimum uncertainty, 1/4.
    CHECK(math::Abs(moments.VarianceXWorldSquared / (2 * beam.SigmaXWorldSquared) -
                    moments.CovarianceX * moments.CovarianceX - 0.25) < 1e-14);
    const WaveBeam saved = propagated;
    const WaveMoments savedMoments = moments;
    CHECK(TryPropagateWaveBeam(beam, -1, propagated, moments) == false);
    CHECK(propagated.CorrelationWorldSquared == saved.CorrelationWorldSquared);
    CHECK(moments.VarianceXWorldSquared == savedMoments.VarianceXWorldSquared);
}

TEST_CASE("Unobstructed scalar diffraction agrees with analytic Gaussian free propagation", "[renderer][wave-tracing]")
{
    // Waist amplitude exp(-x^2/(2 SigmaX)-y^2/(2 SigmaY)), propagated with the Fresnel kernel.
    const WaveAperture aperture{0.02, 0.01, 0.04, 0.5, 1, 128};
    const Array<float64> transmission = MakeTransmission(aperture.Resolution, 1);
    const math::Vector3d receiverApertureWorld{0.03, -0.02, 10};
    const WaveField field = Evaluate(aperture, transmission, receiverApertureWorld);
    const float64 rho = receiverApertureWorld.Z * aperture.WavelengthWorld / math::kTau;
    const float64 widthX = aperture.SigmaXWorldSquared + rho * rho / aperture.SigmaXWorldSquared;
    const float64 widthY = aperture.SigmaYWorldSquared + rho * rho / aperture.SigmaYWorldSquared;
    const float64 expected = math::Sqrt(aperture.SigmaXWorldSquared * aperture.SigmaYWorldSquared / (widthX * widthY)) *
                             math::Exp(-receiverApertureWorld.X * receiverApertureWorld.X / widthX -
                                       receiverApertureWorld.Y * receiverApertureWorld.Y / widthY);
    CHECK(math::Abs(ComputePower(field) - expected) < 2e-6);
}

TEST_CASE("Separated apertures retain constructive and destructive inter-region cross terms",
          "[renderer][wave-tracing]")
{
    const WaveAperture aperture{0.02, 1, 1, 0.2, 0.1, 128};
    Array<float64> left = MakeTransmission(aperture.Resolution, 0);
    Array<float64> right = MakeTransmission(aperture.Resolution, 0);
    Array<float64> both = MakeTransmission(aperture.Resolution, 0);
    for (uint32 y = 0; y < aperture.Resolution; ++y)
    {
        for (uint32 x = 0; x < aperture.Resolution; ++x)
        {
            const float64 positionXWorld =
                (x + 0.5) * (2 * aperture.HalfWidthWorld / aperture.Resolution) - aperture.HalfWidthWorld;
            const usize index = usize{y} * aperture.Resolution + x;
            left[index] = math::Abs(positionXWorld + 0.1) < 0.015 ? 1 : 0;
            right[index] = math::Abs(positionXWorld - 0.1) < 0.015 ? 1 : 0;
            both[index] = left[index] + right[index];
        }
    }
    const math::Vector3d centre{0, 0, 10};
    const float64 incoherent =
        ComputePower(Evaluate(aperture, left, centre)) + ComputePower(Evaluate(aperture, right, centre));
    CHECK(ComputePower(Evaluate(aperture, both, centre)) > 1.99 * incoherent);
    // First double-slit null, x = lambda*z/(2*separation). Fresnel quadratic phase is symmetric here.
    const math::Vector3d nullApertureWorld{0.5, 0, 10};
    const float64 separate = ComputePower(Evaluate(aperture, left, nullApertureWorld)) +
                             ComputePower(Evaluate(aperture, right, nullApertureWorld));
    CHECK(ComputePower(Evaluate(aperture, both, nullApertureWorld)) < separate * 0.01);
    CHECK(ComputePower(Evaluate(aperture, MakeTransmission(aperture.Resolution, 0), centre)) == 0);
}

TEST_CASE("Scalar aperture validation preserves outputs and explains phase undersampling", "[renderer][wave-tracing]")
{
    WaveAperture aperture{0.02, 0.01, 0.01, 0.3, 0.3, 32};
    Array<float64> transmission = MakeTransmission(aperture.Resolution, 1);
    WaveField field{7, 9};
    CHECK(TryEvaluateWaveAperture(aperture, {nullptr, transmission.GetSize()}, {0, 0, 1}, field) == false);
    CHECK(TryEvaluateWaveAperture(aperture, {transmission.GetData(), transmission.GetSize() - 1}, {0, 0, 1}, field) ==
          false);
    transmission[0] = -1;
    CHECK(TryEvaluateWaveAperture(aperture, {transmission.GetData(), transmission.GetSize()}, {0, 0, 1}, field) ==
          false);
    transmission[0] = 1;
    aperture.WavelengthWorld = 1e-9;
    CHECK(TryEvaluateWaveAperture(aperture, {transmission.GetData(), transmission.GetSize()}, {0, 0, 1}, field) ==
          false);
    CHECK(field.Real == 7);
    CHECK(field.Imaginary == 9);
    CHECK(DescribeRenderGraphFailureReason(static_cast<uint32>(RenderGraphFailureReason::WaveResolution)) != nullptr);
}
