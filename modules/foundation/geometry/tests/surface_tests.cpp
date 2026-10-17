// PreparedSurface topology, orientation, corner identity and evaluation (phase G4).

#include "geometry_test_support.hpp"

#include <ludus/foundation/geometry/surface.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>

using namespace ludus::foundation::geometry;
using ludus::foundation::BorrowedView;
using ludus::foundation::core::StaticArray;
namespace fixtures = ludus::geometry_fixtures;
namespace math = ludus::foundation::math;
namespace test_support = ludus::foundation::geometry::test_support;
using math::Vector3;
using math::Vector3d;

namespace
{
constexpr usize PAIR_PATCH_COUNT = 2;
constexpr usize TRIPLE_PATCH_COUNT = 3;
constexpr uint64 FIRST_ID = 10;
constexpr uint64 SECOND_ID = 20;
constexpr uint64 THIRD_ID = 30;
// Reference samples are exact; float32 evaluation keeps them within this many asset units.
constexpr float64 SAMPLE_TOLERANCE = 1e-5;

[[nodiscard]] bool AreClose(const Vector3& value, const fixtures::ReferencePoint& reference, float64 tolerance) noexcept
{
    return math::Abs(static_cast<float64>(value.X) - reference.X) <= tolerance &&
           math::Abs(static_cast<float64>(value.Y) - reference.Y) <= tolerance &&
           math::Abs(static_cast<float64>(value.Z) - reference.Z) <= tolerance;
}

[[nodiscard]] Vector3 ToFloat(const fixtures::ControlPoint& point) noexcept
{
    return Vector3{point.X, point.Y, point.Z};
}

// edge_first and edge_second joined along first U1 / second U0 with v reversed.
struct SharedPair final
{
    StaticArray<SurfacePatchSource, PAIR_PATCH_COUNT> Patches;
    StaticArray<SurfaceAdjacencySource, 1> Adjacency;
};

[[nodiscard]] SharedPair MakeSharedPair(bool isReversed) noexcept
{
    const fixtures::SharedEdgeFixture& shared = fixtures::SHARED_EDGE_FIXTURES[0];
    SharedPair pair;
    pair.Patches[0] = test_support::MakePatchSource(fixtures::PATCH_FIXTURES[shared.FirstPatch], FIRST_ID);
    pair.Patches[1] = test_support::MakePatchSource(fixtures::PATCH_FIXTURES[shared.SecondPatch], SECOND_ID);
    pair.Adjacency[0] = SurfaceAdjacencySource
    {
        .FirstPatch = 0,
        .FirstSide = PatchSide::U1,
        .SecondPatch = 1,
        .SecondSide = PatchSide::U0,
        .IsReversed = isReversed,
    };
    return pair;
}

[[nodiscard]] GeometryStatus PreparePair(const SharedPair& pair,
                                         PreparedSurface& outSurface,
                                         GeometryDiagnostic* outDiagnosticOrNull = nullptr) noexcept
{
    return test_support::PrepareSurface(
        BorrowedView<const SurfacePatchSource>(pair.Patches.GetData(), pair.Patches.GetSize()),
        BorrowedView<const SurfaceAdjacencySource>(pair.Adjacency.GetData(), pair.Adjacency.GetSize()),
        outSurface,
        outDiagnosticOrNull);
}

// The bilinear saddle z = 9uv restricted to u in [0, 1/2] (first) or [1/2, 1] (second); exact float32 controls.
[[nodiscard]] SurfacePatchSource MakeSaddleHalf(bool isHigh, uint64 id) noexcept
{
    SurfacePatchSource patch;
    patch.Id = id;
    for (usize i = 0; i < PATCH_CONTROL_ORDER; ++i)
    {
        const float64 x = (isHigh == true ? 1.5 : 0.0) + 0.5 * static_cast<float64>(i);
        for (usize j = 0; j < PATCH_CONTROL_ORDER; ++j)
        {
            const float64 y = static_cast<float64>(j);
            patch.Control[i][j] = Vector3d{x, y, x * y};
        }
    }
    return patch;
}

// A planar patch spanning [x0, x0 + 3] x [0, height].
// Placement of a planar test patch.
struct PlanarExtent final
{
    float64 X0 = 0.0;
    float64 Height = 0.0;
};

[[nodiscard]] SurfacePatchSource MakePlanarPatch(PlanarExtent extent, uint64 id) noexcept
{
    const float64 x0 = extent.X0;
    const float64 height = extent.Height;
    SurfacePatchSource patch;
    patch.Id = id;
    for (usize i = 0; i < PATCH_CONTROL_ORDER; ++i)
    {
        for (usize j = 0; j < PATCH_CONTROL_ORDER; ++j)
        {
            patch.Control[i][j] = Vector3d{x0 + static_cast<float64>(i), height * static_cast<float64>(j) / 3.0, 0.0};
        }
    }
    return patch;
}
} // namespace

