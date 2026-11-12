// Analytic answers, edge cases and failure contracts for the cooked static triangle mesh.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;
using ludus::foundation::math::Aabb3;

namespace
{
constexpr float64 NOT_A_NUMBER = std::numeric_limits<float64>::quiet_NaN();
constexpr float64 INFINITE = std::numeric_limits<float64>::infinity();

/// A recognizable result that a failing query must leave untouched.
RaycastResult MakeRaySentinel()
{
    RaycastResult sentinel;
    sentinel.HasHit = true;
    sentinel.DistanceMeters = 123.0;
    sentinel.TriangleIndex = 77;
    return sentinel;
}

BoxCastResult MakeBoxSentinel()
{
    BoxCastResult sentinel;
    sentinel.HasHit = true;
    sentinel.Fraction = 0.75;
    sentinel.TriangleIndex = 77;
    return sentinel;
}

bool IsRaySentinel(const RaycastResult& result)
{
    return result.HasHit == true && result.DistanceMeters == 123.0 && result.TriangleIndex == 77;
}

bool IsBoxSentinel(const BoxCastResult& result)
{
    return result.HasHit == true && result.Fraction == 0.75 && result.TriangleIndex == 77;
}

RaycastInput MakeDownwardRay(float64 x, float64 z, float64 height = 1.0, float64 maxDistance = 10.0)
{
    return { .OriginMesh = {x, height, z}, .DirectionMesh = {0.0, -1.0, 0.0}, .MaxDistanceMeters = maxDistance };
}

TriangleMesh MakeFloor()
{
    MeshData data;
    AddFloorQuad(data);
    return CookOrFail(data);
}

/// Wall in the plane x = 2 spanning y, z in [-5, 5], wound to face -x.
void AddWall(MeshData& inoutMesh)
{
    inoutMesh.AddTriangle({2.0f, -5.0f, -5.0f}, {2.0f, -5.0f, 5.0f}, {2.0f, 5.0f, -5.0f});
    inoutMesh.AddTriangle({2.0f, 5.0f, -5.0f}, {2.0f, -5.0f, 5.0f}, {2.0f, 5.0f, 5.0f});
}

void RequireSameMesh(const TriangleMesh& mesh, usize triangleCount)
{
    REQUIRE(mesh.IsCooked() == true);
    REQUIRE(mesh.GetTriangleCount() == triangleCount);
    RaycastResult result;
    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(0.25, 0.75), result) == Status::Success);
    REQUIRE(result.HasHit == true);
    REQUIRE(result.DistanceMeters == 1.0);
}
} // namespace

TEST_CASE("Cooking a floor quad reports counts, bounds and depth", "[collision][cook]")
{
    const TriangleMesh mesh = MakeFloor();
    CHECK(mesh.IsCooked() == true);
    CHECK(mesh.GetTriangleCount() == 2);
    CHECK(mesh.GetNodeCount() == 1);
    CHECK(mesh.GetHierarchyDepth() == 1);
    CHECK(mesh.GetSkippedTriangleCount() == 0);
    const Aabb3 bounds = mesh.GetBoundsMesh();
    CHECK(bounds.Min.X == 0.0f);
    CHECK(bounds.Min.Y == 0.0f);
    CHECK(bounds.Max.X == 1.0f);
    CHECK(bounds.Max.Z == 1.0f);

    const TriangleMesh empty;
    CHECK(empty.IsCooked() == false);
    CHECK(empty.GetTriangleCount() == 0);
    CHECK(ludus::foundation::math::IsEmpty(empty.GetBoundsMesh()) == true);
}

