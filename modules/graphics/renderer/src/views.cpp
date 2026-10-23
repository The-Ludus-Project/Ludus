#include <ludus/graphics/renderer/views.hpp>

#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace math = foundation::math;
using Status = rhi::RasterStatus;
namespace
{
/// Largest drawable width or height a view maps, in pixels.
constexpr uint32 MAX_DRAWABLE_EXTENT = uint32{1} << 24;
bool ValidScreen(VirtualScreen screen) noexcept
{
    return math::IsFinite(screen.Width) && math::IsFinite(screen.Height) && screen.Width > 0 && screen.Height > 0 &&
           static_cast<uint8>(screen.Fit) <= static_cast<uint8>(ScreenFit::IntegerFit);
}
bool ValidMapping(const ViewMapping& mapping) noexcept
{
    const auto& r = mapping.Region;
    const auto& v = mapping.Viewport;
    return ValidScreen(mapping.Screen) && math::IsFinite(mapping.Origin) && math::IsFinite(mapping.Scale) &&
           math::IsFinite(mapping.ClipTransform) && mapping.Scale.X > 0 && mapping.Scale.Y > 0 &&
           mapping.DrawableWidth > 0 && mapping.DrawableHeight > 0 && mapping.DrawableWidth <= MAX_DRAWABLE_EXTENT &&
           mapping.DrawableHeight <= MAX_DRAWABLE_EXTENT && r.Width > 0 && r.Height > 0 &&
           r.X <= mapping.DrawableWidth && r.Y <= mapping.DrawableHeight && r.Width <= mapping.DrawableWidth - r.X &&
           r.Height <= mapping.DrawableHeight - r.Y && v.Width > 0 && v.Height > 0 && v.X >= r.X && v.Y >= r.Y &&
           static_cast<uint64>(v.X) + v.Width <= static_cast<uint64>(r.X) + r.Width &&
           static_cast<uint64>(v.Y) + v.Height <= static_cast<uint64>(r.Y) + r.Height;
}

} // namespace
Status ResolveViewMapping(VirtualScreen screen,
                          rhi::RasterRectangle region,
                          uint32 drawableWidth,
                          uint32 drawableHeight,
                          ViewMapping& output) noexcept
{
    if (ValidScreen(screen) == false || drawableWidth > MAX_DRAWABLE_EXTENT || drawableHeight > MAX_DRAWABLE_EXTENT ||
        region.X > drawableWidth || region.Y > drawableHeight || region.Width > drawableWidth - region.X ||
        region.Height > drawableHeight - region.Y)
    {
        return Status::InvalidDescription;
    }
    if (drawableWidth == 0 || drawableHeight == 0 || region.Width == 0 || region.Height == 0)
    {
        return Status::NotReady;
    }
    ViewMapping result;
    result.DrawableWidth = drawableWidth;
    result.DrawableHeight = drawableHeight;
    result.Region = region;
    result.Viewport = region;
    result.Screen = screen;
    float64 sx = static_cast<float64>(region.Width) / static_cast<float64>(screen.Width);
    float64 sy = static_cast<float64>(region.Height) / static_cast<float64>(screen.Height);
    float64 width = region.Width, height = region.Height;
    if (screen.Fit != ScreenFit::Stretch)
    {
        float64 scale = screen.Fit == ScreenFit::Fill ? math::Max(sx, sy) : math::Min(sx, sy);
        if (screen.Fit == ScreenFit::IntegerFit)
        {
            const auto whole = math::Floor(scale);
            result.FractionalFallback = whole < 1;
            if (result.FractionalFallback == false)
            {
                scale = whole;
            }
        }
        width = static_cast<float64>(screen.Width) * scale;
        height = static_cast<float64>(screen.Height) * scale;
        if (screen.Fit != ScreenFit::Fill)
        {
            const auto w = static_cast<uint32>(math::Min(static_cast<float64>(region.Width), math::Floor(width + 0.5)));
            const auto h =
                static_cast<uint32>(math::Min(static_cast<float64>(region.Height), math::Floor(height + 0.5)));
            if (w == 0 || h == 0)
            {
                return Status::NotReady;
            }
            result.Viewport = {region.X + (region.Width - w) / 2, region.Y + (region.Height - h) / 2, w, h};
            width = w;
            height = h;
        }
        sx = width / static_cast<float64>(screen.Width);
        sy = height / static_cast<float64>(screen.Height);
    }
    result.Scale = {static_cast<float32>(sx), static_cast<float32>(sy)};
    result.Origin =
        screen.Fit == ScreenFit::Fill
            ? math::Vector2{static_cast<float32>(static_cast<float64>(region.X) + (region.Width - width) / 2),
                            static_cast<float32>(static_cast<float64>(region.Y) + (region.Height - height) / 2)}
            : math::Vector2{static_cast<float32>(result.Viewport.X), static_cast<float32>(result.Viewport.Y)};
    if (screen.Fit == ScreenFit::Fill)
    {
        result.ClipTransform.Columns[0].X = static_cast<float32>(width / region.Width);
        result.ClipTransform.Columns[1].Y = static_cast<float32>(height / region.Height);
    }
    if (ValidMapping(result) == false || math::IsFinite(result.ClipTransform) == false)
    {
        return Status::InvalidDescription;
    }
    output = result;
    return Status::Ready;
}
Status SplitViewRectangle(rhi::RasterRectangle parent,
                          uint32 columns,
                          uint32 rows,
                          uint32 column,
                          uint32 row,
                          rhi::RasterRectangle& output) noexcept
{
    if (columns == 0 || rows == 0 || column >= columns || row >= rows ||
        static_cast<uint64>(parent.X) + parent.Width > ~uint32{0} ||
        static_cast<uint64>(parent.Y) + parent.Height > ~uint32{0})
    {
        return Status::InvalidDescription;
    }
    const auto left = static_cast<uint32>(static_cast<uint64>(parent.Width) * column / columns);
    const auto right =
        static_cast<uint32>(static_cast<uint64>(parent.Width) * (static_cast<uint64>(column) + 1) / columns);
    const auto top = static_cast<uint32>(static_cast<uint64>(parent.Height) * row / rows);
    const auto bottom = static_cast<uint32>(static_cast<uint64>(parent.Height) * (static_cast<uint64>(row) + 1) / rows);
    if (left == right || top == bottom)
    {
        return Status::NotReady;
    }
    output = {parent.X + left, parent.Y + top, right - left, bottom - top};
    return Status::Ready;
}
Status MapPhysicalToVirtual(const ViewMapping& mapping, math::Vector2 pixel, math::Vector2& output, bool clamp) noexcept
{
    if (ValidMapping(mapping) == false || math::IsFinite(pixel) == false)
    {
        return Status::InvalidDescription;
    }
    const auto& r = mapping.Region;
    float64 x = (static_cast<float64>(pixel.X) - static_cast<float64>(mapping.Origin.X)) /
                static_cast<float64>(mapping.Scale.X);
    float64 y = (static_cast<float64>(pixel.Y) - static_cast<float64>(mapping.Origin.Y)) /
                static_cast<float64>(mapping.Scale.Y);
    if (clamp == false &&
        (pixel.X < static_cast<float32>(r.X) || pixel.Y < static_cast<float32>(r.Y) ||
         static_cast<float64>(pixel.X) >= static_cast<float64>(r.X) + r.Width ||
         static_cast<float64>(pixel.Y) >= static_cast<float64>(r.Y) + r.Height || x < 0 || y < 0 ||
         x >= static_cast<float64>(mapping.Screen.Width) || y >= static_cast<float64>(mapping.Screen.Height)))
    {
        return Status::InvalidDescription;
    }
    if (clamp)
    {
        // Clamp to the visible region first: Fill's cropped content remains unpickable.
        const auto px =
            math::Clamp(static_cast<float64>(pixel.X), static_cast<float64>(r.X), static_cast<float64>(r.X) + r.Width);
        const auto py =
            math::Clamp(static_cast<float64>(pixel.Y), static_cast<float64>(r.Y), static_cast<float64>(r.Y) + r.Height);
        x = math::Clamp((px - static_cast<float64>(mapping.Origin.X)) / static_cast<float64>(mapping.Scale.X),
                        float64{0},
                        static_cast<float64>(mapping.Screen.Width));
        y = math::Clamp((py - static_cast<float64>(mapping.Origin.Y)) / static_cast<float64>(mapping.Scale.Y),
                        float64{0},
                        static_cast<float64>(mapping.Screen.Height));
    }
    const math::Vector2 result{static_cast<float32>(x), static_cast<float32>(y)};
    if (math::IsFinite(result) == false)
    {
        return Status::InvalidDescription;
    }
    output = result;
    return Status::Ready;
}
Status MapWindowToVirtual(const ViewMapping& mapping,
                          math::Vector2 point,
                          math::Vector2 pixelsPerWindowUnit,
                          math::Vector2& output,
                          bool clamp) noexcept
{
    if (math::IsFinite(point) == false || math::IsFinite(pixelsPerWindowUnit) == false || pixelsPerWindowUnit.X <= 0 ||
        pixelsPerWindowUnit.Y <= 0)
    {
        return Status::InvalidDescription;
    }
    const math::Vector2 physical{point.X * pixelsPerWindowUnit.X, point.Y * pixelsPerWindowUnit.Y};
    return MapPhysicalToVirtual(mapping, physical, output, clamp);
}
Status MapVirtualToPhysical(const ViewMapping& mapping, math::Vector2 point, math::Vector2& output) noexcept
{
    if (ValidMapping(mapping) == false || math::IsFinite(point) == false)
    {
        return Status::InvalidDescription;
    }
    const math::Vector2 result{
        static_cast<float32>(static_cast<float64>(point.X) * static_cast<float64>(mapping.Scale.X) +
                             static_cast<float64>(mapping.Origin.X)),
        static_cast<float32>(static_cast<float64>(point.Y) * static_cast<float64>(mapping.Scale.Y) +
                             static_cast<float64>(mapping.Origin.Y))};
    if (math::IsFinite(result) == false)
    {
        return Status::InvalidDescription;
    }
    output = result;
    return Status::Ready;
}
Status BuildViewDescription(const ProjectionDescription& projection,
                            const ViewMapping& mapping,
                            ViewDescription& output,
                            ProjectionSpace space) noexcept
{
    if (ValidMapping(mapping) == false || math::IsFinite(projection.WorldToView) == false ||
        (space != ProjectionSpace::LogicalScreen && space != ProjectionSpace::OutputRegion))
    {
        return Status::InvalidDescription;
    }
    const float32 aspect = mapping.Screen.Width / mapping.Screen.Height;
    math::Matrix4 clipProjection;
    auto status = math::MathStatus::InvalidArgument;
    switch (projection.Kind)
    {
        case ProjectionKind::Orthographic: {
            if (math::IsFinite(projection.OrthographicHeight) == false || projection.OrthographicHeight <= 0)
            {
                return Status::InvalidDescription;
            }
            const float32 halfHeight = projection.OrthographicHeight / 2;
            const float32 halfWidth = halfHeight * aspect;
            status = math::TryOrthographicReverseZ(-halfWidth,
                                                   halfWidth,
                                                   -halfHeight,
                                                   halfHeight,
                                                   projection.Near,
                                                   projection.Far,
                                                   clipProjection);
            break;
        }
        case ProjectionKind::Perspective:
            status = math::TryPerspectiveReverseZ(projection.VerticalFovRadians,
                                                  aspect,
                                                  projection.Near,
                                                  projection.Far,
                                                  clipProjection);
            break;
        case ProjectionKind::PerspectiveInfinite:
            status = math::TryPerspectiveReverseZInfinite(projection.VerticalFovRadians,
                                                          aspect,
                                                          projection.Near,
                                                          clipProjection);
            break;
        default:
            return Status::InvalidDescription;
    }
    if (status != math::MathStatus::Success)
    {
        return Status::InvalidDescription;
    }
    ViewDescription result;
    const auto crop = space == ProjectionSpace::OutputRegion ? mapping.ClipTransform : math::Matrix4::Identity();
    result.WorldToClip = crop * clipProjection * projection.WorldToView;
    result.Depth = DepthConvention::ReverseZ;
    result.InfiniteFar = projection.Kind == ProjectionKind::PerspectiveInfinite;
    // Direct drawing crops geometry and overlays together; composition crops the sampled image.
    result.OverlayToClip = crop;
    if (math::IsFinite(result.WorldToClip) == false)
    {
        return Status::InvalidDescription;
    }
    output = result;
    return Status::Ready;
}
} // namespace ludus::graphics::renderer
