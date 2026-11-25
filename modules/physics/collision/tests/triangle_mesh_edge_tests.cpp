// Internal-edge classification of cooked triangle meshes: which edges and corners keep their own contact normals.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;

namespace
{
/// Two triangles sharing the edge from (0, 0, 0) to (0, 0, 1): one in the plane y = 0 on the -x side, the other
/// rising or falling toward +x by `rise` meters per meter. Both face +y. Triangle 0 holds the shared edge as its
/// edge 2, triangle 1 as its edge 0.
MeshData MakeFold(float32 rise)
{
    MeshData mesh;
    mesh.Positions.Add({0.0f, 0.0f, 0.0f});
    mesh.Positions.Add({0.0f, 0.0f, 1.0f});
    mesh.Positions.Add({-1.0f, 0.0f, 0.5f});
    mesh.Positions.Add({1.0f, rise, 0.5f});
    const StaticArray<uint32, QUAD_INDEX_COUNT> indices{0, 2, 1, 0, 1, 3};
    for (const uint32 index : indices)
    {
        mesh.Indices.Add(index);
    }
    return mesh;
}

TriangleFeatureFlags GetFeatures(const TriangleMesh& mesh, uint32 triangle)
{
    TriangleFeatureFlags flags = 0;
    REQUIRE(mesh.TryGetTriangleFeatures(triangle, flags) == Status::Success);
    return flags;
}

constexpr uint32 FOLD_FIRST_SHARED_EDGE = 2;
constexpr uint32 FOLD_SECOND_SHARED_EDGE = 0;
} // namespace

TEST_CASE("A flat seam between two triangles is inactive on both sides and boundaries stay active",
          "[collision][mesh][edges]")
{
    const TriangleMesh mesh = CookOrFail(MakeFold(0.0f));
    const TriangleFeatureFlags first = GetFeatures(mesh, 0);
    const TriangleFeatureFlags second = GetFeatures(mesh, 1);
    for (const bool isFrontSide : {true, false})
    {
        REQUIRE(IsTriangleEdgeActive(first, FOLD_FIRST_SHARED_EDGE, isFrontSide) == false);
        REQUIRE(IsTriangleEdgeActive(second, FOLD_SECOND_SHARED_EDGE, isFrontSide) == false);
        // The other two edges of each triangle are boundaries.
        REQUIRE(IsTriangleEdgeActive(first, 0, isFrontSide) == true);
        REQUIRE(IsTriangleEdgeActive(first, 1, isFrontSide) == true);
        REQUIRE(IsTriangleEdgeActive(second, 1, isFrontSide) == true);
        REQUIRE(IsTriangleEdgeActive(second, 2, isFrontSide) == true);
        // Every vertex touches a boundary edge, so every corner is active.
        for (uint32 corner = 0; corner < TRIANGLE_FEATURE_COUNT; ++corner)
        {
            REQUIRE(IsTriangleCornerActive(first, corner, isFrontSide) == true);
            REQUIRE(IsTriangleCornerActive(second, corner, isFrontSide) == true);
        }
    }
}

TEST_CASE("A ridge is active on its front side and a valley on its back side", "[collision][mesh][edges]")
{
    const TriangleMesh ridge = CookOrFail(MakeFold(-0.5f));
    REQUIRE(IsTriangleEdgeActive(GetFeatures(ridge, 0), FOLD_FIRST_SHARED_EDGE, true) == true);
    REQUIRE(IsTriangleEdgeActive(GetFeatures(ridge, 0), FOLD_FIRST_SHARED_EDGE, false) == false);
    REQUIRE(IsTriangleEdgeActive(GetFeatures(ridge, 1), FOLD_SECOND_SHARED_EDGE, true) == true);
    REQUIRE(IsTriangleEdgeActive(GetFeatures(ridge, 1), FOLD_SECOND_SHARED_EDGE, false) == false);

    const TriangleMesh valley = CookOrFail(MakeFold(0.5f));
    REQUIRE(IsTriangleEdgeActive(GetFeatures(valley, 0), FOLD_FIRST_SHARED_EDGE, true) == false);
    REQUIRE(IsTriangleEdgeActive(GetFeatures(valley, 0), FOLD_FIRST_SHARED_EDGE, false) == true);
    REQUIRE(IsTriangleEdgeActive(GetFeatures(valley, 1), FOLD_SECOND_SHARED_EDGE, true) == false);
    REQUIRE(IsTriangleEdgeActive(GetFeatures(valley, 1), FOLD_SECOND_SHARED_EDGE, false) == true);
}

TEST_CASE("The crease threshold decides whether a shallow ridge is a seam", "[collision][mesh][edges]")
{
    // A rise of 0.05 m per meter folds the surface by about 2.9 degrees: below the default five, above one degree.
    const MeshData fold = MakeFold(-0.05f);
    const TriangleMesh loose = CookOrFail(fold);
    REQUIRE(IsTriangleEdgeActive(GetFeatures(loose, 0), FOLD_FIRST_SHARED_EDGE, true) == false);
    constexpr float64 COSINE_OF_ONE_DEGREE = 0.99984769515639127;
    const TriangleMesh strict = CookOrFail(fold, { .ActiveEdgeCosine = COSINE_OF_ONE_DEGREE });
    REQUIRE(IsTriangleEdgeActive(GetFeatures(strict, 0), FOLD_FIRST_SHARED_EDGE, true) == true);
    REQUIRE(IsTriangleEdgeActive(GetFeatures(strict, 0), FOLD_FIRST_SHARED_EDGE, false) == false);
}

