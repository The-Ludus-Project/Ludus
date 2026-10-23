#include "internal/viewport_size.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::editor
{
namespace
{
using namespace foundation;
struct ViewportSizeDescription final
{
    ViewportSize Size;
    const char* Key;
    const char* Label;
    const char* Description;
    /// Whether the menu shows a separator before this entry.
    bool IsGroupStart;
};
// Thanks to Unity Technologies: the Game view's aspect and fixed-resolution menu, "Game view"
// in the Unity Manual (https://docs.unity3d.com/Manual/GameView.html), suggested offering both
// letterboxed aspect ratios and exact pixel sizes. Ludus also sends the size to the renderer.
// Aspect ratios cover common monitors, ultrawide, legacy and portrait phone screens; fixed sizes
// cover the common 16:9 game output resolutions.
constexpr StaticArray<ViewportSizeDescription, VIEWPORT_SIZE_PRESET_COUNT> SIZE_DESCRIPTIONS{{
    {{ViewportSizeKind::Fill, 0, 0}, "fill", "Fill", "Use the whole viewport at its pixel density", false},
    {{ViewportSizeKind::Aspect, 16, 9}, "16:9", "16:9", "16:9 widescreen at the viewport's pixel density", true},
    {{ViewportSizeKind::Aspect, 16, 10}, "16:10", "16:10", "16:10 at the viewport's pixel density", false},
    {{ViewportSizeKind::Aspect, 21, 9}, "21:9", "21:9", "21:9 ultrawide at the viewport's pixel density", false},
    {{ViewportSizeKind::Aspect, 4, 3}, "4:3", "4:3", "4:3 at the viewport's pixel density", false},
    {{ViewportSizeKind::Aspect, 1, 1}, "1:1", "1:1", "Square at the viewport's pixel density", false},
    {{ViewportSizeKind::Aspect, 9, 16}, "9:16", "9:16", "9:16 portrait at the viewport's pixel density", false},
    {{ViewportSizeKind::Fixed, 1280, 720}, "1280x720", "720p", "Exactly 1280 × 720 pixels, scaled to fit", true},
    {{ViewportSizeKind::Fixed, 1920, 1080}, "1920x1080", "1080p", "Exactly 1920 × 1080 pixels, scaled to fit", false},
    {{ViewportSizeKind::Fixed, 2560, 1440}, "2560x1440", "1440p", "Exactly 2560 × 1440 pixels, scaled to fit", false},
    {{ViewportSizeKind::Fixed, 3840, 2160},
     "3840x2160",
     "2160p",
     "Exactly 3840 × 2160 pixels (4K), scaled to fit",
     false},
}};
const ViewportSizeDescription& Describe(usize index) noexcept
{
    return SIZE_DESCRIPTIONS[index < VIEWPORT_SIZE_PRESET_COUNT ? index : 0];
}
} // namespace
ViewportSize GetViewportSizePreset(usize index) noexcept
{
    return Describe(index).Size;
}
QString GetViewportSizeKey(usize index)
{
    return QString::fromUtf8(Describe(index).Key);
}
bool TryParseViewportSizeKey(const QString& key, usize& outIndex)
{
    for (usize i = 0; i < VIEWPORT_SIZE_PRESET_COUNT; ++i)
    {
        if (key == QLatin1StringView(SIZE_DESCRIPTIONS[i].Key))
        {
            outIndex = i;
            return true;
        }
    }
    return false;
}
QString GetViewportSizeLabel(usize index)
{
    return QString::fromUtf8(Describe(index).Label);
}
QString GetViewportSizeDescription(usize index)
{
    return QString::fromUtf8(Describe(index).Description);
}
bool IsViewportSizeGroupStart(usize index) noexcept
{
    return Describe(index).IsGroupStart;
}
QRect ComputeViewportFrameRectangle(ViewportSize size, const QRect& areaLogical) noexcept
{
    if (size.Kind == ViewportSizeKind::Fill || size.Width == 0 || size.Height == 0 || areaLogical.isEmpty() == true)
    {
        return areaLogical;
    }
    const float64 scale = math::Min(static_cast<float64>(areaLogical.width()) / static_cast<float64>(size.Width),
                                    static_cast<float64>(areaLogical.height()) / static_cast<float64>(size.Height));
    const int32 width =
        math::Clamp(static_cast<int32>(static_cast<float64>(size.Width) * scale + 0.5), 1, areaLogical.width());
    const int32 height =
        math::Clamp(static_cast<int32>(static_cast<float64>(size.Height) * scale + 0.5), 1, areaLogical.height());
    return {areaLogical.x() + (areaLogical.width() - width) / 2,
            areaLogical.y() + (areaLogical.height() - height) / 2,
            width,
            height};
}
QSize ComputeViewportDrawableSize(ViewportSize size, const QSize& frameLogical, float64 pixelRatio) noexcept
{
    if (size.Kind == ViewportSizeKind::Fixed && size.Width != 0 && size.Height != 0)
    {
        return {static_cast<int32>(size.Width), static_cast<int32>(size.Height)};
    }
    return {math::Max(0, qRound(static_cast<float64>(frameLogical.width()) * pixelRatio)),
            math::Max(0, qRound(static_cast<float64>(frameLogical.height()) * pixelRatio))};
}
} // namespace ludus::editor
