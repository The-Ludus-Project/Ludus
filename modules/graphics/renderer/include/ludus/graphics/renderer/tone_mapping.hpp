#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer
{
/// Display curve applied to exposed scene-linear colour before sRGB encoding. The numeric values are the
/// ones renderer_output.slang reads, so they are part of that shader's contract.
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

/// Apply a display curve to scene-linear colour that already includes exposure; the result is display-linear.
[[nodiscard]] inline ludus::foundation::math::Vector3 ApplyToneCurve(ToneCurve curve,
                                                                     ludus::foundation::math::Vector3 color) noexcept
{
    return curve == ToneCurve::PbrNeutral ? ApplyPbrNeutral(color) : ApplyReinhard(color);
}
} // namespace ludus::graphics::renderer
