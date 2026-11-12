// Hierarchy traversal against the all-triangles oracle on seeded random scenes.
//
// Both traversals share the per-triangle arithmetic, so any difference is a
// pruning or ordering defect in the hierarchy. Results are compared exactly.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;

namespace
{
constexpr uint32 SCENE_SEED_COUNT = 4;
constexpr uint32 QUERY_COUNT = 1500;
constexpr uint32 SOUP_TRIANGLE_COUNT = 400;
constexpr float32 SOUP_EXTENT_METERS = 10.0f;
constexpr uint32 TERRAIN_CELLS = 24;

bool AreEqual(const RaycastResult& first, const RaycastResult& second)
{
    return first.HasHit == second.HasHit && first.DistanceMeters == second.DistanceMeters &&
           first.PositionMesh == second.PositionMesh && first.NormalMesh == second.NormalMesh &&
           first.TriangleIndex == second.TriangleIndex && first.IsFrontFace == second.IsFrontFace;
}

bool AreEqual(const BoxCastResult& first, const BoxCastResult& second)
{
    return first.HasHit == second.HasHit && first.Fraction == second.Fraction &&
           first.DistanceMeters == second.DistanceMeters &&
           first.HasStartedOverlapping == second.HasStartedOverlapping && first.HasNormal == second.HasNormal &&
           first.NormalMesh == second.NormalMesh && first.TriangleIndex == second.TriangleIndex;
}

TriangleFacePolicy NextFacePolicy(RandomStream& inoutRandom)
{
    constexpr uint32 POLICY_COUNT = 3;
    return static_cast<TriangleFacePolicy>(inoutRandom.NextUInt32() % POLICY_COUNT);
}

InitialOverlapPolicy NextOverlapPolicy(RandomStream& inoutRandom)
{
    constexpr uint32 POLICY_COUNT = 3;
    return static_cast<InitialOverlapPolicy>(inoutRandom.NextUInt32() % POLICY_COUNT);
}

/// Counts that show a scene exercised hits as well as misses.
struct Coverage final
{
    uint32 Hits = 0;
    uint32 Misses = 0;
    uint32 Overlaps = 0;
    uint32 Rejections = 0;
};

void CompareRay(const TriangleMesh& mesh, RaycastInput input, Coverage& inoutCoverage)
{
    RaycastResult hierarchy;
    RaycastResult oracle;
    input.Traversal = QueryTraversal::Hierarchy;
    REQUIRE(mesh.RaycastClosest(input, hierarchy) == Status::Success);
    input.Traversal = QueryTraversal::AllTriangles;
    REQUIRE(mesh.RaycastClosest(input, oracle) == Status::Success);
    REQUIRE(AreEqual(hierarchy, oracle) == true);
    ++(hierarchy.HasHit == true ? inoutCoverage.Hits : inoutCoverage.Misses);
}

void CompareBox(const TriangleMesh& mesh, BoxCastInput input, Coverage& inoutCoverage)
{
    BoxCastResult hierarchy;
    BoxCastResult oracle;
    input.Traversal = QueryTraversal::Hierarchy;
    const Status hierarchyStatus = mesh.CastBoxClosest(input, hierarchy);
    input.Traversal = QueryTraversal::AllTriangles;
    const Status oracleStatus = mesh.CastBoxClosest(input, oracle);
    REQUIRE(hierarchyStatus == oracleStatus);
    if (hierarchyStatus == Status::InitialOverlap)
    {
        ++inoutCoverage.Rejections;
        return;
    }
    REQUIRE(hierarchyStatus == Status::Success);
    REQUIRE(AreEqual(hierarchy, oracle) == true);
    ++(hierarchy.HasHit == true ? inoutCoverage.Hits : inoutCoverage.Misses);
    if (hierarchy.HasStartedOverlapping == true)
    {
        ++inoutCoverage.Overlaps;
    }
}

RaycastInput NextRay(RandomStream& inoutRandom, float32 extent)
{
    return
    {
        .OriginMesh = Widen(NextPoint(inoutRandom, extent)),
        .DirectionMesh = NextDirection(inoutRandom),
        .MaxDistanceMeters = static_cast<float64>(NextInRange(inoutRandom, 0.5f, 4.0f * extent)),
        .FacePolicy = NextFacePolicy(inoutRandom),
    };
}

BoxCastInput NextBoxCast(RandomStream& inoutRandom, float32 extent)
{
    const float32 halfSize = NextInRange(inoutRandom, 0.05f, 1.5f);
    BoxCastInput input
    {
        .CenterMesh = Widen(NextPoint(inoutRandom, extent)),
        .HalfExtentsMesh = {static_cast<float64>(halfSize * NextInRange(inoutRandom, 0.25f, 1.0f)),
                            static_cast<float64>(halfSize * NextInRange(inoutRandom, 0.25f, 1.0f)),
                            static_cast<float64>(halfSize * NextInRange(inoutRandom, 0.25f, 1.0f))},
        .TranslationMesh = Widen(NextPoint(inoutRandom, extent)),
        .OverlapPolicy = NextOverlapPolicy(inoutRandom),
    };
    // Exercise zero and axis-aligned translations, whose stationary axes take a separate branch.
    constexpr uint32 SPECIAL_CASE_COUNT = 8;
    const uint32 special = inoutRandom.NextUInt32() % SPECIAL_CASE_COUNT;
    if (special == 0)
    {
        input.TranslationMesh = {};
    }
    else if (special == 1)
    {
        input.TranslationMesh = {0.0, input.TranslationMesh.Y, 0.0};
    }
    else if (special == 2)
    {
        input.TranslationMesh = {input.TranslationMesh.X, 0.0, input.TranslationMesh.Z};
    }
    return input;
}
} // namespace

