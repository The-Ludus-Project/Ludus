#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer
{
struct ViewDescription;
/// Number of minor grid intervals on either side of the snapped camera focus.
inline constexpr foundation::int32 WORLD_GRID_HALF_LINE_COUNT = 40;
/// Number of Cartesian coordinate axes in a world grid.
inline constexpr foundation::usize WORLD_GRID_AXIS_COUNT = 3;
/// Maximum line ribbons in one world grid, including its coordinate axes.
inline constexpr foundation::usize WORLD_GRID_LINE_CAPACITY =
    2 * (2 * static_cast<foundation::usize>(WORLD_GRID_HALF_LINE_COUNT) + 1) + WORLD_GRID_AXIS_COUNT;
/// Optional camera-relative world guide. It is never part of a scene snapshot or shadow caster set.
struct WorldGridDescription final
{
    /// Draw the guide with the view's ordinary depth convention. Disabled by default.
    bool IsVisible = false;
    /// Positive finite camera focus distance in world units, at most ten million.
    foundation::float32 FocusDistance = 1;
    /// Positive drawable width in pixels when visible; controls grid coverage on wide views.
    foundation::uint32 Width = 0;
    /// Positive drawable height in pixels when visible; controls ribbon thickness.
    foundation::uint32 Height = 0;
};
/// One opaque world-space line ribbon. Its four corners are start/end plus/minus HalfWidthWorld.
struct WorldGridLine final
{
    /// Center of the ribbon's first edge, in world units.
    foundation::math::Vector3 StartWorld{};
    /// Center of the ribbon's last edge, in world units.
    foundation::math::Vector3 EndWorld{};
    /// Half-width vector perpendicular to the line, in world units.
    foundation::math::Vector3 HalfWidthWorld{};
    /// Straight linear RGBA guide color; alpha is one.
    foundation::math::Vector4 Color{};
};
/// Build an adaptive XZ grid (perspective), or the Cartesian plane most facing an orthographic
/// camera, plus red X, green Y and blue Z axes. Storage is borrowed only during this call.
/// @param view Camera, projection and optional WorldGrid inputs. Shadow caster views omit the guide.
/// @param outLines Valid caller-owned storage of at least WORLD_GRID_LINE_CAPACITY elements.
/// @param outCount Receives the number of initialized lines; zero for a disabled grid.
/// @param outSpacing Receives minor spacing in world units; zero for a disabled grid.
/// @return False for insufficient storage or invalid visible-grid camera inputs, preserving all
/// outputs. Success writes only the first outCount elements. No allocation, GPU work or thread affinity.
[[nodiscard]] bool BuildWorldGridLines(const ViewDescription& view,
                                       foundation::BorrowedView<WorldGridLine> outLines,
                                       foundation::usize& outCount,
                                       foundation::float32& outSpacing) noexcept;
} // namespace ludus::graphics::renderer
