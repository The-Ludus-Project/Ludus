// Strict and approximate projection against the G0 corpus: symmetric ties, apex and endpoint minima, the
// three-way line_backtrack tie, ties across spans, budgets and scratch capacity.

#include "geometry_test_support.hpp"

#include <ludus/foundation/geometry/query.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation::geometry;
using namespace ludus::foundation::geometry::test_support;
using ludus::foundation::BorrowedView;
namespace math = ludus::foundation::math;
using math::Vector3;
using math::Vector3d;

namespace
{
// Projection work allowed in corpus tests; the corpus needs a few hundred splits.
constexpr usize PROJECTION_ITERATIONS = 4096;
// The exact references are rounded once to float64.
constexpr float64 REFERENCE_ROUNDING = 1e-15;
// Float32 rounding of a returned position near unit scale (regression envelope, not a bound).
constexpr float64 POSITION_ROUNDING = 1e-6;
// A parameter within ProjectionGap of a simple minimum lies within about sqrt(2 gap / d'') of it.
constexpr float64 PARAMETER_TOLERANCE = 1e-2;

[[nodiscard]] Vector3d ToVector(const fixtures::ReferencePoint& point) noexcept
{
    return Vector3d{point.X, point.Y, point.Z};
}

[[nodiscard]] float64 ComputeDistance(const Vector3& position, const Vector3d& point) noexcept
{
    const Vector3d widened{static_cast<float64>(position.X),
                           static_cast<float64>(position.Y),
                           static_cast<float64>(position.Z)};
    return math::Length(widened - point);
}

[[nodiscard]] ProjectionScratch MakeScratch(const PathView& path, usize iterations) noexcept
{
    ProjectionScratch scratch;
    usize capacity = 0;
    if (ComputeProjectionScratchCapacity(path, iterations, capacity) == GeometryStatus::Success)
    {
        (void)scratch.TryReserve(capacity);
    }
    return scratch;
}

[[nodiscard]] PathSpanSource MakeLine(const Vector3d& start, const Vector3d& end) noexcept
{
    PathSpanSource span;
    span.Kind = SpanKind::Line;
    span.Control[0] = start;
    span.Control[SPAN_CONTROL_COUNT - 1] = end;
    return span;
}
} // namespace

TEST_CASE("Strict projection finds every arch minimum with a Certified gap", "[geometry][query]")
{
    const fixtures::CubicFixture& arch = FindCubic("arch");
    PathSpanSource span;
    PreparedPath path;
    REQUIRE(PrepareFixture(arch, span, path) == GeometryStatus::Success);
    const PathView view = path.GetView();
    ProjectionScratch scratch = MakeScratch(view, PROJECTION_ITERATIONS);
    for (usize referenceIndex = 0; referenceIndex < arch.Projections.GetSize(); ++referenceIndex)
    {
        const fixtures::ProjectionReference& reference = arch.Projections[referenceIndex];
        const Vector3d point = ToVector(reference.Query);
        ProjectionResult result;
        REQUIRE(TryProjectPoint(view,
                                ProjectionQuery{ .Point = point, .MaxIterations = PROJECTION_ITERATIONS },
                                scratch,
                                result) == GeometryStatus::Success);
        REQUIRE(result.Guarantee == GuaranteeClass::Certified);
        REQUIRE(result.IsGapMet == true);
        REQUIRE(result.Gap <= view.Tolerance.ProjectionGap);
        // The Certified enclosure contains the exact global minimum.
        REQUIRE(result.LowerBound <= reference.Distance + REFERENCE_ROUNDING);
        REQUIRE(reference.Distance <= result.Distance + REFERENCE_ROUNDING);
        REQUIRE(ComputeDistance(result.Position, point) <=
                reference.Distance + view.Tolerance.ProjectionGap + POSITION_ROUNDING);
        // Ties resolve to the earliest minimum.
        REQUIRE(result.Parameter.SpanIndex == 0);
        REQUIRE(math::Abs(result.Parameter.U - reference.U) <= PARAMETER_TOLERANCE);
        if (reference.TieCount > 1)
        {
            REQUIRE(result.Parameter.U < (reference.MinimumU[0] + reference.MinimumU[1]) * 0.5);
        }
        if (reference.U == 0.0)
        {
            // An endpoint minimum is the stored endpoint itself (Exact).
            REQUIRE(result.Parameter.U == 0.0);
            REQUIRE(result.Position == Vector3{0.0F, 0.0F, 0.0F});
        }
    }
}

