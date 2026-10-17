// Sweep planning and emission: ribbons and tubes with caps, UV seams, winding, closed seams, profiles,
// error and output budgets, self-intersection risk and transactional emission.

#include "geometry_test_support.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/geometry/frame.hpp>
#include <ludus/foundation/geometry/sweep.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation::geometry;
using namespace ludus::foundation::geometry::test_support;
using ludus::foundation::BorrowedView;
using ludus::foundation::core::Array;
namespace math = ludus::foundation::math;
using math::Vector3;
using math::Vector3d;

namespace
{
// Float32 storage of emitted values near unit scale (regression envelope, not a bound).
constexpr float64 VERTEX_TOLERANCE = 1e-5;
// Budget margin for the Estimated longitudinal error checked at extra samples (regression envelope).
constexpr float64 ESTIMATE_MARGIN = 2.0;
// Independent samples per ring interval for the longitudinal check.
constexpr usize CHECK_SAMPLES = 7;
constexpr usize OUTPUT_LIMIT = 1U << 20U;

[[nodiscard]] Vector3d Widen(const Vector3& value) noexcept
{
    return Vector3d{static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

[[nodiscard]] SweepRequest MakeRequest(SweepSection section,
                                       BorrowedView<const SweepProfileKey> profile,
                                       SweepCaps caps = SweepCaps::None) noexcept
{
    WorkLimits limits = MakeLimits();
    limits.MaxDepth = FRAME_TEST_DEPTH;
    return SweepRequest
    {
        .Section = section,
        .Profile = profile,
        .Caps = caps,
        .LongitudinalError = 1e-3,
        .CrossSectionError = 1e-3,
        .MaxSectionSegments = 256,
        .MaxVertexCount = OUTPUT_LIMIT,
        .MaxIndexCount = OUTPUT_LIMIT,
        .Limits = limits,
    };
}

struct Mesh final
{
    Array<SweepVertex> Vertices;
    Array<uint32> Indices;
};

[[nodiscard]] GeometryStatus Emit(const PathView& path, const SweepMeshPlan& plan, Mesh& outMesh) noexcept
{
    const SweepMeshReport& report = plan.GetView().Report;
    if (outMesh.Vertices.TryResize(report.VertexCount) == false ||
        outMesh.Indices.TryResize(report.IndexCount) == false)
    {
        return GeometryStatus::OutOfMemory;
    }
    return TryEmitSweepMesh(path,
                            plan.GetView(),
                            BorrowedView<SweepVertex>(outMesh.Vertices.GetData(), outMesh.Vertices.GetSize()),
                            BorrowedView<uint32>(outMesh.Indices.GetData(), outMesh.Indices.GetSize()));
}

// Sign of each triangle's geometric normal against its first vertex normal: +1 all agree, -1 all oppose.
[[nodiscard]] int CountFacing(const Mesh& mesh, usize firstTriangle, usize triangleCount, bool isFront) noexcept
{
    int mismatches = 0;
    for (usize t = firstTriangle; t < firstTriangle + triangleCount; ++t)
    {
        const SweepVertex& a = mesh.Vertices[mesh.Indices[3 * t]];
        const SweepVertex& b = mesh.Vertices[mesh.Indices[3 * t + 1]];
        const SweepVertex& c = mesh.Vertices[mesh.Indices[3 * t + 2]];
        const Vector3d face = math::Cross(Widen(b.Position) - Widen(a.Position), Widen(c.Position) - Widen(a.Position));
        const float64 facing = math::Dot(face, Widen(a.Normal));
        if ((isFront == true && facing <= 0.0) || (isFront == false && facing >= 0.0))
        {
            mismatches += 1;
        }
    }
    return mismatches;
}

struct FramedFixture final
{
    PreparedFixture Fixture;
    PreparedFrames Frames;
};

[[nodiscard]] GeometryStatus BuildFrames(const Vector3d& seed, FramedFixture& inoutFixture) noexcept
{
    return TryBuildFrames(inoutFixture.Fixture.Path.GetView(),
                          inoutFixture.Fixture.Map.GetView(),
                          MakeFramePreparation(seed),
                          inoutFixture.Frames,
                          nullptr);
}
} // namespace

TEST_CASE("A straight ribbon has two rings, explicit UVs and the requested winding", "[geometry][sweep]")
{
    FramedFixture framed;
    REQUIRE(PrepareCubic(FindCubic("line_uniform"), framed.Fixture) == GeometryStatus::Success);
    REQUIRE(BuildFrames(Vector3d{0.0, 1.0, 0.0}, framed) == GeometryStatus::Success);
    const PathView path = framed.Fixture.Path.GetView();
    const SweepProfileKey profile[] = {{ .Distance = PathDistance{}, .Scale = 2.0 }};
    SweepRequest request = MakeRequest(SweepSection::Ribbon, BorrowedView<const SweepProfileKey>(profile));
    SweepMeshPlan plan;
    REQUIRE(TryPlanSweepMesh(path, framed.Fixture.Map.GetView(), framed.Frames.GetView(), request, plan, nullptr) ==
            GeometryStatus::Success);
    const SweepMeshReport& report = plan.GetView().Report;
    REQUIRE(report.RingCount == 2);
    REQUIRE(report.VertexCount == 4);
    REQUIRE(report.IndexCount == 6);
    REQUIRE(report.ErrorGuarantee == GuaranteeClass::Estimated);
    REQUIRE(report.RiskGuarantee == GuaranteeClass::Estimated);
    Mesh mesh;
    REQUIRE(Emit(path, plan, mesh) == GeometryStatus::Success);
    // Tangent +X and normal +Y put the binormal at +Z; the -Binormal edge comes first.
    REQUIRE(mesh.Vertices[0].Position == Vector3{0.0F, 0.0F, -1.0F});
    REQUIRE(mesh.Vertices[1].Position == Vector3{0.0F, 0.0F, 1.0F});
    REQUIRE(mesh.Vertices[2].Position == Vector3{3.0F, 0.0F, -1.0F});
    REQUIRE(mesh.Vertices[3].Position == Vector3{3.0F, 0.0F, 1.0F});
    REQUIRE(mesh.Vertices[3].Uv == math::Vector2{1.0F, 3.0F});
    REQUIRE(mesh.Vertices[0].Normal == Vector3{0.0F, 1.0F, 0.0F});
    REQUIRE(CountFacing(mesh, 0, 2, true) == 0);

    request.Winding = SweepWinding::Clockwise;
    REQUIRE(TryPlanSweepMesh(path, framed.Fixture.Map.GetView(), framed.Frames.GetView(), request, plan, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(Emit(path, plan, mesh) == GeometryStatus::Success);
    REQUIRE(CountFacing(mesh, 0, 2, false) == 0);
}

TEST_CASE("A capped tube follows the frames within its budgets", "[geometry][sweep]")
{
    FramedFixture framed;
    REQUIRE(PrepareCubic(FindCubic("twisted"), framed.Fixture) == GeometryStatus::Success);
    REQUIRE(BuildFrames(Vector3d{0.0, 1.0, 0.0}, framed) == GeometryStatus::Success);
    const PathView path = framed.Fixture.Path.GetView();
    constexpr float64 RADIUS = 0.05;
    const SweepProfileKey profile[] = {{ .Distance = PathDistance{}, .Scale = RADIUS }};
    const SweepRequest request =
        MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(profile), SweepCaps::Flat);
    SweepMeshPlan plan;
    REQUIRE(TryPlanSweepMesh(path, framed.Fixture.Map.GetView(), framed.Frames.GetView(), request, plan, nullptr) ==
            GeometryStatus::Success);
    const SweepMeshPlanView view = plan.GetView();
    const SweepMeshReport& report = view.Report;
    const usize segments = report.SectionSegmentCount;
    REQUIRE(segments >= 3);
    // Fewest segments: one fewer would exceed the cross-section budget.
    REQUIRE(RADIUS * (1.0 - math::Cos(math::kPi / static_cast<float64>(segments))) <= request.CrossSectionError);
    REQUIRE(RADIUS * (1.0 - math::Cos(math::kPi / static_cast<float64>(segments - 1))) > request.CrossSectionError);
    REQUIRE(report.CrossSectionErrorEstimate <= request.CrossSectionError);
    REQUIRE(report.LongitudinalErrorEstimate <= request.LongitudinalError);
    REQUIRE(report.SelfIntersectionRiskCount == 0);
    const usize ringVertexCount = segments + 1;
    REQUIRE(report.VertexCount == report.RingCount * ringVertexCount + 2 * ringVertexCount);
    REQUIRE(report.IndexCount == 6 * segments * (report.RingCount - 1) + segments * 6);

    Mesh mesh;
    REQUIRE(Emit(path, plan, mesh) == GeometryStatus::Success);
    for (usize ring = 0; ring < report.RingCount; ++ring)
    {
        const SweepRing& planned = view.Rings[ring];
        Vector3 onPath;
        REQUIRE(TryEvaluatePath(path, planned.Parameter, onPath) == GeometryStatus::Success);
        REQUIRE(math::Length(Widen(onPath) - planned.Center) <= VERTEX_TOLERANCE);
        const SweepVertex& seamStart = mesh.Vertices[ring * ringVertexCount];
        const SweepVertex& seamEnd = mesh.Vertices[ring * ringVertexCount + segments];
        // The UV seam duplicates the first column's position bitwise with U = 1.
        REQUIRE(seamStart.Position == seamEnd.Position);
        REQUIRE(seamStart.Uv.X == 0.0F);
        REQUIRE(seamEnd.Uv.X == 1.0F);
        for (usize j = 0; j < ringVertexCount; ++j)
        {
            const SweepVertex& vertex = mesh.Vertices[ring * ringVertexCount + j];
            REQUIRE(math::Abs(math::Length(Widen(vertex.Position) - planned.Center) - RADIUS) <= VERTEX_TOLERANCE);
            REQUIRE(math::Abs(math::Dot(Widen(vertex.Normal), planned.Tangent)) <= VERTEX_TOLERANCE);
        }
    }
    const usize ringTriangles = 2 * segments * (report.RingCount - 1);
    REQUIRE(CountFacing(mesh, 0, ringTriangles, true) == 0);
    REQUIRE(CountFacing(mesh, ringTriangles, 2 * segments, true) == 0);
    // Cap rims copy the ring positions bitwise, with their own flat normals.
    const usize firstCap = report.RingCount * ringVertexCount;
    const usize lastRing = (report.RingCount - 1) * ringVertexCount;
    for (usize j = 0; j < segments; ++j)
    {
        REQUIRE(mesh.Vertices[firstCap + 1 + j].Position == mesh.Vertices[j].Position);
        REQUIRE(mesh.Vertices[firstCap + ringVertexCount + 1 + j].Position == mesh.Vertices[lastRing + j].Position);
    }

    // Independent check: the swept surface between rings stays near the straight edges (Estimated budget).
    float64 worst = 0.0;
    for (usize ring = 0; ring + 1 < report.RingCount; ++ring)
    {
        const SweepRing& start = view.Rings[ring];
        const SweepRing& end = view.Rings[ring + 1];
        const float64 endU = end.Parameter.SpanIndex == start.Parameter.SpanIndex ? end.Parameter.U : 1.0;
        for (usize k = 1; k <= CHECK_SAMPLES; ++k)
        {
            const float64 t = static_cast<float64>(k) / static_cast<float64>(CHECK_SAMPLES + 1);
            const SegmentParameter parameter
            {
                .SpanIndex = start.Parameter.SpanIndex,
                .U = start.Parameter.U + (endU - start.Parameter.U) * t,
            };
            TransportedFrame frame;
            REQUIRE(TryEvaluateFrame(path, framed.Frames.GetView(), parameter, 0.0, frame) == GeometryStatus::Success);
            for (usize j = 0; j < segments; ++j)
            {
                const float64 angle = math::kTau * static_cast<float64>(j) / static_cast<float64>(segments);
                const Vector3d surface =
                    Widen(frame.Position) +
                    (Widen(frame.Binormal) * math::Cos(angle) + Widen(frame.Normal) * math::Sin(angle)) * RADIUS;
                const Vector3d edge = Widen(mesh.Vertices[ring * ringVertexCount + j].Position) * (1.0 - t) +
                                      Widen(mesh.Vertices[(ring + 1) * ringVertexCount + j].Position) * t;
                worst = math::Max(worst, math::Length(surface - edge));
            }
        }
    }
    REQUIRE(worst <= request.LongitudinalError * ESTIMATE_MARGIN);
}

TEST_CASE("A closed tube shares its seam ring and needs periodic frames", "[geometry][sweep]")
{
    const fixtures::FrameFixture& loop = FindFrameFixture("loop_holonomy");
    FramedFixture framed;
    REQUIRE(PrepareFrameFixture(loop, framed.Fixture) == GeometryStatus::Success);
    REQUIRE(BuildFrames(Vector3d{loop.SeedUp.X, loop.SeedUp.Y, loop.SeedUp.Z}, framed) == GeometryStatus::Success);
    const PathView path = framed.Fixture.Path.GetView();
    const SweepProfileKey profile[] = {{ .Distance = PathDistance{}, .Scale = 0.1 }};
    SweepRequest request = MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(profile));
    SweepMeshPlan plan;
    REQUIRE(TryPlanSweepMesh(path, framed.Fixture.Map.GetView(), framed.Frames.GetView(), request, plan, nullptr) ==
            GeometryStatus::Success);
    const SweepMeshPlanView view = plan.GetView();
    REQUIRE(view.IsClosed == true);
    Mesh mesh;
    REQUIRE(Emit(path, plan, mesh) == GeometryStatus::Success);
    const usize ringVertexCount = view.Report.SectionSegmentCount + 1;
    const usize lastRing = (view.Report.RingCount - 1) * ringVertexCount;
    for (usize j = 0; j < ringVertexCount; ++j)
    {
        REQUIRE(mesh.Vertices[lastRing + j].Position == mesh.Vertices[j].Position);
        REQUIRE(mesh.Vertices[lastRing + j].Normal == mesh.Vertices[j].Normal);
        REQUIRE(mesh.Vertices[j].Uv.Y == 0.0F);
        REQUIRE(mesh.Vertices[lastRing + j].Uv.Y == static_cast<float32>(framed.Fixture.Map.GetView().TotalEstimate));
    }

    request.Caps = SweepCaps::Flat;
    REQUIRE(TryPlanSweepMesh(path, framed.Fixture.Map.GetView(), framed.Frames.GetView(), request, plan, nullptr) ==
            GeometryStatus::InvalidArgument);
    request.Caps = SweepCaps::None;
    FramePreparation holonomy = MakeFramePreparation(Vector3d{0.0, 0.0, 1.0});
    holonomy.ClosedPolicy = ClosedFramePolicy::PreserveHolonomy;
    PreparedFrames aperiodic;
    REQUIRE(TryBuildFrames(path, framed.Fixture.Map.GetView(), holonomy, aperiodic, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(TryPlanSweepMesh(path, framed.Fixture.Map.GetView(), aperiodic.GetView(), request, plan, nullptr) ==
            GeometryStatus::DegenerateFrame);
    REQUIRE(plan.GetView().Report.RingCount == view.Report.RingCount);
}

TEST_CASE("Width and roll profiles refine rings and scale the section", "[geometry][sweep]")
{
    FramedFixture framed;
    REQUIRE(PrepareCubic(FindCubic("line_uniform"), framed.Fixture) == GeometryStatus::Success);
    REQUIRE(BuildFrames(Vector3d{0.0, 1.0, 0.0}, framed) == GeometryStatus::Success);
    const PathView path = framed.Fixture.Path.GetView();
    const SweepProfileKey profile[] = {
        { .Distance = PathDistance{ .Value = 0.0 }, .Scale = 1.0, .RollRadians = 0.0 },
        { .Distance = PathDistance{ .Value = 3.0 }, .Scale = 2.0, .RollRadians = math::kPi },
    };
    const SweepRequest request = MakeRequest(SweepSection::Ribbon, BorrowedView<const SweepProfileKey>(profile));
    SweepMeshPlan plan;
    REQUIRE(TryPlanSweepMesh(path, framed.Fixture.Map.GetView(), framed.Frames.GetView(), request, plan, nullptr) ==
            GeometryStatus::Success);
    const SweepMeshPlanView view = plan.GetView();
    REQUIRE(view.Report.RingCount > 8);
    REQUIRE(view.Report.LongitudinalErrorEstimate <= request.LongitudinalError);
    Mesh mesh;
    REQUIRE(Emit(path, plan, mesh) == GeometryStatus::Success);
    for (usize ring = 0; ring < view.Report.RingCount; ++ring)
    {
        const SweepRing& planned = view.Rings[ring];
        const float64 expectedScale = 1.0 + planned.Distance / 3.0;
        REQUIRE(math::Abs(planned.Scale - expectedScale) <= VERTEX_TOLERANCE);
        const Vector3d width = Widen(mesh.Vertices[2 * ring + 1].Position) - Widen(mesh.Vertices[2 * ring].Position);
        REQUIRE(math::Abs(math::Length(width) - expectedScale) <= VERTEX_TOLERANCE);
    }
    // Half a turn of roll flips the last ring's normal from +Y to -Y.
    REQUIRE(math::Length(view.Rings[view.Report.RingCount - 1].Normal - Vector3d{0.0, -1.0, 0.0}) <= VERTEX_TOLERANCE);
}

TEST_CASE("Sweeps reject singular frames, corners and bad profiles and enforce budgets", "[geometry][sweep]")
{
    FramedFixture framed;
    REQUIRE(PrepareCubic(FindCubic("arch"), framed.Fixture) == GeometryStatus::Success);
    REQUIRE(BuildFrames(Vector3d{0.0, 0.0, 1.0}, framed) == GeometryStatus::Success);
    const PathView path = framed.Fixture.Path.GetView();
    const DistanceMapView map = framed.Fixture.Map.GetView();
    const FramesView frames = framed.Frames.GetView();
    const SweepProfileKey thin[] = {{ .Distance = PathDistance{}, .Scale = 0.1 }};
    const SweepProfileKey thick[] = {{ .Distance = PathDistance{}, .Scale = 1.0 }};
    SweepMeshPlan plan;
    REQUIRE(TryPlanSweepMesh(path,
                             map,
                             frames,
                             MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(thin)),
                             plan,
                             nullptr) == GeometryStatus::Success);
    REQUIRE(plan.GetView().Report.SelfIntersectionRiskCount == 0);
    const usize ringCount = plan.GetView().Report.RingCount;

    // The arch apex has curvature 2: a unit tube folds there, which is reported as an Estimated risk.
    SweepMeshPlan risky;
    REQUIRE(TryPlanSweepMesh(path,
                             map,
                             frames,
                             MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(thick)),
                             risky,
                             nullptr) == GeometryStatus::Success);
    REQUIRE(risky.GetView().Report.SelfIntersectionRiskCount > 0);
    REQUIRE(risky.GetView().Report.MaxBendRatio >= 2.0 - VERTEX_TOLERANCE);

    SweepRequest request = MakeRequest(SweepSection::Ribbon, BorrowedView<const SweepProfileKey>(thin));
    request.Caps = SweepCaps::Flat;
    REQUIRE(TryPlanSweepMesh(path, map, frames, request, plan, nullptr) == GeometryStatus::InvalidArgument);
    const SweepProfileKey zero[] = {{ .Distance = PathDistance{}, .Scale = 0.0 }};
    REQUIRE(TryPlanSweepMesh(path,
                             map,
                             frames,
                             MakeRequest(SweepSection::Ribbon, BorrowedView<const SweepProfileKey>(zero)),
                             plan,
                             nullptr) == GeometryStatus::InvalidArgument);
    const SweepProfileKey unordered[] = {
        { .Distance = PathDistance{ .Value = 1.0 }, .Scale = 1.0 },
        { .Distance = PathDistance{ .Value = 1.0 }, .Scale = 1.0 },
    };
    REQUIRE(TryPlanSweepMesh(path,
                             map,
                             frames,
                             MakeRequest(SweepSection::Ribbon, BorrowedView<const SweepProfileKey>(unordered)),
                             plan,
                             nullptr) == GeometryStatus::InvalidArgument);
    const SweepProfileKey infinite[] = {{ .Distance = PathDistance{}, .Scale = math::kPi / 0.0 }};
    REQUIRE(TryPlanSweepMesh(path,
                             map,
                             frames,
                             MakeRequest(SweepSection::Ribbon, BorrowedView<const SweepProfileKey>(infinite)),
                             plan,
                             nullptr) == GeometryStatus::NonFiniteInput);

    request = MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(thin));
    request.MaxSectionSegments = 3;
    REQUIRE(TryPlanSweepMesh(path, map, frames, request, plan, nullptr) == GeometryStatus::BudgetExceeded);
    request = MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(thin));
    request.MaxVertexCount = 8;
    REQUIRE(TryPlanSweepMesh(path, map, frames, request, plan, nullptr) == GeometryStatus::BudgetExceeded);
    request = MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(thin));
    request.Limits.MaxLeaves = 2;
    REQUIRE(TryPlanSweepMesh(path, map, frames, request, plan, nullptr) == GeometryStatus::BudgetExceeded);
    request = MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(thin));
    request.Limits.MaxDepth = 1;
    GeometryDiagnostic diagnostic;
    REQUIRE(TryPlanSweepMesh(path, map, frames, request, plan, &diagnostic) == GeometryStatus::BudgetExceeded);
    REQUIRE(diagnostic.AchievedGuarantee == GuaranteeClass::Estimated);
    REQUIRE(plan.GetView().Report.RingCount == ringCount);

    REQUIRE(TryPlanSweepMesh(path,
                             map,
                             PreparedFrames{}.GetView(),
                             MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(thin)),
                             plan,
                             nullptr) == GeometryStatus::MissingCache);

    // Frames split at a cusp have two runs, so the sweep cannot cross the singularity.
    FramedFixture cusp;
    REQUIRE(PrepareCubic(FindCubic("cusp"), cusp.Fixture) == GeometryStatus::Success);
    FramePreparation split = MakeFramePreparation(Vector3d{0.0, 0.0, 1.0});
    split.SingularityPolicy = FrameSingularityPolicy::SplitRuns;
    REQUIRE(TryBuildFrames(cusp.Fixture.Path.GetView(), cusp.Fixture.Map.GetView(), split, cusp.Frames, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(TryPlanSweepMesh(cusp.Fixture.Path.GetView(),
                             cusp.Fixture.Map.GetView(),
                             cusp.Frames.GetView(),
                             MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(thin)),
                             plan,
                             nullptr) == GeometryStatus::DegenerateFrame);
    REQUIRE(plan.GetView().Report.RingCount == ringCount);
}