TEST_CASE("A single patch has four boundary edges and four corners with stable identities", "[geometry][surface]")
{
    const SurfacePatchSource patch = test_support::MakePatchSource(test_support::FindPatch("plane"), FIRST_ID);
    PreparedSurface surface;
    REQUIRE(test_support::PrepareSurface(BorrowedView<const SurfacePatchSource>(&patch, 1), {}, surface) ==
            GeometryStatus::Success);
    const SurfaceView view = surface.GetView();
    REQUIRE(view.Patches.GetSize() == 1);
    REQUIRE(view.Edges.GetSize() == PATCH_SIDE_COUNT);
    REQUIRE(view.Corners.GetSize() == PATCH_CORNER_COUNT);
    REQUIRE(view.CornerIncidences.GetSize() == PATCH_CORNER_COUNT);
    REQUIRE(view.Revision == 7);
    for (usize side = 0; side < PATCH_SIDE_COUNT; ++side)
    {
        const SurfaceEdge& edge = view.Edges[side];
        REQUIRE(view.Patches[0].Edge[side] == side);
        REQUIRE(edge.IncidenceCount == 1);
        REQUIRE(edge.Id.PatchId == FIRST_ID);
        REQUIRE(edge.Id.Side == static_cast<PatchSide>(side));
        REQUIRE(edge.IsOrientationConsistent == false);
    }
    // Canonical curves are the stored side controls in increasing along-edge parameter.
    const math::BicubicBezierPatch& control = view.Patches[0].Control;
    for (usize k = 0; k < PATCH_CONTROL_ORDER; ++k)
    {
        REQUIRE(view.Edges[0].Control[k] == control.Control[0][k]);
        REQUIRE(view.Edges[1].Control[k] == control.Control[3][k]);
        REQUIRE(view.Edges[2].Control[k] == control.Control[k][0]);
        REQUIRE(view.Edges[3].Control[k] == control.Control[k][3]);
    }
    for (usize corner = 0; corner < PATCH_CORNER_COUNT; ++corner)
    {
        REQUIRE(view.Corners[corner].IncidenceCount == 1);
        REQUIRE(view.Corners[corner].Id.PatchId == FIRST_ID);
    }
    REQUIRE(view.Corners[view.Patches[0].Corner[static_cast<usize>(PatchCorner::U1V1)]].Position ==
            Vector3{3.0F, 3.0F, 0.0F});
    REQUIRE(view.PositionFloor == 4.76837158203125e-07);
}

