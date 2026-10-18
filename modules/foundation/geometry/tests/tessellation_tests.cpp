// Surface mesh preparation: Certified cell bounds, shared edges and failure behavior (phase G4).

#include "geometry_test_support.hpp"

#include <ludus/foundation/geometry/surface.hpp>
#include <ludus/foundation/geometry/tessellation.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>

using namespace ludus::foundation::geometry;
using ludus::foundation::BorrowedView;
using ludus::foundation::core::StaticArray;
using ludus::foundation::core::uint32;
namespace fixtures = ludus::geometry_fixtures;
namespace math = ludus::foundation::math;
namespace test_support = ludus::foundation::geometry::test_support;
using math::Vector3;
using math::Vector3d;

namespace
{
constexpr usize PAIR_PATCH_COUNT = 2;
constexpr usize CHAIN_PATCH_COUNT = 3;
constexpr uint64 FIRST_ID = 10;
constexpr uint64 SECOND_ID = 20;
constexpr uint64 THIRD_ID = 30;
constexpr usize CORPUS_MAX_LEVEL = 8;
// Interior points sampled along each cell direction by the certificate check.
constexpr usize CELL_SAMPLES = 4;
// Float32 surface evaluation and float64 triangle arithmetic stay within this of the exact distance.
constexpr float64 SAMPLING_SLACK = 1e-5;
// Relative slack of outward rounding over an exact bound.
constexpr float64 OUTWARD_SLACK = 1e-12;

[[nodiscard]] Vector3d ToDouble(const Vector3& value) noexcept
{
    return Vector3d{static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

[[nodiscard]] Vector3d ToDouble(const fixtures::ReferencePoint& value) noexcept
{
    return Vector3d{value.X, value.Y, value.Z};
}

struct PreparedFixture final
{
    SurfacePatchSource Patch;
    PreparedSurface Surface;
};

void PrepareSingle(const char* name, PreparedFixture& outFixture)
{
    outFixture.Patch = test_support::MakePatchSource(test_support::FindPatch(name), FIRST_ID);
    REQUIRE(test_support::PrepareSurface(BorrowedView<const SurfacePatchSource>(&outFixture.Patch, 1),
                                         {},
                                         outFixture.Surface) == GeometryStatus::Success);
}

// Point of the emitted triangle pair of cell (i, j) at local parameters (a, b), split along (0, 0)-(1, 1).
[[nodiscard]] Vector3d EvaluateTriangles(const SurfaceMeshView& mesh,
                                         const SurfaceMeshPatch& patch,
                                         usize i,
                                         usize j,
                                         math::PatchParameter local) noexcept
{
    const float64 a = local.U;
    const float64 b = local.V;
    const usize rowLength = (usize{1} << patch.LevelV) + 1;
    const usize v00 = patch.FirstVertex + i * rowLength + j;
    const Vector3d p00 = ToDouble(mesh.Vertices[v00].Position);
    const Vector3d p10 = ToDouble(mesh.Vertices[v00 + rowLength].Position);
    const Vector3d p01 = ToDouble(mesh.Vertices[v00 + 1].Position);
    const Vector3d p11 = ToDouble(mesh.Vertices[v00 + rowLength + 1].Position);
    if (a >= b)
    {
        return p00 + a * (p10 - p00) + b * (p11 - p10);
    }
    return p00 + b * (p01 - p00) + a * (p11 - p01);
}

// Largest sampled distance between a patch and its triangles; the Certified bound must exceed it.
// Helpers that use REQUIRE are not noexcept, because Catch2 reports failures by throwing.
[[nodiscard]] float64 MeasureSampledDeviation(const SurfaceView& surface, const SurfaceMeshView& mesh, usize patchIndex)
{
    const SurfaceMeshPatch& patch = mesh.Patches[patchIndex];
    const usize uCells = usize{1} << patch.LevelU;
    const usize vCells = usize{1} << patch.LevelV;
    float64 deviation = 0.0;
    for (usize i = 0; i < uCells; ++i)
    {
        for (usize j = 0; j < vCells; ++j)
        {
            for (usize sa = 0; sa <= CELL_SAMPLES; ++sa)
            {
                for (usize sb = 0; sb <= CELL_SAMPLES; ++sb)
                {
                    const float64 a = static_cast<float64>(sa) / static_cast<float64>(CELL_SAMPLES);
                    const float64 b = static_cast<float64>(sb) / static_cast<float64>(CELL_SAMPLES);
                    const SurfaceParameter parameter
                    {
                        .PatchIndex = patchIndex,
                        .U = (static_cast<float64>(i) + a) / static_cast<float64>(uCells),
                        .V = (static_cast<float64>(j) + b) / static_cast<float64>(vCells),
                    };
                    Vector3 exact;
                    REQUIRE(TryEvaluateSurface(surface, parameter, exact) == GeometryStatus::Success);
                    deviation = math::Max(
                        deviation,
                        math::Length(ToDouble(exact) -
                                     EvaluateTriangles(mesh, patch, i, j, math::PatchParameter{ .U = a, .V = b })));
                }
            }
        }
    }
    return deviation;
}

void RequireCertificate(const SurfaceView& surface, const SurfaceMeshView& mesh)
{
    REQUIRE(mesh.Report.PositionGuarantee == GuaranteeClass::Certified);
    for (usize patchIndex = 0; patchIndex < mesh.Patches.GetSize(); ++patchIndex)
    {
        REQUIRE(MeasureSampledDeviation(surface, mesh, patchIndex) <=
                mesh.Patches[patchIndex].PositionBound + SAMPLING_SLACK);
    }
    for (usize i = 0; i < mesh.Positions.GetSize(); ++i)
    {
        const SurfaceMeshPosition& position = mesh.Positions[i];
        if (position.Kind == SurfaceSampleKind::Corner)
        {
            REQUIRE(position.ErrorBound == 0.0);
        }
    }
}

// edge_first, edge_second and a planar third patch on the second's straight u = 1 side.
struct Chain final
{
    StaticArray<SurfacePatchSource, CHAIN_PATCH_COUNT> Patches;
    StaticArray<SurfaceAdjacencySource, CHAIN_PATCH_COUNT - 1> Adjacency;
    PreparedSurface Surface;
};

void PrepareChain(usize patchCount, Chain& outChain)
{
    const fixtures::SharedEdgeFixture& shared = fixtures::SHARED_EDGE_FIXTURES[0];
    outChain.Patches[0] = test_support::MakePatchSource(fixtures::PATCH_FIXTURES[shared.FirstPatch], FIRST_ID);
    outChain.Patches[1] = test_support::MakePatchSource(fixtures::PATCH_FIXTURES[shared.SecondPatch], SECOND_ID);
    outChain.Patches[2].Id = THIRD_ID;
    for (usize i = 0; i < PATCH_CONTROL_ORDER; ++i)
    {
        for (usize j = 0; j < PATCH_CONTROL_ORDER; ++j)
        {
            outChain.Patches[2].Control[i][j] =
                Vector3d{6.0 + static_cast<float64>(i), 3.0 - static_cast<float64>(j), 0.0};
        }
    }
    outChain.Adjacency[0] = SurfaceAdjacencySource
    {
        .FirstPatch = 0,
        .FirstSide = PatchSide::U1,
        .SecondPatch = 1,
        .SecondSide = PatchSide::U0,
        .IsReversed = true,
    };
    outChain.Adjacency[1] = SurfaceAdjacencySource
    {
        .FirstPatch = 1,
        .FirstSide = PatchSide::U1,
        .SecondPatch = 2,
        .SecondSide = PatchSide::U0,
    };
    REQUIRE(test_support::PrepareSurface(
                BorrowedView<const SurfacePatchSource>(outChain.Patches.GetData(), patchCount),
                BorrowedView<const SurfaceAdjacencySource>(outChain.Adjacency.GetData(), patchCount - 1),
                outChain.Surface) == GeometryStatus::Success);
}
} // namespace

TEST_CASE("A planar patch needs one cell and keeps its corners exactly", "[geometry][tessellation][fixtures]")
{
    PreparedFixture fixture;
    PrepareSingle("plane", fixture);
    SurfaceMesh mesh;
    REQUIRE(TryBuildSurfaceMesh(fixture.Surface.GetView(),
                                test_support::MakeMeshProfile(1e-6, CORPUS_MAX_LEVEL),
                                mesh,
                                nullptr) == GeometryStatus::Success);
    const SurfaceMeshView view = mesh.GetView();
    REQUIRE(view.Patches[0].LevelU == 0);
    REQUIRE(view.Patches[0].LevelV == 0);
    REQUIRE(view.Positions.GetSize() == PATCH_CORNER_COUNT);
    REQUIRE(view.Vertices.GetSize() == PATCH_CORNER_COUNT);
    REQUIRE(view.Indices.GetSize() == 6);
    REQUIRE(view.Report.PositionBound <= 1e-12);
    REQUIRE(view.Report.IsWithinPositionError == true);
    REQUIRE(view.SurfaceRevision == 7);
    // Counterclockwise in (u, v) along the (0, 0)-(1, 1) diagonal: (00, 10, 11) and (00, 11, 01).
    const StaticArray<uint32, 6> expected{{0, 2, 3, 0, 3, 1}};
    for (usize i = 0; i < expected.GetSize(); ++i)
    {
        REQUIRE(view.Indices[i] == expected[i]);
    }
    REQUIRE(view.Vertices[3].Position == Vector3{3.0F, 3.0F, 0.0F});
    REQUIRE(view.Vertices[3].Normal == Vector3{0.0F, 0.0F, 1.0F});
    RequireCertificate(fixture.Surface.GetView(), view);
}

TEST_CASE("The saddle's twisted bilinear interior has the tight 9/4 bound", "[geometry][tessellation][fixtures]")
{
    const fixtures::TessellationReference& reference = test_support::FindPatch("saddle").Tessellation;
    PreparedFixture fixture;
    PrepareSingle("saddle", fixture);
    SurfaceMesh mesh;
    // At level 0 the control net is bilinear, so the whole bound is the twist term.
    REQUIRE(TryBuildSurfaceMesh(fixture.Surface.GetView(),
                                test_support::MakeMeshProfile(reference.InteriorBound * 1.001, 0),
                                mesh,
                                nullptr) == GeometryStatus::Success);
    SurfaceMeshView view = mesh.GetView();
    REQUIRE(view.Report.PositionBound >= reference.TwistBound);
    REQUIRE(view.Report.PositionBound <= reference.TwistBound * (1.0 + OUTWARD_SLACK));
    // The analytic maximum at the centre: the patch is at z = 9/4 and the diagonal at z = 9/2.
    const Vector3d triangles = EvaluateTriangles(view,
                                                 view.Patches[0],
                                                 0,
                                                 0,
                                                 math::PatchParameter
                                                 {
                                                     .U = 0.5,
                                                     .V = 0.5,
                                                 });
    REQUIRE(math::Abs(triangles.Z - 4.5) <= 1e-12);
    REQUIRE(math::Abs((triangles.Z - 2.25) - reference.TrueMaxDeviation) <= 1e-12);
    RequireCertificate(fixture.Surface.GetView(), view);

    // Below the twist bound one cell is not enough; refinement halves the twist per level.
    GeometryDiagnostic diagnostic;
    REQUIRE(TryBuildSurfaceMesh(fixture.Surface.GetView(), test_support::MakeMeshProfile(2.24, 0), mesh, &diagnostic) ==
            GeometryStatus::BudgetExceeded);
    REQUIRE(std::strcmp(diagnostic.Property, "Limits.MaxLevel") == 0);
    REQUIRE(diagnostic.AchievedBound >= reference.TwistBound);
    REQUIRE(diagnostic.AchievedGuarantee == GuaranteeClass::Certified);
    REQUIRE(mesh.GetView().Patches[0].LevelU == 0);

    REQUIRE(TryBuildSurfaceMesh(fixture.Surface.GetView(),
                                test_support::MakeMeshProfile(0.01, CORPUS_MAX_LEVEL),
                                mesh,
                                nullptr) == GeometryStatus::Success);
    view = mesh.GetView();
    const SurfaceMeshPatch& patch = view.Patches[0];
    // (9/4) / 2^(LevelU + LevelV) <= 0.01 first holds at a level sum of 8; near-ties refine the coarser direction.
    REQUIRE(patch.LevelU + patch.LevelV == 8);
    REQUIRE(patch.LevelU == patch.LevelV);
    REQUIRE(patch.PositionBound <= 0.01);
    REQUIRE(patch.PositionBound >= reference.TwistBound / 256.0);
    REQUIRE(patch.IsWithinTolerance == true);
    RequireCertificate(fixture.Surface.GetView(), view);
}

TEST_CASE("The bulge's control-net bound is 3 against a true deviation of 27/16", "[geometry][tessellation][fixtures]")
{
    const fixtures::TessellationReference& reference = test_support::FindPatch("bulge").Tessellation;
    PreparedFixture fixture;
    PrepareSingle("bulge", fixture);
    SurfaceMesh mesh;
    REQUIRE(TryBuildSurfaceMesh(fixture.Surface.GetView(),
                                test_support::MakeMeshProfile(reference.InteriorBound * 1.001, 0),
                                mesh,
                                nullptr) == GeometryStatus::Success);
    const SurfaceMeshView view = mesh.GetView();
    REQUIRE(view.Report.PositionBound >= reference.ControlNetBound);
    REQUIRE(view.Report.PositionBound <= reference.InteriorBound * (1.0 + OUTWARD_SLACK));
    REQUIRE(reference.TrueMaxDeviation < view.Report.PositionBound);
    const Vector3d triangles = EvaluateTriangles(view,
                                                 view.Patches[0],
                                                 0,
                                                 0,
                                                 math::PatchParameter
                                                 {
                                                     .U = 0.5,
                                                     .V = 0.5,
                                                 });
    Vector3 centre;
    REQUIRE(TryEvaluateSurface(fixture.Surface.GetView(), SurfaceParameter{ .U = 0.5, .V = 0.5 }, centre) ==
            GeometryStatus::Success);
    REQUIRE(math::Abs(math::Length(ToDouble(centre) - triangles) - reference.TrueMaxDeviation) <= 1e-6);
    RequireCertificate(fixture.Surface.GetView(), view);

    SurfaceMesh refined;
    REQUIRE(TryBuildSurfaceMesh(fixture.Surface.GetView(),
                                test_support::MakeMeshProfile(0.05, CORPUS_MAX_LEVEL),
                                refined,
                                nullptr) == GeometryStatus::Success);
    REQUIRE(refined.GetView().Report.PositionBound <= 0.05);
    RequireCertificate(fixture.Surface.GetView(), refined.GetView());
}

TEST_CASE("Every corpus patch's level-0 bound matches its exact control-net plus twist reference",
          "[geometry][tessellation][fixtures]")
{
    for (const fixtures::PatchFixture& patchFixture : fixtures::PATCH_FIXTURES)
    {
        const fixtures::TessellationReference& reference = patchFixture.Tessellation;
        PreparedFixture fixture;
        PrepareSingle(patchFixture.Name, fixture);
        SurfaceMesh mesh;
        SurfaceMeshProfile profile = test_support::MakeMeshProfile(reference.InteriorBound + 1.0, 0);
        REQUIRE(TryBuildSurfaceMesh(fixture.Surface.GetView(), profile, mesh, nullptr) == GeometryStatus::Success);
        const float64 bound = mesh.GetView().Report.PositionBound;
        REQUIRE(bound >= reference.InteriorBound * (1.0 - OUTWARD_SLACK));
        REQUIRE(bound <= reference.InteriorBound * (1.0 + OUTWARD_SLACK) + 1e-12);
        if (reference.HasTrueMaxDeviation == true)
        {
            REQUIRE(reference.TrueMaxDeviation <= bound);
        }
    }
}

TEST_CASE("The apex patch reports or rejects its singular normals and collapsed triangles",
          "[geometry][tessellation][fixtures]")
{
    PreparedFixture fixture;
    PrepareSingle("apex", fixture);
    const SurfaceView surface = fixture.Surface.GetView();
    SurfaceMesh mesh;
    REQUIRE(TryBuildSurfaceMesh(surface, test_support::MakeMeshProfile(0.05, CORPUS_MAX_LEVEL), mesh, nullptr) ==
            GeometryStatus::Success);
    const SurfaceMeshView view = mesh.GetView();
    const SurfaceMeshPatch& patch = view.Patches[0];
    const usize vCells = usize{1} << patch.LevelV;
    // The whole u = 0 row is the apex: every vertex there has zero DV, and one triangle per cell collapses.
    REQUIRE(view.Report.DegenerateNormalCount == vCells + 1);
    REQUIRE(view.Report.CollapsedTriangleCount == vCells);
    for (usize j = 0; j <= vCells; ++j)
    {
        const SurfaceMeshVertex& vertex = view.Vertices[patch.FirstVertex + j];
        REQUIRE(vertex.NormalStatus == GeometryStatus::Degenerate);
        REQUIRE(vertex.Normal == Vector3{});
        REQUIRE(vertex.Position == Vector3{0.0F, 0.0F, 3.0F});
    }
    REQUIRE(view.Report.PositionBound <= 0.05);
    RequireCertificate(surface, view);

    SurfaceMeshProfile reject = test_support::MakeMeshProfile(0.05, CORPUS_MAX_LEVEL);
    reject.Degeneracy = SurfaceDegeneracyPolicy::Reject;
    GeometryDiagnostic diagnostic;
    REQUIRE(TryBuildSurfaceMesh(surface, reject, mesh, &diagnostic) == GeometryStatus::Degenerate);
    REQUIRE(diagnostic.Domain == DomainKind::SurfaceParameter);
    REQUIRE(diagnostic.RangeStart == 0.0);
    REQUIRE(mesh.GetView().Report.DegenerateNormalCount == vCells + 1);

    // A position-close mesh is not evidence of normal accuracy: a Strict normal request has no certificate.
    SurfaceMeshProfile strictNormals = test_support::MakeMeshProfile(0.05, CORPUS_MAX_LEVEL);
    strictNormals.NormalAngleError = 0.1;
    REQUIRE(TryBuildSurfaceMesh(surface, strictNormals, mesh, &diagnostic) == GeometryStatus::UnsupportedPrecision);
    REQUIRE(diagnostic.AchievedGuarantee == GuaranteeClass::Estimated);
}

TEST_CASE("Shared edges use one level and emit their canonical samples once", "[geometry][tessellation][fixtures]")
{
    const fixtures::SharedEdgeFixture& shared = fixtures::SHARED_EDGE_FIXTURES[0];
    Chain chain;
    PrepareChain(PAIR_PATCH_COUNT, chain);
    const SurfaceView surface = chain.Surface.GetView();
    SurfaceMesh mesh;
    REQUIRE(TryBuildSurfaceMesh(surface, test_support::MakeMeshProfile(0.05, CORPUS_MAX_LEVEL), mesh, nullptr) ==
            GeometryStatus::Success);
    const SurfaceMeshView view = mesh.GetView();
    RequireCertificate(surface, view);
    const usize edgeIndex = surface.Patches[0].Edge[static_cast<usize>(PatchSide::U1)];
    const SurfaceMeshPatch& first = view.Patches[0];
    const SurfaceMeshPatch& second = view.Patches[1];
    REQUIRE(first.LevelV == second.LevelV);
    REQUIRE(view.EdgeLevels[edgeIndex] == first.LevelV);
    REQUIRE(first.LevelV >= 2);

    // Each canonical edge sample exists once, and both patches' boundary vertices name it.
    const usize edgeCells = usize{1} << first.LevelV;
    usize edgeSampleCount = 0;
    for (usize i = 0; i < view.Positions.GetSize(); ++i)
    {
        const SurfaceMeshPosition& position = view.Positions[i];
        if (position.Kind == SurfaceSampleKind::Edge && position.OwnerIndex == edgeIndex)
        {
            ++edgeSampleCount;
        }
    }
    REQUIRE(edgeSampleCount == edgeCells - 1);
    const usize firstRow = (usize{1} << first.LevelU) * (edgeCells + 1);
    bool hasNormalSeam = false;
    for (usize j = 0; j <= edgeCells; ++j)
    {
        // First patch at (1, v); second patch at (0, 1 - v).
        const SurfaceMeshVertex& a = view.Vertices[first.FirstVertex + firstRow + j];
        const SurfaceMeshVertex& b = view.Vertices[second.FirstVertex + (edgeCells - j)];
        REQUIRE(a.PositionIndex == b.PositionIndex);
        REQUIRE(a.Position == b.Position);
        REQUIRE(a.Parameter.U == 1.0);
        REQUIRE(b.Parameter.U == 0.0);
        REQUIRE(a.Parameter.V == 1.0 - b.Parameter.V);
        hasNormalSeam = hasNormalSeam == true || (a.Normal == b.Normal) == false;
    }
    // The patches are not G1 and are oppositely oriented, so their normals differ at the shared positions.
    REQUIRE(hasNormalSeam == true);

    // Emitted canonical samples agree with the exact references within their Certified rounding bounds.
    for (usize sampleIndex = 0; sampleIndex < shared.Samples.GetSize(); ++sampleIndex)
    {
        const fixtures::EdgeSampleReference& sample = shared.Samples[sampleIndex];
        const usize j = static_cast<usize>(sample.FirstV * static_cast<float64>(edgeCells));
        const SurfaceMeshVertex& vertex = view.Vertices[first.FirstVertex + firstRow + j];
        const SurfaceMeshPosition& position = view.Positions[vertex.PositionIndex];
        REQUIRE(math::Length(ToDouble(position.Position) - ToDouble(sample.Position)) <= position.ErrorBound);
        if (position.Kind == SurfaceSampleKind::Edge)
        {
            REQUIRE(position.EdgeParameter == sample.FirstV);
        }
    }
    usize expectedPositions = surface.Corners.GetSize();
    for (usize i = 0; i < view.EdgeLevels.GetSize(); ++i)
    {
        expectedPositions += (usize{1} << view.EdgeLevels[i]) - 1;
    }
    for (usize i = 0; i < view.Patches.GetSize(); ++i)
    {
        expectedPositions += ((usize{1} << view.Patches[i].LevelU) - 1) * ((usize{1} << view.Patches[i].LevelV) - 1);
    }
    REQUIRE(view.Positions.GetSize() == expectedPositions);
}

TEST_CASE("Edge levels propagate through a patch network", "[geometry][tessellation][fixtures]")
{
    Chain chain;
    PrepareChain(CHAIN_PATCH_COUNT, chain);
    const SurfaceView surface = chain.Surface.GetView();
    SurfaceMesh mesh;
    REQUIRE(TryBuildSurfaceMesh(surface, test_support::MakeMeshProfile(0.05, CORPUS_MAX_LEVEL), mesh, nullptr) ==
            GeometryStatus::Success);
    const SurfaceMeshView view = mesh.GetView();
    // The planar third patch alone needs one cell, but its u = 0 side inherits the curved edge's v level
    // through the second patch's opposite side.
    REQUIRE(view.Patches[2].LevelU == 0);
    REQUIRE(view.Patches[2].LevelV == view.Patches[0].LevelV);
    REQUIRE(view.Patches[1].LevelV == view.Patches[0].LevelV);
    REQUIRE(view.Patches[2].PositionBound <= 1e-12);
    RequireCertificate(surface, view);

    SurfaceMesh again;
    REQUIRE(TryBuildSurfaceMesh(surface, test_support::MakeMeshProfile(0.05, CORPUS_MAX_LEVEL), again, nullptr) ==
            GeometryStatus::Success);
    const SurfaceMeshView repeat = again.GetView();
    REQUIRE(repeat.Positions.GetSize() == view.Positions.GetSize());
    REQUIRE(repeat.Indices.GetSize() == view.Indices.GetSize());
    for (usize i = 0; i < view.Positions.GetSize(); ++i)
    {
        REQUIRE(repeat.Positions[i].Position == view.Positions[i].Position);
    }
    for (usize i = 0; i < view.Indices.GetSize(); ++i)
    {
        REQUIRE(repeat.Indices[i] == view.Indices[i]);
    }
    REQUIRE(repeat.Report.CellTestCount == view.Report.CellTestCount);
}

TEST_CASE("Strict and visual profiles fail or degrade explicitly when budgets run out", "[geometry][tessellation]")
{
    PreparedFixture fixture;
    PrepareSingle("saddle", fixture);
    const SurfaceView surface = fixture.Surface.GetView();
    SurfaceMesh active;
    REQUIRE(TryBuildSurfaceMesh(surface, test_support::MakeMeshProfile(0.5, CORPUS_MAX_LEVEL), active, nullptr) ==
            GeometryStatus::Success);
    const usize activeVertexCount = active.GetView().Vertices.GetSize();

    SECTION("Strict exhaustion keeps the active mesh")
    {
        GeometryDiagnostic diagnostic;
        REQUIRE(TryBuildSurfaceMesh(surface, test_support::MakeMeshProfile(0.01, 1), active, &diagnostic) ==
                GeometryStatus::BudgetExceeded);
        REQUIRE(diagnostic.AchievedBound >= 9.0 / 16.0);
        REQUIRE(diagnostic.RequestedTolerance == 0.01);
    }
    SECTION("Visual exhaustion fails unless a coarser mesh is explicitly accepted")
    {
        SurfaceMeshProfile visual = test_support::MakeMeshProfile(0.01, 1);
        visual.Precision = PrecisionProfile::Visual;
        REQUIRE(TryBuildSurfaceMesh(surface, visual, active, nullptr) == GeometryStatus::BudgetExceeded);
        REQUIRE(active.GetView().Vertices.GetSize() == activeVertexCount);
        visual.IsCoarserMeshAccepted = true;
        SurfaceMesh coarse;
        REQUIRE(TryBuildSurfaceMesh(surface, visual, coarse, nullptr) == GeometryStatus::Success);
        const SurfaceMeshView view = coarse.GetView();
        REQUIRE(view.Patches[0].LevelU == 1);
        REQUIRE(view.Patches[0].LevelV == 1);
        REQUIRE(view.Patches[0].IsWithinTolerance == false);
        REQUIRE(view.Report.IsWithinPositionError == false);
        REQUIRE(view.Report.PositionBound >= 9.0 / 16.0);
        REQUIRE(view.Report.PositionGuarantee == GuaranteeClass::Certified);
        RequireCertificate(surface, view);
    }
    SECTION("A Strict profile cannot accept a coarser mesh or an Estimated normal bound")
    {
        SurfaceMeshProfile strict = test_support::MakeMeshProfile(0.01, 1);
        strict.IsCoarserMeshAccepted = true;
        REQUIRE(TryBuildSurfaceMesh(surface, strict, active, nullptr) == GeometryStatus::InvalidArgument);
        strict.IsCoarserMeshAccepted = false;
        strict.NormalAngleError = 0.2;
        REQUIRE(TryBuildSurfaceMesh(surface, strict, active, nullptr) == GeometryStatus::UnsupportedPrecision);
    }
    SECTION("Work and output budgets")
    {
        SurfaceMeshProfile limited = test_support::MakeMeshProfile(0.01, CORPUS_MAX_LEVEL);
        limited.Limits.MaxCellTests = 64;
        GeometryDiagnostic diagnostic;
        REQUIRE(TryBuildSurfaceMesh(surface, limited, active, &diagnostic) == GeometryStatus::BudgetExceeded);
        REQUIRE(std::strcmp(diagnostic.Property, "Limits.MaxCellTests") == 0);
        limited = test_support::MakeMeshProfile(0.01, CORPUS_MAX_LEVEL);
        limited.Limits.MaxVertices = 100;
        REQUIRE(TryBuildSurfaceMesh(surface, limited, active, nullptr) == GeometryStatus::BudgetExceeded);
        limited = test_support::MakeMeshProfile(0.01, CORPUS_MAX_LEVEL);
        limited.Limits.MaxTriangles = 100;
        REQUIRE(TryBuildSurfaceMesh(surface, limited, active, nullptr) == GeometryStatus::BudgetExceeded);
        limited = test_support::MakeMeshProfile(0.01, CORPUS_MAX_LEVEL);
        limited.Limits.MaxOutputBytes = 1024;
        REQUIRE(TryBuildSurfaceMesh(surface, limited, active, nullptr) == GeometryStatus::BudgetExceeded);
    }
    SECTION("Invalid profiles")
    {
        const float64 nan = std::numeric_limits<float64>::quiet_NaN();
        REQUIRE(TryBuildSurfaceMesh(surface, test_support::MakeMeshProfile(nan, 4), active, nullptr) ==
                GeometryStatus::NonFiniteInput);
        REQUIRE(TryBuildSurfaceMesh(surface, test_support::MakeMeshProfile(-1.0, 4), active, nullptr) ==
                GeometryStatus::InvalidArgument);
        REQUIRE(TryBuildSurfaceMesh(surface,
                                    test_support::MakeMeshProfile(0.1, MAX_SURFACE_MESH_LEVEL + 1),
                                    active,
                                    nullptr) == GeometryStatus::InvalidArgument);
        REQUIRE(TryBuildSurfaceMesh(surface, test_support::MakeMeshProfile(1e-8, 4), active, nullptr) ==
                GeometryStatus::UnsupportedPrecision);
        SurfaceMeshProfile badNormal = test_support::MakeMeshProfile(0.1, 4);
        badNormal.Precision = PrecisionProfile::Visual;
        badNormal.NormalAngleError = math::kHalfPi;
        REQUIRE(TryBuildSurfaceMesh(surface, badNormal, active, nullptr) == GeometryStatus::InvalidArgument);
        REQUIRE(TryBuildSurfaceMesh(SurfaceView{}, test_support::MakeMeshProfile(0.1, 4), active, nullptr) ==
                GeometryStatus::InvalidArgument);
    }
    REQUIRE(active.GetView().Vertices.GetSize() == activeVertexCount);
}

TEST_CASE("A visual normal bound refines by an Estimated sampled angle", "[geometry][tessellation][fixtures]")
{
    PreparedFixture fixture;
    PrepareSingle("saddle", fixture);
    SurfaceMeshProfile visual = test_support::MakeMeshProfile(1.0, CORPUS_MAX_LEVEL);
    visual.Precision = PrecisionProfile::Visual;
    SurfaceMesh positionOnly;
    REQUIRE(TryBuildSurfaceMesh(fixture.Surface.GetView(), visual, positionOnly, nullptr) == GeometryStatus::Success);
    REQUIRE(positionOnly.GetView().Report.HasNormalAngleEstimate == false);

    visual.NormalAngleError = 0.2;
    SurfaceMesh mesh;
    REQUIRE(TryBuildSurfaceMesh(fixture.Surface.GetView(), visual, mesh, nullptr) == GeometryStatus::Success);
    const SurfaceMeshView view = mesh.GetView();
    REQUIRE(view.Report.HasNormalAngleEstimate == true);
    REQUIRE(view.Report.NormalGuarantee == GuaranteeClass::Estimated);
    REQUIRE(view.Report.NormalAngleEstimate <= 0.2);
    REQUIRE(view.Report.PositionGuarantee == GuaranteeClass::Certified);
    REQUIRE(view.Vertices.GetSize() > positionOnly.GetView().Vertices.GetSize());
    REQUIRE(SurfaceMesh{}.IsEmpty() == true);
}
