#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer
{
/// Display curve applied to exposed scene-linear colour before sRGB encoding. The numeric values are the
/// ones renderer_output.lusl reads, so they are part of that shader's contract.
enum class ToneCurve : ludus::foundation::uint8
{
    /// Per-channel x / (1 + x). Simple and monotone, but shifts hue and desaturates bright saturated colours.
    Reinhard = 1,
    /// Khronos PBR Neutral: keeps hue and saturation, leaves colours below its compression start (0.76)
    /// almost untouched, and compresses highlights toward white.
    PbrNeutral = 2,
};
/// Constants of the Khronos PBR Neutral tone mapper (https://github.com/KhronosGroup/ToneMapping).
inline constexpr ludus::foundation::float32 PBR_NEUTRAL_FRESNEL_AT_NORMAL = 0.04F;
/// Value at which compression of the brightest channel starts.
inline constexpr ludus::foundation::float32 PBR_NEUTRAL_COMPRESSION_START = 0.8F - PBR_NEUTRAL_FRESNEL_AT_NORMAL;
/// Strength of the desaturation of compressed highlights toward white.
inline constexpr ludus::foundation::float32 PBR_NEUTRAL_DESATURATION = 0.15F;
/// Largest display-linear value InvertPbrNeutral reproduces; it keeps the inverse finite.
inline constexpr ludus::foundation::float32 PBR_NEUTRAL_INVERSE_LIMIT = 0.995F;

namespace detail
{
/// Offset the neutral curve subtracts from every channel, given the smallest channel.
[[nodiscard]] inline ludus::foundation::float32 GetPbrNeutralOffset(ludus::foundation::float32 smallest) noexcept
{
    constexpr ludus::foundation::float32 TOE_END = 2 * PBR_NEUTRAL_FRESNEL_AT_NORMAL;
    constexpr ludus::foundation::float32 TOE_DIVISOR = 4 * PBR_NEUTRAL_FRESNEL_AT_NORMAL;
    return smallest <= TOE_END ? smallest - smallest * smallest / TOE_DIVISOR : PBR_NEUTRAL_FRESNEL_AT_NORMAL;
}
} // namespace detail

/// Khronos PBR Neutral applied to scene-linear colour. Negative channels count as zero; a non-finite channel
/// maps to one, as the Reinhard path does. The result is display-linear and may reach but not exceed one.
[[nodiscard]] inline ludus::foundation::math::Vector3 ApplyPbrNeutral(ludus::foundation::math::Vector3 color) noexcept
{
    namespace math = ludus::foundation::math;
    if (math::IsFinite(color.X) == false || math::IsFinite(color.Y) == false || math::IsFinite(color.Z) == false)
    {
        return {1, 1, 1};
    }
    color = {math::Max(color.X, 0.0F), math::Max(color.Y, 0.0F), math::Max(color.Z, 0.0F)};
    const ludus::foundation::float32 offset =
        detail::GetPbrNeutralOffset(math::Min(color.X, math::Min(color.Y, color.Z)));
    color = color - math::Vector3{offset, offset, offset};
    const ludus::foundation::float32 peak = math::Max(color.X, math::Max(color.Y, color.Z));
    if (peak <= PBR_NEUTRAL_COMPRESSION_START)
    {
        return color;
    }
    constexpr ludus::foundation::float32 HEADROOM =
        (1 - PBR_NEUTRAL_COMPRESSION_START) * (1 - PBR_NEUTRAL_COMPRESSION_START);
    const ludus::foundation::float32 newPeak = 1 - HEADROOM / (peak + 1 - 2 * PBR_NEUTRAL_COMPRESSION_START);
    const ludus::foundation::float32 mix = 1 / (PBR_NEUTRAL_DESATURATION * (peak - newPeak) + 1);
    return color * (newPeak / peak) * mix + math::Vector3{newPeak, newPeak, newPeak} * (1 - mix);
}

/// The scene-linear colour that ApplyPbrNeutral maps to a wanted display-linear colour, so that diagnostic
/// colours authored for the screen appear unchanged after the viewport's presentation. Channels are clamped
/// to [0, PBR_NEUTRAL_INVERSE_LIMIT]; saturated colours whose peak is high cannot all be reached and clamp to
/// the nearest representable colour.
[[nodiscard]] inline ludus::foundation::math::Vector3
InvertPbrNeutral(ludus::foundation::math::Vector3 display) noexcept
{
    namespace math = ludus::foundation::math;
    constexpr ludus::foundation::float32 LIMIT = PBR_NEUTRAL_INVERSE_LIMIT;
    display = {math::Clamp(display.X, 0.0F, LIMIT),
               math::Clamp(display.Y, 0.0F, LIMIT),
               math::Clamp(display.Z, 0.0F, LIMIT)};
    const ludus::foundation::float32 displayPeak = math::Max(display.X, math::Max(display.Y, display.Z));
    math::Vector3 offsetColor = display;
    if (displayPeak > PBR_NEUTRAL_COMPRESSION_START)
    {
        // The compressed peak equals the new peak, which fixes the old peak, the desaturation mix and the scale.
        constexpr ludus::foundation::float32 HEADROOM =
            (1 - PBR_NEUTRAL_COMPRESSION_START) * (1 - PBR_NEUTRAL_COMPRESSION_START);
        const ludus::foundation::float32 peak = HEADROOM / (1 - displayPeak) - 1 + 2 * PBR_NEUTRAL_COMPRESSION_START;
        const ludus::foundation::float32 mix = 1 / (PBR_NEUTRAL_DESATURATION * (peak - displayPeak) + 1);
        const ludus::foundation::float32 scale = displayPeak / peak * mix;
        const ludus::foundation::float32 white = displayPeak * (1 - mix);
        offsetColor = {math::Max((display.X - white) / scale, 0.0F),
                       math::Max((display.Y - white) / scale, 0.0F),
                       math::Max((display.Z - white) / scale, 0.0F)};
    }
    // Undo the per-channel offset, which depends only on the smallest channel.
    const ludus::foundation::float32 smallest = math::Min(offsetColor.X, math::Min(offsetColor.Y, offsetColor.Z));
    const ludus::foundation::float32 original = smallest < PBR_NEUTRAL_FRESNEL_AT_NORMAL
                                                    ? 2 * math::Sqrt(PBR_NEUTRAL_FRESNEL_AT_NORMAL * smallest)
                                                    : smallest + PBR_NEUTRAL_FRESNEL_AT_NORMAL;
    const ludus::foundation::float32 offset = detail::GetPbrNeutralOffset(original);
    return offsetColor + math::Vector3{offset, offset, offset};
}

/// Guarded per-channel Reinhard: negative channels count as zero, a non-finite channel maps to one.
[[nodiscard]] inline ludus::foundation::math::Vector3 ApplyReinhard(ludus::foundation::math::Vector3 color) noexcept
{
    namespace math = ludus::foundation::math;
    const auto map = [](ludus::foundation::float32 value) {
        const ludus::foundation::float32 positive = math::Max(value, 0.0F);
        return math::IsFinite(positive) == true ? positive / (1 + positive) : 1.0F;
    };
    return {map(color.X), map(color.Y), map(color.Z)};
}

/// How a clustered deferred ToneMap chooses its linear exposure multiplier.
enum class ExposureMode : ludus::foundation::uint8
{
    /// The graph's authored ToneMap multiplier, so that one scene-linear nit is near display white.
    Graph,
    /// A camera exposure value at ISO 100 (ViewDescription::ExposureValue100) for scenes in physical units.
    Physical,
};
/// Last ExposureMode enumerator.
inline constexpr ludus::foundation::uint32 EXPOSURE_MODE_LAST =
    static_cast<ludus::foundation::uint32>(ExposureMode::Physical);
/// Darkest admitted exposure value at ISO 100, below a moonless overcast night.
inline constexpr ludus::foundation::float32 MIN_EXPOSURE_VALUE_100 = -16;
/// Brightest admitted exposure value at ISO 100, above sunlit snow and the solar disk's surroundings.
inline constexpr ludus::foundation::float32 MAX_EXPOSURE_VALUE_100 = 24;
/// ISO arithmetic speed that defines EV100.
inline constexpr ludus::foundation::float64 EXPOSURE_REFERENCE_SPEED = 100;
/// Saturation-based sensitivity constant of ISO 12232 (78) over the lens and vignetting attenuation q = 0.65.
inline constexpr ludus::foundation::float64 SATURATION_EXPOSURE_FACTOR = 78 / (0.65 * EXPOSURE_REFERENCE_SPEED);
/// Incident-light meter calibration constant for a flat receptor, in lux. ISO 2720:1974 recommends 240 to 400.
inline constexpr ludus::foundation::float64 INCIDENT_METER_CALIBRATION_LUX = 250;
/// Illuminance of a moonless clear night sky with airglow, in lux. The incident meter adds it to every reading,
/// so the darkest metered exposure stays finite and matches a natural night rather than an empty sky.
inline constexpr ludus::foundation::float64 NATURAL_NIGHT_SKY_ILLUMINANCE_LUX = 0.002;

/// @brief The linear ToneMap multiplier that maps a camera exposure value at ISO 100 to display.
///
/// The saturation-based photometric exposure 1 / (1.2 * 2^EV100) maps the largest luminance the camera records
/// without clipping to one, after the camera exposure discussion and its convertEV100ToExposure listing in Sébastien
/// Lagarde and Charles de Rousiers, "Moving Frostbite to Physically Based Rendering", SIGGRAPH 2014 course notes v3,
/// https://seblagarde.wordpress.com/wp-content/uploads/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf.
/// An exposure value of -log2(1.2), about -0.26, reproduces the graph's default multiplier of one.
/// @param exposureValue100 Finite, within [MIN_EXPOSURE_VALUE_100, MAX_EXPOSURE_VALUE_100]; other values return zero.
/// @return The positive multiplier, or zero for an invalid exposure value.
[[nodiscard]] inline ludus::foundation::float32
ComputeExposureMultiplier(ludus::foundation::float32 exposureValue100) noexcept
{
    namespace math = ludus::foundation::math;
    if (math::IsFinite(exposureValue100) == false || exposureValue100 < MIN_EXPOSURE_VALUE_100 ||
        exposureValue100 > MAX_EXPOSURE_VALUE_100)
    {
        return 0;
    }
    return static_cast<ludus::foundation::float32>(
        1 / (SATURATION_EXPOSURE_FACTOR * math::Pow(2, static_cast<ludus::foundation::float64>(exposureValue100))));
}

/// @brief The exposure value at ISO 100 that an incident-light meter reads under an illuminance.
///
/// Solves the incident-light exposure equation N^2 / t = E * S / C for EV = log2(N^2 / t) with S = 100 and
/// C = INCIDENT_METER_CALIBRATION_LUX, after the incident-light calibration in Wikipedia's "Light meter" article
/// (https://en.wikipedia.org/wiki/Light_meter). NATURAL_NIGHT_SKY_ILLUMINANCE_LUX
/// (Wikipedia, "Lux", illuminance examples) is added to the reading. Clear midday sun reads about EV100 15, a
/// full Moon about -3, and a moonless night about -10. The result is clamped to the admitted exposure range.
/// @param illuminanceLux Finite and nonnegative illuminance on the meter's flat receptor; other values read as zero.
[[nodiscard]] inline ludus::foundation::float32
ComputeIncidentExposureValue100(ludus::foundation::float64 illuminanceLux) noexcept
{
    namespace math = ludus::foundation::math;
    const ludus::foundation::float64 reading =
        (math::IsFinite(illuminanceLux) == true ? math::Max(illuminanceLux, 0.0) : 0.0) +
        NATURAL_NIGHT_SKY_ILLUMINANCE_LUX;
    const ludus::foundation::float64 exposureValue =
        math::Log(reading * EXPOSURE_REFERENCE_SPEED / INCIDENT_METER_CALIBRATION_LUX) / math::Log(2.0);
    return static_cast<ludus::foundation::float32>(
        math::Clamp(exposureValue,
                    static_cast<ludus::foundation::float64>(MIN_EXPOSURE_VALUE_100),
                    static_cast<ludus::foundation::float64>(MAX_EXPOSURE_VALUE_100)));
}

/// Apply a display curve to scene-linear colour that already includes exposure; the result is display-linear.
[[nodiscard]] inline ludus::foundation::math::Vector3 ApplyToneCurve(ToneCurve curve,
                                                                     ludus::foundation::math::Vector3 color) noexcept
{
    return curve == ToneCurve::PbrNeutral ? ApplyPbrNeutral(color) : ApplyReinhard(color);
}
} // namespace ludus::graphics::renderer