TEST_CASE("Nonmanifold, inconsistently wound and unwelded edges stay active on both sides", "[collision][mesh][edges]")
{
    SECTION("three triangles on one edge")
    {
        MeshData mesh = MakeFold(0.0f);
        mesh.Positions.Add({0.0f, 1.0f, 0.5f});
        mesh.Indices.Add(0);
        mesh.Indices.Add(1);
        mesh.Indices.Add(4);
        const TriangleMesh cooked = CookOrFail(mesh);
        for (const bool isFrontSide : {true, false})
        {
            REQUIRE(IsTriangleEdgeActive(GetFeatures(cooked, 0), FOLD_FIRST_SHARED_EDGE, isFrontSide) == true);
            REQUIRE(IsTriangleEdgeActive(GetFeatures(cooked, 1), FOLD_SECOND_SHARED_EDGE, isFrontSide) == true);
            REQUIRE(IsTriangleEdgeActive(GetFeatures(cooked, 2), 0, isFrontSide) == true);
        }
    }
    SECTION("a neighbour wound the other way")
    {
        MeshData mesh = MakeFold(0.0f);
        // Triangle 1 becomes (1, 0, 3): it now runs along the shared edge in the same direction as triangle 0.
        mesh.Indices[3] = 1;
        mesh.Indices[4] = 0;
        const TriangleMesh cooked = CookOrFail(mesh);
        for (const bool isFrontSide : {true, false})
        {
            REQUIRE(IsTriangleEdgeActive(GetFeatures(cooked, 0), FOLD_FIRST_SHARED_EDGE, isFrontSide) == true);
            REQUIRE(IsTriangleEdgeActive(GetFeatures(cooked, 1), 0, isFrontSide) == true);
        }
    }
    SECTION("a seam whose vertices are duplicated instead of shared")
    {
        MeshData mesh;
        AddFloorQuad(mesh);
        MeshData unwelded;
        unwelded.AddTriangle(mesh.Positions[0], mesh.Positions[1], mesh.Positions[2]);
        unwelded.AddTriangle(mesh.Positions[0], mesh.Positions[2], mesh.Positions[3]);
        const TriangleMesh welded = CookOrFail(mesh);
        const TriangleMesh split = CookOrFail(unwelded);
        // Edge 2 of triangle 0 and edge 0 of triangle 1 are the diagonal.
        REQUIRE(IsTriangleEdgeActive(GetFeatures(welded, 0), 2, true) == false);
        REQUIRE(IsTriangleEdgeActive(GetFeatures(welded, 1), 0, true) == false);
        REQUIRE(IsTriangleEdgeActive(GetFeatures(split, 0), 2, true) == true);
        REQUIRE(IsTriangleEdgeActive(GetFeatures(split, 1), 0, true) == true);
    }
}

TEST_CASE("Interior corners of a flat grid are inactive and KeepAll marks everything active",
          "[collision][mesh][edges]")
{
    constexpr uint32 CELLS = 3;
    const MeshData grid = MakeGrid(CELLS);
    const TriangleMesh classified = CookOrFail(grid);
    const TriangleMesh conservative = CookOrFail(grid, { .EdgePolicy = InternalEdgePolicy::KeepAll });
    // The middle cell's first triangle touches the grid border nowhere: every edge and corner is interior.
    constexpr uint32 MIDDLE_TRIANGLE = 2 * (CELLS + 1);
    REQUIRE(GetFeatures(classified, MIDDLE_TRIANGLE) == 0);
    REQUIRE(GetFeatures(conservative, MIDDLE_TRIANGLE) == ALL_TRIANGLE_FEATURES_ACTIVE);
    // A corner cell keeps its border edges, and its corners on the border are active.
    const TriangleFeatureFlags corner = GetFeatures(classified, 0);
    REQUIRE(IsTriangleEdgeActive(corner, 0, true) == true);
    REQUIRE(IsTriangleEdgeActive(corner, 2, true) == false);
    REQUIRE(IsTriangleCornerActive(corner, 0, true) == true);
    REQUIRE(IsTriangleCornerActive(corner, 2, true) == false);
}

TEST_CASE("Edge settings are validated and unknown triangles are reported", "[collision][mesh][edges]")
{
    const MeshData fold = MakeFold(0.0f);
    TriangleMesh mesh;
    constexpr float64 ABOVE_ONE = 1.5;
    REQUIRE(TriangleMesh::TryCook(fold.GetSource(), { .ActiveEdgeCosine = ABOVE_ONE }, mesh).Result ==
            Status::InvalidArgument);
    REQUIRE(TriangleMesh::TryCook(fold.GetSource(),
                                  {
                                      .EdgePolicy = static_cast<InternalEdgePolicy>(7),
                                  },
                                  mesh)
                .Result == Status::InvalidArgument);
    REQUIRE(mesh.IsCooked() == false);
    TriangleFeatureFlags flags = ALL_TRIANGLE_FEATURES_ACTIVE;
    REQUIRE(mesh.TryGetTriangleFeatures(0, flags) == Status::NoGeometry);
    const TriangleMesh cooked = CookOrFail(fold);
    REQUIRE(cooked.TryGetTriangleFeatures(2, flags) == Status::IndexOutOfRange);
    REQUIRE(flags == ALL_TRIANGLE_FEATURES_ACTIVE);
}
