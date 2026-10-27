#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>

#include <QSize>
#include <QString>
#include <QVector>

// World-space drawing of a frozen ray path capture: the event segments, the frozen camera's
// frustum and the selected pixel's footprint. Pure CPU geometry; RenderPreview projects the
// lines with the free editor camera each frame. See "Drawing and inspection" in
// docs/architecture/render-graph-authoring.md#ray-path-inspection-proposed.
namespace ludus::editor
{
/// Most lines one overlay draws: OverlayList::MAX_VERTICES (3072) holds 512 six-vertex lines.
inline constexpr foundation::usize MAX_RAY_PATH_LINES = 512;
/// Length of a miss segment when no scene bounds are known, in world meters.
inline constexpr foundation::float32 DEFAULT_RAY_PATH_MISS_LENGTH_WORLD = 10;

/// How event segments are colored.
enum class RayPathColorMode : foundation::uint8
{
    Kind,        ///< By query kind and outcome: camera/bounce hit, miss, visibility, raster first hit, failure.
    BounceDepth, ///< By the event's index among closest queries of its sample and pass.
};

/// One world-space line of the overlay.
struct RayPathLine final
{
    foundation::math::Vector3 StartWorld{};
    foundation::math::Vector3 EndWorld{};
    /// Straight linear display RGBA in [0,1].
    foundation::math::Vector4 Color{1, 1, 1, 1};
    /// Width in physical pixels, at least one.
    foundation::float32 WidthPixels = 2;
};

/// Lines RenderPreview composes into its raster frames while ray paths are inspected.
struct RayPathOverlay final
{
    QVector<RayPathLine> Lines;
    /// Draw through geometry (OverlayDepth::Always) instead of depth-testing against the scene.
    bool IsXRay = false;
    /// Lines left out because MAX_RAY_PATH_LINES was reached.
    foundation::usize DroppedLines = 0;
};

/// The frozen graph camera and pixel a capture traced, as the overlay draws them.
struct RayPathCamera final
{
    /// Graph camera world-to-clip transform (conventional depth: clip z 0 near, 1 far).
    foundation::math::Matrix4 WorldToClip = foundation::math::Matrix4::Identity();
    /// Launch extent of the traced ray pass, in pixels; zero hides the footprint.
    QSize LaunchExtent;
    foundation::uint32 PixelX = 0; ///< Launch pixel column.
    foundation::uint32 PixelY = 0; ///< Launch pixel row, top row zero.
};

/// What BuildRayPathOverlay draws and how.
struct RayPathOverlayOptions final
{
    RayPathColorMode ColorMode = RayPathColorMode::Kind;
    bool IsXRay = false;
    /// Plan pass whose events are drawn; negative draws every pass.
    foundation::int32 PassFilter = -1;
    /// Progressive sample whose events are drawn; negative draws every sample.
    foundation::int32 SampleFilter = -1;
    /// Index into the event list of the highlighted event; negative highlights none.
    foundation::int32 SelectedEvent = -1;
    /// World bounds of the scene; a miss is drawn to where it leaves them. Without bounds,
    /// misses use MissLengthWorld.
    foundation::math::Aabb3 SceneBoundsWorld{};
    bool HasSceneBounds = false;
    /// Length of a miss segment when no scene bounds are known, in world meters.
    foundation::float32 MissLengthWorld = DEFAULT_RAY_PATH_MISS_LENGTH_WORLD;
};

/// Index of an event among the closest-hit queries (Closest and RasterFirstHit) of its sample
/// and pass, which the overlay colors as bounce depth; visibility queries report the depth of
/// the closest query they follow. Events must be sorted by pixel, sample, pass and sequence.
/// @return One depth per event, parallel to events.
[[nodiscard]] QVector<foundation::uint32>
ComputeRayPathBounceDepths(foundation::BorrowedView<const graphics::renderer::RayPathEvent> events);

/// Whether the summary of an event's invocation reports a truncated path.
/// @return False when no summary matches the event.
[[nodiscard]] bool
IsRayPathEventTruncated(const graphics::renderer::RayPathEvent& event,
                        foundation::BorrowedView<const graphics::renderer::RayPathSummary> summaries);

/// Build the overlay lines of a capture: the frozen frustum (to an image plane between its near
/// and far planes), the selected pixel's footprint on that plane, then one segment per event,
/// from Origin + Min * Direction to the hit (misses to the scene bounds, with a faded tail).
/// Events of truncated paths are drawn thin and translucent, never as complete paths. Lines past
/// MAX_RAY_PATH_LINES are dropped and counted, events first in order, so the frustum stays.
/// @param camera Frozen graph camera; a singular WorldToClip omits the frustum and footprint.
/// @param events Decoded events, sorted as RenderGraph::GetRayPathEvents returns them.
/// @param summaries Decoded summaries of the same capture.
[[nodiscard]] RayPathOverlay
BuildRayPathOverlay(const RayPathCamera& camera,
                    foundation::BorrowedView<const graphics::renderer::RayPathEvent> events,
                    foundation::BorrowedView<const graphics::renderer::RayPathSummary> summaries,
                    const RayPathOverlayOptions& options);

/// Short human-readable kind and outcome of an event, for the inspector ("Closest · hit").
[[nodiscard]] QString DescribeRayPathEvent(const graphics::renderer::RayPathEvent& event);
} // namespace ludus::editor
