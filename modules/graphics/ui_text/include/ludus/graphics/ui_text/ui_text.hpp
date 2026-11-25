#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/renderer/overlays.hpp>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/text/coverage_atlas.hpp>
#include <ludus/graphics/ui_render/ui_render.hpp>
#include <ludus/ui/context.h>

#include <span>

/// @file
/// UiText: the U1 bridge that draws shaped text attached to `Ludus::Ui` elements as GraphicsText
/// coverage quads, interleaved with the element fills `Ludus::UiRender` emits. Ui stays free of
/// text and GPU work: a label names its element by `ludus::ui::WidgetId` and carries glyph quads
/// the caller prepared. See the "U1 text adapter" section of docs/architecture/ui.md.

namespace ludus::graphics::ui_text
{
/// Shaped text drawn on one Ui element.
///
/// The label is positioned from the element's resolved layout box (`ludus::ui::LayoutBox::Bounds`,
/// before clipping) and clipped by the element's effective clip, the clip its fill and hit region
/// use. Text that extends outside the element's own bounds is drawn where that clip allows; size the
/// element to contain its ink, or nest the label in a clipping element, to bound it further.
struct Label final
{
    /// Element that positions and clips the label; nonzero. Several labels may name one element.
    ludus::ui::WidgetId Element = 0;
    /// Baseline-relative physical-pixel glyph placements, usually from GraphicsText `PrepareText`,
    /// rasterized for Scale. Borrowed only during AppendUi. Their UVs must address the atlas version
    /// the caller passes to `OverlayRenderer::Prepare` with the resulting list.
    std::span<const text::TextQuad> Glyphs;
    /// Logical pen origin, from the left edge of the element's layout box; finite.
    foundation::float32 OffsetX = 0;
    /// Logical baseline, from the top edge of the element's layout box; finite.
    foundation::float32 BaselineY = 0;
    /// Straight-alpha linear RGB text color, each channel in [0, 1].
    ludus::ui::Color Color{1, 1, 1, 1};
    /// Physical pixels per logical pixel the glyphs were rasterized for. AppendUi requires it to equal
    /// the mapping's Scale, so a label shaped before a display-scale change cannot draw at the wrong
    /// raster size; re-prepare the label at the new scale instead.
    foundation::float32 Scale = 1;
};

/// Batching and overdraw counters for one successful AppendUi call.
struct UiTextReport final
{
    /// Paint commands read.
    foundation::usize Commands = 0;
    /// Solid fill quads appended; commands that cover no pixel centre append none.
    foundation::usize FillQuads = 0;
    /// Paint commands that covered no physical pixel centre after snapping.
    foundation::usize CulledFills = 0;
    /// Labels whose element was visible and whose glyphs were considered.
    foundation::usize Labels = 0;
    /// Labels whose element is absent from the layout or not visible; they append nothing.
    foundation::usize HiddenLabels = 0;
    /// Glyph coverage quads appended.
    foundation::usize GlyphQuads = 0;
    /// Glyphs skipped because their rectangle lies outside the label's scissor. Zero-area glyphs
    /// (whitespace) are neither appended nor counted.
    foundation::usize CulledGlyphs = 0;
    /// New overlay runs (draws) opened; fills and glyphs sharing a scissor merge into one run.
    foundation::usize Runs = 0;
    /// Appended runs that contain at least one glyph quad from this call.
    foundation::usize TextRuns = 0;
    /// Physical pixel centres shaded by the appended quads inside their scissors, counted once per
    /// quad; divide by the framebuffer area for an overdraw factor.
    foundation::uint64 ShadedPixels = 0;
};

/// Append Ui fills and their labels to an overlay list in paint order.
///
/// Walks the layout in submission (paint) order. For each element it appends the element's fill
/// exactly as `ui_render::AppendPaint` would, then every label naming that element, so a later
/// sibling's fill covers an earlier element's text. Glyph quads use the fill's overlay style when
/// the element has a fill and otherwise the snapped effective clip (`LayoutBox::Clip`, equal to
/// `PaintCommand::Clip`) as their scissor, so a fill and its text share one run. Label baselines map
/// to physical pixels with the mapping's scale and origin and are not snapped.
///
/// Every input, including labels of hidden elements, and the list's remaining vertex and run
/// capacity are validated before anything is appended, so every failure leaves the list and
/// outReport unchanged and does not increment the list's DroppedCommands. Cost is linear in the
/// glyph count plus layout boxes times labels.
/// @param inoutList Initialized overlay list owned by the calling thread; existing geometry is kept.
/// @param layout Layout boxes of one committed document, `ludus::ui::Context::GetLayout()`.
/// @param commands Paint commands of the same commit, `ludus::ui::Context::GetPaintCommands()`.
/// @param labels Labels to draw. Copied during the call; nothing is retained.
/// @param mapping Valid mapping for the framebuffer the list will be drawn into.
/// @param outReport Receives counters on Ready.
/// @return Ready on success; NotReady for an uninitialized list; InvalidDescription for an invalid
/// mapping, command or label (zero element, nonfinite offset or glyph, unordered glyph edges, UVs
/// outside [0, 1], a color channel outside [0, 1], or a Scale different from the mapping's), or
/// commands that are not the layout's paint subsequence; CapacityExceeded when the geometry does
/// not fit in the list's remaining capacity.
[[nodiscard]] rhi::RasterStatus AppendUi(renderer::OverlayList& inoutList,
                                         std::span<const ludus::ui::LayoutBox> layout,
                                         std::span<const ludus::ui::PaintCommand> commands,
                                         std::span<const Label> labels,
                                         const ui_render::SurfaceMapping& mapping,
                                         UiTextReport& outReport) noexcept;
} // namespace ludus::graphics::ui_text
