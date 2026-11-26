#include "internal/spectral_sensor.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

// Thanks to Chris Wyman, Peter-Pike Sloan and Peter Shirley, "Simple Analytic Approximations to the CIE XYZ Color
// Matching Functions", JCGT 2(2), 2013, https://jcgt.org/published/0002/02/01/ : EvaluateCieXyz1931 is their
// multi-lobe piecewise-Gaussian fit. This is the CPU twin of shaders/spectral_sensor.slang; both read the fit, the
// sensor normalization and the uplift matrix from spectral_sensor_layout.h.
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
struct CieLobe final
{
    float32 Weight = 0;
    float32 MeanNanometers = 0;
    float32 LowerDeviationNanometers = 1;
    float32 UpperDeviationNanometers = 1;
};
constexpr float32 GAUSSIAN_EXPONENT_SCALE = -0.5F;
constexpr float32 GREEN_BAND_MINIMUM_NANOMETERS = static_cast<float32>(LUDUS_SPECTRUM_GREEN_BAND_MINIMUM_NANOMETERS);
constexpr float32 RED_BAND_MINIMUM_NANOMETERS = static_cast<float32>(LUDUS_SPECTRUM_RED_BAND_MINIMUM_NANOMETERS);
constexpr math::Vector3 SENSOR_SCALE{static_cast<float32>(LUDUS_SPECTRUM_SENSOR_SCALE_RED),
                                     static_cast<float32>(LUDUS_SPECTRUM_SENSOR_SCALE_GREEN),
                                     static_cast<float32>(LUDUS_SPECTRUM_SENSOR_SCALE_BLUE)};
constexpr math::Vector3 UPLIFT_RED_BAND{static_cast<float32>(LUDUS_SPECTRUM_UPLIFT_RED_BAND_RED),
                                        static_cast<float32>(LUDUS_SPECTRUM_UPLIFT_RED_BAND_GREEN),
                                        static_cast<float32>(LUDUS_SPECTRUM_UPLIFT_RED_BAND_BLUE)};
constexpr math::Vector3 UPLIFT_GREEN_BAND{static_cast<float32>(LUDUS_SPECTRUM_UPLIFT_GREEN_BAND_RED),
                                          static_cast<float32>(LUDUS_SPECTRUM_UPLIFT_GREEN_BAND_GREEN),
                                          static_cast<float32>(LUDUS_SPECTRUM_UPLIFT_GREEN_BAND_BLUE)};
constexpr math::Vector3 UPLIFT_BLUE_BAND{static_cast<float32>(LUDUS_SPECTRUM_UPLIFT_BLUE_BAND_RED),
                                         static_cast<float32>(LUDUS_SPECTRUM_UPLIFT_BLUE_BAND_GREEN),
                                         static_cast<float32>(LUDUS_SPECTRUM_UPLIFT_BLUE_BAND_BLUE)};
/// Rows of the CIE XYZ to linear sRGB (D65) matrix.
constexpr math::Vector3 XYZ_TO_RED{static_cast<float32>(LUDUS_XYZ_TO_SRGB_00),
                                   static_cast<float32>(LUDUS_XYZ_TO_SRGB_01),
                                   static_cast<float32>(LUDUS_XYZ_TO_SRGB_02)};
constexpr math::Vector3 XYZ_TO_GREEN{static_cast<float32>(LUDUS_XYZ_TO_SRGB_10),
                                     static_cast<float32>(LUDUS_XYZ_TO_SRGB_11),
                                     static_cast<float32>(LUDUS_XYZ_TO_SRGB_12)};
constexpr math::Vector3 XYZ_TO_BLUE{static_cast<float32>(LUDUS_XYZ_TO_SRGB_20),
                                    static_cast<float32>(LUDUS_XYZ_TO_SRGB_21),
                                    static_cast<float32>(LUDUS_XYZ_TO_SRGB_22)};