TEST_CASE("Raycast hits a floor with analytic distance, normal and face", "[collision][ray]")
{
    const TriangleMesh mesh = MakeFloor();
    RaycastResult result;
    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(0.25, 0.75, 2.5), result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.DistanceMeters == 2.5);
    CHECK(result.PositionMesh == Vector3d{0.25, 0.0, 0.75});
    CHECK(result.NormalMesh == Vector3d{0.0, 1.0, 0.0});
    CHECK(result.IsFrontFace == true);
    CHECK(result.TriangleIndex == 0);
    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(0.75, 0.25), result) == Status::Success);
    CHECK(result.TriangleIndex == 1);

    RaycastInput upward{ .OriginMesh = {0.5, -1.0, 0.25}, .DirectionMesh = {0.0, 1.0, 0.0}, .MaxDistanceMeters = 5.0 };
    REQUIRE(mesh.RaycastClosest(upward, result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.DistanceMeters == 1.0);
    CHECK(result.IsFrontFace == false);
    CHECK(result.NormalMesh == Vector3d{0.0, 1.0, 0.0});

    upward.FacePolicy = TriangleFacePolicy::FrontFacesOnly;
    REQUIRE(mesh.RaycastClosest(upward, result) == Status::Success);
    CHECK(result.HasHit == false);
    CHECK(result.TriangleIndex == INVALID_TRIANGLE_INDEX);
    upward.FacePolicy = TriangleFacePolicy::BackFacesOnly;
    REQUIRE(mesh.RaycastClosest(upward, result) == Status::Success);
    CHECK(result.HasHit == true);

    RaycastInput downward = MakeDownwardRay(0.5, 0.25);
    downward.FacePolicy = TriangleFacePolicy::BackFacesOnly;
    REQUIRE(mesh.RaycastClosest(downward, result) == Status::Success);
    CHECK(result.HasHit == false);

    RaycastInput slanted
    {
        .OriginMesh = {0.0, 1.0, 0.5},
        .DirectionMesh = {0.6, -0.8, 0.0},
        .MaxDistanceMeters = 5.0,
    };
    REQUIRE(mesh.RaycastClosest(slanted, result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.DistanceMeters == Catch::Approx(1.25).epsilon(1e-12));
    CHECK(result.PositionMesh.X == Catch::Approx(0.75).epsilon(1e-12));
}

TEST_CASE("Raycast admits the distance limit and an origin on the surface", "[collision][ray]")
{
    const TriangleMesh mesh = MakeFloor();
    RaycastResult result;
    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(0.3, 0.6, 1.0, 1.0), result) == Status::Success);
    CHECK(result.HasHit == true);
    CHECK(result.DistanceMeters == 1.0);
    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(0.3, 0.6, 1.0, 0.999), result) == Status::Success);
    CHECK(result.HasHit == false);

    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(0.3, 0.6, 0.0), result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.DistanceMeters == 0.0);
    CHECK(result.NormalMesh == Vector3d{0.0, 1.0, 0.0});
    // Leaving the surface: the origin is on the plane, but the ray points away from it.
    const RaycastInput away
    {
        .OriginMesh = {0.3, 0.0, 0.6},
        .DirectionMesh = {0.0, 1.0, 0.0},
        .MaxDistanceMeters = 1.0,
    };
    REQUIRE(mesh.RaycastClosest(away, result) == Status::Success);
    CHECK(result.HasHit == true);
    CHECK(result.DistanceMeters == 0.0);
}

TEST_CASE("Raycast is watertight across shared edges and vertices", "[collision][ray]")
{
    const TriangleMesh mesh = MakeFloor();
    RaycastResult result;
    constexpr uint32 SAMPLE_COUNT = 64;
    for (uint32 sample = 1; sample < SAMPLE_COUNT; ++sample)
    {
        const float64 along = static_cast<float64>(sample) / SAMPLE_COUNT;
        REQUIRE(mesh.RaycastClosest(MakeDownwardRay(along, along), result) == Status::Success);
        REQUIRE(result.HasHit == true);
        CHECK(result.TriangleIndex == 0);
        const RaycastInput slanted
        {
            .OriginMesh = {along - 0.3, 0.4, along},
            .DirectionMesh = {0.6, -0.8, 0.0},
            .MaxDistanceMeters = 2.0,
        };
        REQUIRE(mesh.RaycastClosest(slanted, result) == Status::Success);
        REQUIRE(result.HasHit == true);
    }

    const MeshData gridData = MakeGrid(4);
    const TriangleMesh grid = CookOrFail(gridData);
    for (uint32 z = 1; z < 4; ++z)
    {
        for (uint32 x = 1; x < 4; ++x)
        {
            const RaycastInput atVertex = MakeDownwardRay(static_cast<float64>(x), static_cast<float64>(z));
            REQUIRE(grid.RaycastClosest(atVertex, result) == Status::Success);
            CHECK(result.HasHit == true);
            const RaycastInput atEdge = MakeDownwardRay(static_cast<float64>(x) + 0.5, static_cast<float64>(z));
            REQUIRE(grid.RaycastClosest(atEdge, result) == Status::Success);
            CHECK(result.HasHit == true);
        }
    }
}

