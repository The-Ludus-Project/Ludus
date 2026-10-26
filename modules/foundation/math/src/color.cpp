#include <ludus/foundation/math/scalar.hpp>

#include <cmath>
#include <limits>

// Thanks to W3C, "CSS Color Module Level 4", section 10.2 (sRGB color space),
// https://www.w3.org/TR/css-color-4/#predefined-sRGB: adopt the piecewise transfer,
// restricted here to finite [0,1] channels rather than extended negative/HDR RGB.
namespace ludus::foundation::math
{
namespace
{
constexpr float32 SRGB_LINEAR_THRESHOLD = .04045F;
constexpr float32 SRGB_LINEAR_SCALE = 12.92F;
constexpr float32 SRGB_POWER_OFFSET = .055F;
constexpr float32 SRGB_POWER_SCALE = 1.055F;
constexpr float32 SRGB_GAMMA = 2.4F;
} // namespace
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
    if (value == 0 || value == 1)
    {
        return value;
    }
    const float32 linear = value <= SRGB_LINEAR_THRESHOLD
                               ? value / SRGB_LINEAR_SCALE
                               : std::pow((value + SRGB_POWER_OFFSET) / SRGB_POWER_SCALE, SRGB_GAMMA);
    return Clamp(linear, 0.0F, 1.0F);
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
