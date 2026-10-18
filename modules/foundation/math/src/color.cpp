#include <ludus/foundation/math/scalar.hpp>

#include <cmath>
#include <limits>

// Thanks to W3C, "CSS Color Module Level 4", section 10.2 (sRGB color space),
// https://www.w3.org/TR/css-color-4/#predefined-sRGB: adopt the piecewise transfer,
// restricted here to finite [0,1] channels rather than extended negative/HDR RGB.
namespace ludus::foundation::math
{
float32 NextFloatDown(float32 value) noexcept
{
    return std::nextafter(value, -std::numeric_limits<float32>::infinity());
}
float32 DecodeSrgb(float32 value) noexcept
{
    if (IsFinite(value) == false || value < 0 || value > 1)
    {
        return std::numeric_limits<float32>::quiet_NaN();
    }
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}
float32 EncodeSrgb(float32 value) noexcept
{
    if (IsFinite(value) == false || value < 0 || value > 1)
    {
        return std::numeric_limits<float32>::quiet_NaN();
    }
    return value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}
} // namespace ludus::foundation::math
