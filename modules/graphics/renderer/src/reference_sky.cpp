#include "internal/reference_sky.h"
#include "../shaders/renderer_celestial_constants.h"

#include "internal/render_graph_atmosphere.h"

#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/graphics/renderer/reference.hpp>

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace
{
namespace math = foundation::math;

constexpr uint32 RANDOM_WORD_BITS = 32;
constexpr uint16 STAR_BRIGHTNESS_DIMENSION = 3;
constexpr uint32 TRANSMITTANCE_LAST = LUDUS_REFERENCE_SKY_TRANSMITTANCE_SAMPLES - 1;

[[nodiscard]] math::Vector3 GetRgb(math::Vector4 word) noexcept
{
    return {word.X, word.Y, word.Z};
}

[[nodiscard]] math::Vector4 MakeWord(math::Vector3 vector, float32 scalar) noexcept
{
    return {vector.X, vector.Y, vector.Z, scalar};
}

[[nodiscard]] bool IsRadiance(math::Vector3 radiance) noexcept
{
    return math::IsFinite(radiance) == true && radiance.X >= 0 && radiance.Y >= 0 && radiance.Z >= 0;
}

[[nodiscard]] bool IsAngularRadius(float32 radius) noexcept
{
    return math::IsFinite(radius) == true && radius >= 0 && radius < math::kHalfPiF;
}

[[nodiscard]] float32 ComputeChordRadius(float32 angularRadius) noexcept
{
    return 2 * math::Sin(angularRadius * 0.5F);
}

[[nodiscard]] bool TryPackMoon(const CelestialBackgroundDescription& celestial,
                               math::Vector3 sunWorld,
                               ReferenceSkyWords& inoutWords) noexcept
{
    const float32 radius = celestial.LunarAngularRadiusRadians;
    if (IsAngularRadius(radius) == false)
    {
        return false;
    }
    if (radius == 0)
    {
        return true;
    }
    math::Vector3 moonWorld;
    math::Vector3 lunarLightWorld = sunWorld;
    const float32 sine = math::Sin(radius);
    if (sine <= 0 || IsRadiance(celestial.LunarPeakRadiance) == false ||
        math::TryNormalize(celestial.DirectionToMoonWorld, moonWorld) != math::MathStatus::Success ||
        (celestial.DirectionToSunAtMoonWorld != math::Vector3{} &&
         math::TryNormalize(celestial.DirectionToSunAtMoonWorld, lunarLightWorld) != math::MathStatus::Success))
    {
        return false;
    }
    inoutWords[LUDUS_REFERENCE_SKY_MOON_WORLD] = MakeWord(moonWorld, ComputeChordRadius(radius));
    inoutWords[LUDUS_REFERENCE_SKY_MOON_RADIANCE] = MakeWord(celestial.LunarPeakRadiance, sine);
    inoutWords[LUDUS_REFERENCE_SKY_LUNAR_LIGHT_WORLD] = MakeWord(lunarLightWorld, 0);
    return true;
}

[[nodiscard]] bool TryPackNightSky(const CelestialBackgroundDescription& celestial,
                                   ReferenceSkyWords& inoutWords) noexcept
{
    if (math::IsFinite(celestial.StarRadiance) == false || celestial.StarRadiance < 0 ||
        math::IsFinite(celestial.MilkyWayRadiance) == false || celestial.MilkyWayRadiance < 0)
    {
        return false;
    }
    if (celestial.MilkyWayRadiance > 0)
    {
        math::Vector3 northWorld;
        math::Vector3 centerWorld;
        math::Vector3 tangentWorld;
        if (math::TryNormalize(celestial.GalacticNorthWorld, northWorld) != math::MathStatus::Success ||
            math::TryNormalize(celestial.GalacticCenterWorld, centerWorld) != math::MathStatus::Success ||
            math::TryNormalize(math::Cross(northWorld, centerWorld), tangentWorld) != math::MathStatus::Success)
        {
            return false;
        }
        inoutWords[LUDUS_REFERENCE_SKY_GALACTIC_NORTH_WORLD] = MakeWord(northWorld, 1);
        inoutWords[LUDUS_REFERENCE_SKY_GALACTIC_CENTER_WORLD] = MakeWord(math::Cross(tangentWorld, northWorld), 0);
    }
    inoutWords[LUDUS_REFERENCE_SKY_SUN_RADIANCE].W = celestial.StarRadiance;
    uint32 count = 0;
    for (const PlanetBackgroundDescription& planet : celestial.Planets)
    {
        if (IsRadiance(planet.Illuminance) == false)
        {
            return false;
        }
        if (planet.Illuminance == math::Vector3{})
        {
            continue;
        }
        math::Vector3 planetWorld;
        if (math::TryNormalize(planet.DirectionWorld, planetWorld) != math::MathStatus::Success)
        {
            return false;
        }
        const uint32 word = LUDUS_REFERENCE_SKY_PLANETS + count * LUDUS_REFERENCE_SKY_PLANET_WORDS;
        inoutWords[word] = MakeWord(planetWorld, 0);
        inoutWords[word + 1] = MakeWord(planet.Illuminance, 0);
        ++count;
    }
    inoutWords[LUDUS_REFERENCE_SKY_LUNAR_LIGHT_WORLD].W = static_cast<float32>(count);
    return true;
}

/// Center-of-image angular pixel width, used to filter unresolved stars and planets as in raster.
[[nodiscard]] float32 ComputeAngularFootprint(const ReferenceCaptureDescription& description) noexcept
{
    if (description.OrthographicCamera == true)
    {
        return 0;
    }
    const math::Matrix4& worldToClip = description.WorldToClip;
    const math::Vector3 horizontalWorld{worldToClip.Columns[0].X, worldToClip.Columns[1].X, worldToClip.Columns[2].X};
    const math::Vector3 verticalWorld{worldToClip.Columns[0].Y, worldToClip.Columns[1].Y, worldToClip.Columns[2].Y};
    const math::Vector3 depthWorld{worldToClip.Columns[0].W, worldToClip.Columns[1].W, worldToClip.Columns[2].W};
    const float32 scale = math::Min(math::Length(horizontalWorld) * static_cast<float32>(description.Width),
                                    math::Length(verticalWorld) * static_cast<float32>(description.Height));
    const float32 depthScale = math::Length(depthWorld);
    return scale > 0 && math::IsFinite(depthScale) == true ? math::Min(1.0F, 2 * depthScale / scale) : 0;
}

[[nodiscard]] math::RandomAddress MakeCelestialAddress(uint32 x, uint32 y, uint32 face, uint16 dimension) noexcept
{
    return {(uint64{y} << RANDOM_WORD_BITS) | x, face, dimension};
}

// Matches renderer_atmosphere_celestial.slang's cell addressing, star positions and filter.
[[nodiscard]] float32 EvaluateStars(const ReferenceSkyWords& words, math::Vector3 directionCelestial) noexcept
{
    const float32 peak = words[LUDUS_REFERENCE_SKY_SUN_RADIANCE].W;
    if (peak == 0)
    {
        return 0;
    }
    const float32 angularFootprint = words[LUDUS_REFERENCE_SKY_GALACTIC_CENTER_WORLD].W;
    constexpr uint32 FACE_CELLS = LUDUS_CELESTIAL_STAR_FACE_CELLS;
    constexpr float32 OCCUPANCY = LUDUS_CELESTIAL_STAR_OCCUPANCY;
    constexpr float32 RADIUS = LUDUS_CELESTIAL_STAR_CELL_RADIUS;
    constexpr float32 MARGIN = LUDUS_CELESTIAL_STAR_CELL_MARGIN;
    constexpr uint32 POSITIVE_Y_FACE = 2;
    constexpr uint32 POSITIVE_Z_FACE = 4;
    const math::Vector3 magnitude{math::Abs(directionCelestial.X),
                                  math::Abs(directionCelestial.Y),
                                  math::Abs(directionCelestial.Z)};
    math::Vector2 facePosition;
    uint32 face = 0;
    if (magnitude.X >= magnitude.Y && magnitude.X >= magnitude.Z)
    {
        facePosition = {directionCelestial.Y / magnitude.X, directionCelestial.Z / magnitude.X};
        face = directionCelestial.X >= 0 ? 0 : 1;
    }
    else if (magnitude.Y >= magnitude.Z)
    {
        facePosition = {directionCelestial.X / magnitude.Y, directionCelestial.Z / magnitude.Y};
        face = POSITIVE_Y_FACE + (directionCelestial.Y >= 0 ? 0 : 1);
    }
    else
    {
        facePosition = {directionCelestial.X / magnitude.Z, directionCelestial.Y / magnitude.Z};
        face = POSITIVE_Z_FACE + (directionCelestial.Z >= 0 ? 0 : 1);
    }
    const math::Vector2 coordinate{
        math::Clamp(facePosition.X * 0.5F + 0.5F, 0.0F, 1.0F) * FACE_CELLS,
        math::Clamp(facePosition.Y * 0.5F + 0.5F, 0.0F, 1.0F) * FACE_CELLS,
    };
    const uint32 x = math::Min(static_cast<uint32>(coordinate.X), FACE_CELLS - 1);
    const uint32 y = math::Min(static_cast<uint32>(coordinate.Y), FACE_CELLS - 1);
    const float32 brightness = math::SampleFloat01({}, MakeCelestialAddress(x, y, face, STAR_BRIGHTNESS_DIMENSION));
    if (brightness >= OCCUPANCY)
    {
        return 0;
    }
    const float32 range = 1 - 2 * MARGIN;
    const float32 offsetX = MARGIN + range * math::SampleFloat01({}, MakeCelestialAddress(x, y, face, 0));
    const float32 offsetY = MARGIN + range * math::SampleFloat01({}, MakeCelestialAddress(x, y, face, 1));
    const math::Vector2 delta{coordinate.X - static_cast<float32>(x) - offsetX,
                              coordinate.Y - static_cast<float32>(y) - offsetY};
    const float32 radius =
        math::Clamp(angularFootprint * (0.5F * FACE_CELLS), RADIUS, LUDUS_CELESTIAL_STAR_MAXIMUM_FILTER_RADIUS);
    const float32 coverage = 1 - math::SmoothStep01(math::Length(delta) / radius);
    const float32 areaRatio = RADIUS / radius;
    return peak * (1 - brightness / OCCUPANCY) * coverage * areaRatio * areaRatio;
}

[[nodiscard]] math::Vector3 EvaluateSun(math::Vector3 radiance, float32 chord, float32 radius) noexcept
{
    // Normalized limb darkening matches the resolved profile in renderer_atmosphere_celestial.slang.
    const float32 halfChord = chord * 0.5F;
    const float32 halfRadius = radius * 0.5F;
    const float32 ratio = chord / radius;
    const float32 muSquared =
        math::Clamp(1 - ratio * ratio * (1 - halfChord * halfChord) / (1 - halfRadius * halfRadius), 0.0F, 1.0F);
    constexpr math::Vector3 HALF_LIMB_EXPONENT{0.5F * LUDUS_CELESTIAL_SOLAR_LIMB_RED,
                                               0.5F * LUDUS_CELESTIAL_SOLAR_LIMB_GREEN,
                                               0.5F * LUDUS_CELESTIAL_SOLAR_LIMB_BLUE};
    const float64 wideMuSquared = static_cast<float64>(muSquared);
    return {radiance.X * (1 + HALF_LIMB_EXPONENT.X) *
                static_cast<float32>(math::Pow(wideMuSquared, static_cast<float64>(HALF_LIMB_EXPONENT.X))),
            radiance.Y * (1 + HALF_LIMB_EXPONENT.Y) *
                static_cast<float32>(math::Pow(wideMuSquared, static_cast<float64>(HALF_LIMB_EXPONENT.Y))),
            radiance.Z * (1 + HALF_LIMB_EXPONENT.Z) *
                static_cast<float32>(math::Pow(wideMuSquared, static_cast<float64>(HALF_LIMB_EXPONENT.Z)))};
}

[[nodiscard]] math::Vector3
ComputeLunarNormal(math::Vector3 directionWorld, math::Vector3 moonWorld, float32 sineRadius) noexcept
{
    const math::Vector3 transverseWorld = directionWorld * math::Dot(directionWorld, moonWorld) - moonWorld;
    const float32 root = math::Sqrt(math::Max(0.0F, sineRadius * sineRadius - math::LengthSquared(transverseWorld)));
    return (transverseWorld - root * directionWorld) / sineRadius;
}

[[nodiscard]] math::Vector3 EvaluateNightSky(const ReferenceSkyWords& words, math::Vector3 directionWorld) noexcept
{
    const float32 footprint = words[LUDUS_REFERENCE_SKY_GALACTIC_CENTER_WORLD].W;
    const math::Vector4 north = words[LUDUS_REFERENCE_SKY_GALACTIC_NORTH_WORLD];
    math::Vector3 directionCelestial = directionWorld;
    if (north.W > 0)
    {
        const math::Vector3 northWorld = GetRgb(north);
        const math::Vector3 centerWorld = GetRgb(words[LUDUS_REFERENCE_SKY_GALACTIC_CENTER_WORLD]);
        directionCelestial = {math::Dot(directionWorld, centerWorld),
                              math::Dot(directionWorld, math::Cross(northWorld, centerWorld)),
                              math::Dot(directionWorld, northWorld)};
    }
    const float32 star = EvaluateStars(words, directionCelestial);
    math::Vector3 radiance{star, star, star};
    const uint32 count = static_cast<uint32>(words[LUDUS_REFERENCE_SKY_LUNAR_LIGHT_WORLD].W);
    const float32 radius = math::Max(LUDUS_CELESTIAL_PLANET_MINIMUM_CHORD_RADIUS,
                                     LUDUS_CELESTIAL_PLANET_FILTER_WIDTH_IN_PIXELS * footprint);
    const float32 squaredRadius = radius * radius;
    const float32 normalization = LUDUS_CELESTIAL_PLANET_KERNEL_NORMALIZATION / (math::kPiF * squaredRadius);
    for (uint32 index = 0; index < count; ++index)
    {
        const uint32 word = LUDUS_REFERENCE_SKY_PLANETS + index * LUDUS_REFERENCE_SKY_PLANET_WORDS;
        const math::Vector3 offsetWorld = directionWorld - GetRgb(words[word]);
        const float32 coverage = math::Max(0.0F, 1 - math::LengthSquared(offsetWorld) / squaredRadius);
        radiance = radiance + GetRgb(words[word + 1]) * (normalization * coverage * coverage);
    }
    return radiance;
}
} // namespace

