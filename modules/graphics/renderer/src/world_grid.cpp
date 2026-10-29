#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/world_grid.hpp>

#include <ludus/foundation/containers/static_array.hpp>
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
constexpr float32 AXIS_PIXELS = 2;
// Linear values of the design tokens grid.minor (#303e49) and grid.major (#4a5a65), which measure
// 1.5:1 and 2.2:1 against the viewport surface, the contrast of a guide that is not text.
constexpr math::Vector4 MINOR_COLOR{.0296F, .0482F, .0666F, 1};
constexpr math::Vector4 MAJOR_COLOR{.0685F, .1022F, .1301F, 1};
constexpr float32 PLANE_EXTENT_SPACINGS = 100;
constexpr float32 PLANE_FADE_START_SPACINGS = 40;
constexpr float32 HALF = 0.5F;
constexpr math::Vector4 X_COLOR{.6F, .07F, .07F, 1};
constexpr math::Vector4 Y_COLOR{.11F, .46F, .13F, 1};
constexpr math::Vector4 Z_COLOR{.08F, .24F, .72F, 1};
/// Caller-owned count and spacing, borrowed together only during grid generation.
struct WorldGridOutputs final
{
    usize& Count;
    float32& SpacingWorld;
};
usize GetAxisIndex(const math::Vector3& axisWorld) noexcept
{
    if (math::Abs(axisWorld.X) >= math::Abs(axisWorld.Y) && math::Abs(axisWorld.X) >= math::Abs(axisWorld.Z))
    {
        return 0;
    }
    return math::Abs(axisWorld.Y) >= math::Abs(axisWorld.Z) ? 1 : 2;
}
math::Vector4 GetAxisColor(usize axisIndex) noexcept
{
    constexpr StaticArray<math::Vector4, WORLD_GRID_AXIS_COUNT> COLOURS{{X_COLOR, Y_COLOR, Z_COLOR}};
    return COLOURS[axisIndex];
}
math::Vector4 GetGridColor(float32 coordinate) noexcept
{
    const float32 decade = static_cast<float32>(math::Floor(static_cast<float64>(coordinate / GRID_DECADE)));
    return coordinate == decade * GRID_DECADE ? MAJOR_COLOR : MINOR_COLOR;
}
/// Everything both builders derive from a visible grid view.
struct GridFrame final
{
    math::Vector3 ForwardWorld{};
    math::Vector3 HorizontalWorld{1, 0, 0};
    math::Vector3 VerticalWorld{0, 0, 1};
    math::Vector3 CenterWorld{};
    float32 SpacingWorld = 1;
    float32 HalfWidthWorld = 0;
    float32 HorizontalIndex = 0;
    float32 VerticalIndex = 0;
};
bool BuildGridFrame(const ViewDescription& view, GridFrame& outFrame) noexcept
{
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
    const float32 halfWidth = math::Min(span / static_cast<float32>(grid.Height) * LINE_PIXELS, spacing * HALF) * HALF;
    if (math::IsFinite(centerWorld) == false || halfWidth <= 0 ||
        math::IsFinite(centerWorld + horizontalWorld * extent + verticalWorld * extent) == false ||
        math::IsFinite(centerWorld - horizontalWorld * extent - verticalWorld * extent) == false)
    {
        return false;
    }
    outFrame = {forwardWorld, horizontalWorld, verticalWorld, centerWorld, spacing, halfWidth, horizontal, vertical};
    return true;
}
} // namespace
bool BuildWorldGridPlane(const ViewDescription& view, WorldGridPlane& outPlane, bool& outIsVisible) noexcept
{
    outIsVisible = view.WorldGrid.IsVisible == true && view.ShadowCasters == false;
    if (outIsVisible == false)
    {
        return true;
    }
    GridFrame frame;
    if (BuildGridFrame(view, frame) == false)
    {
        return false;
    }
    const float32 extent = PLANE_EXTENT_SPACINGS * frame.SpacingWorld;
    const usize horizontalAxis = GetAxisIndex(frame.HorizontalWorld);
    const usize verticalAxis = GetAxisIndex(frame.VerticalWorld);
    if (horizontalAxis == verticalAxis || math::IsFinite(extent) == false ||
        math::IsFinite(frame.CenterWorld + frame.HorizontalWorld * extent + frame.VerticalWorld * extent) == false)
    {
        return false;
    }
    outPlane = {frame.CenterWorld,
                frame.HorizontalWorld,
                frame.VerticalWorld,
                frame.SpacingWorld,
                extent,
                PLANE_FADE_START_SPACINGS * frame.SpacingWorld,
                LINE_PIXELS,
                AXIS_PIXELS,
                MINOR_COLOR,
                MAJOR_COLOR,
                GetAxisColor(horizontalAxis),
                GetAxisColor(verticalAxis),
                WORLD_GRID_AXIS_COUNT - horizontalAxis - verticalAxis};
    return true;
}
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
    GridFrame frame;
    if (BuildGridFrame(view, frame) == false)
    {
        return false;
    }
    const math::Vector3 forwardWorld = frame.ForwardWorld;
    const math::Vector3 horizontalWorld = frame.HorizontalWorld;
    const math::Vector3 verticalWorld = frame.VerticalWorld;
    const math::Vector3 centerWorld = frame.CenterWorld;
    const float32 spacing = frame.SpacingWorld;
    const float32 halfWidth = frame.HalfWidthWorld;
    const float32 extent = static_cast<float32>(WORLD_GRID_HALF_LINE_COUNT) * spacing;
    const float32 vertical = frame.VerticalIndex;
    const float32 horizontal = frame.HorizontalIndex;
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
