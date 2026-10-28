#include "internal/preview_shading.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <iterator>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rr = graphics::renderer;
/// Farthest depth-view distance, matching the renderer's DebugDepthRange admission bound.
constexpr float32 MAX_DEPTH_DISTANCE = 10000000.0F;
/// Span used when the camera planes and bounds give no usable far distance.
constexpr float32 FALLBACK_DEPTH_SPAN = 1.0F;
/// Corners of an axis-aligned box.
constexpr uint32 BOX_CORNER_COUNT = 8;
/// Bit selecting the maximum Z coordinate of a box corner index.
constexpr uint32 CORNER_Z_BIT = 4;
struct ShadingDescription final
{
    PreviewShading Shading;
    const char* Key;
    const char* Label;
    const char* Status;
    rr::MaterialDebugView DebugView;
};
// Labels follow the existing "Mode — explanation" style; Canadian spelling.
constexpr ShadingDescription SHADING_DESCRIPTIONS[PREVIEW_SHADING_COUNT]{
    {PreviewShading::Solid,
     "solid",
     "Solid — neutral preview shading",
     " · Solid: neutral preview shading",
     rr::MaterialDebugView::None},
    {PreviewShading::Albedo,
     "albedo",
     "Albedo — unlit surface colours",
     " · Albedo: unlit surface colours",
     rr::MaterialDebugView::None},
    {PreviewShading::Lit, "lit", "Lit — scene lighting", " · Lit: scene lighting", rr::MaterialDebugView::None},
    {PreviewShading::Normal,
     "normal",
     "Normal — world-space normals as colour",
     " · Normal: world-space normal as RGB (X red, Y green, Z blue)",
     rr::MaterialDebugView::NormalWorld},
    {PreviewShading::Depth,
     "depth",
     "Depth — distance from camera, near purple to far yellow",
     " · Depth: linear camera distance, near purple to far yellow",
     rr::MaterialDebugView::LinearDepth},
    {PreviewShading::Roughness,
     "roughness",
     "Roughness — smooth black to rough white",
     " · Roughness: perceptual roughness as grey",
     rr::MaterialDebugView::Roughness},
    {PreviewShading::Metallic,
     "metallic",
     "Metallic — dielectric black to metal white",
     " · Metallic: metallic fraction as grey",
     rr::MaterialDebugView::Metallic},
    {PreviewShading::Uv,
     "uv",
     "UV — texture coordinates as red/green",
     " · UV: fractional UV0, U red and V green",
     rr::MaterialDebugView::TexcoordUv},
    {PreviewShading::LightCount,
     "lightCount",
     "Light count — lights reaching each pixel as a ramp",
     " · Light count: direct lights per pixel, dark purple none to yellow many",
     rr::MaterialDebugView::LightCount},
};
/// Degree of the viridis polynomial that renderer_lit.slang evaluates.
constexpr usize VIRIDIS_DEGREE = 6;
/// Colour channels per coefficient.
constexpr usize VIRIDIS_CHANNELS = 3;
/// Number of coefficients, one per degree and channel.
constexpr usize VIRIDIS_COEFFICIENT_COUNT = (VIRIDIS_DEGREE + 1) * VIRIDIS_CHANNELS;
/// Coefficients of the degree-six viridis fit, lowest degree first, red, green and blue of each degree in turn.
/// Thanks to Matt Zucker, "Viridis approximation" (Shadertoy WlfXRN), a fit of matplotlib's viridis
/// (Nathaniel Smith and Stefan van der Walt, CC0).
constexpr float32 VIRIDIS_COEFFICIENTS[VIRIDIS_COEFFICIENT_COUNT] = {
    0.2777273272234177F, 0.005407344544966578F, 0.3340998053353061F, 0.1050930431085774F,  1.404613529898575F,
    1.384590162594685F,  -0.3308618287255563F,  0.214847559468213F,  0.09509516302823659F, -4.634230498983486F,
    -5.799100973351585F, -19.33244095627987F,   6.228269936347081F,  14.17993336680509F,   56.69055260068105F,
    4.776384997670288F,  -13.74514537774601F,   -65.35303263337234F, -5.435455855934631F,  4.645852612178535F,
    26.3124352495832F};