TEST_CASE("A reversed shared edge maps orientation, resolves corner identity and matches samples",
          "[geometry][surface][fixtures]")
{
    const fixtures::SharedEdgeFixture& shared = fixtures::SHARED_EDGE_FIXTURES[0];
    REQUIRE(shared.IsReversed == true);
    const SharedPair pair = MakeSharedPair(true);
    PreparedSurface surface;
    REQUIRE(PreparePair(pair, surface) == GeometryStatus::Success);
    const SurfaceView view = surface.GetView();
    // Eight sides, one shared edge; eight patch corners, two joined pairs.
    REQUIRE(view.Edges.GetSize() == 7);
    REQUIRE(view.Corners.GetSize() == 6);

    const usize edgeIndex = view.Patches[0].Edge[static_cast<usize>(PatchSide::U1)];
    REQUIRE(view.Patches[1].Edge[static_cast<usize>(PatchSide::U0)] == edgeIndex);
    const SurfaceEdge& edge = view.Edges[edgeIndex];
    REQUIRE(edge.IncidenceCount == 2);
    REQUIRE(edge.IsReversed == true);
    REQUIRE(edge.Id.PatchId == FIRST_ID);
    REQUIRE(edge.Id.Side == PatchSide::U1);
    REQUIRE(edge.Incidence[0].PatchIndex == 0);
    REQUIRE(edge.Incidence[1].PatchIndex == 1);
    REQUIRE(edge.Incidence[1].Side == PatchSide::U0);
    // Both patches traverse the curve in the same direction along their counterclockwise boundaries.
    REQUIRE(edge.IsOrientationConsistent == false);
    REQUIRE(edge.IsC1 == false);
    REQUIRE(math::Abs(edge.CrossDerivativeDifference - math::Sqrt(2.0)) <= 1e-12);

    // Corner identity: first U1V0 is second U0V1 at (3, 0, 0); first U1V1 is second U0V0 at (3, 3, 0).
    const usize startCorner = edge.Corner[0];
    const usize endCorner = edge.Corner[1];
    REQUIRE(view.Patches[0].Corner[static_cast<usize>(PatchCorner::U1V0)] == startCorner);
    REQUIRE(view.Patches[1].Corner[static_cast<usize>(PatchCorner::U0V1)] == startCorner);
    REQUIRE(view.Patches[0].Corner[static_cast<usize>(PatchCorner::U1V1)] == endCorner);
    REQUIRE(view.Patches[1].Corner[static_cast<usize>(PatchCorner::U0V0)] == endCorner);
    REQUIRE(view.Corners[startCorner].Position == Vector3{3.0F, 0.0F, 0.0F});
    REQUIRE(view.Corners[endCorner].Position == Vector3{3.0F, 3.0F, 0.0F});
    REQUIRE(view.Corners[startCorner].IncidenceCount == 2);
    REQUIRE(view.Corners[startCorner].Id.PatchId == FIRST_ID);
    REQUIRE(view.Corners[startCorner].Id.Corner == PatchCorner::U1V0);
    usize incidenceTotal = 0;
    for (usize i = 0; i < view.Corners.GetSize(); ++i)
    {
        incidenceTotal += view.Corners[i].IncidenceCount;
    }
    REQUIRE(incidenceTotal == PAIR_PATCH_COUNT * PATCH_CORNER_COUNT);
    for (usize k = 0; k < PATCH_CONTROL_ORDER; ++k)
    {
        REQUIRE(edge.Control[k] == ToFloat(fixtures::PATCH_FIXTURES[shared.FirstPatch].Control[3][k]));
    }

    for (usize sampleIndex = 0; sampleIndex < shared.Samples.GetSize(); ++sampleIndex)
    {
        const fixtures::EdgeSampleReference& sample = shared.Samples[sampleIndex];
        SurfaceParameter first;
        SurfaceParameter second;
        REQUIRE(TryMapEdgeParameter(view, edgeIndex, 0, sample.FirstV, first) == GeometryStatus::Success);
        REQUIRE(TryMapEdgeParameter(view, edgeIndex, 1, sample.FirstV, second) == GeometryStatus::Success);
        REQUIRE(first.PatchIndex == 0);
        REQUIRE(first.U == shared.FirstU);
        REQUIRE(first.V == sample.FirstV);
        REQUIRE(second.PatchIndex == 1);
        REQUIRE(second.U == shared.SecondU);
        REQUIRE(second.V == sample.SecondV);
        Vector3 firstPosition;
        Vector3 secondPosition;
        REQUIRE(TryEvaluateSurface(view, first, firstPosition) == GeometryStatus::Success);
        REQUIRE(TryEvaluateSurface(view, second, secondPosition) == GeometryStatus::Success);
        REQUIRE(AreClose(firstPosition, sample.Position, SAMPLE_TOLERANCE) == true);
        REQUIRE(AreClose(secondPosition, sample.Position, SAMPLE_TOLERANCE) == true);
        if (sample.FirstV == 0.0 || sample.FirstV == 1.0)
        {
            REQUIRE(firstPosition == secondPosition);
        }
    }
}