TEST_CASE("Raycast parallel to a triangle misses it", "[collision][ray]")
{
    const TriangleMesh mesh = MakeFloor();
    RaycastResult result = MakeRaySentinel();
    const RaycastInput coplanar
    {
        .OriginMesh = {-1.0, 0.0, 0.5},
        .DirectionMesh = {1.0, 0.0, 0.0},
        .MaxDistanceMeters = 5.0,
    };
    REQUIRE(mesh.RaycastClosest(coplanar, result) == Status::Success);
    CHECK(result.HasHit == false);
    CHECK(result.DistanceMeters == 0.0);
    const RaycastInput above
    {
        .OriginMesh = {-1.0, 0.5, 0.5},
        .DirectionMesh = {1.0, 0.0, 0.0},
        .MaxDistanceMeters = 5.0,
    };
    REQUIRE(mesh.RaycastClosest(above, result) == Status::Success);
    CHECK(result.HasHit == false);
    // Beyond the quad's edge: a grazing miss, not a hit on the boundary.
    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(1.0001, 0.5), result) == Status::Success);
    CHECK(result.HasHit == false);
    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(1.0, 0.5), result) == Status::Success);
    CHECK(result.HasHit == true);
}

TEST_CASE("Equal distances resolve to the lowest source triangle", "[collision][ray][ordering]")
{
    MeshData data;
    data.AddTriangle({-5.0f, -1.0f, -5.0f}, {-5.0f, -1.0f, 5.0f}, {5.0f, -1.0f, 0.0f});
    data.AddTriangle({-5.0f, 0.0f, -5.0f}, {-5.0f, 0.0f, 5.0f}, {5.0f, 0.0f, 0.0f});
    data.AddTriangle({-6.0f, -2.0f, -6.0f}, {-6.0f, -2.0f, 6.0f}, {6.0f, -2.0f, 0.0f});
    data.AddTriangle({-4.0f, 0.0f, -4.0f}, {-4.0f, 0.0f, 4.0f}, {4.0f, 0.0f, 0.0f});
    const TriangleMesh mesh = CookOrFail(data);
    for (const QueryTraversal traversal : {QueryTraversal::Hierarchy, QueryTraversal::AllTriangles})
    {
        RaycastInput ray = MakeDownwardRay(0.0, 0.0, 3.0);
        ray.Traversal = traversal;
        RaycastResult result;
        REQUIRE(mesh.RaycastClosest(ray, result) == Status::Success);
        CHECK(result.TriangleIndex == 1);
        CHECK(result.DistanceMeters == 3.0);

        BoxCastInput box
        {
            .CenterMesh = {0.0, 2.0, 0.0},
            .HalfExtentsMesh = {0.5, 0.5, 0.5},
            .TranslationMesh = {0.0, -4.0, 0.0},
            .Traversal = traversal,
        };
        BoxCastResult boxResult;
        REQUIRE(mesh.CastBoxClosest(box, boxResult) == Status::Success);
        CHECK(boxResult.TriangleIndex == 1);
        CHECK(boxResult.Fraction == 0.375);
    }
}

