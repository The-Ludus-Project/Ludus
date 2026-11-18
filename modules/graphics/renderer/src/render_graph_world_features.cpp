#include "internal/render_graph_world_features.h"

#include <ludus/foundation/math/scalar.hpp>

#include "internal/atmosphere_sky_tables.h"

// Ray kernels draw a World's Atmosphere through two renderer-owned tables, resolved on the CPU with the same
// clear-sky model as the raster's tables: the distant sky (ResolveAtmosphereEnvironment, also the RT1 reference's sky
// since #1576) and the haze along view segments (ResolveAtmosphereAerialPerspective). Thanks to Sebastien Hillaire,
// "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", Computer Graphics Forum 39(4), 2020,
// sections 4-5, https://doi.org/10.1111/cgf.14050. See docs/development/render-graph-authoring.md#world-features.
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
using Status = rhi::RasterStatus;
namespace
{
/// Largest constant environment SceneShaderInputs admits per channel (IsSceneRadianceValid).
constexpr float32 MAX_CONSTANT_ENVIRONMENT_RADIANCE = 10000;

[[nodiscard]] Status ToRasterStatus(AtmosphereEnvironmentStatus status) noexcept
{
    switch (status)
    {
        case AtmosphereEnvironmentStatus::Ready:
        case AtmosphereEnvironmentStatus::NotLit:
            return Status::Ready;
        case AtmosphereEnvironmentStatus::InvalidDescription:
            return Status::InvalidDescription;
        case AtmosphereEnvironmentStatus::OutOfMemory:
            return Status::OutOfMemory;
    }
    return Status::InvalidDescription;
}

/// Solid-angle-weighted mean radiance of a latitude/longitude image of the given width; rows are uniform in polar
/// angle.
[[nodiscard]] math::Vector3 ComputeMeanRadiance(BorrowedView<const math::Vector4> pixels, uint32 width) noexcept
{
    const uint32 height = static_cast<uint32>(pixels.GetSize() / width);
    float64 weightSum = 0;
    float64 red = 0;
    float64 green = 0;
    float64 blue = 0;
    for (uint32 y = 0; y < height; ++y)
    {
        const float64 weight = math::Sin(math::kPi * (static_cast<float64>(y) + 0.5) / static_cast<float64>(height));
        for (uint32 x = 0; x < width; ++x)
        {
            const math::Vector4 value = pixels[static_cast<usize>(y) * width + x];
            red += weight * static_cast<float64>(value.X);
            green += weight * static_cast<float64>(value.Y);
            blue += weight * static_cast<float64>(value.Z);
            weightSum += weight;
        }
    }
    const auto clamp = [weightSum](float64 sum) noexcept {
        return weightSum > 0
                   ? math::Clamp(static_cast<float32>(sum / weightSum), 0.0F, MAX_CONSTANT_ENVIRONMENT_RADIANCE)
                   : 0.0F;
    };
    return {clamp(red), clamp(green), clamp(blue)};
}
} // namespace

bool AreRayAtmospheresEqual(const AtmosphereDescription& first, const AtmosphereDescription& second) noexcept
{
    if (first.IsEnabled != second.IsEnabled)
    {
        return false;
    }
    return first.IsEnabled == false ||
           (first.ShouldLightScene == second.ShouldLightScene &&
            first.SurfaceAnchorWorld == second.SurfaceAnchorWorld && first.SurfaceUpWorld == second.SurfaceUpWorld &&
            first.DirectionToSunWorld == second.DirectionToSunWorld &&
            first.SolarIlluminance == second.SolarIlluminance &&
            first.SolarAngularRadiusRadians == second.SolarAngularRadiusRadians &&
            first.SunSourceId == second.SunSourceId);
}