TEST_CASE("Stable edge and corner identities do not depend on patch order", "[geometry][surface][fixtures]")
{
    const SharedPair pair = MakeSharedPair(true);
    SharedPair swapped;
    swapped.Patches[0] = pair.Patches[1];
    swapped.Patches[1] = pair.Patches[0];
    swapped.Adjacency[0] = SurfaceAdjacencySource
    {
        .FirstPatch = 0,
        .FirstSide = PatchSide::U0,
        .SecondPatch = 1,
        .SecondSide = PatchSide::U1,
        .IsReversed = true,
    };
    PreparedSurface original;
    PreparedSurface reordered;
    REQUIRE(PreparePair(pair, original) == GeometryStatus::Success);
    REQUIRE(PreparePair(swapped, reordered) == GeometryStatus::Success);
    const SurfaceView a = original.GetView();
    const SurfaceView b = reordered.GetView();
    const SurfaceEdge& edgeA = a.Edges[a.Patches[0].Edge[static_cast<usize>(PatchSide::U1)]];
    const SurfaceEdge& edgeB = b.Edges[b.Patches[1].Edge[static_cast<usize>(PatchSide::U1)]];
    REQUIRE(edgeA.Id.PatchId == edgeB.Id.PatchId);
    REQUIRE(edgeA.Id.Side == edgeB.Id.Side);
    // The owner is the lower author ID, so the canonical orientation is the same in both orders.
    REQUIRE(edgeB.Incidence[0].PatchIndex == 1);
    for (usize k = 0; k < PATCH_CONTROL_ORDER; ++k)
    {
        REQUIRE(edgeA.Control[k] == edgeB.Control[k]);
    }
    const SurfaceCorner& cornerA = a.Corners[edgeA.Corner[0]];
    const SurfaceCorner& cornerB = b.Corners[edgeB.Corner[0]];
    REQUIRE(cornerA.Id.PatchId == cornerB.Id.PatchId);
    REQUIRE(cornerA.Id.Corner == cornerB.Id.Corner);
}

TEST_CASE("Joined halves of one polynomial are C1 and consistently oriented", "[geometry][surface]")
{
    const StaticArray<SurfacePatchSource, PAIR_PATCH_COUNT> patches{
        {MakeSaddleHalf(false, FIRST_ID), MakeSaddleHalf(true, SECOND_ID)}};
    const SurfaceAdjacencySource adjacency
    {
        .FirstPatch = 0,
        .FirstSide = PatchSide::U1,
        .SecondPatch = 1,
        .SecondSide = PatchSide::U0,
    };
    PreparedSurface surface;
    REQUIRE(test_support::PrepareSurface(BorrowedView<const SurfacePatchSource>(patches.GetData(), patches.GetSize()),
                                         BorrowedView<const SurfaceAdjacencySource>(&adjacency, 1),
                                         surface) == GeometryStatus::Success);
    const SurfaceView view = surface.GetView();
    const SurfaceEdge& edge = view.Edges[view.Patches[0].Edge[static_cast<usize>(PatchSide::U1)]];
    REQUIRE(edge.IsOrientationConsistent == true);
    REQUIRE(edge.CrossDerivativeDifference == 0.0);
    REQUIRE(edge.IsC1 == true);
}