TEST_CASE("Hierarchy raycasts match the all-triangles oracle on random soups", "[collision][oracle][ray]")
{
    for (uint32 seed = 1; seed <= SCENE_SEED_COUNT; ++seed)
    {
        const MeshData data = MakeTriangleSoup(
        {
            .Seed = seed,
            .TriangleCount = SOUP_TRIANGLE_COUNT,
            .ExtentMeters = SOUP_EXTENT_METERS,
        });
        const TriangleMesh mesh = CookOrFail(data);
        REQUIRE(mesh.GetHierarchyDepth() > 1);
        RandomStream random;
        REQUIRE(random.TryReseed(seed, 1) == true);
        Coverage coverage;
        for (uint32 query = 0; query < QUERY_COUNT; ++query)
        {
            CompareRay(mesh, NextRay(random, SOUP_EXTENT_METERS), coverage);
        }
        CHECK(coverage.Hits > QUERY_COUNT / 10);
        CHECK(coverage.Misses > QUERY_COUNT / 10);
    }
}

TEST_CASE("Hierarchy box casts match the all-triangles oracle on random soups", "[collision][oracle][box]")
{
    for (uint32 seed = 1; seed <= SCENE_SEED_COUNT; ++seed)
    {
        const MeshData data = MakeTriangleSoup(
        {
            .Seed = seed,
            .TriangleCount = SOUP_TRIANGLE_COUNT,
            .ExtentMeters = SOUP_EXTENT_METERS,
        });
        const TriangleMesh mesh = CookOrFail(data);
        RandomStream random;
        REQUIRE(random.TryReseed(seed, 2) == true);
        Coverage coverage;
        for (uint32 query = 0; query < QUERY_COUNT; ++query)
        {
            CompareBox(mesh, NextBoxCast(random, 1.5f * SOUP_EXTENT_METERS), coverage);
        }
        CHECK(coverage.Hits > QUERY_COUNT / 10);
        CHECK(coverage.Misses > QUERY_COUNT / 10);
        CHECK(coverage.Overlaps > 0);
        CHECK(coverage.Rejections > 0);
    }
}

