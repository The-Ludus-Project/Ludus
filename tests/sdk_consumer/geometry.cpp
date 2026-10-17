// Installed Ludus::FoundationGeometry: prepare a path and track, build a
// Certified distance map and invert a distance, then bake frames, project a
// point and emit a ribbon sweep through public headers only.

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/distance.hpp>
#include <ludus/foundation/geometry/frame.hpp>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/query.hpp>
#include <ludus/foundation/geometry/sweep.hpp>
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

    PreparedFrames frames;
    TransportedFrame frame;
    const bool isFrameOk = isPathOk == true &&
                           TryBuildFrames(path.GetView(),
                                          map.GetView(),
                                          FramePreparation{ .SeedUp = Vector3d{0.0, 1.0, 0.0}, .Limits = limits },
                                          frames,
                                          nullptr) == GeometryStatus::Success &&
                           TryEvaluateFrame(path.GetView(),
                                            frames.GetView(),
                                            SegmentParameter
                                            {
                                                .U = 0.5,
                                            },
                                            0.0,
                                            frame) == GeometryStatus::Success &&
                           frame.Binormal.Z == 1.0F;

    constexpr ludus::foundation::usize PROJECTION_ITERATIONS = 64;
    ProjectionScratch scratch;
    ludus::foundation::usize capacity = 0;
    ProjectionResult nearest;
    const bool isProjectionOk =
        isPathOk == true &&
        ComputeProjectionScratchCapacity(path.GetView(), PROJECTION_ITERATIONS, capacity) == GeometryStatus::Success &&
        scratch.TryReserve(capacity) == GeometryStatus::Success &&
        TryProjectPoint(path.GetView(),
                        ProjectionQuery{ .Point = Vector3d{2.0, 1.0, 0.0}, .MaxIterations = PROJECTION_ITERATIONS },
                        scratch,
                        nearest) == GeometryStatus::Success &&
        nearest.Guarantee == GuaranteeClass::Certified && nearest.Gap <= tolerance.ProjectionGap;

    constexpr ludus::foundation::usize RIBBON_VERTEX_COUNT = 4;
    constexpr ludus::foundation::usize RIBBON_INDEX_COUNT = 6;
    const SweepProfileKey profile[] = {{ .Scale = 1.0 }};
    SweepMeshPlan plan;
    ludus::foundation::core::StaticArray<SweepVertex, RIBBON_VERTEX_COUNT> vertices;
    ludus::foundation::core::StaticArray<uint32, RIBBON_INDEX_COUNT> indices;
    const bool isSweepOk =
        isFrameOk == true &&
        TryPlanSweepMesh(path.GetView(),
                         map.GetView(),
                         frames.GetView(),
                         SweepRequest
                         {
                             .Profile = BorrowedView<const SweepProfileKey>(profile),
                             .LongitudinalError = 1e-3,
                             .CrossSectionError = 1e-3,
                             .MaxVertexCount = RIBBON_VERTEX_COUNT,
                             .MaxIndexCount = RIBBON_INDEX_COUNT,
                             .Limits = limits,
                         },
                         plan,
                         nullptr) == GeometryStatus::Success &&
        TryEmitSweepMesh(path.GetView(),
                         plan.GetView(),
                         BorrowedView<SweepVertex>(vertices.GetData(), vertices.GetSize()),
                         BorrowedView<uint32>(indices.GetData(), indices.GetSize())) == GeometryStatus::Success;
    return isPathOk == true && isTrackOk == true && isFrameOk == true && isProjectionOk == true && isSweepOk == true &&
           IsStrictGuarantee(GuaranteeClass::Certified) == true;
}
