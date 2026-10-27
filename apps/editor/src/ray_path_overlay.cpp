#include "internal/ray_path_overlay.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::editor
{
using namespace foundation;
namespace renderer = graphics::renderer;
namespace
{
constexpr math::Vector4 CLOSEST_HIT_COLOR{1, .55F, .1F, 1};
constexpr math::Vector4 CLOSEST_MISS_COLOR{.35F, .7F, 1, 1};
constexpr math::Vector4 UNOCCLUDED_COLOR{.3F, 1, .4F, 1};
constexpr math::Vector4 OCCLUDED_COLOR{.85F, .2F, .2F, 1};
constexpr math::Vector4 RASTER_FIRST_HIT_COLOR{1, .95F, .3F, 1};
/// The editor's error color, shared with failed-material diagnostics.
constexpr math::Vector4 FAILURE_COLOR{1, 0, 1, 1};
constexpr math::Vector4 FRUSTUM_COLOR{.75F, .75F, .75F, 1};
constexpr math::Vector4 FOOTPRINT_COLOR{1, 1, 0, 1};
constexpr usize DEPTH_COLOR_COUNT = 8;
constexpr StaticArray<math::Vector4, DEPTH_COLOR_COUNT> DEPTH_COLORS{
    {
        {1, .95F, .3F, 1},
        {1, .55F, .1F, 1},
        {1, .2F, .2F, 1},
        {.9F, .2F, .8F, 1},
        {.55F, .35F, 1, 1},
        {.25F, .55F, 1, 1},
        {.2F, .85F, .9F, 1},
        {.3F, 1, .4F, 1},
    },
};
constexpr float32 LINE_WIDTH_PIXELS = 2;
constexpr float32 SELECTED_WIDTH_PIXELS = 4;
constexpr float32 THIN_WIDTH_PIXELS = 1;
/// Alpha of a truncated path's events, which must never read as a complete path.
constexpr float32 TRUNCATED_ALPHA = .45F;
/// Alpha of unselected events while one event is highlighted.
constexpr float32 UNSELECTED_ALPHA = .5F;
/// Alpha of the tail drawn past a miss's exit from the scene bounds.
constexpr float32 MISS_TAIL_ALPHA = .3F;
/// Miss tail length and hit marker size, as fractions of the scene bounds' diagonal.
constexpr float32 MISS_TAIL_FRACTION = .1F;
constexpr float32 MARKER_FRACTION = .005F;
/// Image plane of the frustum and footprint, as a fraction of the way from the near to the far plane.
constexpr float32 IMAGE_PLANE_FRACTION = .1F;
/// Homogeneous w below which an unprojected corner is treated as singular.
constexpr float32 MINIMUM_CLIP_W = 1e-8F;
constexpr usize FRUSTUM_CORNERS = 4;
/// Lines of a hit marker: one per world axis.
constexpr usize MARKER_AXES = 3;

bool IsSameInvocation(const renderer::RayPathEvent& a, const renderer::RayPathEvent& b) noexcept
{
    return a.PixelX == b.PixelX && a.PixelY == b.PixelY && a.Sample == b.Sample && a.Pass == b.Pass;
}

bool Unproject(const math::Matrix4& clipToWorld, math::Vector3 clip, math::Vector3& outWorld) noexcept
{
    const math::Vector4 world = clipToWorld * math::Vector4{clip.X, clip.Y, clip.Z, 1};
    if (math::Abs(world.W) < MINIMUM_CLIP_W)
    {
        return false;
    }
    outWorld = {world.X / world.W, world.Y / world.W, world.Z / world.W};
    return math::IsFinite(outWorld.X) == true && math::IsFinite(outWorld.Y) == true &&
           math::IsFinite(outWorld.Z) == true;
}

/// Image-plane corners of a clip-space rectangle: the near and far corners lerped by the same
/// fraction lie on one plane parallel to both, for perspective and orthographic cameras alike.
bool GetPlaneCorners(const math::Matrix4& clipToWorld,
                     const StaticArray<math::Vector2, FRUSTUM_CORNERS>& rectangle,
                     StaticArray<math::Vector3, FRUSTUM_CORNERS>& outNear,
                     StaticArray<math::Vector3, FRUSTUM_CORNERS>& outPlane) noexcept
{
    for (usize i = 0; i < FRUSTUM_CORNERS; ++i)
    {
        math::Vector3 farWorld;
        if (Unproject(clipToWorld, {rectangle[i].X, rectangle[i].Y, 0}, outNear[i]) == false ||
            Unproject(clipToWorld, {rectangle[i].X, rectangle[i].Y, 1}, farWorld) == false)
        {
            return false;
        }
        outPlane[i] = outNear[i] + (farWorld - outNear[i]) * IMAGE_PLANE_FRACTION;
    }
    return true;
}

math::Vector4 WithAlpha(math::Vector4 color, float32 alpha) noexcept
{
    return {color.X, color.Y, color.Z, color.W * alpha};
}

math::Vector4 GetKindColor(const renderer::RayPathEvent& event) noexcept
{
    if (event.Outcome == renderer::RayPathOutcome::Failure)
    {
        return FAILURE_COLOR;
    }
    switch (event.Kind)
    {
        case renderer::RayPathEventKind::Closest:
            return event.Outcome == renderer::RayPathOutcome::Hit ? CLOSEST_HIT_COLOR : CLOSEST_MISS_COLOR;
        case renderer::RayPathEventKind::Visibility:
            return event.Outcome == renderer::RayPathOutcome::Hit ? OCCLUDED_COLOR : UNOCCLUDED_COLOR;
        case renderer::RayPathEventKind::RasterFirstHit:
            return RASTER_FIRST_HIT_COLOR;
    }
    return FAILURE_COLOR;
}

/// Appends while capacity remains; returns false once a line was dropped.
struct LineSink final
{
    RayPathOverlay& Overlay;
    bool Add(math::Vector3 startWorld, math::Vector3 endWorld, math::Vector4 color, float32 widthPixels)
    {
        if (static_cast<usize>(Overlay.Lines.size()) >= MAX_RAY_PATH_LINES)
        {
            ++Overlay.DroppedLines;
            return false;
        }
        Overlay.Lines.append({startWorld, endWorld, color, widthPixels});
        return true;
    }
};

void AddRectangle(LineSink& inoutSink,
                  const StaticArray<math::Vector3, FRUSTUM_CORNERS>& corners,
                  math::Vector4 color,
                  float32 widthPixels)
{
    for (usize i = 0; i < FRUSTUM_CORNERS; ++i)
    {
        static_cast<void>(inoutSink.Add(corners[i], corners[(i + 1) % FRUSTUM_CORNERS], color, widthPixels));
    }
}

void AddCamera(LineSink& inoutSink, const RayPathCamera& camera)
{
    math::Matrix4 clipToWorld;
    if (math::TryInverse(camera.WorldToClip, clipToWorld) != math::MathStatus::Success)
    {
        return;
    }
    const StaticArray<math::Vector2, FRUSTUM_CORNERS> image{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    StaticArray<math::Vector3, FRUSTUM_CORNERS> nearWorld{};
    StaticArray<math::Vector3, FRUSTUM_CORNERS> planeWorld{};
    if (GetPlaneCorners(clipToWorld, image, nearWorld, planeWorld) == false)
    {
        return;
    }
    AddRectangle(inoutSink, nearWorld, FRUSTUM_COLOR, THIN_WIDTH_PIXELS);
    AddRectangle(inoutSink, planeWorld, FRUSTUM_COLOR, THIN_WIDTH_PIXELS);
    for (usize i = 0; i < FRUSTUM_CORNERS; ++i)
    {
        static_cast<void>(inoutSink.Add(nearWorld[i], planeWorld[i], FRUSTUM_COLOR, THIN_WIDTH_PIXELS));
    }
    if (camera.LaunchExtent.isEmpty() == true)
    {
        return;
    }
    // Pixel rows count down from the top, clip y up.
    const float32 width = static_cast<float32>(camera.LaunchExtent.width());
    const float32 height = static_cast<float32>(camera.LaunchExtent.height());
    const float32 left = 2 * static_cast<float32>(camera.PixelX) / width - 1;
    const float32 right = 2 * static_cast<float32>(camera.PixelX + 1) / width - 1;
    const float32 top = 1 - 2 * static_cast<float32>(camera.PixelY) / height;
    const float32 bottom = 1 - 2 * static_cast<float32>(camera.PixelY + 1) / height;
    const StaticArray<math::Vector2, FRUSTUM_CORNERS> pixel{
        {{left, bottom}, {right, bottom}, {right, top}, {left, top}}};
    if (GetPlaneCorners(clipToWorld, pixel, nearWorld, planeWorld) == true)
    {
        AddRectangle(inoutSink, planeWorld, FOOTPRINT_COLOR, LINE_WIDTH_PIXELS);
    }
}

/// End distance of a miss or failure: where the ray leaves the scene bounds, within its interval.
float32 GetOpenEndDistance(const renderer::RayPathEvent& event,
                           const RayPathOverlayOptions& options,
                           float32 directionLength) noexcept
{
    const float32 start = event.MinDistanceWorld;
    const float32 fallback = start + options.MissLengthWorld / directionLength;
    if (options.HasSceneBounds == false)
    {
        return math::Min(event.MaxDistanceWorld, fallback);
    }
    math::RayHit hit;
    const math::Ray3 ray{event.OriginWorld, event.DirectionWorld};
    const math::RayInterval interval{static_cast<float64>(start), static_cast<float64>(event.MaxDistanceWorld)};
    if (math::TryRayAabb(ray, options.SceneBoundsWorld, interval, hit) == math::MathStatus::Success &&
        hit.Result == math::HitResult::Hit)
    {
        return static_cast<float32>(hit.ExitT);
    }
    return math::Min(event.MaxDistanceWorld, fallback);
}
} // namespace

QVector<uint32> ComputeRayPathBounceDepths(BorrowedView<const renderer::RayPathEvent> events)
{
    QVector<uint32> depths;
    depths.reserve(static_cast<qsizetype>(events.GetSize()));
    uint32 closest = 0;
    for (usize i = 0; i < events.GetSize(); ++i)
    {
        if (i == 0 || IsSameInvocation(events[i], events[i - 1]) == false)
        {
            closest = 0;
        }
        if (events[i].Kind == renderer::RayPathEventKind::Visibility)
        {
            depths.append(closest == 0 ? 0 : closest - 1);
        }
        else
        {
            depths.append(closest);
            ++closest;
        }
    }
    return depths;
}

bool IsRayPathEventTruncated(const renderer::RayPathEvent& event,
                             BorrowedView<const renderer::RayPathSummary> summaries)
{
    for (usize i = 0; i < summaries.GetSize(); ++i)
    {
        const renderer::RayPathSummary& summary = summaries[i];
        if (summary.PixelX == event.PixelX && summary.PixelY == event.PixelY && summary.Sample == event.Sample &&
            summary.Pass == event.Pass)
        {
            return summary.IsTruncated;
        }
    }
    return false;
}

RayPathOverlay BuildRayPathOverlay(const RayPathCamera& camera,
                                   BorrowedView<const renderer::RayPathEvent> events,
                                   BorrowedView<const renderer::RayPathSummary> summaries,
                                   const RayPathOverlayOptions& options)
{
    RayPathOverlay overlay;
    overlay.IsXRay = options.IsXRay;
    LineSink sink{overlay};
    AddCamera(sink, camera);
    const QVector<uint32> depths = ComputeRayPathBounceDepths(events);
    const float32 diagonal =
        options.HasSceneBounds == true ? math::Length(options.SceneBoundsWorld.Max - options.SceneBoundsWorld.Min) : 0;
    const float32 markerSize = diagonal > 0 ? diagonal * MARKER_FRACTION : options.MissLengthWorld * MARKER_FRACTION;
    const float32 tailLength =
        diagonal > 0 ? diagonal * MISS_TAIL_FRACTION : options.MissLengthWorld * MISS_TAIL_FRACTION;
    const bool hasSelection =
        options.SelectedEvent >= 0 && static_cast<usize>(options.SelectedEvent) < events.GetSize();
    QVector<math::Vector3> hits;
    QVector<math::Vector4> hitColors;
    for (usize i = 0; i < events.GetSize(); ++i)
    {
        const renderer::RayPathEvent& event = events[i];
        if ((options.PassFilter >= 0 && event.Pass != static_cast<uint32>(options.PassFilter)) ||
            (options.SampleFilter >= 0 && event.Sample != static_cast<uint32>(options.SampleFilter)))
        {
            continue;
        }
        const float32 directionLength = math::Length(event.DirectionWorld);
        if (math::IsFinite(directionLength) == false || directionLength <= 0)
        {
            continue;
        }
        math::Vector4 color =
            options.ColorMode == RayPathColorMode::Kind || event.Outcome == renderer::RayPathOutcome::Failure
                ? GetKindColor(event)
                : DEPTH_COLORS[depths[static_cast<qsizetype>(i)] % DEPTH_COLOR_COUNT];
        float32 width = LINE_WIDTH_PIXELS;
        const bool isSelected = hasSelection == true && static_cast<usize>(options.SelectedEvent) == i;
        if (IsRayPathEventTruncated(event, summaries) == true)
        {
            color = WithAlpha(color, TRUNCATED_ALPHA);
            width = THIN_WIDTH_PIXELS;
        }
        if (hasSelection == true)
        {
            color = isSelected == true ? color : WithAlpha(color, UNSELECTED_ALPHA);
            width = isSelected == true ? SELECTED_WIDTH_PIXELS : width;
        }
        const bool isHit = event.Outcome == renderer::RayPathOutcome::Hit;
        const float32 end =
            isHit == true ? event.HitDistanceWorld : GetOpenEndDistance(event, options, directionLength);
        const math::Vector3 startWorld = event.OriginWorld + event.DirectionWorld * event.MinDistanceWorld;
        const math::Vector3 endWorld = event.OriginWorld + event.DirectionWorld * end;
        if (sink.Add(startWorld, endWorld, color, width) == false)
        {
            continue;
        }
        if (isHit == true && event.Kind != renderer::RayPathEventKind::Visibility)
        {
            hits.append(endWorld);
            hitColors.append(color);
        }
        else if (event.Outcome == renderer::RayPathOutcome::Miss &&
                 event.Kind != renderer::RayPathEventKind::Visibility)
        {
            const math::Vector3 tailWorld = endWorld + event.DirectionWorld * (tailLength / directionLength);
            static_cast<void>(sink.Add(endWorld, tailWorld, WithAlpha(color, MISS_TAIL_ALPHA), THIN_WIDTH_PIXELS));
        }
    }
    // Hit markers last: a small axis cross, kept only while capacity remains.
    for (qsizetype i = 0; i < hits.size(); ++i)
    {
        const math::Vector3 center = hits[i];
        const StaticArray<math::Vector3, MARKER_AXES> axes{
            {{markerSize, 0, 0}, {0, markerSize, 0}, {0, 0, markerSize}}};
        for (const math::Vector3& axis : axes)
        {
            static_cast<void>(sink.Add(center - axis, center + axis, hitColors[i], THIN_WIDTH_PIXELS));
        }
    }
    return overlay;
}

QString DescribeRayPathEvent(const renderer::RayPathEvent& event)
{
    QString kind;
    switch (event.Kind)
    {
        case renderer::RayPathEventKind::Closest:
            kind = QStringLiteral("Closest");
            break;
        case renderer::RayPathEventKind::Visibility:
            kind = QStringLiteral("Visibility");
            break;
        case renderer::RayPathEventKind::RasterFirstHit:
            kind = QStringLiteral("Raster first hit");
            break;
    }
    QString outcome;
    switch (event.Outcome)
    {
        case renderer::RayPathOutcome::Miss:
            outcome = event.Kind == renderer::RayPathEventKind::Visibility ? QStringLiteral("unoccluded")
                                                                           : QStringLiteral("miss");
            break;
        case renderer::RayPathOutcome::Hit:
            outcome = event.Kind == renderer::RayPathEventKind::Visibility ? QStringLiteral("occluded")
                                                                           : QStringLiteral("hit");
            break;
        case renderer::RayPathOutcome::Failure:
            outcome = QStringLiteral("failure");
            break;
    }
    return kind + QStringLiteral(" · ") + outcome;
}
} // namespace ludus::editor