rhi::RasterStatus PrepareReferenceSky(const ReferenceCaptureDescription& description, ReferenceSky& outSky) noexcept
{
    outSky = {};
    const AtmosphereDescription* atmosphereOrNull = description.AtmosphereBackgroundOrNull;
    if (atmosphereOrNull == nullptr || atmosphereOrNull->IsEnabled == false)
    {
        return rhi::RasterStatus::Ready;
    }
    const AtmosphereDescription& atmosphere = *atmosphereOrNull;
    float64 altitudeMetres = 0;
    math::Vector3 upWorld;
    if (TryGetAtmosphereObserver(atmosphere, description.CameraPosition, altitudeMetres, upWorld) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    if (altitudeMetres < 0 || altitudeMetres > GetAtmosphereTopAltitude())
    {
        return rhi::RasterStatus::Ready;
    }
    const float32 radius = atmosphere.SolarAngularRadiusRadians;
    math::Vector3 sunWorld;
    if (IsAngularRadius(radius) == false || radius == 0 || IsRadiance(atmosphere.SolarIlluminance) == false ||
        math::TryNormalize(atmosphere.DirectionToSunWorld, sunWorld) != math::MathStatus::Success)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const float32 sineRadius = math::Sin(radius);
    const math::Vector3 solarRadiance = atmosphere.SolarIlluminance / (math::kPiF * sineRadius * sineRadius);
    if (IsRadiance(solarRadiance * 2) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    ReferenceSkyWords& words = outSky.Words;
    words[LUDUS_REFERENCE_SKY_SUN_WORLD] = MakeWord(sunWorld, ComputeChordRadius(radius));
    words[LUDUS_REFERENCE_SKY_SUN_RADIANCE] = MakeWord(solarRadiance, 0);
    if (atmosphere.Celestial.IsEnabled == true && (TryPackMoon(atmosphere.Celestial, sunWorld, words) == false ||
                                                   TryPackNightSky(atmosphere.Celestial, words) == false))
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const float64 horizon = WriteReferenceSkyTransmittance(altitudeMetres, words);
    words[LUDUS_REFERENCE_SKY_UP_WORLD] = MakeWord(upWorld, static_cast<float32>(horizon));
    words[LUDUS_REFERENCE_SKY_GALACTIC_CENTER_WORLD].W = ComputeAngularFootprint(description);
    return rhi::RasterStatus::Ready;
}

math::Vector3 EvaluateReferenceSky(const ReferenceSky& sky, math::Vector3 directionWorld) noexcept
{
    const ReferenceSkyWords& words = sky.Words;
    const math::Vector4 up = words[LUDUS_REFERENCE_SKY_UP_WORLD];
    const float32 zenithCosine = math::Dot(directionWorld, GetRgb(up));
    if (zenithCosine < up.W)
    {
        return {};
    }
    const math::Vector4 moon = words[LUDUS_REFERENCE_SKY_MOON_WORLD];
    const math::Vector4 sun = words[LUDUS_REFERENCE_SKY_SUN_WORLD];
    const math::Vector3 moonWorld = GetRgb(moon);
    const float32 sunChord = math::Length(directionWorld - GetRgb(sun));
    math::Vector3 radiance;
    if (moon.W > 0 && math::Length(directionWorld - moonWorld) <= moon.W)
    {
        const math::Vector4 lunarRadiance = words[LUDUS_REFERENCE_SKY_MOON_RADIANCE];
        const math::Vector3 normalWorld = ComputeLunarNormal(directionWorld, moonWorld, lunarRadiance.W);
        const math::Vector3 lunarLightWorld = GetRgb(words[LUDUS_REFERENCE_SKY_LUNAR_LIGHT_WORLD]);
        const float32 phase = math::Max(0.0F, math::Dot(normalWorld, lunarLightWorld));
        radiance = GetRgb(lunarRadiance) * phase;
    }
    else if (sunChord <= sun.W && GetRgb(words[LUDUS_REFERENCE_SKY_SUN_RADIANCE]) != math::Vector3{})
    {
        radiance = EvaluateSun(GetRgb(words[LUDUS_REFERENCE_SKY_SUN_RADIANCE]), sunChord, sun.W);
    }
    else
    {
        radiance = EvaluateNightSky(words, directionWorld);
    }
    const float32 coordinate =
        math::Sqrt(math::Clamp((zenithCosine - up.W) / (1 - up.W), 0.0F, 1.0F)) * TRANSMITTANCE_LAST;
    const uint32 lower = static_cast<uint32>(coordinate);
    const uint32 upper = math::Min(lower + 1, TRANSMITTANCE_LAST);
    const math::Vector3 transmittance = math::Lerp(GetRgb(words[LUDUS_REFERENCE_SKY_TRANSMITTANCE + lower]),
                                                   GetRgb(words[LUDUS_REFERENCE_SKY_TRANSMITTANCE + upper]),
                                                   coordinate - static_cast<float32>(lower));
    return math::Hadamard(radiance, transmittance);
}
} // namespace ludus::graphics::renderer::internal