/// Lobes of the x, y and z functions.
constexpr usize X_LOBE_COUNT = 3;
constexpr usize Y_LOBE_COUNT = 2;
constexpr usize Z_LOBE_COUNT = 2;
constexpr StaticArray<CieLobe, X_LOBE_COUNT> X_LOBES{{
    {LUDUS_CIE_X_LOBE0_WEIGHT, LUDUS_CIE_X_LOBE0_MEAN, LUDUS_CIE_X_LOBE0_LOWER, LUDUS_CIE_X_LOBE0_UPPER},
    {LUDUS_CIE_X_LOBE1_WEIGHT, LUDUS_CIE_X_LOBE1_MEAN, LUDUS_CIE_X_LOBE1_LOWER, LUDUS_CIE_X_LOBE1_UPPER},
    {LUDUS_CIE_X_LOBE2_WEIGHT, LUDUS_CIE_X_LOBE2_MEAN, LUDUS_CIE_X_LOBE2_LOWER, LUDUS_CIE_X_LOBE2_UPPER},
}};
constexpr StaticArray<CieLobe, Y_LOBE_COUNT> Y_LOBES{{
    {LUDUS_CIE_Y_LOBE0_WEIGHT, LUDUS_CIE_Y_LOBE0_MEAN, LUDUS_CIE_Y_LOBE0_LOWER, LUDUS_CIE_Y_LOBE0_UPPER},
    {LUDUS_CIE_Y_LOBE1_WEIGHT, LUDUS_CIE_Y_LOBE1_MEAN, LUDUS_CIE_Y_LOBE1_LOWER, LUDUS_CIE_Y_LOBE1_UPPER},
}};
constexpr StaticArray<CieLobe, Z_LOBE_COUNT> Z_LOBES{{
    {LUDUS_CIE_Z_LOBE0_WEIGHT, LUDUS_CIE_Z_LOBE0_MEAN, LUDUS_CIE_Z_LOBE0_LOWER, LUDUS_CIE_Z_LOBE0_UPPER},
    {LUDUS_CIE_Z_LOBE1_WEIGHT, LUDUS_CIE_Z_LOBE1_MEAN, LUDUS_CIE_Z_LOBE1_LOWER, LUDUS_CIE_Z_LOBE1_UPPER},
}};
float32 EvaluateLobe(const CieLobe& lobe, float32 wavelengthNanometers) noexcept
{
    const float32 deviation =
        wavelengthNanometers < lobe.MeanNanometers ? lobe.LowerDeviationNanometers : lobe.UpperDeviationNanometers;
    const float32 t = (wavelengthNanometers - lobe.MeanNanometers) / deviation;
    return lobe.Weight * static_cast<float32>(math::Exp(static_cast<float64>(GAUSSIAN_EXPONENT_SCALE * t * t)));
}
template <usize COUNT>
float32 EvaluateLobes(const StaticArray<CieLobe, COUNT>& lobes, float32 wavelengthNanometers) noexcept
{
    float32 sum = 0;
    for (const CieLobe& lobe : lobes)
    {
        sum += EvaluateLobe(lobe, wavelengthNanometers);
    }
    return sum;
}
} // namespace
math::Vector3 EvaluateCieXyz1931(float32 wavelengthNanometers) noexcept
{
    return {EvaluateLobes(X_LOBES, wavelengthNanometers),
            EvaluateLobes(Y_LOBES, wavelengthNanometers),
            EvaluateLobes(Z_LOBES, wavelengthNanometers)};
}
math::Vector3 EvaluateSensorSensitivity(float32 wavelengthNanometers) noexcept
{
    const math::Vector3 xyz = EvaluateCieXyz1931(wavelengthNanometers);
    const math::Vector3 rgb{math::Dot(XYZ_TO_RED, xyz), math::Dot(XYZ_TO_GREEN, xyz), math::Dot(XYZ_TO_BLUE, xyz)};
    return math::Hadamard({math::Max(rgb.X, 0.F), math::Max(rgb.Y, 0.F), math::Max(rgb.Z, 0.F)}, SENSOR_SCALE);
}
float32 EvaluateUpliftedSpectrum(math::Vector3 linearRgb, float32 wavelengthNanometers) noexcept
{
    math::Vector3 band = UPLIFT_BLUE_BAND;
    if (wavelengthNanometers >= RED_BAND_MINIMUM_NANOMETERS)
    {
        band = UPLIFT_RED_BAND;
    }
    else if (wavelengthNanometers >= GREEN_BAND_MINIMUM_NANOMETERS)
    {
        band = UPLIFT_GREEN_BAND;
    }
    return math::Max(math::Dot(band, linearRgb), 0.F);
}
float32 SampleWavelengthNanometers(float32 randomValue) noexcept
{
    return SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS + randomValue * SPECTRUM_WAVELENGTH_RANGE_NANOMETERS;
}
math::Vector3 ConvertSpectralRadiance(float32 spectralRadiance, float32 wavelengthNanometers) noexcept
{
    return EvaluateSensorSensitivity(wavelengthNanometers) * (spectralRadiance * SPECTRUM_WAVELENGTH_RANGE_NANOMETERS);
}
} // namespace ludus::graphics::renderer::internal
