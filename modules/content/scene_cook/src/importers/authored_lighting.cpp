#include "authored_lighting.h"

#include "../native_files.h"
#include "transform_utilities.h"

#include <ludus/content/content.h>
#include <ludus/content/scene_cook/limits.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
namespace format = scene_format;
constexpr usize MAX_ENVIRONMENT_FILE_BYTES = MAX_SOURCE_IMAGE_BYTES;

[[nodiscard]] float64 ComputeLuminance(math::Vector3d rgb) noexcept
{
    return .2126 * rgb.X + .7152 * rgb.Y + .0722 * rgb.Z;
}
[[nodiscard]] bool IsRepresentable(math::Vector3d rgb, float64 maximum) noexcept
{
    return math::IsFinite(rgb.X) == true && math::IsFinite(rgb.Y) == true && math::IsFinite(rgb.Z) == true &&
           rgb.X >= 0 && rgb.Y >= 0 && rgb.Z >= 0 && rgb.X <= maximum && rgb.Y <= maximum && rgb.Z <= maximum;
}
// Thanks to Pharr, Jakob and Humphreys, Physically Based Rendering, fourth
// edition, section 12 and pbrt-v4 Light::Create, for the photometric source
// normalization. RGB/Y replaces spectral photometry explicitly in this profile.
// The squared engine spot lobe integrates to 2*PI*((1-inner)+(inner-outer)/3),
// rather than PBRT's smoothstep lobe integral with /2.
// https://pbrt.org/fileformat-v4#lights
// https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/lights.cpp
[[nodiscard]] bool ConvertAnalytic(const IrAnalyticLight& source, float64 range, format::LightRecord& outLight) noexcept
{
    const float64 luminance = ComputeLuminance(source.Strength);
    float64 intensity = source.Scale;
    const float64 inner = math::Cos(math::DegreesToRadians(source.InnerAngleDegrees));
    const float64 outer = math::Cos(math::DegreesToRadians(source.OuterAngleDegrees));
    if (source.HasPhotometricNormalization == true)
    {
        float64 solidAngle = 1;
        if (source.Kind == IrLightKind::Point)
        {
            solidAngle = 4 * math::kPi;
        }
        else if (source.Kind == IrLightKind::Spot)
        {
            solidAngle = 2 * math::kPi * ((1 - inner) + (inner - outer) / 3);
        }
        intensity *= source.PhotometricNormalization / solidAngle;
    }
    const math::Vector3d rgb = luminance > 0 ? source.Strength * (intensity / luminance) : math::Vector3d{};
    if (IsRepresentable(rgb, static_cast<float64>(format::MAX_LIGHT_INTENSITY)) == false || range <= 0 ||
        range > 1000000 || math::Abs(source.PositionWorld.X) > 1000000 || math::Abs(source.PositionWorld.Y) > 1000000 ||
        math::Abs(source.PositionWorld.Z) > 1000000)
    {
        return false;
    }
    outLight.Kind = source.Kind == IrLightKind::Directional ? format::LIGHT_DIRECTIONAL
                    : source.Kind == IrLightKind::Point     ? format::LIGHT_POINT
                                                            : format::LIGHT_SPOT;
    outLight.Flags = luminance > 0 && intensity > 0 ? format::LIGHT_ENABLED : 0;
    outLight.IntensityRgb[0] = static_cast<float32>(rgb.X);
    outLight.IntensityRgb[1] = static_cast<float32>(rgb.Y);
    outLight.IntensityRgb[2] = static_cast<float32>(rgb.Z);
    outLight.PositionWorld[0] = static_cast<float32>(source.PositionWorld.X);
    outLight.PositionWorld[1] = static_cast<float32>(source.PositionWorld.Y);
    outLight.PositionWorld[2] = static_cast<float32>(source.PositionWorld.Z);
    outLight.DirectionWorld[0] = static_cast<float32>(source.DirectionWorld.X);
    outLight.DirectionWorld[1] = static_cast<float32>(source.DirectionWorld.Y);
    outLight.DirectionWorld[2] = static_cast<float32>(source.DirectionWorld.Z);
    outLight.Range = static_cast<float32>(range);
    outLight.InnerCos = static_cast<float32>(inner);
    outLight.OuterCos = static_cast<float32>(outer);
    return outLight.OuterCos > -1 && outLight.OuterCos < outLight.InnerCos && outLight.InnerCos <= 1;
}