TEST_CASE("Raycast rejects invalid requests and preserves its output", "[collision][ray][failure]")
{
    const TriangleMesh mesh = MakeFloor();
    RaycastResult result = MakeRaySentinel();
    const TriangleMesh uncooked;
    CHECK(uncooked.RaycastClosest(MakeDownwardRay(0.5, 0.5), result) == Status::NoGeometry);

    RaycastInput input = MakeDownwardRay(0.5, 0.5);
    input.OriginMesh.X = NOT_A_NUMBER;
    CHECK(mesh.RaycastClosest(input, result) == Status::NonFiniteInput);
    input = MakeDownwardRay(0.5, 0.5);
    input.MaxDistanceMeters = INFINITE;
    CHECK(mesh.RaycastClosest(input, result) == Status::NonFiniteInput);
    input = MakeDownwardRay(0.5, 0.5);
    input.DirectionMesh = {};
    CHECK(mesh.RaycastClosest(input, result) == Status::InvalidArgument);
    input.DirectionMesh = {0.0, -2.0, 0.0};
    CHECK(mesh.RaycastClosest(input, result) == Status::InvalidArgument);
    input = MakeDownwardRay(0.5, 0.5, 1.0, 0.0);
    CHECK(mesh.RaycastClosest(input, result) == Status::InvalidArgument);
    input.MaxDistanceMeters = -1.0;
    CHECK(mesh.RaycastClosest(input, result) == Status::InvalidArgument);
    input = MakeDownwardRay(0.5, 0.5);
    input.FacePolicy = static_cast<TriangleFacePolicy>(9);
    CHECK(mesh.RaycastClosest(input, result) == Status::InvalidArgument);
    input = MakeDownwardRay(0.5, 0.5);
    input.Traversal = static_cast<QueryTraversal>(9);
    CHECK(mesh.RaycastClosest(input, result) == Status::InvalidArgument);
    input = MakeDownwardRay(0.5, 0.5, 2.0e5);
    CHECK(mesh.RaycastClosest(input, result) == Status::OutOfRange);
    input = MakeDownwardRay(0.5, 0.5, 1.0, 2.0e6);
    CHECK(mesh.RaycastClosest(input, result) == Status::OutOfRange);
    CHECK(IsRaySentinel(result) == true);

    // A miss is a success with an explicit empty result, never a failure.
    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(5.0, 5.0), result) == Status::Success);
    CHECK(result.HasHit == false);
}