TEST_CASE("Emission checks the pinned revision and capacity before writing", "[geometry][sweep]")
{
    FramedFixture framed;
    REQUIRE(PrepareCubic(FindCubic("inflection"), framed.Fixture) == GeometryStatus::Success);
    REQUIRE(BuildFrames(Vector3d{0.0, 0.0, 1.0}, framed) == GeometryStatus::Success);
    const PathView path = framed.Fixture.Path.GetView();
    const SweepProfileKey profile[] = {{ .Distance = PathDistance{}, .Scale = 0.2 }};
    SweepMeshPlan plan;
    REQUIRE(TryPlanSweepMesh(path,
                             framed.Fixture.Map.GetView(),
                             framed.Frames.GetView(),
                             MakeRequest(SweepSection::Tube, BorrowedView<const SweepProfileKey>(profile)),
                             plan,
                             nullptr) == GeometryStatus::Success);
    const SweepMeshReport& report = plan.GetView().Report;
    Mesh mesh;
    const SweepVertex sentinel{ .Position = Vector3{7.0F, 7.0F, 7.0F} };
    REQUIRE(mesh.Vertices.TryResize(report.VertexCount, sentinel) == true);
    REQUIRE(mesh.Indices.TryResize(report.IndexCount - 1, 77U) == true);
    const BorrowedView<SweepVertex> vertices(mesh.Vertices.GetData(), mesh.Vertices.GetSize());
    const BorrowedView<uint32> indices(mesh.Indices.GetData(), mesh.Indices.GetSize());
    REQUIRE(TryEmitSweepMesh(path, plan.GetView(), vertices, indices) == GeometryStatus::CapacityExceeded);
    PathView other = path;
    other.Revision = 9;
    REQUIRE(mesh.Indices.TryResize(report.IndexCount, 77U) == true);
    const BorrowedView<uint32> fullIndices(mesh.Indices.GetData(), mesh.Indices.GetSize());
    REQUIRE(TryEmitSweepMesh(other, plan.GetView(), vertices, fullIndices) == GeometryStatus::StaleRevision);
    REQUIRE(TryEmitSweepMesh(path, SweepMeshPlan{}.GetView(), vertices, fullIndices) == GeometryStatus::MissingCache);
    for (usize i = 0; i < mesh.Vertices.GetSize(); ++i)
    {
        REQUIRE(mesh.Vertices[i].Position == sentinel.Position);
    }
    for (usize i = 0; i < mesh.Indices.GetSize(); ++i)
    {
        REQUIRE(mesh.Indices[i] == 77U);
    }
    REQUIRE(TryEmitSweepMesh(path, plan.GetView(), vertices, fullIndices) == GeometryStatus::Success);
    for (usize i = 0; i < mesh.Indices.GetSize(); ++i)
    {
        REQUIRE(mesh.Indices[i] < report.VertexCount);
    }
}