TEST_CASE("A side joined to another side of the same patch closes the patch", "[geometry][surface]")
{
    SurfacePatchSource patch;
    patch.Id = FIRST_ID;
    const StaticArray<Vector3d, PATCH_CONTROL_ORDER> loop{
        {Vector3d{0.0, 0.0, 0.0}, Vector3d{1.0, 0.0, 1.0}, Vector3d{1.0, 0.0, -1.0}, Vector3d{0.0, 0.0, 0.0}}};
    for (usize i = 0; i < PATCH_CONTROL_ORDER; ++i)
    {
        for (usize j = 0; j < PATCH_CONTROL_ORDER; ++j)
        {
            patch.Control[i][j] = Vector3d{loop[i].X, static_cast<float64>(j), loop[i].Z};
        }
    }
    const SurfaceAdjacencySource closing
    {
        .FirstPatch = 0,
        .FirstSide = PatchSide::U0,
        .SecondPatch = 0,
        .SecondSide = PatchSide::U1,
    };
    PreparedSurface surface;
    REQUIRE(test_support::PrepareSurface(BorrowedView<const SurfacePatchSource>(&patch, 1),
                                         BorrowedView<const SurfaceAdjacencySource>(&closing, 1),
                                         surface) == GeometryStatus::Success);
    const SurfaceView view = surface.GetView();
    REQUIRE(view.Edges.GetSize() == 3);
    REQUIRE(view.Corners.GetSize() == 2);
    REQUIRE(view.Patches[0].Edge[static_cast<usize>(PatchSide::U0)] ==
            view.Patches[0].Edge[static_cast<usize>(PatchSide::U1)]);
    REQUIRE(view.Patches[0].Corner[static_cast<usize>(PatchCorner::U0V0)] ==
            view.Patches[0].Corner[static_cast<usize>(PatchCorner::U1V0)]);
    REQUIRE(view.Edges[0].IsOrientationConsistent == true);

    const SurfaceAdjacencySource self
    {
        .FirstPatch = 0,
        .FirstSide = PatchSide::U0,
        .SecondPatch = 0,
        .SecondSide = PatchSide::U0,
    };
    GeometryDiagnostic diagnostic;
    REQUIRE(test_support::PrepareSurface(BorrowedView<const SurfacePatchSource>(&patch, 1),
                                         BorrowedView<const SurfaceAdjacencySource>(&self, 1),
                                         surface,
                                         &diagnostic) == GeometryStatus::InvalidTopology);
    REQUIRE(surface.GetView().Edges.GetSize() == 3);
}

