#include "internal/preview_shading.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/view_description.hpp>

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
     "Depth — distance from camera, near black to far white",
     " · Depth: linear camera distance, near black to far white",
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
     "Light count — lights reaching each pixel as a heat map",
     " · Light count: direct lights per pixel, black none, blue few to red many",
     rr::MaterialDebugView::LightCount},
};
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