TEST_CASE("A three-way tie resolves to the smallest parameter", "[geometry][query]")
{
    const fixtures::CubicFixture& backtrack = FindCubic("line_backtrack");
    PathSpanSource span;
    PreparedPath path;
    REQUIRE(PrepareFixture(backtrack, span, path) == GeometryStatus::Success);
    const PathView view = path.GetView();
    ProjectionScratch scratch = MakeScratch(view, PROJECTION_ITERATIONS);
    const fixtures::ProjectionReference& reference = backtrack.Projections[0];
    REQUIRE(reference.TieCount == 3);
    ProjectionResult result;
    REQUIRE(TryProjectPoint(view,
                            ProjectionQuery
                            {
                                .Point = ToVector(reference.Query),
                                .MaxIterations = PROJECTION_ITERATIONS,
                            },
                            scratch,
                            result) == GeometryStatus::Success);
    REQUIRE(result.LowerBound <= reference.Distance + REFERENCE_ROUNDING);
    REQUIRE(reference.Distance <= result.Distance + REFERENCE_ROUNDING);
    REQUIRE(math::Abs(result.Parameter.U - reference.MinimumU[0]) <= PARAMETER_TOLERANCE);
    REQUIRE(result.Parameter.U < reference.MinimumU[1]);

    // Repeating the query reproduces the result exactly.
    ProjectionResult repeated;
    REQUIRE(TryProjectPoint(view,
                            ProjectionQuery
                            {
                                .Point = ToVector(reference.Query),
                                .MaxIterations = PROJECTION_ITERATIONS,
                            },
                            scratch,
                            repeated) == GeometryStatus::Success);
    REQUIRE(repeated.Parameter.U == result.Parameter.U);
    REQUIRE(repeated.Distance == result.Distance);
    REQUIRE(repeated.IterationCount == result.IterationCount);
}

TEST_CASE("Ties across spans resolve to the earlier span", "[geometry][query]")
{
    // A roof: the origin is 1/sqrt(2) from the midpoint of both lines.
    const PathSpanSource spans[] = {
        MakeLine(Vector3d{-1.0, 0.0, 0.0}, Vector3d{0.0, 1.0, 0.0}),
        MakeLine(Vector3d{0.0, 1.0, 0.0}, Vector3d{1.0, 0.0, 0.0}),
    };
    PreparedPath path;
    REQUIRE(TryPreparePath(PathSource{ .Spans = BorrowedView<const PathSpanSource>(spans), .Revision = 1 },
                           MakePreparation(),
                           path,
                           nullptr) == GeometryStatus::Success);
    const PathView view = path.GetView();
    ProjectionScratch scratch = MakeScratch(view, PROJECTION_ITERATIONS);
    ProjectionResult result;
    REQUIRE(TryProjectPoint(view,
                            ProjectionQuery{ .Point = Vector3d{}, .MaxIterations = PROJECTION_ITERATIONS },
                            scratch,
                            result) == GeometryStatus::Success);
    REQUIRE(result.Parameter.SpanIndex == 0);
    REQUIRE(math::Abs(result.Parameter.U - 0.5) <= PARAMETER_TOLERANCE);
    REQUIRE(result.LowerBound <= math::Sqrt(0.5));
    REQUIRE(math::Sqrt(0.5) <= result.Distance + REFERENCE_ROUNDING);

    // A minimum at the shared joint ties with the end of the earlier span within the gap; the earlier span wins.
    REQUIRE(TryProjectPoint(view,
                            ProjectionQuery{ .Point = Vector3d{0.0, 3.0, 0.0}, .MaxIterations = PROJECTION_ITERATIONS },
                            scratch,
                            result) == GeometryStatus::Success);
    REQUIRE(result.LowerBound <= 2.0);
    REQUIRE(result.Distance - 2.0 <= view.Tolerance.ProjectionGap);
    REQUIRE(result.Parameter.SpanIndex == 0);
    REQUIRE(result.Parameter.U > 1.0 - PARAMETER_TOLERANCE);
}

TEST_CASE("Projection on a far-origin path subtracts the origin with outward rounding", "[geometry][query]")
{
    const fixtures::CubicFixture& arch = FindCubic("arch");
    const Vector3d origin{1024.0, -2048.0, 512.0};
    PathSpanSource span = MakeSpan(arch);
    for (usize i = 0; i < SPAN_CONTROL_COUNT; ++i)
    {
        span.Control[i] = span.Control[i] + origin;
    }
    PreparedPath path;
    REQUIRE(TryPreparePath(
                PathSource
                {
                    .Origin = origin,
                    .Spans = BorrowedView<const PathSpanSource>(&span, 1),
                    .Revision = 1,
                },
                MakePreparation(),
                path,
                nullptr) == GeometryStatus::Success);
    const PathView view = path.GetView();
    ProjectionScratch scratch = MakeScratch(view, PROJECTION_ITERATIONS);
    const fixtures::ProjectionReference& reference = arch.Projections[2];
    ProjectionResult result;
    REQUIRE(TryProjectPoint(view,
                            ProjectionQuery
                            {
                                .Point = ToVector(reference.Query) + origin,
                                .MaxIterations = PROJECTION_ITERATIONS,
                            },
                            scratch,
                            result) == GeometryStatus::Success);
    REQUIRE(result.LowerBound <= reference.Distance + REFERENCE_ROUNDING);
    REQUIRE(reference.Distance <= result.Distance + REFERENCE_ROUNDING);
    REQUIRE(math::Abs(result.Parameter.U - reference.U) <= PARAMETER_TOLERANCE);
}

