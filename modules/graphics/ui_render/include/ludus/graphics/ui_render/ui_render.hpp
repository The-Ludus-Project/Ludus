#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/renderer/overlays.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/ui/context.h>

#include <span>

/// @file
/// UiRender: the U1 bridge from `Ludus::Ui` paint commands to the generic renderer
/// overlay list. Ui stays free of GPU work; this adapter owns the logical-to-physical
/// mapping that both painted pixels and routed pointer positions use. See
/// the "U1 render adapter" section of docs/architecture/ui.md for the contract and its evidence.

namespace ludus::graphics::ui_render
{
/// Uniform mapping between one Ui context's logical pixels and a physical framebuffer.
///
/// Physical position p maps to logical (p - Origin) / Scale on each axis. The caller removes
/// safe-area insets by choosing Origin and resolves the Ui viewport in the same logical units.
/// Change the mapping only between frames, re-resolving the Ui document before routing events.
struct SurfaceMapping final
{
    /// Physical pixels per logical pixel; finite and positive. Integer and fractional scales are valid.
    foundation::float32 Scale = 1;
    /// Physical X of the logical origin; finite.
    foundation::float32 OriginX = 0;
    /// Physical Y of the logical origin; finite.
    foundation::float32 OriginY = 0;
    /// Physical framebuffer width in pixels; nonzero.
    foundation::uint32 Width = 0;
    /// Physical framebuffer height in pixels; nonzero.
    foundation::uint32 Height = 0;
};

/// Logical position of a physical framebuffer point, for `ludus::ui::Event` coordinates.
struct LogicalPoint final
{
    /// Logical X, top-left origin, +X right.
    foundation::float32 X = 0;
    /// Logical Y, top-left origin, +Y down.
    foundation::float32 Y = 0;
};

/// Counters for one successful AppendPaint call.
struct PaintReport final
{
    /// Paint commands read from the span.
    foundation::usize Commands = 0;
    /// Quads appended; commands that cover no physical pixel centre append none.
    foundation::usize Quads = 0;
    /// Commands that covered no physical pixel centre after snapping.
    foundation::usize Culled = 0;
    /// New overlay compatibility runs (draws) appended; adjacent quads with one scissor merge.
    foundation::usize Runs = 0;
};

/// Check a mapping before using it.
/// @return true when Scale is finite and positive, origins are finite and both extents are nonzero.
[[nodiscard]] bool IsValid(const SurfaceMapping& mapping) noexcept;

/// Map a physical framebuffer point to Ui logical coordinates with the adapter's rounding.
///
/// Uses the same float64 computation, rounded once to float32, that AppendPaint uses to decide
/// pixel coverage. Pass physical pixel centres (x + 0.5) to reproduce exact paint coverage.
/// @pre IsValid(mapping) and finite inputs; otherwise the result is unspecified.
[[nodiscard]] LogicalPoint MapPhysicalToLogical(const SurfaceMapping& mapping,
                                                foundation::float64 physicalX,
                                                foundation::float64 physicalY) noexcept;

/// Snap a logical half-open rectangle to the physical pixels whose centres it contains.
///
/// Physical pixel (i, j) is inside exactly when MapPhysicalToLogical(i + 0.5, j + 0.5) lies inside
/// the half-open rectangle [X, X + Width) x [Y, Y + Height), with the right and bottom edges computed
/// in float32 as `Ludus::Ui` hit testing computes them. The result is clamped to the framebuffer;
/// it is empty (zero width or height) when no pixel centre is inside.
/// @param rectangle Finite logical rectangle with nonnegative extents.
/// @param mapping Valid mapping.
/// @param outRectangle Receives the physical rectangle on success; unchanged on failure.
/// @return false for an invalid mapping or a nonfinite or negative-extent rectangle.
[[nodiscard]] bool TryComputePhysicalRectangle(const ludus::ui::Rect& rectangle,
                                               const SurfaceMapping& mapping,
                                               rhi::RasterRectangle& outRectangle) noexcept;

/// Append committed Ui paint commands to an overlay list in paint order.
///
/// Each command becomes one solid pixel-aligned quad over TryComputePhysicalRectangle(Bounds) with a
/// scissor over TryComputePhysicalRectangle(Clip); commands that cover no pixel centre are culled.
/// Colors stay straight-alpha linear RGB; the overlay shader premultiplies and the RHI target
/// performs any attachment encoding. Adjacent commands with the same clip merge into one draw;
/// translucent order is never changed.
///
/// The call validates every command, the mapping and the list's remaining vertex and run capacity
/// before appending anything, so every failure leaves the list and outReport unchanged and does not
/// increment the list's DroppedCommands.
/// @param inoutList Initialized overlay list owned by the calling thread; existing geometry is kept.
/// @param commands Paint commands, usually `ludus::ui::Context::GetPaintCommands()`. Copied during the
/// call; nothing is retained, so the span may expire immediately afterwards.
/// @param mapping Valid mapping for the framebuffer the list will be drawn into.
/// @param outReport Receives counters on Ready.
/// @return Ready on success; NotReady for an uninitialized list; InvalidDescription for an invalid
/// mapping, a nonfinite or negative-extent rectangle or a color channel outside [0, 1];
/// CapacityExceeded when the commands do not fit in the list's remaining capacity.
[[nodiscard]] rhi::RasterStatus AppendPaint(renderer::OverlayList& inoutList,
                                            std::span<const ludus::ui::PaintCommand> commands,
                                            const SurfaceMapping& mapping,
                                            PaintReport& outReport) noexcept;
} // namespace ludus::graphics::ui_render
