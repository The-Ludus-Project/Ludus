// Installed Ludus::FoundationGeometry: prepare a path and track, build a
// Certified distance map and invert a distance, then prepare a twisted patch and
// build a mesh with a Certified bound, through public headers only.

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/distance.hpp>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/surface.hpp>
#include <ludus/foundation/geometry/tessellation.hpp>
#include <ludus/foundation/geometry/track.hpp>
#include <ludus/foundation/math/enclosure.hpp>

bool ExerciseInstalledGeometry() noexcept
{
    using namespace ludus::foundation::geometry;
    using ludus::foundation::BorrowedView;
    using ludus::foundation::math::Vector3d;

    PathSpanSource span;
    span.Kind = SpanKind::Line;
    span.Control[0] = Vector3d{0.0, 0.0, 0.0};
    span.Control[3] = Vector3d{4.0, 0.0, 0.0};
    const ToleranceProfile tolerance
    {
        .PositionError = 1e-5,
        .DistanceError = 1e-6,
        .TangentAngleError = 1e-3,
        .ProjectionGap = 1e-5,
        .ContinuityAngle = 1e-6,
        .NormalAngleError = 1e-2,
        .CoincidenceDistance = 1e-6,
    };
    const WorkLimits limits
    {
        .MaxSpans = 16,
        .MaxLeaves = 64,
        .MaxDepth = 8,
        .MaxSubdivisions = 64,
        .MaxIterations = 128,
        .MaxPreparedBytes = 1U << 16U,
        .MaxPatches = 4,
    };
    PreparedPath path;
    PreparedDistanceMap map;
    DistanceResolution resolution;
    const bool isPathOk =
        TryPreparePath(PathSource{ .Spans = BorrowedView<const PathSpanSource>(&span, 1) },
                       PathPreparation{ .Tolerance = tolerance, .Limits = limits },
                       path,
                       nullptr) == GeometryStatus::Success &&
        TryBuildDistanceMap(path.GetView(), DistancePreparation{ .Limits = limits }, map, nullptr) ==
            GeometryStatus::Success &&
        TryResolveDistance(path.GetView(),
                           map.GetView(),
                           DistanceQuery{ .Distance = PathDistance{ .Value = 1.0 }, .MaxIterations = 128 },
                           resolution) == GeometryStatus::Success &&
        resolution.Guarantee == GuaranteeClass::Certified && resolution.ResidualBound <= 1e-6;

    constexpr ludus::foundation::usize KEY_COUNT = 2;
    const ludus::foundation::core::StaticArray<TrackKey, KEY_COUNT> keys{{
        TrackKey{ .Time = 0.0, .Value = 0.0F },
        TrackKey{ .Time = 2.0, .Value = 4.0F },
    }};
    PreparedTrack track;
    TrackSample sample;
    const bool isTrackOk =
        TryPrepareTrack(BorrowedView<const TrackKey>(keys.GetData(), keys.GetSize()), limits, track, nullptr) ==
            GeometryStatus::Success &&
        TryEvaluateTrack(track.GetView(),
                         TrackTime{ .Seconds = 3.0 },
                         BoundaryPolicy::Loop,
                         SpanSide::Outgoing,
                         sample) == GeometryStatus::Success &&
        sample.Value == 2.0F && sample.DerivativePerSecond == 2.0F;

    // The saddle z = 9uv as a bicubic patch: one cell's triangle pair deviates by exactly the 9/4 twist bound.
    SurfacePatchSource patch;
    patch.Id = 1;
    for (ludus::foundation::usize i = 0; i < PATCH_CONTROL_ORDER; ++i)
    {
        for (ludus::foundation::usize j = 0; j < PATCH_CONTROL_ORDER; ++j)
        {
            const ludus::foundation::core::float64 u = static_cast<ludus::foundation::core::float64>(i);
            const ludus::foundation::core::float64 v = static_cast<ludus::foundation::core::float64>(j);
            patch.Control[i][j] = Vector3d{u, v, u * v};
        }
    }
    PreparedSurface surface;
    SurfaceMesh mesh;
    const SurfaceMeshProfile profile
    {
        .PositionError = 2.25 * 1.001,
        .Degeneracy = SurfaceDegeneracyPolicy::Report,
        .Limits =
            SurfaceMeshLimits
            {
                .MaxCellTests = 4,
                .MaxVertices = 16,
                .MaxTriangles = 16,
                .MaxOutputBytes = 1U << 16U,
            },
    };
    const bool isSurfaceOk =
        TryPrepareSurface(
            SurfaceSource
            {
                .Patches = BorrowedView<const SurfacePatchSource>(&patch, 1),
                .Adjacency = {},
            },
            SurfacePreparation{ .Tolerance = tolerance, .Limits = limits },
            surface,
            nullptr) == GeometryStatus::Success &&
        TryBuildSurfaceMesh(surface.GetView(), profile, mesh, nullptr) == GeometryStatus::Success &&
        mesh.GetView().Report.PositionGuarantee == GuaranteeClass::Certified &&
        mesh.GetView().Report.PositionBound >= 2.25 && mesh.GetView().Report.PositionBound <= 2.25 * (1.0 + 1e-12) &&
        mesh.GetView().Indices.GetSize() == 6;
    return isPathOk == true && isTrackOk == true && isSurfaceOk == true &&
           IsStrictGuarantee(GuaranteeClass::Certified) == true;
}