TEST_CASE("Box cast lands on a floor and stops at a wall", "[collision][box]")
{
    const TriangleMesh floor = MakeFloor();
    BoxCastResult result;
    const BoxCastInput falling
    {
        .CenterMesh = {0.25, 2.0, 0.25},
        .HalfExtentsMesh = {0.5, 0.5, 0.5},
        .TranslationMesh = {0.0, -3.0, 0.0},
    };
    REQUIRE(floor.CastBoxClosest(falling, result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.Fraction == 0.5);
    CHECK(result.DistanceMeters == 1.5);
    CHECK(result.HasNormal == true);
    CHECK(result.HasStartedOverlapping == false);
    CHECK(result.NormalMesh == Vector3d{0.0, 1.0, 0.0});

    MeshData wallData;
    AddWall(wallData);
    const TriangleMesh wall = CookOrFail(wallData);
    const BoxCastInput sliding
    {
        .CenterMesh = {0.0, 0.0, 0.0},
        .HalfExtentsMesh = {0.5, 0.5, 0.5},
        .TranslationMesh = {3.0, 0.0, 0.0},
    };
    REQUIRE(wall.CastBoxClosest(sliding, result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.Fraction == 0.5);
    CHECK(result.NormalMesh == Vector3d{-1.0, 0.0, 0.0});

    // Moving toward the back of the wall: casts are two-sided.
    const BoxCastInput fromBehind
    {
        .CenterMesh = {4.0, 0.0, 0.0},
        .HalfExtentsMesh = {0.5, 0.5, 0.5},
        .TranslationMesh = {-3.0, 0.0, 0.0},
    };
    REQUIRE(wall.CastBoxClosest(fromBehind, result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.Fraction == 0.5);
    CHECK(result.NormalMesh == Vector3d{1.0, 0.0, 0.0});

    // Contact exactly at the end of the translation is a miss.
    const BoxCastInput endsTouching
    {
        .CenterMesh = {0.0, 0.0, 0.0},
        .HalfExtentsMesh = {0.5, 0.5, 0.5},
        .TranslationMesh = {1.5, 0.0, 0.0},
    };
    REQUIRE(wall.CastBoxClosest(endsTouching, result) == Status::Success);
    CHECK(result.HasHit == false);
}

TEST_CASE("Box cast touching cases are deterministic", "[collision][box]")
{
    const TriangleMesh floor = MakeFloor();
    BoxCastResult result;
    BoxCastInput resting
    {
        .CenterMesh = {0.5, 0.5, 0.5},
        .HalfExtentsMesh = {0.5, 0.5, 0.5},
        .TranslationMesh = {0.0, -1.0, 0.0},
    };
    REQUIRE(floor.CastBoxClosest(resting, result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.Fraction == 0.0);
    CHECK(result.HasStartedOverlapping == false);
    CHECK(result.HasNormal == true);
    CHECK(result.NormalMesh == Vector3d{0.0, 1.0, 0.0});

    resting.TranslationMesh = {0.0, 1.0, 0.0};
    REQUIRE(floor.CastBoxClosest(resting, result) == Status::Success);
    CHECK(result.HasHit == false);
    resting.TranslationMesh = {2.0, 0.0, 0.0};
    REQUIRE(floor.CastBoxClosest(resting, result) == Status::Success);
    CHECK(result.HasHit == false);

    // Sweeping past the quad's edge without touching it.
    const BoxCastInput past
    {
        .CenterMesh = {2.0, 0.25, -2.0},
        .HalfExtentsMesh = {0.5, 0.5, 0.5},
        .TranslationMesh = {0.0, 0.0, 4.0},
    };
    REQUIRE(floor.CastBoxClosest(past, result) == Status::Success);
    CHECK(result.HasHit == false);
}

TEST_CASE("Box cast initial-overlap policies are explicit", "[collision][box][overlap]")
{
    MeshData data;
    AddFloorQuad(data);
    AddFloorQuad(data, -2.0f);
    const TriangleMesh mesh = CookOrFail(data);
    BoxCastInput straddling
    {
        .CenterMesh = {0.5, 0.25, 0.5},
        .HalfExtentsMesh = {0.25, 0.5, 0.25},
        .TranslationMesh = {0.0, -3.0, 0.0},
    };
    BoxCastResult result;
    REQUIRE(mesh.CastBoxClosest(straddling, result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.Fraction == 0.0);
    CHECK(result.DistanceMeters == 0.0);
    CHECK(result.HasStartedOverlapping == true);
    CHECK(result.HasNormal == false);
    CHECK(result.NormalMesh == Vector3d{});
    CHECK(result.TriangleIndex == 0);

    straddling.OverlapPolicy = InitialOverlapPolicy::Ignore;
    REQUIRE(mesh.CastBoxClosest(straddling, result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.HasStartedOverlapping == false);
    CHECK(result.TriangleIndex == 2);
    CHECK(result.Fraction == Catch::Approx(1.75 / 3.0).epsilon(1e-12));

    straddling.OverlapPolicy = InitialOverlapPolicy::Reject;
    result = MakeBoxSentinel();
    CHECK(mesh.CastBoxClosest(straddling, result) == Status::InitialOverlap);
    CHECK(IsBoxSentinel(result) == true);
    straddling.Traversal = QueryTraversal::AllTriangles;
    CHECK(mesh.CastBoxClosest(straddling, result) == Status::InitialOverlap);

    // An overlap ties at fraction zero with a touching contact and is ordered first.
    MeshData touchData;
    touchData.AddTriangle({-1.0f, -0.5f, -1.0f}, {-1.0f, -0.5f, 1.0f}, {1.0f, -0.5f, 0.0f});
    touchData.AddTriangle({-1.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f});
    const TriangleMesh touching = CookOrFail(touchData);
    const BoxCastInput tied
    {
        .CenterMesh = {0.0, 0.0, 0.0},
        .HalfExtentsMesh = {0.25, 0.5, 0.25},
        .TranslationMesh = {0.0, -1.0, 0.0},
    };
    REQUIRE(touching.CastBoxClosest(tied, result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.HasStartedOverlapping == true);
    CHECK(result.TriangleIndex == 1);
}

TEST_CASE("A zero-translation box cast performs only the overlap test", "[collision][box][overlap]")
{
    const TriangleMesh floor = MakeFloor();
    BoxCastResult result;
    BoxCastInput still{ .CenterMesh = {0.5, 0.0, 0.5}, .HalfExtentsMesh = {0.25, 0.25, 0.25} };
    REQUIRE(floor.CastBoxClosest(still, result) == Status::Success);
    REQUIRE(result.HasHit == true);
    CHECK(result.HasStartedOverlapping == true);
    CHECK(result.Fraction == 0.0);
    CHECK(result.DistanceMeters == 0.0);

    still.OverlapPolicy = InitialOverlapPolicy::Reject;
    CHECK(floor.CastBoxClosest(still, result) == Status::InitialOverlap);
    still.OverlapPolicy = InitialOverlapPolicy::Ignore;
    REQUIRE(floor.CastBoxClosest(still, result) == Status::Success);
    CHECK(result.HasHit == false);

    still.OverlapPolicy = InitialOverlapPolicy::Report;
    still.CenterMesh = {0.5, 0.25, 0.5};
    REQUIRE(floor.CastBoxClosest(still, result) == Status::Success);
    CHECK(result.HasHit == false);
    still.CenterMesh = {0.5, 1.0, 0.5};
    REQUIRE(floor.CastBoxClosest(still, result) == Status::Success);
    CHECK(result.HasHit == false);
}

TEST_CASE("Box cast rejects invalid requests and preserves its output", "[collision][box][failure]")
{
    const TriangleMesh floor = MakeFloor();
    BoxCastResult result = MakeBoxSentinel();
    const BoxCastInput valid
    {
        .CenterMesh = {0.5, 2.0, 0.5},
        .HalfExtentsMesh = {0.5, 0.5, 0.5},
        .TranslationMesh = {0.0, -3.0, 0.0},
    };
    const TriangleMesh uncooked;
    CHECK(uncooked.CastBoxClosest(valid, result) == Status::NoGeometry);

    BoxCastInput input = valid;
    input.HalfExtentsMesh.Y = 0.0;
    CHECK(floor.CastBoxClosest(input, result) == Status::InvalidArgument);
    input.HalfExtentsMesh.Y = -0.5;
    CHECK(floor.CastBoxClosest(input, result) == Status::InvalidArgument);
    input = valid;
    input.TranslationMesh.Z = NOT_A_NUMBER;
    CHECK(floor.CastBoxClosest(input, result) == Status::NonFiniteInput);
    input = valid;
    input.CenterMesh.X = INFINITE;
    CHECK(floor.CastBoxClosest(input, result) == Status::NonFiniteInput);
    input = valid;
    input.CenterMesh.X = 2.0e5;
    CHECK(floor.CastBoxClosest(input, result) == Status::OutOfRange);
    input = valid;
    input.TranslationMesh.Y = -2.0e6;
    CHECK(floor.CastBoxClosest(input, result) == Status::OutOfRange);
    input = valid;
    input.OverlapPolicy = static_cast<InitialOverlapPolicy>(9);
    CHECK(floor.CastBoxClosest(input, result) == Status::InvalidArgument);
    CHECK(IsBoxSentinel(result) == true);
}

TEST_CASE("Cooking validates its source and preserves the previous mesh", "[collision][cook][failure]")
{
    MeshData floorData;
    AddFloorQuad(floorData);
    TriangleMesh mesh = CookOrFail(floorData);
    const TriangleMeshCookSettings settings{};

    MeshData data;
    AddFloorQuad(data);
    data.Indices.RemoveLast();
    TriangleMeshCookResult result = TriangleMesh::TryCook(data.GetSource(), settings, mesh);
    CHECK(result.Result == Status::InvalidArgument);
    CHECK(result.TriangleIndex == INVALID_TRIANGLE_INDEX);
    RequireSameMesh(mesh, 2);

    const MeshData empty;
    CHECK(TriangleMesh::TryCook(empty.GetSource(), settings, mesh).Result == Status::NoGeometry);
    RequireSameMesh(mesh, 2);

    data = {};
    AddFloorQuad(data);
    AddFloorQuad(data, 1.0f);
    data.Indices[10] = 99;
    result = TriangleMesh::TryCook(data.GetSource(), settings, mesh);
    CHECK(result.Result == Status::IndexOutOfRange);
    CHECK(result.TriangleIndex == 3);
    RequireSameMesh(mesh, 2);

    data = {};
    AddFloorQuad(data);
    AddFloorQuad(data, 1.0f);
    data.Positions[5].Y = std::numeric_limits<float32>::quiet_NaN();
    result = TriangleMesh::TryCook(data.GetSource(), settings, mesh);
    CHECK(result.Result == Status::NonFiniteInput);
    CHECK(result.TriangleIndex == 2);
    RequireSameMesh(mesh, 2);

    data.Positions[5].Y = 2.0e5f;
    result = TriangleMesh::TryCook(data.GetSource(), settings, mesh);
    CHECK(result.Result == Status::OutOfRange);
    CHECK(result.TriangleIndex == 2);
    RequireSameMesh(mesh, 2);

    data = {};
    AddFloorQuad(data);
    data.AddTriangle({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f});
    result = TriangleMesh::TryCook(data.GetSource(), settings, mesh);
    CHECK(result.Result == Status::DegenerateTriangle);
    CHECK(result.TriangleIndex == 2);
    RequireSameMesh(mesh, 2);

    const MeshData invalidView;
    TriangleMeshSource source = invalidView.GetSource();
    source.Indices = ludus::foundation::BorrowedView<const uint32>(nullptr, 3);
    CHECK(TriangleMesh::TryCook(source, settings, mesh).Result == Status::InvalidArgument);
    source = floorData.GetSource();
    const TriangleMeshCookSettings unknownPolicy{ .DegeneratePolicy = static_cast<DegenerateTrianglePolicy>(9) };
    CHECK(TriangleMesh::TryCook(source, unknownPolicy, mesh).Result == Status::InvalidArgument);
    RequireSameMesh(mesh, 2);

    // Admission is decided from the extent before any element is read, so a
    // view with an oversized extent over one element is never dereferenced.
    source.Indices =
        ludus::foundation::BorrowedView<const uint32>(floorData.Indices.GetData(),
                                                      (static_cast<usize>(MAX_TRIANGLE_MESH_TRIANGLES) + 1) * 3);
    result = TriangleMesh::TryCook(source, settings, mesh);
    CHECK(result.Result == Status::CapacityExceeded);
    RequireSameMesh(mesh, 2);
}

TEST_CASE("Skipping degenerate triangles keeps source triangle indices", "[collision][cook]")
{
    MeshData data;
    data.AddTriangle({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    AddFloorQuad(data);
    data.AddTriangle({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f});
    const TriangleMeshCookSettings skip{ .DegeneratePolicy = DegenerateTrianglePolicy::Skip };
    const TriangleMesh mesh = CookOrFail(data, skip);
    CHECK(mesh.GetTriangleCount() == 2);
    CHECK(mesh.GetSkippedTriangleCount() == 2);
    RaycastResult result;
    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(0.25, 0.75), result) == Status::Success);
    CHECK(result.TriangleIndex == 1);
    REQUIRE(mesh.RaycastClosest(MakeDownwardRay(0.75, 0.25), result) == Status::Success);
    CHECK(result.TriangleIndex == 2);

    MeshData allDegenerate;
    allDegenerate.AddTriangle({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f});
    TriangleMesh untouched;
    CHECK(TriangleMesh::TryCook(allDegenerate.GetSource(), skip, untouched).Result == Status::NoGeometry);
    CHECK(untouched.IsCooked() == false);
}

TEST_CASE("Degeneracy is scale free and bounded by the shape ratio", "[collision][cook]")
{
    const TriangleMeshCookSettings settings{};
    for (const float32 scale : {0.001f, 1.0f, 1000.0f})
    {
        // Twice the area over the longest edge squared equals the height ratio for a thin isoceles triangle.
        MeshData thin;
        thin.AddTriangle({0.0f, 0.0f, 0.0f}, {scale, 0.0f, 0.0f}, {0.5f * scale, 1.0e-4f * scale, 0.0f});
        TriangleMesh mesh;
        CHECK(TriangleMesh::TryCook(thin.GetSource(), settings, mesh).Result == Status::Success);
        MeshData sliver;
        sliver.AddTriangle({0.0f, 0.0f, 0.0f}, {scale, 0.0f, 0.0f}, {0.5f * scale, 1.0e-8f * scale, 0.0f});
        CHECK(TriangleMesh::TryCook(sliver.GetSource(), settings, mesh).Result == Status::DegenerateTriangle);
    }
}

TEST_CASE("The hierarchy stays bounded and cooking is deterministic", "[collision][cook]")
{
    constexpr uint32 CELLS = 120;
    RandomStream heights;
    REQUIRE(heights.TryReseed(7, 0) == true);
    const MeshData data = MakeGrid(CELLS, &heights);
    const TriangleMesh first = CookOrFail(data);
    const TriangleMesh second = CookOrFail(data);
    const usize triangleCount = static_cast<usize>(CELLS) * CELLS * 2;
    CHECK(first.GetTriangleCount() == triangleCount);
    CHECK(first.GetNodeCount() <= triangleCount);
    CHECK(first.GetNodeCount() == second.GetNodeCount());
    CHECK(first.GetHierarchyDepth() == second.GetHierarchyDepth());
    CHECK(first.GetHierarchyDepth() > 1);
    CHECK(first.GetHierarchyDepth() <= MAX_TRIANGLE_MESH_HIERARCHY_DEPTH);
    const Aabb3 bounds = first.GetBoundsMesh();
    CHECK(bounds.Min.X == 0.0f);
    CHECK(bounds.Max.Z == static_cast<float32>(CELLS));

    RandomStream rays;
    REQUIRE(rays.TryReseed(8, 0) == true);
    constexpr uint32 RAY_COUNT = 200;
    for (uint32 ray = 0; ray < RAY_COUNT; ++ray)
    {
        constexpr float32 SPAN = static_cast<float32>(CELLS);
        const RaycastInput input = MakeDownwardRay(static_cast<float64>(NextInRange(rays, 0.0f, SPAN)),
                                                   static_cast<float64>(NextInRange(rays, 0.0f, SPAN)),
                                                   2.0);
        RaycastResult firstResult;
        RaycastResult secondResult;
        REQUIRE(first.RaycastClosest(input, firstResult) == Status::Success);
        REQUIRE(second.RaycastClosest(input, secondResult) == Status::Success);
        REQUIRE(firstResult.HasHit == true);
        CHECK(firstResult.TriangleIndex == secondResult.TriangleIndex);
        CHECK(firstResult.DistanceMeters == secondResult.DistanceMeters);
    }
}

TEST_CASE("A moved-from mesh is empty and its geometry moves with it", "[collision][cook]")
{
    TriangleMesh mesh = MakeFloor();
    TriangleMesh moved = ludus::foundation::Move(mesh);
    CHECK(moved.IsCooked() == true);
    CHECK(mesh.IsCooked() ==
          false); // NOLINT(bugprone-use-after-move): the moved-from state is the contract under test.
    RaycastResult result;
    CHECK(mesh.RaycastClosest(MakeDownwardRay(0.5, 0.5), result) ==
          Status::NoGeometry); // NOLINT(bugprone-use-after-move)
}