// Thanks to the PBRT authors and Paul Clarberg for the equal-area square/sphere
// map used by pbrt-v4 environments. This inverse uses FoundationMath Atan2
// instead of PBRT's minimax polynomial; the hemisphere folds and edge wrapping
// preserve the original parameterization before conversion to engine lat/long.
// https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/util/math.cpp
[[nodiscard]] math::Vector2 ComputeEqualAreaUv(math::Vector3d direction) noexcept
{
    const float64 radius = math::Sqrt(math::Max(0.0, 1 - math::Abs(direction.Z)));
    const float64 angle = math::Atan2(math::Abs(direction.Y), math::Abs(direction.X)) * 2 / math::kPi;
    float64 u = radius * (1 - angle);
    float64 v = radius * angle;
    if (direction.Z < 0)
    {
        const float64 previousU = u;
        u = 1 - v;
        v = 1 - previousU;
    }
    u = direction.X < 0 ? -u : u;
    v = direction.Y < 0 ? -v : v;
    return {static_cast<float32>((u + 1) * .5), static_cast<float32>((v + 1) * .5)};
}
struct TexelCoordinate final
{
    int32 Column = 0;
    int32 Row = 0;
};
[[nodiscard]] math::Vector3d FetchWrapped(const DecodedLightingImage& image, TexelCoordinate coordinate) noexcept
{
    float64 u = (static_cast<float64>(coordinate.Column) + .5) / image.Width;
    float64 v = (static_cast<float64>(coordinate.Row) + .5) / image.Height;
    if (u < 0 || u > 1)
    {
        u = u < 0 ? -u : 2 - u;
        v = 1 - v;
    }
    if (v < 0 || v > 1)
    {
        v = v < 0 ? -v : 2 - v;
        u = 1 - u;
    }
    const uint32 column =
        static_cast<uint32>(math::Clamp(math::Floor(u * image.Width), 0.0, static_cast<float64>(image.Width - 1)));
    const uint32 row =
        static_cast<uint32>(math::Clamp(math::Floor(v * image.Height), 0.0, static_cast<float64>(image.Height - 1)));
    const usize at = (usize{row} * image.Width + column) * LIGHTING_RGB_COMPONENTS;
    return {static_cast<float64>(image.Rgb[at]),
            static_cast<float64>(image.Rgb[at + 1]),
            static_cast<float64>(image.Rgb[at + 2])};
}
[[nodiscard]] math::Vector3d SampleEqualArea(const DecodedLightingImage& image, math::Vector3d direction) noexcept
{
    const math::Vector2 uv = ComputeEqualAreaUv(direction);
    const float64 x = static_cast<float64>(uv.X) * image.Width - .5;
    const float64 y = static_cast<float64>(uv.Y) * image.Height - .5;
    const int32 ix = static_cast<int32>(math::Floor(x));
    const int32 iy = static_cast<int32>(math::Floor(y));
    const float64 tx = x - ix;
    const float64 ty = y - iy;
    return FetchWrapped(image, {ix, iy}) * ((1 - tx) * (1 - ty)) + FetchWrapped(image, {ix + 1, iy}) * (tx * (1 - ty)) +
           FetchWrapped(image, {ix, iy + 1}) * ((1 - tx) * ty) + FetchWrapped(image, {ix + 1, iy + 1}) * (tx * ty);
}

