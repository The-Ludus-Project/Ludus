#include <ludus/graphics/ui_text/ui_text.hpp>

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::ui_text
{
namespace
{
using foundation::float32;
using foundation::float64;
using foundation::uint32;
using foundation::uint64;
using foundation::usize;
namespace math = foundation::math;
using Status = rhi::RasterStatus;

/// OverlayList::Quad expands one rectangle into two independent triangles.
constexpr usize QUAD_VERTEX_COUNT = 6;
/// Offset from a physical pixel's top-left corner to its sample centre.
constexpr float64 PIXEL_CENTER = 0.5;
/// Solid quads ignore their UVs; pass the full ordered range OverlayList validates.
constexpr math::Vector4 SOLID_UV{0, 0, 1, 1};
/// OverlayList maps physical X to clip space as 2x / width - 1 (and Y as 1 - 2y / height).
constexpr float64 CLIP_SPACE_SPAN = 2;

[[nodiscard]] bool IsUnitChannel(float32 channel) noexcept
{
    return math::IsFinite(channel) == true && channel >= 0 && channel <= 1;
}

[[nodiscard]] bool IsValidColor(const ludus::ui::Color& color) noexcept
{
    return IsUnitChannel(color.R) == true && IsUnitChannel(color.G) == true && IsUnitChannel(color.B) == true &&
           IsUnitChannel(color.A) == true;
}

[[nodiscard]] math::Vector4 ToVector(const ludus::ui::Color& color) noexcept
{
    return {color.R, color.G, color.B, color.A};
}

/// The baseline-independent checks OverlayList::Quad and text::AppendText apply to a glyph.
[[nodiscard]] bool IsValidGlyph(const text::TextQuad& glyph) noexcept
{
    const math::Vector4& rectangle = glyph.Rectangle;
    const math::Vector4& uv = glyph.Uv;
    return math::IsFinite(rectangle) == true && math::IsFinite(uv) == true && rectangle.Z >= rectangle.X &&
           rectangle.W >= rectangle.Y && uv.X >= 0 && uv.Y >= 0 && uv.Z <= 1 && uv.W <= 1 && uv.Z >= uv.X &&
           uv.W >= uv.Y;
}

[[nodiscard]] bool IsValidLabel(const Label& label, const ui_render::SurfaceMapping& mapping) noexcept
{
    if (label.Element == 0 || math::IsFinite(label.OffsetX) == false || math::IsFinite(label.BaselineY) == false ||
        IsValidColor(label.Color) == false || label.Scale != mapping.Scale)
    {
        return false;
    }
    for (const text::TextQuad& glyph : label.Glyphs)
    {
        if (IsValidGlyph(glyph) == false)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsClipCoordinateFinite(float32 physical, uint32 extent) noexcept
{
    return math::IsFinite(static_cast<float32>(CLIP_SPACE_SPAN * static_cast<float64>(physical) / extent)) == true;
}

/// Pixel centres of [start, end) inside the scissor span [first, first + extent) on one axis.
[[nodiscard]] uint64 CountCentres(float32 start, float32 end, uint32 first, uint32 extent) noexcept
{
    const float64 low = static_cast<float64>(first);
    const float64 high = low + static_cast<float64>(extent);
    const float64 begin = math::Min(math::Max(math::Ceil(static_cast<float64>(start) - PIXEL_CENTER), low), high);
    const float64 stop = math::Min(math::Max(math::Ceil(static_cast<float64>(end) - PIXEL_CENTER), low), high);
    return stop > begin ? static_cast<uint64>(stop - begin) : 0;
}

[[nodiscard]] bool IsEmpty(const rhi::RasterRectangle& rectangle) noexcept
{
    return rectangle.Width == 0 || rectangle.Height == 0;
}

/// Whether a glyph rectangle overlaps the scissor; a glyph outside it cannot shade any pixel.
[[nodiscard]] bool DoesOverlap(const math::Vector4& rectangle, const rhi::RasterRectangle& scissor) noexcept
{
    if (IsEmpty(scissor) == true)
    {
        return false;
    }
    const float64 left = static_cast<float64>(scissor.X);
    const float64 top = static_cast<float64>(scissor.Y);
    const float64 right = left + static_cast<float64>(scissor.Width);
    const float64 bottom = top + static_cast<float64>(scissor.Height);
    return static_cast<float64>(rectangle.Z) > left &&
           static_cast<float64>(rectangle.X)<right&& static_cast<float64>(rectangle.W)> top &&
           static_cast<float64>(rectangle.Y) < bottom;
}

/// Counts runs exactly as OverlayList merges adjacent compatible commands.
struct RunTracker final
{
    renderer::OverlayStyle Previous;
    bool HasPrevious = false;
    bool HasText = false;
};

void CountQuad(RunTracker& inoutTracker,
               const renderer::OverlayStyle& style,
               bool isGlyph,
               UiTextReport& inoutReport) noexcept
{
    if (inoutTracker.HasPrevious == false || renderer::CanMergeOverlayStyles(inoutTracker.Previous, style) == false)
    {
        ++inoutReport.Runs;
        inoutTracker.Previous = style;
        inoutTracker.HasPrevious = true;
        inoutTracker.HasText = false;
    }
    if (isGlyph == true && inoutTracker.HasText == false)
    {
        ++inoutReport.TextRuns;
        inoutTracker.HasText = true;
    }
}

/// Inputs shared by the counting and appending walks.
struct Walk final
{
    std::span<const ludus::ui::LayoutBox> Layout;
    std::span<const ludus::ui::PaintCommand> Commands;
    std::span<const Label> Labels;
    ui_render::SurfaceMapping Mapping;
};

void AppendQuad(renderer::OverlayList* listOrNull,
                const math::Vector4& rectangle,
                const math::Vector4& uv,
                const math::Vector4& color,
                bool isCoverage,
                const renderer::OverlayStyle& style,
                const ui_render::SurfaceMapping& mapping) noexcept
{
    if (listOrNull == nullptr)
    {
        return;
    }
    const Status appended = listOrNull->Quad(rectangle, uv, color, mapping.Width, mapping.Height, isCoverage, style);
    // The counting walk checked every input and the capacity OverlayList enforces.
    LUDUS_ASSERT(appended == Status::Ready);
    static_cast<void>(appended);
}

/// Append (or, with a null list, only validate and count) one label's glyphs.
[[nodiscard]] bool VisitLabel(const Label& label,
                              const ludus::ui::LayoutBox& box,
                              const renderer::OverlayStyle& style,
                              const ui_render::SurfaceMapping& mapping,
                              renderer::OverlayList* listOrNull,
                              RunTracker& inoutTracker,
                              UiTextReport& inoutReport) noexcept
{
    const float64 scale = static_cast<float64>(mapping.Scale);
    const float32 baselineX =
        static_cast<float32>(static_cast<float64>(mapping.OriginX) +
                             (static_cast<float64>(box.Bounds.X) + static_cast<float64>(label.OffsetX)) * scale);
    const float32 baselineY =
        static_cast<float32>(static_cast<float64>(mapping.OriginY) +
                             (static_cast<float64>(box.Bounds.Y) + static_cast<float64>(label.BaselineY)) * scale);
    if (math::IsFinite(baselineX) == false || math::IsFinite(baselineY) == false)
    {
        return false;
    }
    const math::Vector4 color = ToVector(label.Color);
    ++inoutReport.Labels;
    for (const text::TextQuad& glyph : label.Glyphs)
    {
        const math::Vector4& placed = glyph.Rectangle;
        const math::Vector4 shifted{placed.X + baselineX,
                                    placed.Y + baselineY,
                                    placed.Z + baselineX,
                                    placed.W + baselineY};
        if (math::IsFinite(shifted) == false || IsClipCoordinateFinite(shifted.X, mapping.Width) == false ||
            IsClipCoordinateFinite(shifted.Z, mapping.Width) == false ||
            IsClipCoordinateFinite(shifted.Y, mapping.Height) == false ||
            IsClipCoordinateFinite(shifted.W, mapping.Height) == false)
        {
            return false;
        }
        if (shifted.Z == shifted.X || shifted.W == shifted.Y)
        {
            continue;
        }
        if (DoesOverlap(shifted, style.Scissor) == false)
        {
            ++inoutReport.CulledGlyphs;
            continue;
        }
        ++inoutReport.GlyphQuads;
        inoutReport.ShadedPixels += CountCentres(shifted.X, shifted.Z, style.Scissor.X, style.Scissor.Width) *
                                    CountCentres(shifted.Y, shifted.W, style.Scissor.Y, style.Scissor.Height);
        CountQuad(inoutTracker, style, true, inoutReport);
        AppendQuad(listOrNull, shifted, glyph.Uv, color, true, style, mapping);
    }
    return true;
}

/// Walk the document in paint order. With a null list this validates and counts only; with a list
/// it appends the geometry the counting walk already admitted.
[[nodiscard]] Status
VisitDocument(const Walk& walk, renderer::OverlayList* listOrNull, RunTracker tracker, UiTextReport& outReport) noexcept
{
    UiTextReport report;
    report.Commands = walk.Commands.size();
    usize nextCommand = 0;
    for (const ludus::ui::LayoutBox& box : walk.Layout)
    {
        renderer::OverlayStyle style;
        bool hasStyle = false;
        if (nextCommand < walk.Commands.size() && walk.Commands[nextCommand].Id == box.Id)
        {
            const ludus::ui::PaintCommand& command = walk.Commands[nextCommand];
            ++nextCommand;
            ui_render::PaintQuad fill;
            if (ui_render::TryComputePaintQuad(command, walk.Mapping, fill) == false)
            {
                return Status::InvalidDescription;
            }
            style = fill.Style;
            hasStyle = true;
            if (IsEmpty(fill.Quad) == true)
            {
                ++report.CulledFills;
            }
            else
            {
                ++report.FillQuads;
                report.ShadedPixels += uint64{fill.Quad.Width} * fill.Quad.Height;
                CountQuad(tracker, style, false, report);
                const math::Vector4 rectangle{static_cast<float32>(fill.Quad.X),
                                              static_cast<float32>(fill.Quad.Y),
                                              static_cast<float32>(fill.Quad.X + fill.Quad.Width),
                                              static_cast<float32>(fill.Quad.Y + fill.Quad.Height)};
                AppendQuad(listOrNull, rectangle, SOLID_UV, ToVector(command.Fill), false, style, walk.Mapping);
            }
        }
        if (box.Visible == false)
        {
            continue;
        }
        for (const Label& label : walk.Labels)
        {
            if (label.Element != box.Id)
            {
                continue;
            }
            if (hasStyle == false)
            {
                rhi::RasterRectangle clip;
                if (ui_render::TryComputePhysicalRectangle(box.Clip, walk.Mapping, clip) == false)
                {
                    return Status::InvalidDescription;
                }
                style.Depth = renderer::OverlayDepth::Always;
                style.Clip = true;
                style.Scissor = clip;
                hasStyle = true;
            }
            if (VisitLabel(label, box, style, walk.Mapping, listOrNull, tracker, report) == false)
            {
                return Status::InvalidDescription;
            }
        }
    }
    if (nextCommand != walk.Commands.size())
    {
        // A command that is not in layout order came from another commit or another document.
        return Status::InvalidDescription;
    }
    report.HiddenLabels = walk.Labels.size() - report.Labels;
    outReport = report;
    return Status::Ready;
}
} // namespace

rhi::RasterStatus AppendUi(renderer::OverlayList& inoutList,
                           std::span<const ludus::ui::LayoutBox> layout,
                           std::span<const ludus::ui::PaintCommand> commands,
                           std::span<const Label> labels,
                           const ui_render::SurfaceMapping& mapping,
                           UiTextReport& outReport) noexcept
{
    if (inoutList.Vertices() == nullptr)
    {
        return Status::NotReady;
    }
    if (ui_render::IsValid(mapping) == false)
    {
        return Status::InvalidDescription;
    }
    for (const Label& label : labels)
    {
        if (IsValidLabel(label, mapping) == false)
        {
            return Status::InvalidDescription;
        }
    }
    RunTracker tracker;
    const usize existingCommands = inoutList.CommandCount();
    if (existingCommands != 0)
    {
        tracker.Previous = inoutList.Commands()[existingCommands - 1].Style;
        tracker.HasPrevious = true;
    }
    const Walk walk{layout, commands, labels, mapping};
    UiTextReport report;
    const Status counted = VisitDocument(walk, nullptr, tracker, report);
    if (counted != Status::Ready)
    {
        return counted;
    }
    const usize availableVertices = renderer::OverlayList::MAX_VERTICES - inoutList.VertexCount();
    const usize availableRuns = renderer::OverlayList::MAX_COMMANDS - existingCommands;
    if (report.FillQuads + report.GlyphQuads > availableVertices / QUAD_VERTEX_COUNT || report.Runs > availableRuns)
    {
        return Status::CapacityExceeded;
    }
    UiTextReport appended;
    const Status visited = VisitDocument(walk, &inoutList, tracker, appended);
    LUDUS_ASSERT(visited == Status::Ready);
    static_cast<void>(visited);
    outReport = report;
    return Status::Ready;
}
} // namespace ludus::graphics::ui_text
