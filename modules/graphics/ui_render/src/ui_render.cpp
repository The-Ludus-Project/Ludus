#include <ludus/graphics/ui_render/ui_render.hpp>

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::ui_render
{
namespace
{
using foundation::float32;
using foundation::float64;
using foundation::uint32;
using foundation::usize;
namespace math = foundation::math;
using Status = rhi::RasterStatus;

/// OverlayList::Quad expands one rectangle into two independent triangles.
constexpr usize QUAD_VERTEX_COUNT = 6;
/// Offset from a physical pixel's top-left corner to its sample centre.
constexpr float64 PIXEL_CENTER = 0.5;
/// Solid quads ignore their UVs; pass the full ordered range OverlayList validates.
constexpr math::Vector4 SOLID_UV{0, 0, 1, 1};

[[nodiscard]] float32 MapAxis(float64 physical, float64 origin, float64 scale) noexcept
{
    return static_cast<float32>((physical - origin) / scale);
}

[[nodiscard]] bool IsCentreAtOrAfter(uint32 pixel, float32 edge, float64 origin, float64 scale) noexcept
{
    return MapAxis(static_cast<float64>(pixel) + PIXEL_CENTER, origin, scale) >= edge;
}

/// Smallest pixel index in [0, extent] whose centre maps to a logical value at or after edge.
///
/// MapAxis is monotone, so the pixels inside a half-open logical span [start, end) are exactly
/// [SnapEdge(start), SnapEdge(end)). The float64 estimate is corrected against the float32 mapping
/// that pointer routing uses, so coverage and hit testing agree even on pixel-centre ties.
[[nodiscard]] uint32 SnapEdge(float32 edge, float64 origin, float64 scale, uint32 extent) noexcept
{
    const float64 estimate = math::Ceil(static_cast<float64>(edge) * scale + origin - PIXEL_CENTER);
    const float64 clamped = math::Min(math::Max(estimate, 0.0), static_cast<float64>(extent));
    uint32 pixel = static_cast<uint32>(clamped);
    while (pixel > 0 && IsCentreAtOrAfter(pixel - 1, edge, origin, scale) == true)
    {
        --pixel;
    }
    while (pixel < extent && IsCentreAtOrAfter(pixel, edge, origin, scale) == false)
    {
        ++pixel;
    }
    return pixel;
}

[[nodiscard]] bool IsValidRectangle(const ludus::ui::Rect& rectangle) noexcept
{
    return math::IsFinite(rectangle.X) == true && math::IsFinite(rectangle.Y) == true &&
           math::IsFinite(rectangle.Width) == true && math::IsFinite(rectangle.Height) == true &&
           rectangle.Width >= 0 && rectangle.Height >= 0 && math::IsFinite(rectangle.X + rectangle.Width) == true &&
           math::IsFinite(rectangle.Y + rectangle.Height) == true;
}

[[nodiscard]] bool IsUnitChannel(float32 channel) noexcept
{
    return math::IsFinite(channel) == true && channel >= 0 && channel <= 1;
}

[[nodiscard]] bool IsValidColor(const ludus::ui::Color& color) noexcept
{
    return IsUnitChannel(color.R) == true && IsUnitChannel(color.G) == true && IsUnitChannel(color.B) == true &&
           IsUnitChannel(color.A) == true;
}

[[nodiscard]] bool IsEmpty(const rhi::RasterRectangle& rectangle) noexcept
{
    return rectangle.Width == 0 || rectangle.Height == 0;
}

/// Smallest rectangle containing both; an empty first rectangle yields the second.
[[nodiscard]] rhi::RasterRectangle ComputeHull(const rhi::RasterRectangle& first,
                                               const rhi::RasterRectangle& second) noexcept
{
    if (IsEmpty(first) == true)
    {
        return second;
    }
    const uint32 left = math::Min(first.X, second.X);
    const uint32 top = math::Min(first.Y, second.Y);
    const uint32 right = math::Max(first.X + first.Width, second.X + second.Width);
    const uint32 bottom = math::Max(first.Y + first.Height, second.Y + second.Height);
    return {left, top, right - left, bottom - top};
}
} // namespace

bool TryComputePaintQuad(const ludus::ui::PaintCommand& command,
                         const SurfaceMapping& mapping,
                         PaintQuad& outQuad) noexcept
{
    rhi::RasterRectangle quad;
    rhi::RasterRectangle clip;
    if (IsValidColor(command.Fill) == false || TryComputePhysicalRectangle(command.Bounds, mapping, quad) == false ||
        TryComputePhysicalRectangle(command.Clip, mapping, clip) == false)
    {
        return false;
    }
    outQuad.Quad = quad;
    outQuad.Style.Depth = renderer::OverlayDepth::Always;
    outQuad.Style.Clip = true;
    // Ui computes Bounds as an intersection with Clip in float32, so the snapped quad is inside the
    // snapped clip except for a possible one-ulp rounding of the right/bottom edge. The hull keeps
    // painted pixels equal to the pixels Ui routes to the command in that case too.
    outQuad.Style.Scissor = IsEmpty(quad) == true ? clip : ComputeHull(clip, quad);
    return true;
}

bool IsValid(const SurfaceMapping& mapping) noexcept
{
    return math::IsFinite(mapping.Scale) == true && mapping.Scale > 0 && math::IsFinite(mapping.OriginX) == true &&
           math::IsFinite(mapping.OriginY) == true && mapping.Width != 0 && mapping.Height != 0;
}

LogicalPoint MapPhysicalToLogical(const SurfaceMapping& mapping, float64 physicalX, float64 physicalY) noexcept
{
    const float64 scale = static_cast<float64>(mapping.Scale);
    return {MapAxis(physicalX, static_cast<float64>(mapping.OriginX), scale),
            MapAxis(physicalY, static_cast<float64>(mapping.OriginY), scale)};
}

bool TryComputePhysicalRectangle(const ludus::ui::Rect& rectangle,
                                 const SurfaceMapping& mapping,
                                 rhi::RasterRectangle& outRectangle) noexcept
{
    if (IsValid(mapping) == false || IsValidRectangle(rectangle) == false)
    {
        return false;
    }
    const float64 scale = static_cast<float64>(mapping.Scale);
    const float64 originX = static_cast<float64>(mapping.OriginX);
    const float64 originY = static_cast<float64>(mapping.OriginY);
    // Ui hit testing compares against these float32 sums; reproduce them exactly.
    const float32 right = rectangle.X + rectangle.Width;
    const float32 bottom = rectangle.Y + rectangle.Height;
    const uint32 left = SnapEdge(rectangle.X, originX, scale, mapping.Width);
    const uint32 top = SnapEdge(rectangle.Y, originY, scale, mapping.Height);
    const uint32 end = SnapEdge(right, originX, scale, mapping.Width);
    const uint32 last = SnapEdge(bottom, originY, scale, mapping.Height);
    outRectangle = {left, top, end > left ? end - left : 0, last > top ? last - top : 0};
    return true;
}

rhi::RasterStatus AppendPaint(renderer::OverlayList& inoutList,
                              std::span<const ludus::ui::PaintCommand> commands,
                              const SurfaceMapping& mapping,
                              PaintReport& outReport) noexcept
{
    if (inoutList.Vertices() == nullptr)
    {
        return Status::NotReady;
    }
    if (IsValid(mapping) == false)
    {
        return Status::InvalidDescription;
    }
    // Preflight: validate every command and count quads and runs exactly as OverlayList will merge
    // them, so a failure appends nothing.
    PaintReport report;
    report.Commands = commands.size();
    const usize existingCommands = inoutList.CommandCount();
    bool hasPreviousStyle = existingCommands != 0;
    renderer::OverlayStyle previousStyle;
    if (hasPreviousStyle == true)
    {
        previousStyle = inoutList.Commands()[existingCommands - 1].Style;
    }
    for (const ludus::ui::PaintCommand& command : commands)
    {
        PaintQuad snapped;
        if (TryComputePaintQuad(command, mapping, snapped) == false)
        {
            return Status::InvalidDescription;
        }
        if (IsEmpty(snapped.Quad) == true)
        {
            ++report.Culled;
            continue;
        }
        ++report.Quads;
        if (hasPreviousStyle == false || renderer::CanMergeOverlayStyles(previousStyle, snapped.Style) == false)
        {
            ++report.Runs;
            previousStyle = snapped.Style;
            hasPreviousStyle = true;
        }
    }
    const usize availableVertices = renderer::OverlayList::MAX_VERTICES - inoutList.VertexCount();
    const usize availableRuns = renderer::OverlayList::MAX_COMMANDS - existingCommands;
    if (report.Quads > availableVertices / QUAD_VERTEX_COUNT || report.Runs > availableRuns)
    {
        return Status::CapacityExceeded;
    }
    for (const ludus::ui::PaintCommand& command : commands)
    {
        PaintQuad snapped;
        static_cast<void>(TryComputePaintQuad(command, mapping, snapped));
        if (IsEmpty(snapped.Quad) == true)
        {
            continue;
        }
        const rhi::RasterRectangle& quad = snapped.Quad;
        const math::Vector4 rectangle{static_cast<float32>(quad.X),
                                      static_cast<float32>(quad.Y),
                                      static_cast<float32>(quad.X + quad.Width),
                                      static_cast<float32>(quad.Y + quad.Height)};
        const math::Vector4 color{command.Fill.R, command.Fill.G, command.Fill.B, command.Fill.A};
        const Status appended =
            inoutList.Quad(rectangle, SOLID_UV, color, mapping.Width, mapping.Height, false, snapped.Style);
        // The preflight checked every input and the capacity OverlayList enforces.
        LUDUS_ASSERT(appended == Status::Ready);
        static_cast<void>(appended);
    }
    outReport = report;
    return Status::Ready;
}
} // namespace ludus::graphics::ui_render