TEST_CASE("Mismatched, partial and nonmanifold joins are rejected without welding", "[geometry][surface][fixtures]")
{
    PreparedSurface surface;
    REQUIRE(PreparePair(MakeSharedPair(true), surface) == GeometryStatus::Success);
    const usize activeEdgeCount = surface.GetView().Edges.GetSize();

    SECTION("The shared curve without its declared reversal does not match")
    {
        GeometryDiagnostic diagnostic;
        REQUIRE(PreparePair(MakeSharedPair(false), surface, &diagnostic) == GeometryStatus::InvalidTopology);
        REQUIRE(std::strcmp(diagnostic.Property, "Adjacency.Control") == 0);
        REQUIRE(diagnostic.AchievedBound > 6.0);
        REQUIRE(diagnostic.AchievedGuarantee == GuaranteeClass::Rounded);
    }
    SECTION("A partial join covering half a side is a mismatch")
    {
        const StaticArray<SurfacePatchSource, PAIR_PATCH_COUNT> patches{
            {MakePlanarPatch(PlanarExtent{ .X0 = 0.0, .Height = 3.0 }, FIRST_ID),
             MakePlanarPatch(PlanarExtent{ .X0 = 3.0, .Height = 1.5 }, SECOND_ID)}};
        const SurfaceAdjacencySource partial
        {
            .FirstPatch = 0,
            .FirstSide = PatchSide::U1,
            .SecondPatch = 1,
            .SecondSide = PatchSide::U0,
        };
        REQUIRE(
            test_support::PrepareSurface(BorrowedView<const SurfacePatchSource>(patches.GetData(), patches.GetSize()),
                                         BorrowedView<const SurfaceAdjacencySource>(&partial, 1),
                                         surface) == GeometryStatus::InvalidTopology);
    }
    SECTION("A third patch on one edge is nonmanifold")
    {
        const StaticArray<SurfacePatchSource, TRIPLE_PATCH_COUNT> patches{
            {MakePlanarPatch(PlanarExtent{ .X0 = 0.0, .Height = 3.0 }, FIRST_ID),
             MakePlanarPatch(PlanarExtent{ .X0 = 3.0, .Height = 3.0 }, SECOND_ID),
             MakePlanarPatch(PlanarExtent{ .X0 = 3.0, .Height = 3.0 }, THIRD_ID)}};
        const StaticArray<SurfaceAdjacencySource, 2> joins{{
            SurfaceAdjacencySource
            {
                .FirstPatch = 0,
                .FirstSide = PatchSide::U1,
                .SecondPatch = 1,
                .SecondSide = PatchSide::U0,
            },
            SurfaceAdjacencySource
            {
                .FirstPatch = 0,
                .FirstSide = PatchSide::U1,
                .SecondPatch = 2,
                .SecondSide = PatchSide::U0,
            },
        }};
        GeometryDiagnostic diagnostic;
        REQUIRE(
            test_support::PrepareSurface(BorrowedView<const SurfacePatchSource>(patches.GetData(), patches.GetSize()),
                                         BorrowedView<const SurfaceAdjacencySource>(joins.GetData(), joins.GetSize()),
                                         surface,
                                         &diagnostic) == GeometryStatus::InvalidTopology);
        REQUIRE(std::strcmp(diagnostic.Property, "Adjacency.Incidence") == 0);
        REQUIRE(diagnostic.SourceElement == 1);
    }
    SECTION("Duplicate IDs, unknown patches and unknown sides")
    {
        SharedPair pair = MakeSharedPair(true);
        pair.Patches[1].Id = FIRST_ID;
        REQUIRE(PreparePair(pair, surface) == GeometryStatus::InvalidTopology);
        pair = MakeSharedPair(true);
        pair.Adjacency[0].SecondPatch = 2;
        REQUIRE(PreparePair(pair, surface) == GeometryStatus::InvalidTopology);
        pair = MakeSharedPair(true);
        pair.Adjacency[0].SecondSide = static_cast<PatchSide>(9);
        REQUIRE(PreparePair(pair, surface) == GeometryStatus::InvalidArgument);
    }
    REQUIRE(surface.GetView().Edges.GetSize() == activeEdgeCount);
}