TEST_CASE("Strict projection fails transactionally; the approximate mode returns its candidate", "[geometry][query]")
{
    const fixtures::CubicFixture& arch = FindCubic("arch");
    PathSpanSource span;
    PreparedPath path;
    REQUIRE(PrepareFixture(arch, span, path) == GeometryStatus::Success);
    const PathView view = path.GetView();
    constexpr usize SHORT_ITERATIONS = 2;
    ProjectionScratch scratch = MakeScratch(view, SHORT_ITERATIONS);
    const fixtures::ProjectionReference& reference = arch.Projections[0];
    const ProjectionQuery query{ .Point = ToVector(reference.Query), .MaxIterations = SHORT_ITERATIONS };

    ProjectionResult result{ .IterationCount = 99 };
    REQUIRE(TryProjectPoint(view, query, scratch, result) == GeometryStatus::BudgetExceeded);
    REQUIRE(result.IterationCount == 99);

    REQUIRE(ProjectPointApproximate(view, query, scratch, result) == GeometryStatus::Success);
    REQUIRE(result.Guarantee == GuaranteeClass::Approximate);
    REQUIRE(result.IsGapMet == false);
    REQUIRE(result.IterationCount == SHORT_ITERATIONS);
    REQUIRE(result.Gap > view.Tolerance.ProjectionGap);
    REQUIRE(result.LowerBound <= reference.Distance + REFERENCE_ROUNDING);
    REQUIRE(reference.Distance <= result.Distance + REFERENCE_ROUNDING);

    // With enough work the approximate mode reports the gap as met, still labelled Approximate.
    ProjectionScratch large = MakeScratch(view, PROJECTION_ITERATIONS);
    REQUIRE(ProjectPointApproximate(view,
                                    ProjectionQuery{ .Point = query.Point, .MaxIterations = PROJECTION_ITERATIONS },
                                    large,
                                    result) == GeometryStatus::Success);
    REQUIRE(result.IsGapMet == true);
    REQUIRE(result.Guarantee == GuaranteeClass::Approximate);

    // An endpoint answer can be certified without splitting.
    PreparedFixture line;
    REQUIRE(PrepareCubic(FindCubic("line_uniform"), line) == GeometryStatus::Success);
    ProjectionScratch none = MakeScratch(line.Path.GetView(), 0);
    REQUIRE(TryProjectPoint(line.Path.GetView(),
                            ProjectionQuery{ .Point = Vector3d{-1.0, 0.0, 0.0}, .MaxIterations = 0 },
                            none,
                            result) == GeometryStatus::Success);
    REQUIRE(result.IterationCount == 0);
    REQUIRE(result.Parameter.U == 0.0);
    REQUIRE(math::Abs(result.Distance - 1.0) <= REFERENCE_ROUNDING);
}

TEST_CASE("Projection validates inputs and scratch capacity before searching", "[geometry][query]")
{
    const fixtures::CubicFixture& arch = FindCubic("arch");
    PathSpanSource span;
    PreparedPath path;
    REQUIRE(PrepareFixture(arch, span, path) == GeometryStatus::Success);
    const PathView view = path.GetView();
    ProjectionResult result{ .IterationCount = 5 };

    ProjectionScratch small = MakeScratch(view, 4);
    REQUIRE(TryProjectPoint(view,
                            ProjectionQuery
                            {
                                .Point = Vector3d{1.5, 0.0, 0.0},
                                .MaxIterations = 64,
                            },
                            small,
                            result) == GeometryStatus::CapacityExceeded);
    REQUIRE(ProjectPointApproximate(view,
                                    ProjectionQuery{ .Point = Vector3d{1.5, 0.0, 0.0}, .MaxIterations = 64 },
                                    small,
                                    result) == GeometryStatus::CapacityExceeded);
    REQUIRE(TryProjectPoint(view, ProjectionQuery{ .Point = Vector3d{math::kPi / 0.0, 0.0, 0.0} }, small, result) ==
            GeometryStatus::NonFiniteInput);
    REQUIRE(TryProjectPoint(PathView{}, ProjectionQuery{}, small, result) == GeometryStatus::InvalidArgument);
    REQUIRE(result.IterationCount == 5);

    usize capacity = 0;
    REQUIRE(ComputeProjectionScratchCapacity(view, static_cast<usize>(-1), capacity) == GeometryStatus::OutOfRange);
    REQUIRE(ComputeProjectionScratchCapacity(view, 10, capacity) == GeometryStatus::Success);
    REQUIRE(capacity == view.Spans.GetSize() + 1 + 10);
    REQUIRE(small.TryReserve(capacity) == GeometryStatus::Success);
    REQUIRE(small.GetCapacity() >= capacity);
}