const ShadingDescription& Describe(PreviewShading shading) noexcept
{
    const usize index = static_cast<usize>(shading);
    return SHADING_DESCRIPTIONS[index < PREVIEW_SHADING_COUNT ? index : 0];
}
} // namespace
bool IsPreviewDebugShading(PreviewShading shading) noexcept
{
    return GetMaterialDebugView(shading) != rr::MaterialDebugView::None;
}
rr::MaterialDebugView GetMaterialDebugView(PreviewShading shading) noexcept
{
    return Describe(shading).DebugView;
}
QString GetPreviewShadingKey(PreviewShading shading)
{
    return QString::fromUtf8(Describe(shading).Key);
}
bool TryParsePreviewShading(const QString& key, PreviewShading& outShading)
{
    for (const ShadingDescription& description : SHADING_DESCRIPTIONS)
    {
        if (key == QLatin1StringView(description.Key))
        {
            outShading = description.Shading;
            return true;
        }
    }
    return false;
}
QString GetPreviewShadingLabel(PreviewShading shading)
{
    return QString::fromUtf8(Describe(shading).Label);
}
QString GetPreviewShadingStatus(PreviewShading shading)
{
    return QString::fromUtf8(Describe(shading).Status);
}
math::Vector3 GetPreviewRampColour(PreviewLegendRamp ramp, float32 position) noexcept
{
    const float32 x = math::Clamp(position, 0.0F, 1.0F);
    if (ramp == PreviewLegendRamp::Grey)
    {
        return {x, x, x};
    }
    float32 channels[VIRIDIS_CHANNELS]{};
    for (usize channel = 0; channel < VIRIDIS_CHANNELS; ++channel)
    {
        float32 value = VIRIDIS_COEFFICIENTS[VIRIDIS_DEGREE * VIRIDIS_CHANNELS + channel];
        for (usize degree = VIRIDIS_DEGREE; degree > 0; --degree)
        {
            value = VIRIDIS_COEFFICIENTS[(degree - 1) * VIRIDIS_CHANNELS + channel] + value * x;
        }
        channels[channel] = math::Clamp(value, 0.0F, 1.0F);
    }
    return {channels[0], channels[1], channels[2]};
}
bool TryGetPreviewLegend(PreviewShading shading, PreviewLegend& outLegend)
{
    switch (shading)
    {
        case PreviewShading::Depth:
            outLegend = {QStringLiteral("Distance from camera"),
                         QStringLiteral("near"),
                         QStringLiteral("far"),
                         PreviewLegendRamp::Viridis};
            return true;
        case PreviewShading::Roughness:
            outLegend = {QStringLiteral("Roughness"),
                         QStringLiteral("smooth"),
                         QStringLiteral("rough"),
                         PreviewLegendRamp::Grey};
            return true;
        case PreviewShading::Metallic:
            outLegend = {QStringLiteral("Metallic"),
                         QStringLiteral("dielectric"),
                         QStringLiteral("metal"),
                         PreviewLegendRamp::Grey};
            return true;
        case PreviewShading::LightCount:
            outLegend = {
                QStringLiteral("Lights reaching the pixel"),
                QStringLiteral("0"),
                QStringLiteral("%1 or more").arg(static_cast<int32>(rr::MATERIAL_DEBUG_LIGHT_COUNT_SATURATION)),
                PreviewLegendRamp::Viridis};
            return true;
        default:
            return false;
    }
}
math::Vector2 ComputeDebugDepthRange(const rr::ViewDescription& view,
                                     float32 nearPlane,
                                     float32 farPlane,
                                     const math::Aabb3* boundsWorldOrNull) noexcept
{
    float32 nearDistance = math::IsFinite(nearPlane) == true
                               ? math::Clamp(nearPlane, 0.0F, MAX_DEPTH_DISTANCE - FALLBACK_DEPTH_SPAN)
                               : 0.0F;
    float32 farDistance = math::IsFinite(farPlane) == true && farPlane > nearDistance ? farPlane : MAX_DEPTH_DISTANCE;
    if (boundsWorldOrNull != nullptr)
    {
        math::Vector3 forward{};
        const bool isOrthographic = view.OrthographicCamera == true &&
                                    math::TryNormalize(view.CameraForward, 0.0F, forward) == math::MathStatus::Success;
        float32 farthest = 0;
        float32 nearest = MAX_DEPTH_DISTANCE;
        for (uint32 corner = 0; corner < BOX_CORNER_COUNT; ++corner)
        {
            const math::Vector3 pointWorld{(corner & 1U) != 0 ? boundsWorldOrNull->Max.X : boundsWorldOrNull->Min.X,
                                           (corner & 2U) != 0 ? boundsWorldOrNull->Max.Y : boundsWorldOrNull->Min.Y,
                                           (corner & CORNER_Z_BIT) != 0 ? boundsWorldOrNull->Max.Z
                                                                        : boundsWorldOrNull->Min.Z};
            const math::Vector3 offsetWorld = pointWorld - view.CameraPosition;
            farthest = math::Max(farthest,
                                 isOrthographic == true ? math::Dot(offsetWorld, forward) : math::Length(offsetWorld));
            if (isOrthographic == true)
            {
                nearest = math::Min(nearest, math::Dot(offsetWorld, forward));
            }
        }
        if (isOrthographic == false)
        {
            // Nothing visible is closer than the bounds, so start the ramp there for full contrast.
            const math::Vector3 closestWorld{
                math::Clamp(view.CameraPosition.X, boundsWorldOrNull->Min.X, boundsWorldOrNull->Max.X),
                math::Clamp(view.CameraPosition.Y, boundsWorldOrNull->Min.Y, boundsWorldOrNull->Max.Y),
                math::Clamp(view.CameraPosition.Z, boundsWorldOrNull->Min.Z, boundsWorldOrNull->Max.Z)};
            nearest = math::Length(closestWorld - view.CameraPosition);
        }
        if (math::IsFinite(nearest) == true && nearest > nearDistance && nearest < farthest)
        {
            nearDistance = math::Min(nearest, MAX_DEPTH_DISTANCE - FALLBACK_DEPTH_SPAN);
        }
        if (math::IsFinite(farthest) == true && farthest > nearDistance)
        {
            farDistance = math::Min(farDistance, farthest);
        }
    }
    farDistance = math::Min(farDistance, MAX_DEPTH_DISTANCE);
    if (farDistance <= nearDistance)
    {
        farDistance = nearDistance + FALLBACK_DEPTH_SPAN;
    }
    return {nearDistance, farDistance};
}
} // namespace ludus::editor