TEST_CASE("Invalid surface sources and budgets fail without replacing the surface", "[geometry][surface]")
{
    const SharedPair pair = MakeSharedPair(true);
    PreparedSurface surface;
    REQUIRE(PreparePair(pair, surface) == GeometryStatus::Success);
    const BorrowedView<const SurfacePatchSource> patches(pair.Patches.GetData(), pair.Patches.GetSize());
    const BorrowedView<const SurfaceAdjacencySource> adjacency(pair.Adjacency.GetData(), pair.Adjacency.GetSize());
    const float64 nan = std::numeric_limits<float64>::quiet_NaN();

    SurfacePreparation preparation = test_support::MakeSurfacePreparation();
    const SurfaceSource source{ .Patches = patches, .Adjacency = adjacency };
    REQUIRE(TryPrepareSurface(SurfaceSource{}, preparation, surface, nullptr) == GeometryStatus::InvalidArgument);

    SharedPair broken = pair;
    broken.Patches[1].Control[2][1].Y = nan;
    REQUIRE(PreparePair(broken, surface) == GeometryStatus::NonFiniteInput);

    SurfaceSource badOrigin = source;
    badOrigin.Origin.X = nan;
    REQUIRE(TryPrepareSurface(badOrigin, preparation, surface, nullptr) == GeometryStatus::NonFiniteInput);
    SurfaceSource badScale = source;
    badScale.AssetScale = -1.0;
    REQUIRE(TryPrepareSurface(badScale, preparation, surface, nullptr) == GeometryStatus::InvalidArgument);

    preparation.Tolerance.NormalAngleError = 0.0;
    REQUIRE(TryPrepareSurface(source, preparation, surface, nullptr) == GeometryStatus::InvalidArgument);
    preparation = test_support::MakeSurfacePreparation();
    preparation.Limits.MaxPatches = 1;
    REQUIRE(TryPrepareSurface(source, preparation, surface, nullptr) == GeometryStatus::BudgetExceeded);
    preparation = test_support::MakeSurfacePreparation();
    preparation.Limits.MaxPreparedBytes = 2 * sizeof(PreparedPatch);
    REQUIRE(TryPrepareSurface(source, preparation, surface, nullptr) == GeometryStatus::BudgetExceeded);

    // Two records per patch at most, because each record uses two of the four sides.
    const StaticArray<SurfaceAdjacencySource, 5> tooMany{};
    const SurfaceSource crowded
    {
        .Patches = patches,
        .Adjacency = BorrowedView<const SurfaceAdjacencySource>(tooMany.GetData(), tooMany.GetSize()),
    };
    REQUIRE(TryPrepareSurface(crowded, test_support::MakeSurfacePreparation(), surface, nullptr) ==
            GeometryStatus::InvalidTopology);

    // Controls a billion units out narrow with a floor far above the profile's position error.
    SurfaceSource far = source;
    far.Origin = Vector3d{-1e9, 0.0, 0.0};
    GeometryDiagnostic diagnostic;
    REQUIRE(TryPrepareSurface(far, test_support::MakeSurfacePreparation(), surface, &diagnostic) ==
            GeometryStatus::UnsupportedPrecision);
    REQUIRE(diagnostic.AchievedBound >= 64.0);
    REQUIRE(surface.GetView().Edges.GetSize() == 7);
}

TEST_CASE("Surface evaluation matches the patch corpus and classifies singular normals",
          "[geometry][surface][fixtures]")
{
    for (const fixtures::PatchFixture& fixture : fixtures::PATCH_FIXTURES)
    {
        const SurfacePatchSource patch = test_support::MakePatchSource(fixture, FIRST_ID);
        PreparedSurface surface;
        REQUIRE(test_support::PrepareSurface(BorrowedView<const SurfacePatchSource>(&patch, 1), {}, surface) ==
                GeometryStatus::Success);
        const SurfaceView view = surface.GetView();
        for (usize sampleIndex = 0; sampleIndex < fixture.Samples.GetSize(); ++sampleIndex)
        {
            const fixtures::PatchSampleReference& reference = fixture.Samples[sampleIndex];
            math::PatchSample sample;
            REQUIRE(TryEvaluateSurface(view, SurfaceParameter{ .U = reference.U, .V = reference.V }, sample) ==
                    GeometryStatus::Success);
            REQUIRE(AreClose(sample.Position, reference.Position, SAMPLE_TOLERANCE) == true);
            REQUIRE(AreClose(sample.DU, reference.DU, SAMPLE_TOLERANCE * 10.0) == true);
            REQUIRE(AreClose(sample.DV, reference.DV, SAMPLE_TOLERANCE * 10.0) == true);
        }
        for (usize normalIndex = 0; normalIndex < fixture.Normals.GetSize(); ++normalIndex)
        {
            const fixtures::NormalReference& reference = fixture.Normals[normalIndex];
            SurfacePreparation preparation = test_support::MakeSurfacePreparation();
            preparation.Tolerance.NormalMinDerivativeLength = reference.MinDerivativeLength;
            preparation.Tolerance.NormalMinSinAngle = reference.MinSinAngle;
            PreparedSurface conditioned;
            REQUIRE(TryPrepareSurface(
                        SurfaceSource{ .Patches = BorrowedView<const SurfacePatchSource>(&patch, 1), .Adjacency = {} },
                        preparation,
                        conditioned,
                        nullptr) == GeometryStatus::Success);
            Vector3 normal{9.0F, 9.0F, 9.0F};
            const GeometryStatus status = TryEvaluateSurfaceNormal(conditioned.GetView(),
                                                                   SurfaceParameter
                                                                   {
                                                                       .U = reference.U,
                                                                       .V = reference.V,
                                                                   },
                                                                   normal);
            switch (reference.Status)
            {
                case fixtures::ExpectedStatus::Success:
                    REQUIRE(status == GeometryStatus::Success);
                    REQUIRE(AreClose(normal, reference.Normal, SAMPLE_TOLERANCE) == true);
                    break;
                case fixtures::ExpectedStatus::Degenerate:
                    REQUIRE(status == GeometryStatus::Degenerate);
                    REQUIRE(normal == Vector3{9.0F, 9.0F, 9.0F});
                    break;
                case fixtures::ExpectedStatus::IllConditioned:
                    REQUIRE(status == GeometryStatus::IllConditioned);
                    REQUIRE(normal == Vector3{9.0F, 9.0F, 9.0F});
                    break;
                case fixtures::ExpectedStatus::InvalidDomain:
                    break;
            }
        }
    }
}

