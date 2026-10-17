// Allocation probe for FoundationGeometry. Global operator new/delete are replaced
// with counting versions that can fail the N-th nothrow allocation. Every
// allocation of a successful preparation is failed in turn: the call must return
// OutOfMemory and leave the active owner unchanged. Sampling, inversion and
// conversion into caller-owned ranges must not allocate. Built without
// sanitizers, whose runtimes provide their own operator new/delete. Mirrors
// modules/foundation/containers/tests/allocation_tests.cpp.

#include <ludus/foundation/geometry/distance.hpp>
#include <ludus/foundation/geometry/frame.hpp>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/query.hpp>
#include <ludus/foundation/geometry/sweep.hpp>
#include <ludus/foundation/geometry/track.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <new>

using namespace ludus::foundation::geometry;
using ludus::foundation::BorrowedView;
using ludus::foundation::core::StaticArray;
using ludus::foundation::math::Vector3;
using ludus::foundation::math::Vector3d;

namespace
{
constexpr usize PROBE_SPAN_COUNT = 2;
constexpr usize PROBE_KEY_COUNT = 3;
constexpr usize PROBE_WAYPOINT_COUNT = 4;
constexpr usize PROBE_PROJECTION_ITERATIONS = 2048;

bool isCountingAllocations = false;
usize gAllocationCount = 0;
usize gFailAt = 0; ///< One-based counted nothrow allocation to fail; zero never fails.

void* AllocateAligned(usize bytes, usize alignment) noexcept
{
    if (alignment < alignof(std::max_align_t))
    {
        alignment = alignof(std::max_align_t);
    }
    usize rounded = (bytes + alignment - 1) & ~(alignment - 1);
    if (rounded == 0)
    {
        rounded = alignment;
    }
    return std::aligned_alloc(alignment, rounded);
}

// Counts the allocation and reports whether it must fail.
[[nodiscard]] bool ShouldFail() noexcept
{
    if (isCountingAllocations == false)
    {
        return false;
    }
    ++gAllocationCount;
    return gFailAt != 0 && gAllocationCount == gFailAt;
}

struct CountingScope final
{
    explicit CountingScope(usize failAt) noexcept
    {
        gAllocationCount = 0;
        gFailAt = failAt;
        isCountingAllocations = true;
    }
    CountingScope(const CountingScope&) = delete;
    CountingScope& operator=(const CountingScope&) = delete;
    ~CountingScope()
    {
        isCountingAllocations = false;
        gFailAt = 0;
    }
};

[[nodiscard]] ToleranceProfile MakeProfile() noexcept
{
    return ToleranceProfile
    {
        .PositionError = 1e-5,
        .DistanceError = 1e-6,
        .TangentAngleError = 1e-3,
        .ProjectionGap = 1e-5,
        .ContinuityPosition = 1e-6,
        .ContinuityDerivative = 1e-9,
        .ContinuityAngle = 1e-6,
        .NormalMinDerivativeLength = 1e-6,
        .NormalMinSinAngle = 1e-3,
        .NormalAngleError = 1e-2,
        .CoincidenceDistance = 1e-6,
    };
}

[[nodiscard]] WorkLimits MakeLimits() noexcept
{
    return WorkLimits
    {
        .MaxSpans = 1024,
        .MaxLeaves = 1U << 16U,
        .MaxDepth = 48,
        .MaxSubdivisions = 1U << 16U,
        .MaxIterations = 256,
        .MaxPreparedBytes = 1U << 24U,
    };
}

[[nodiscard]] StaticArray<PathSpanSource, PROBE_SPAN_COUNT> MakeSpans() noexcept
{
    StaticArray<PathSpanSource, PROBE_SPAN_COUNT> spans;
    spans[0].Control = {
        {Vector3d{-1.0, 3.0, 0.0}, Vector3d{1.0, -1.0, 0.0}, Vector3d{-1.0, -1.0, 0.0}, Vector3d{1.0, 3.0, 0.0}}};
    spans[1].Kind = SpanKind::Line;
    spans[1].Control[0] = Vector3d{1.0, 3.0, 0.0};
    spans[1].Control[3] = Vector3d{4.0, 3.0, 0.0};
    return spans;
}

// A regular twisted cubic followed by a G1 line, so frames have one run and sweeps are valid.
[[nodiscard]] StaticArray<PathSpanSource, PROBE_SPAN_COUNT> MakeRegularSpans() noexcept
{
    StaticArray<PathSpanSource, PROBE_SPAN_COUNT> spans;
    spans[0].Control = {
        {Vector3d{0.0, 0.0, 0.0}, Vector3d{1.0, 0.0, 0.0}, Vector3d{1.0, 1.0, 0.0}, Vector3d{1.0, 1.0, 1.0}}};
    spans[1].Kind = SpanKind::Line;
    spans[1].Control[0] = Vector3d{1.0, 1.0, 1.0};
    spans[1].Control[3] = Vector3d{1.0, 1.0, 3.0};
    return spans;
}

[[nodiscard]] FramePreparation MakeFramePreparation() noexcept
{
    WorkLimits limits = MakeLimits();
    limits.MaxDepth = 30;
    return FramePreparation{ .SeedUp = Vector3d{0.0, 1.0, 0.0}, .Limits = limits };
}

[[nodiscard]] SweepRequest MakeSweepRequest(BorrowedView<const SweepProfileKey> profile) noexcept
{
    WorkLimits limits = MakeLimits();
    limits.MaxDepth = 30;
    return SweepRequest
    {
        .Section = SweepSection::Tube,
        .Profile = profile,
        .Caps = SweepCaps::Flat,
        .LongitudinalError = 1e-3,
        .CrossSectionError = 1e-3,
        .MaxSectionSegments = 64,
        .MaxVertexCount = 1U << 20U,
        .MaxIndexCount = 1U << 22U,
        .Limits = limits,
    };
}
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(usize bytes)
{
    (void)ShouldFail();
    void* allocation = AllocateAligned(bytes, alignof(std::max_align_t));
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}
void* operator new(usize bytes, std::align_val_t alignment)
{
    (void)ShouldFail();
    void* allocation = AllocateAligned(bytes, static_cast<usize>(alignment));
    if (allocation == nullptr)
    {
        throw std::bad_alloc{};
    }
    return allocation;
}
void* operator new(usize bytes, const std::nothrow_t&) noexcept
{
    return ShouldFail() == true ? nullptr : AllocateAligned(bytes, alignof(std::max_align_t));
}
void* operator new(usize bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return ShouldFail() == true ? nullptr : AllocateAligned(bytes, static_cast<usize>(alignment));
}
void operator delete(void* allocationOrNull) noexcept
{
    std::free(allocationOrNull);
}
void operator delete(void* allocationOrNull, usize) noexcept
{
    std::free(allocationOrNull);
}
void operator delete(void* allocationOrNull, std::align_val_t) noexcept
{
    std::free(allocationOrNull);
}
void operator delete(void* allocationOrNull, usize, std::align_val_t) noexcept
{
    std::free(allocationOrNull);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

TEST_CASE("Every preparation allocation failure returns OutOfMemory and preserves the owner", "[geometry][alloc]")
{
    const StaticArray<PathSpanSource, PROBE_SPAN_COUNT> spans = MakeSpans();
    const PathSource source
    {
        .Spans = BorrowedView<const PathSpanSource>(spans.GetData(), spans.GetSize()),
        .Revision = 1,
    };
    const PathPreparation preparation{ .Tolerance = MakeProfile(), .Limits = MakeLimits() };
    PreparedPath active;
    REQUIRE(TryPreparePath(source, preparation, active, nullptr) == GeometryStatus::Success);
    PreparedDistanceMap activeMap;
    const DistancePreparation distancePreparation{ .Precision = PrecisionProfile::Strict, .Limits = MakeLimits() };
    REQUIRE(TryBuildDistanceMap(active.GetView(), distancePreparation, activeMap, nullptr) == GeometryStatus::Success);
    const usize activeLeafCount = activeMap.GetView().Leaves.GetSize();

    usize pathAllocations = 0;
    {
        PreparedPath probe;
        const CountingScope scope(0);
        const GeometryStatus status = TryPreparePath(source, preparation, probe, nullptr);
        pathAllocations = gAllocationCount;
        REQUIRE(status == GeometryStatus::Success);
    }
    REQUIRE(pathAllocations > 0);
    for (usize failAt = 1; failAt <= pathAllocations; ++failAt)
    {
        GeometryStatus status = GeometryStatus::Success;
        {
            const CountingScope scope(failAt);
            status = TryPreparePath(source, preparation, active, nullptr);
        }
        REQUIRE(status == GeometryStatus::OutOfMemory);
        REQUIRE(active.GetView().Spans.GetSize() == PROBE_SPAN_COUNT);
    }

    usize mapAllocations = 0;
    {
        PreparedDistanceMap probe;
        const CountingScope scope(0);
        const GeometryStatus status = TryBuildDistanceMap(active.GetView(), distancePreparation, probe, nullptr);
        mapAllocations = gAllocationCount;
        REQUIRE(status == GeometryStatus::Success);
    }
    REQUIRE(mapAllocations > 0);
    for (usize failAt = 1; failAt <= mapAllocations; ++failAt)
    {
        GeometryStatus status = GeometryStatus::Success;
        {
            const CountingScope scope(failAt);
            status = TryBuildDistanceMap(active.GetView(), distancePreparation, activeMap, nullptr);
        }
        REQUIRE(status == GeometryStatus::OutOfMemory);
        REQUIRE(activeMap.GetView().Leaves.GetSize() == activeLeafCount);
    }

    const StaticArray<TrackKey, PROBE_KEY_COUNT> keys{{
        TrackKey{ .Time = 0.0, .Value = 0.0F, .OutSlope = 1.0F, .Interpolation = TrackInterpolation::Hermite },
        TrackKey{ .Time = 1.0, .Value = 1.0F, .Interpolation = TrackInterpolation::Step },
        TrackKey{ .Time = 2.0, .Value = 0.0F },
    }};
    const BorrowedView<const TrackKey> keyView(keys.GetData(), keys.GetSize());
    PreparedTrack track;
    REQUIRE(TryPrepareTrack(keyView, MakeLimits(), track, nullptr) == GeometryStatus::Success);
    {
        GeometryStatus status = GeometryStatus::Success;
        {
            const CountingScope scope(1);
            status = TryPrepareTrack(keyView, MakeLimits(), track, nullptr);
        }
        REQUIRE(status == GeometryStatus::OutOfMemory);
        REQUIRE(track.GetView().Spans.GetSize() == PROBE_KEY_COUNT - 1);
    }
}

TEST_CASE("Sampling, inversion and caller-owned conversion never allocate", "[geometry][alloc]")
{
    const StaticArray<PathSpanSource, PROBE_SPAN_COUNT> spans = MakeSpans();
    const PathSource source{ .Spans = BorrowedView<const PathSpanSource>(spans.GetData(), spans.GetSize()) };
    PreparedPath path;
    REQUIRE(
        TryPreparePath(source, PathPreparation{ .Tolerance = MakeProfile(), .Limits = MakeLimits() }, path, nullptr) ==
        GeometryStatus::Success);
    PreparedDistanceMap map;
    REQUIRE(TryBuildDistanceMap(path.GetView(),
                                DistancePreparation{ .Precision = PrecisionProfile::Strict, .Limits = MakeLimits() },
                                map,
                                nullptr) == GeometryStatus::Success);
    const StaticArray<TrackKey, PROBE_KEY_COUNT> keys{{
        TrackKey{ .Time = 0.0, .Value = 0.0F, .OutSlope = 1.0F, .Interpolation = TrackInterpolation::Hermite },
        TrackKey{ .Time = 1.0, .Value = 1.0F, .Interpolation = TrackInterpolation::Linear },
        TrackKey{ .Time = 2.0, .Value = 0.0F },
    }};
    PreparedTrack track;
    REQUIRE(
        TryPrepareTrack(BorrowedView<const TrackKey>(keys.GetData(), keys.GetSize()), MakeLimits(), track, nullptr) ==
        GeometryStatus::Success);
    const StaticArray<Vector3d, PROBE_WAYPOINT_COUNT> waypoints{{
        Vector3d{0.0, 0.0, 0.0},
        Vector3d{1.0, 0.0, 0.0},
        Vector3d{1.0, 4.0, 0.0},
        Vector3d{10.0, 4.0, 0.0},
    }};
    StaticArray<PathSpanSource, PROBE_WAYPOINT_COUNT> converted;

    bool isEverySampleOk = true;
    usize allocations = 0;
    {
        const CountingScope scope(0);
        for (usize step = 0; step <= 32; ++step)
        {
            const float64 u = static_cast<float64>(step) / 32.0;
            ludus::foundation::math::CubicSample3 sample;
            Vector3 tangent;
            isEverySampleOk =
                isEverySampleOk == true &&
                TryEvaluatePath(path.GetView(), SegmentParameter{ .SpanIndex = 1, .U = u }, sample) ==
                    GeometryStatus::Success &&
                TryEvaluatePathTangent(path.GetView(), SegmentParameter{ .SpanIndex = 1, .U = u }, 0.0, tangent) ==
                    GeometryStatus::Success;
            DistanceResolution resolution;
            const DistanceQuery query
            {
                .Distance = PathDistance{ .Value = map.GetView().TotalEstimate * u },
                .Policy = BoundaryPolicy::Clamp,
                .MaxIterations = 256,
            };
            isEverySampleOk =
                isEverySampleOk == true &&
                TryResolveDistance(path.GetView(), map.GetView(), query, resolution) == GeometryStatus::Success;
            DistanceBound bound;
            isEverySampleOk =
                isEverySampleOk == true &&
                TryBoundDistance(path.GetView(), map.GetView(), SegmentParameter{ .SpanIndex = 0, .U = u }, bound) ==
                    GeometryStatus::Success;
            TrackSample trackSample;
            isEverySampleOk = isEverySampleOk == true && TryEvaluateTrack(track.GetView(),
                                                                          TrackTime{ .Seconds = 5.0 * u - 1.0 },
                                                                          BoundaryPolicy::PingPong,
                                                                          SpanSide::Outgoing,
                                                                          trackSample) == GeometryStatus::Success;
        }
        usize spanCount = 0;
        isEverySampleOk =
            isEverySampleOk == true &&
            TryConvertCentripetalCatmullRom(BorrowedView<const Vector3d>(waypoints.GetData(), waypoints.GetSize()),
                                            true,
                                            1e-6,
                                            BorrowedView<PathSpanSource>(converted.GetData(), converted.GetSize()),
                                            spanCount) == GeometryStatus::Success;
        allocations = gAllocationCount;
    }
    REQUIRE(isEverySampleOk == true);
    REQUIRE(allocations == 0);
}

TEST_CASE("Every frame and sweep plan allocation failure preserves the owners", "[geometry][alloc]")
{
    const StaticArray<PathSpanSource, PROBE_SPAN_COUNT> spans = MakeRegularSpans();
    const PathSource source
    {
        .Spans = BorrowedView<const PathSpanSource>(spans.GetData(), spans.GetSize()),
        .Revision = 1,
    };
    PreparedPath path;
    REQUIRE(
        TryPreparePath(source, PathPreparation{ .Tolerance = MakeProfile(), .Limits = MakeLimits() }, path, nullptr) ==
        GeometryStatus::Success);
    PreparedDistanceMap map;
    REQUIRE(TryBuildDistanceMap(path.GetView(),
                                DistancePreparation{ .Precision = PrecisionProfile::Strict, .Limits = MakeLimits() },
                                map,
                                nullptr) == GeometryStatus::Success);
    PreparedFrames frames;
    REQUIRE(TryBuildFrames(path.GetView(), map.GetView(), MakeFramePreparation(), frames, nullptr) ==
            GeometryStatus::Success);
    const usize sampleCount = frames.GetView().Samples.GetSize();
    usize frameAllocations = 0;
    {
        PreparedFrames probe;
        const CountingScope scope(0);
        REQUIRE(TryBuildFrames(path.GetView(), map.GetView(), MakeFramePreparation(), probe, nullptr) ==
                GeometryStatus::Success);
        frameAllocations = gAllocationCount;
    }
    REQUIRE(frameAllocations > 0);
    for (usize failAt = 1; failAt <= frameAllocations; ++failAt)
    {
        GeometryStatus status = GeometryStatus::Success;
        {
            const CountingScope scope(failAt);
            status = TryBuildFrames(path.GetView(), map.GetView(), MakeFramePreparation(), frames, nullptr);
        }
        REQUIRE(status == GeometryStatus::OutOfMemory);
        REQUIRE(frames.GetView().Samples.GetSize() == sampleCount);
    }

    const SweepProfileKey profile[] = {{ .Distance = PathDistance{}, .Scale = 0.05 }};
    const SweepRequest request = MakeSweepRequest(BorrowedView<const SweepProfileKey>(profile));
    SweepMeshPlan plan;
    REQUIRE(TryPlanSweepMesh(path.GetView(), map.GetView(), frames.GetView(), request, plan, nullptr) ==
            GeometryStatus::Success);
    const usize ringCount = plan.GetView().Report.RingCount;
    usize planAllocations = 0;
    {
        SweepMeshPlan probe;
        const CountingScope scope(0);
        REQUIRE(TryPlanSweepMesh(path.GetView(), map.GetView(), frames.GetView(), request, probe, nullptr) ==
                GeometryStatus::Success);
        planAllocations = gAllocationCount;
    }
    REQUIRE(planAllocations > 0);
    for (usize failAt = 1; failAt <= planAllocations; ++failAt)
    {
        GeometryStatus status = GeometryStatus::Success;
        {
            const CountingScope scope(failAt);
            status = TryPlanSweepMesh(path.GetView(), map.GetView(), frames.GetView(), request, plan, nullptr);
        }
        REQUIRE(status == GeometryStatus::OutOfMemory);
        REQUIRE(plan.GetView().Report.RingCount == ringCount);
    }

    ProjectionScratch scratch;
    {
        GeometryStatus status = GeometryStatus::Success;
        {
            const CountingScope scope(1);
            status = scratch.TryReserve(64);
        }
        REQUIRE(status == GeometryStatus::OutOfMemory);
    }
}

TEST_CASE("Frame evaluation, projection and sweep emission never allocate", "[geometry][alloc]")
{
    const StaticArray<PathSpanSource, PROBE_SPAN_COUNT> spans = MakeRegularSpans();
    const PathSource source{ .Spans = BorrowedView<const PathSpanSource>(spans.GetData(), spans.GetSize()) };
    PreparedPath path;
    REQUIRE(
        TryPreparePath(source, PathPreparation{ .Tolerance = MakeProfile(), .Limits = MakeLimits() }, path, nullptr) ==
        GeometryStatus::Success);
    PreparedDistanceMap map;
    REQUIRE(TryBuildDistanceMap(path.GetView(),
                                DistancePreparation{ .Precision = PrecisionProfile::Strict, .Limits = MakeLimits() },
                                map,
                                nullptr) == GeometryStatus::Success);
    PreparedFrames frames;
    REQUIRE(TryBuildFrames(path.GetView(), map.GetView(), MakeFramePreparation(), frames, nullptr) ==
            GeometryStatus::Success);
    const SweepProfileKey profile[] = {{ .Distance = PathDistance{}, .Scale = 0.05 }};
    SweepMeshPlan plan;
    REQUIRE(TryPlanSweepMesh(path.GetView(),
                             map.GetView(),
                             frames.GetView(),
                             MakeSweepRequest(BorrowedView<const SweepProfileKey>(profile)),
                             plan,
                             nullptr) == GeometryStatus::Success);
    ludus::foundation::core::Array<SweepVertex> vertices;
    ludus::foundation::core::Array<uint32> indices;
    REQUIRE(vertices.TryResize(plan.GetView().Report.VertexCount) == true);
    REQUIRE(indices.TryResize(plan.GetView().Report.IndexCount) == true);
    usize capacity = 0;
    REQUIRE(ComputeProjectionScratchCapacity(path.GetView(), PROBE_PROJECTION_ITERATIONS, capacity) ==
            GeometryStatus::Success);
    ProjectionScratch scratch;
    REQUIRE(scratch.TryReserve(capacity) == GeometryStatus::Success);

    bool isEverySampleOk = true;
    usize allocations = 0;
    {
        const CountingScope scope(0);
        for (usize step = 0; step <= 16; ++step)
        {
            const float64 u = static_cast<float64>(step) / 16.0;
            TransportedFrame frame;
            isEverySampleOk =
                isEverySampleOk == true &&
                TryEvaluateFrame(path.GetView(), frames.GetView(), SegmentParameter{ .U = u }, 0.5, frame) ==
                    GeometryStatus::Success;
            ProjectionResult result;
            const ProjectionQuery query
            {
                .Point = Vector3d{u, 2.0 * u, 1.0},
                .MaxIterations = PROBE_PROJECTION_ITERATIONS,
            };
            isEverySampleOk =
                isEverySampleOk == true &&
                TryProjectPoint(path.GetView(), query, scratch, result) == GeometryStatus::Success &&
                ProjectPointApproximate(path.GetView(), query, scratch, result) == GeometryStatus::Success;
        }
        isEverySampleOk =
            isEverySampleOk == true &&
            TryEmitSweepMesh(path.GetView(),
                             plan.GetView(),
                             BorrowedView<SweepVertex>(vertices.GetData(), vertices.GetSize()),
                             BorrowedView<uint32>(indices.GetData(), indices.GetSize())) == GeometryStatus::Success;
        allocations = gAllocationCount;
    }
    REQUIRE(isEverySampleOk == true);
    REQUIRE(allocations == 0);
}
