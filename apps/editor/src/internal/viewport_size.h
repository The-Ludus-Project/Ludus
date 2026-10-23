#pragma once

#include <ludus/foundation/base/types.h>

#include <QRect>
#include <QSize>
#include <QString>

namespace ludus::editor
{
/// How a viewport size preset shapes the rendered frame inside the viewport's area.
enum class ViewportSizeKind : foundation::uint8
{
    /// The frame covers the whole area; the drawable follows its physical pixels.
    Fill,
    /// The largest centred frame of Width:Height; the drawable follows that frame's physical pixels.
    Aspect,
    /// The frame has the shape of Width x Height, scaled to fit; the drawable is exactly that many pixels.
    Fixed,
};
/// One choice of the viewport toolbar's size menu.
struct ViewportSize final
{
    ViewportSizeKind Kind = ViewportSizeKind::Fill;
    /// Aspect numerator, or the fixed drawable width in physical pixels. Unused for Fill.
    foundation::uint32 Width = 0;
    /// Aspect denominator, or the fixed drawable height in physical pixels. Unused for Fill.
    foundation::uint32 Height = 0;
};
/// Number of viewport size presets. Index 0 is Fill; indices only order the menu, and
/// preferences persist GetViewportSizeKey instead.
inline constexpr foundation::usize VIEWPORT_SIZE_PRESET_COUNT = 11;
/// Preset at index; an out-of-range index returns Fill.
[[nodiscard]] ViewportSize GetViewportSizePreset(foundation::usize index) noexcept;
/// Stable persistence key, such as "fill", "16:9" or "1920x1080".
[[nodiscard]] QString GetViewportSizeKey(foundation::usize index);
/// Parse a persistence key; unknown keys return false and preserve outIndex.
[[nodiscard]] bool TryParseViewportSizeKey(const QString& key, foundation::usize& outIndex);
/// Short menu label, such as "Fill", "16:9" or "1080p".
[[nodiscard]] QString GetViewportSizeLabel(foundation::usize index);
/// One-line explanation for the menu item's tooltip, including a fixed size's exact pixels.
[[nodiscard]] QString GetViewportSizeDescription(foundation::usize index);
/// Whether the preset at index comes after a menu separator (the first aspect and first fixed size).
[[nodiscard]] bool IsViewportSizeGroupStart(foundation::usize index) noexcept;
/// Where the frame sits inside areaLogical, in the same logical coordinates. Fill returns the
/// area; Aspect and Fixed return the largest centred rectangle of the preset's shape, at least
/// one pixel on each side. An empty area is returned unchanged.
[[nodiscard]] QRect ComputeViewportFrameRectangle(ViewportSize size, const QRect& areaLogical) noexcept;
/// Physical drawable size to request from the renderer for a frame of frameLogical size.
/// Fill and Aspect round frameLogical x pixelRatio (zero for an empty frame, which renders
/// nothing); Fixed returns the preset's exact pixels regardless of the frame and pixel ratio.
[[nodiscard]] QSize
ComputeViewportDrawableSize(ViewportSize size, const QSize& frameLogical, foundation::float64 pixelRatio) noexcept;
} // namespace ludus::editor