Status RenderGraphWorldTables::Resolve(const AtmosphereDescription& atmosphere,
                                       math::Vector3 eyeWorld,
                                       bool shouldResolveSky,
                                       bool shouldResolveAerialPerspective,
                                       const SceneShaderInputs& inputs) noexcept
{
    mInputs = inputs;
    mHasSky = false;
    if (mSky.TryResize(shouldResolveSky == true ? ATMOSPHERE_SKY_TABLE_WORDS : 0) == false ||
        mAerialPerspective.TryResize(shouldResolveAerialPerspective == true ? AERIAL_PERSPECTIVE_TABLE_WORDS : 0) ==
            false)
    {
        return Status::OutOfMemory;
    }
    if (shouldResolveAerialPerspective == true)
    {
        // A zero header omits the haze; the entries are never read then.
        mAerialPerspective[0] = {};
        const Status status = ToRasterStatus(
            ResolveAtmosphereAerialPerspective(atmosphere,
                                               eyeWorld,
                                               {mAerialPerspective.GetData(), mAerialPerspective.GetSize()}));
        if (status != Status::Ready)
        {
            return status;
        }
    }
    if (shouldResolveSky == false)
    {
        return Status::Ready;
    }
    // A zero-width header keeps the World environment, as the raster does for a camera outside the shell.
    mSky[0] = {};
    constexpr uint32 WIDTH = LUDUS_ATMOSPHERE_SKY_TABLE_WIDTH;
    constexpr uint32 HEIGHT = LUDUS_ATMOSPHERE_SKY_TABLE_HEIGHT;
    const BorrowedView<math::Vector4> pixels{mSky.GetData() + LUDUS_ATMOSPHERE_SKY_HEADER_WORDS,
                                             mSky.GetSize() - LUDUS_ATMOSPHERE_SKY_HEADER_WORDS};
    math::Vector3 sunIlluminance{};
    const AtmosphereEnvironmentStatus resolved =
        ResolveAtmosphereEnvironment(atmosphere, eyeWorld, WIDTH, HEIGHT, pixels, sunIlluminance);
    if (resolved != AtmosphereEnvironmentStatus::Ready)
    {
        return ToRasterStatus(resolved);
    }
    mSky[0] = {static_cast<float32>(WIDTH), static_cast<float32>(HEIGHT), 0, 0};
    mHasSky = true;
    // As the raster and the RT1 reference do, the sky replaces the World environment and the linked Sun arrives with
    // the illuminance transmitted to the observer. The constant keeps the environment light in the light list.
    if (mLights.TryResize(inputs.Lights.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < inputs.Lights.GetSize(); ++i)
    {
        DirectLight light = inputs.Lights[i];
        if (light.Kind == LightKind::Directional && light.SourceId == atmosphere.SunSourceId)
        {
            light.Intensity = math::Max(sunIlluminance.X, math::Max(sunIlluminance.Y, sunIlluminance.Z));
            light.Color = light.Intensity > 0 ? sunIlluminance / light.Intensity : math::Vector3{1, 1, 1};
        }
        mLights[i] = light;
    }
    mInputs.Lights = {mLights.GetData(), mLights.GetSize()};
    mInputs.Environment = ComputeMeanRadiance(pixels, WIDTH);
    mInputs.EnvironmentPixels = {};
    mInputs.EnvironmentWidth = 0;
    mInputs.EnvironmentHeight = 0;
    return Status::Ready;
}

const SceneShaderInputs& RenderGraphWorldTables::GetInputs() const noexcept
{
    return mInputs;
}

BorrowedView<const math::Vector4> RenderGraphWorldTables::GetSkyTable() const noexcept
{
    return {mSky.GetData(), mSky.GetSize()};
}

BorrowedView<const math::Vector4> RenderGraphWorldTables::GetAerialPerspectiveTable() const noexcept
{
    return {mAerialPerspective.GetData(), mAerialPerspective.GetSize()};
}

bool RenderGraphWorldTables::HasSky() const noexcept
{
    return mHasSky;
}
} // namespace ludus::graphics::renderer::internal