TEST_CASE("Surface evaluation rejects bad parameters and leaves outputs unchanged", "[geometry][surface]")
{
    const SurfacePatchSource patch = test_support::MakePatchSource(test_support::FindPatch("saddle"), FIRST_ID);
    PreparedSurface surface;
    REQUIRE(test_support::PrepareSurface(BorrowedView<const SurfacePatchSource>(&patch, 1), {}, surface) ==
            GeometryStatus::Success);
    const SurfaceView view = surface.GetView();
    Vector3 position{5.0F, 5.0F, 5.0F};
    REQUIRE(TryEvaluateSurface(view, SurfaceParameter{ .PatchIndex = 1 }, position) == GeometryStatus::InvalidDomain);
    REQUIRE(TryEvaluateSurface(view, SurfaceParameter{ .U = 1.5 }, position) == GeometryStatus::InvalidDomain);
    REQUIRE(TryEvaluateSurface(view, SurfaceParameter{ .V = -0.25 }, position) == GeometryStatus::InvalidDomain);
    REQUIRE(TryEvaluateSurface(view, SurfaceParameter{ .U = std::numeric_limits<float64>::quiet_NaN() }, position) ==
            GeometryStatus::NonFiniteInput);
    REQUIRE(position == Vector3{5.0F, 5.0F, 5.0F});
    REQUIRE(TryEvaluateSurface(view, SurfaceParameter{ .U = 1.0, .V = 1.0 }, position) == GeometryStatus::Success);
    REQUIRE(position == Vector3{3.0F, 3.0F, 9.0F});

    SurfaceParameter mapped{ .PatchIndex = 4 };
    REQUIRE(TryMapEdgeParameter(view, 0, 1, 0.5, mapped) == GeometryStatus::InvalidDomain);
    REQUIRE(TryMapEdgeParameter(view, 9, 0, 0.5, mapped) == GeometryStatus::InvalidDomain);
    REQUIRE(TryMapEdgeParameter(view, 0, 0, 2.0, mapped) == GeometryStatus::InvalidDomain);
    REQUIRE(mapped.PatchIndex == 4);
    REQUIRE(TryMapEdgeParameter(view, 3, 0, 0.25, mapped) == GeometryStatus::Success);
    REQUIRE(mapped.U == 0.25);
    REQUIRE(mapped.V == 1.0);
    REQUIRE(PreparedSurface{}.GetView().Patches.IsEmpty() == true);
    REQUIRE(PreparedSurface{}.IsEmpty() == true);
}