TEST_CASE("Hierarchy queries match the oracle on terrain with shared edges", "[collision][oracle]")
{
    for (uint32 seed = 1; seed <= SCENE_SEED_COUNT; ++seed)
    {
        RandomStream heights;
        REQUIRE(heights.TryReseed(seed, 3) == true);
        const MeshData data = MakeGrid(TERRAIN_CELLS, &heights);
        const TriangleMesh mesh = CookOrFail(data);
        RandomStream random;
        REQUIRE(random.TryReseed(seed, 4) == true);
        Coverage rayCoverage;
        Coverage boxCoverage;
        constexpr float32 SPAN = static_cast<float32>(TERRAIN_CELLS);
        for (uint32 query = 0; query < QUERY_COUNT; ++query)
        {
            // Vertical rays through exact grid vertices and edges must always hit a closed terrain.
            const float64 x = static_cast<float64>(random.NextUInt32() % (2 * TERRAIN_CELLS + 1)) * 0.5;
            const float64 z = static_cast<float64>(random.NextUInt32() % (2 * TERRAIN_CELLS + 1)) * 0.5;
            const RaycastInput vertical
            {
                .OriginMesh = {x, 2.0, z},
                .DirectionMesh = {0.0, -1.0, 0.0},
                .MaxDistanceMeters = 4.0,
            };
            RaycastResult result;
            REQUIRE(mesh.RaycastClosest(vertical, result) == Status::Success);
            REQUIRE(result.HasHit == true);
            CompareRay(mesh, vertical, rayCoverage);

            RaycastInput grazing = NextRay(random, SPAN);
            grazing.OriginMesh = {static_cast<float64>(NextInRange(random, 0.0f, SPAN)),
                                  static_cast<float64>(NextInRange(random, -1.0f, 2.0f)),
                                  static_cast<float64>(NextInRange(random, 0.0f, SPAN))};
            CompareRay(mesh, grazing, rayCoverage);

            BoxCastInput box = NextBoxCast(random, SPAN);
            box.CenterMesh.X = static_cast<float64>(NextInRange(random, 0.0f, SPAN));
            box.CenterMesh.Y = static_cast<float64>(NextInRange(random, -1.0f, 3.0f));
            box.CenterMesh.Z = static_cast<float64>(NextInRange(random, 0.0f, SPAN));
            CompareBox(mesh, box, boxCoverage);
        }
        CHECK(rayCoverage.Hits > QUERY_COUNT);
        CHECK(boxCoverage.Hits > QUERY_COUNT / 10);
        CHECK(boxCoverage.Misses > QUERY_COUNT / 10);
    }
}

TEST_CASE("Duplicated geometry ties identically in both traversals", "[collision][oracle][ordering]")
{
    // Every triangle appears twice at different source indices; ties must resolve to the lower index.
    const MeshData soup =
        MakeTriangleSoup({ .Seed = 9, .TriangleCount = SOUP_TRIANGLE_COUNT / 2, .ExtentMeters = SOUP_EXTENT_METERS });
    MeshData doubled;
    const usize triangleCount = soup.Indices.GetSize() / 3;
    for (usize copy = 0; copy < 2; ++copy)
    {
        for (usize triangle = 0; triangle < triangleCount; ++triangle)
        {
            doubled.AddTriangle(soup.Positions[soup.Indices[triangle * 3]],
                                soup.Positions[soup.Indices[triangle * 3 + 1]],
                                soup.Positions[soup.Indices[triangle * 3 + 2]]);
        }
    }
    const TriangleMesh mesh = CookOrFail(doubled);
    RandomStream random;
    REQUIRE(random.TryReseed(9, 5) == true);
    Coverage coverage;
    for (uint32 query = 0; query < QUERY_COUNT; ++query)
    {
        RaycastInput ray = NextRay(random, 1.5f * SOUP_EXTENT_METERS);
        CompareRay(mesh, ray, coverage);
        RaycastResult result;
        REQUIRE(mesh.RaycastClosest(ray, result) == Status::Success);
        if (result.HasHit == true)
        {
            CHECK(result.TriangleIndex < triangleCount);
        }
        BoxCastInput box = NextBoxCast(random, 1.5f * SOUP_EXTENT_METERS);
        box.OverlapPolicy = InitialOverlapPolicy::Report;
        CompareBox(mesh, box, coverage);
        BoxCastResult boxResult;
        REQUIRE(mesh.CastBoxClosest(box, boxResult) == Status::Success);
        if (boxResult.HasHit == true)
        {
            CHECK(boxResult.TriangleIndex < triangleCount);
        }
    }
    CHECK(coverage.Hits > QUERY_COUNT / 10);
}
