#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/world_grid.hpp>

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
constexpr float32 GRID_DECADE = 10;
constexpr float32 MINIMUM_SPACING = 0.000001F;
constexpr float32 MAXIMUM_SPACING = 1000000;
constexpr float32 MAXIMUM_FOCUS_DISTANCE = 10000000;
constexpr float32 LINE_PIXELS = 1.25F;
constexpr math::Vector4 MINOR_COLOR{.025F, .03F, .038F, 1};
constexpr math::Vector4 MAJOR_COLOR{.06F, .07F, .085F, 1};
constexpr math::Vector4 X_COLOR{.6F, .07F, .07F, 1};
constexpr math::Vector4 Y_COLOR{.11F, .46F, .13F, 1};
constexpr math::Vector4 Z_COLOR{.08F, .24F, .72F, 1};
/// Caller-owned count and spacing, borrowed together only during grid generation.
struct WorldGridOutputs final
{
    usize& Count;
    float32& SpacingWorld;
};
math::Vector4 GetGridColor(float32 coordinate) noexcept
{
    const float32 decade = static_cast<float32>(math::Floor(static_cast<float64>(coordinate / GRID_DECADE)));
    return coordinate == decade * GRID_DECADE ? MAJOR_COLOR : MINOR_COLOR;
}
} // namespace
bool BuildWorldGridLines(const ViewDescription& view,
                         BorrowedView<WorldGridLine> outLines,
                         usize& outCount,
                         float32& outSpacing) noexcept
{
    const WorldGridOutputs outputs{ .Count = outCount, .SpacingWorld = outSpacing };
    if (outLines.IsValid() == false || outLines.GetSize() < WORLD_GRID_LINE_CAPACITY)
    {
        return false;
    }
    if (view.WorldGrid.IsVisible == false || view.ShadowCasters == true)
    {
        outputs.Count = 0;
        outputs.SpacingWorld = 0;
        return true;
    }
    const WorldGridDescription& grid = view.WorldGrid;
    const math::Vector3 verticalClipRow{view.WorldToClip.At(1, 0),
                                        view.WorldToClip.At(1, 1),
                                        view.WorldToClip.At(1, 2)};
    const float32 verticalScale = math::Length(verticalClipRow);
    math::Vector3 forwardWorld;
    if (grid.Width == 0 || grid.Height == 0 || math::IsFinite(grid.FocusDistance) == false || grid.FocusDistance <= 0 ||
        grid.FocusDistance > MAXIMUM_FOCUS_DISTANCE || math::IsFinite(view.WorldToClip) == false ||
        math::IsFinite(view.CameraPosition) == false || math::IsFinite(verticalScale) == false || verticalScale <= 0 ||
        math::TryNormalize(view.CameraForward, forwardWorld) != math::MathStatus::Success)
    {
        return false;
    }
    const float32 span = (view.OrthographicCamera == true ? 2 : 2 * grid.FocusDistance) / verticalScale;
    if (math::IsFinite(span) == false || span <= 0)
    {
        return false;
    }
    const float32 aspect = static_cast<float32>(grid.Width) / static_cast<float32>(grid.Height);
    const float32 scale = math::Max(span, MINIMUM_SPACING) * math::Max(1.0F, aspect) / GRID_DECADE;
    float32 spacing = 1;
    while (spacing > scale && spacing > MINIMUM_SPACING)
    {
        spacing /= GRID_DECADE;
    }
    while (spacing * GRID_DECADE <= scale && spacing < MAXIMUM_SPACING)
    {
        spacing *= GRID_DECADE;
    }
    math::Vector3 horizontalWorld{1, 0, 0};
    math::Vector3 verticalWorld{0, 0, 1};
    if (view.OrthographicCamera == true)
    {
        if (math::Abs(forwardWorld.Z) >= math::Abs(forwardWorld.X) &&
            math::Abs(forwardWorld.Z) >= math::Abs(forwardWorld.Y))
        {
            verticalWorld = {0, 1, 0};
        }
        else if (math::Abs(forwardWorld.X) > math::Abs(forwardWorld.Y))
        {
            horizontalWorld = {0, 0, 1};
            verticalWorld = {0, 1, 0};
        }
    }
    const math::Vector3 focusWorld = view.CameraPosition + forwardWorld * grid.FocusDistance;
    if (math::IsFinite(focusWorld) == false)
    {
        return false;
    }
    const float32 horizontal =
        static_cast<float32>(math::Floor(static_cast<float64>(math::Dot(focusWorld, horizontalWorld) / spacing)));
    const float32 vertical =
        static_cast<float32>(math::Floor(static_cast<float64>(math::Dot(focusWorld, verticalWorld) / spacing)));
    const math::Vector3 centerWorld = horizontalWorld * (horizontal * spacing) + verticalWorld * (vertical * spacing);
    const float32 extent = static_cast<float32>(WORLD_GRID_HALF_LINE_COUNT) * spacing;
    const float32 halfWidth = math::Min(span / static_cast<float32>(grid.Height) * LINE_PIXELS, spacing * 0.5F) * 0.5F;
    if (math::IsFinite(centerWorld) == false || halfWidth <= 0 ||
        math::IsFinite(centerWorld + horizontalWorld * extent + verticalWorld * extent) == false ||
        math::IsFinite(centerWorld - horizontalWorld * extent - verticalWorld * extent) == false)
    {
        return false;
    }
    usize count = 0;
    for (int32 i = -WORLD_GRID_HALF_LINE_COUNT; i <= WORLD_GRID_HALF_LINE_COUNT; ++i)
    {
        const float32 offset = static_cast<float32>(i) * spacing;
        const math::Vector3 horizontalCenterWorld = centerWorld + verticalWorld * offset;
        const math::Vector3 verticalCenterWorld = centerWorld + horizontalWorld * offset;
        outLines[count++] = {horizontalCenterWorld - horizontalWorld * extent,
                             horizontalCenterWorld + horizontalWorld * extent,
                             verticalWorld * halfWidth,
                             GetGridColor(vertical + static_cast<float32>(i))};
        outLines[count++] = {verticalCenterWorld - verticalWorld * extent,
                             verticalCenterWorld + verticalWorld * extent,
                             horizontalWorld * halfWidth,
                             GetGridColor(horizontal + static_cast<float32>(i))};
    }
    const auto addAxis = [&](math::Vector3 axisWorld, math::Vector4 color) {
        math::Vector3 widthWorld;
        if (math::TryNormalize(math::Cross(axisWorld, forwardWorld), widthWorld) != math::MathStatus::Success)
        {
            widthWorld = verticalWorld;
        }
        outLines[count++] = {-axisWorld * extent, axisWorld * extent, widthWorld * halfWidth, color};
    };
    addAxis({1, 0, 0}, X_COLOR);
    addAxis({0, 1, 0}, Y_COLOR);
    addAxis({0, 0, 1}, Z_COLOR);
    outputs.Count = count;
    outputs.SpacingWorld = spacing;
    return true;
}
} // namespace ludus::graphics::renderer