[[nodiscard]] Status ConvertEnvironment(const IrEnvironment& source,
                                        std::string_view root,
                                        usize byteBudget,
                                        CookedLighting& inoutLighting,
                                        DiagnosticLog& inoutLog) noexcept
{
    if (source.IsPresent == false)
    {
        return Status::Ok;
    }
    if (source.File.IsEmpty() == true)
    {
        const math::Vector3d input{source.Radiance[0], source.Radiance[1], source.Radiance[2]};
        const float64 luminance = ComputeLuminance(input);
        const float64 scale = source.Scale * (source.HasIlluminance == true ? source.IlluminanceLux / math::kPi : 1.0);
        const math::Vector3d rgb = luminance > 0 ? input * (scale / luminance) : math::Vector3d{};
        if (IsRepresentable(rgb, static_cast<float64>(format::MAX_EMISSION)) == false)
        {
            return Status::Invalid;
        }
        inoutLighting.EnvironmentRgb = {static_cast<float32>(rgb.X),
                                        static_cast<float32>(rgb.Y),
                                        static_cast<float32>(rgb.Z)};
        return Status::Ok;
    }
    String path;
    constexpr usize PATH_PARTS = 3;
    const foundation::StaticArray<std::string_view, PATH_PARTS> parts{root, "/", source.File.GetView()};
    Array<uint8> bytes;
    if (path.TryAppendMany({parts.GetData(), parts.GetSize()}) != foundation::StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    const Status read = detail::ReadFileBounded(path.GetView(), MAX_ENVIRONMENT_FILE_BYTES, bytes);
    if (read != Status::Ok)
    {
        return read;
    }
    inoutLighting.EnvironmentSourceDigest = content::Hash(bytes.AsSpan());
    inoutLighting.HasEnvironmentSourceDigest = true;
    DecodedLightingImage image;
    std::string_view message;
    const ImageResult decoded =
        DecodeLightingImage(source.File.GetView(), {bytes.GetData(), bytes.GetSize()}, image, message);
    if (decoded != ImageResult::Ok)
    {
        (void)inoutLog.Add(DiagnosticSeverity::Error, codes::TEXTURE_UNREADABLE, source.File.GetView(), 0, message);
        return decoded == ImageResult::OutOfMemory ? Status::OutOfMemory : Status::Invalid;
    }
    math::Matrix4d worldToLight;
    if (TryInvertAffine(source.LightToWorld, worldToLight) == false)
    {
        return Status::Invalid;
    }
    uint32 width = image.Width * 2;
    width = width > format::MAX_ENVIRONMENT_DIMENSION ? format::MAX_ENVIRONMENT_DIMENSION : width;
    const uint32 height = width / 2;
    const usize count = usize{width} * height * LIGHTING_RGB_COMPONENTS;
    if (count > byteBudget / sizeof(float32) || inoutLighting.Environment.Rgb.TryResize(count) == false)
    {
        return count > byteBudget / sizeof(float32) ? Status::Invalid : Status::OutOfMemory;
    }
    inoutLighting.Environment.Width = width;
    inoutLighting.Environment.Height = height;
    float64 illuminance = 0;
    for (uint32 y = 0; y < height; ++y)
    {
        const float64 theta = (static_cast<float64>(y) + .5) / height * math::kPi;
        for (uint32 x = 0; x < width; ++x)
        {
            const float64 phi = (static_cast<float64>(x) + .5) / width * math::kTau;
            const math::Vector3d directionWorld{math::Sin(theta) * math::Cos(phi),
                                                math::Cos(theta),
                                                math::Sin(theta) * math::Sin(phi)};
            const math::Vector3d raw = TransformDirection(worldToLight, directionWorld);
            const float64 length = math::Length(raw);
            if (math::IsFinite(length) == false || length <= 0)
            {
                return Status::Invalid;
            }
            const math::Vector3d rgb = SampleEqualArea(image, raw * (1.0 / length));
            const usize at = (usize{y} * width + x) * LIGHTING_RGB_COMPONENTS;
            inoutLighting.Environment.Rgb[at] = static_cast<float32>(rgb.X);
            inoutLighting.Environment.Rgb[at + 1] = static_cast<float32>(rgb.Y);
            inoutLighting.Environment.Rgb[at + 2] = static_cast<float32>(rgb.Z);
            illuminance += ComputeLuminance(rgb) * math::Max(raw.Z / length, 0.0) * math::Sin(theta);
        }
    }
    illuminance *= math::kPi * math::kTau / (static_cast<float64>(width) * height);
    if (source.HasIlluminance == true && illuminance <= 0 && source.IlluminanceLux > 0)
    {
        return Status::Invalid;
    }
    const float64 scale =
        source.Scale * (source.HasIlluminance == true && illuminance > 0 ? source.IlluminanceLux / illuminance : 1.0);
    for (float32& value : inoutLighting.Environment.Rgb)
    {
        const float64 scaled = static_cast<float64>(value) * scale;
        if (math::IsFinite(scaled) == false || scaled < 0 || scaled > static_cast<float64>(format::MAX_EMISSION))
        {
            return Status::Invalid;
        }
        value = static_cast<float32>(scaled);
    }
    inoutLighting.EnvironmentRgb = {1, 1, 1};
    return Status::Ok;
}
} // namespace
Status CookAuthoredLighting(const SceneIr& scene,
                            std::string_view root,
                            const LightingCookDescription& description,
                            CookedLighting& outLighting,
                            DiagnosticLog& inoutLog) noexcept
{
    if (scene.AnalyticLights.GetSize() > format::MAX_LIGHTS || math::IsFinite(description.RangeMetres) == false ||
        description.RangeMetres <= 0 || description.RangeMetres > 1000000)
    {
        return Status::Invalid;
    }
    CookedLighting candidate;
    for (usize i = 0; i < scene.AnalyticLights.GetSize(); ++i)
    {
        format::LightRecord light{};
        light.SourceId = static_cast<uint64>(i) + 1;
        if (ConvertAnalytic(scene.AnalyticLights[i], description.RangeMetres, light) == false)
        {
            return Status::Invalid;
        }
        if (candidate.Lights.TryAdd(light) == false)
        {
            return Status::OutOfMemory;
        }
    }
    const Status environment =
        description.ShouldDecodeEnvironment == true || scene.Environment.File.IsEmpty() == true
            ? ConvertEnvironment(scene.Environment, root, description.EnvironmentByteBudget, candidate, inoutLog)
            : Status::Ok;
    if (environment != Status::Ok)
    {
        return environment;
    }
    outLighting = static_cast<CookedLighting&&>(candidate);
    return Status::Ok;
}
} // namespace ludus::content::scene_cook::importers
