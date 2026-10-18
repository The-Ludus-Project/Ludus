// Tests for the MikkTSpace tangent wrapper (importers/tangent_generation.h).

#include "importers/tangent_generation.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>

namespace
{
using namespace ludus::content::scene_cook::importers;
using ludus::content::Status;
using ludus::foundation::core::float32;
using ludus::foundation::core::uint32;
using ludus::foundation::core::usize;

void Assign(Array<float32>& outValues, std::initializer_list<float32> values)
{
    outValues.Clear();
    for (const float32 value : values)
    {
        REQUIRE(outValues.TryAdd(value));
    }
}

void Assign(Array<uint32>& outValues, std::initializer_list<uint32> values)
{
    outValues.Clear();
    for (const uint32 value : values)
    {
        REQUIRE(outValues.TryAdd(value));
    }
}

/// Unit quad in the XY plane facing +Z; IR uvs are top-down, so v = 1 at y = 0.
IrMesh MakeQuad(bool isMirrored)
{
    IrMesh mesh;
    Assign(mesh.Positions, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0});
    Assign(mesh.Normals, {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
    if (isMirrored)
    {
        Assign(mesh.Uvs, {1, 1, 0, 1, 0, 0, 1, 0});
    }
    else
    {
        Assign(mesh.Uvs, {0, 1, 1, 1, 1, 0, 0, 0});
    }
    Assign(mesh.Indices, {0, 1, 2, 0, 2, 3});
    return mesh;
}
} // namespace

TEST_CASE("MikkTSpace tangents follow +u with the bitangent sign of +v", "[scene_cook][tangents]")
{
    IrMesh mesh = MakeQuad(false);
    REQUIRE(GenerateTangents(mesh) == Status::Ok);
    REQUIRE(mesh.Tangents.GetSize() == mesh.GetVertexCount() * 4);
    CHECK(mesh.GetVertexCount() == 4); // One frame per vertex: nothing split.
    for (usize v = 0; v < mesh.GetVertexCount(); ++v)
    {
        CHECK(std::fabs(mesh.Tangents[v * 4] - 1.0F) < 1e-5F);
        CHECK(std::fabs(mesh.Tangents[v * 4 + 1]) < 1e-5F);
        CHECK(mesh.Tangents[v * 4 + 3] == 1.0F);
    }
}

TEST_CASE("Mirrored texture coordinates flip the tangent and its sign", "[scene_cook][tangents]")
{
    IrMesh mesh = MakeQuad(true);
    REQUIRE(GenerateTangents(mesh) == Status::Ok);
    for (usize v = 0; v < mesh.GetVertexCount(); ++v)
    {
        CHECK(std::fabs(mesh.Tangents[v * 4] + 1.0F) < 1e-5F);
        CHECK(mesh.Tangents[v * 4 + 3] == -1.0F);
    }
}

TEST_CASE("Corners with different frames get their own vertices", "[scene_cook][tangents]")
{
    // Two triangles share vertices 0 and 2, but the second is mirrored in u.
    IrMesh mesh;
    Assign(mesh.Positions, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0});
    Assign(mesh.Normals, {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
    Assign(mesh.Uvs, {0, 1, 1, 1, 1, 0, 2, 0});
    Assign(mesh.Indices, {0, 1, 2, 0, 2, 3});
    REQUIRE(GenerateTangents(mesh) == Status::Ok);
    CHECK(mesh.GetVertexCount() > 4);
    CHECK(mesh.Normals.GetSize() == mesh.GetVertexCount() * 3);
    CHECK(mesh.Uvs.GetSize() == mesh.GetVertexCount() * 2);
    CHECK(mesh.Indices.GetSize() == 6);
    // The split copies keep the original position of the shared corner.
    CHECK(mesh.Positions[usize{mesh.Indices[3]} * 3] == 0.0F);
}

TEST_CASE("Tangent generation without uvs fails and leaves the mesh unchanged", "[scene_cook][tangents]")
{
    IrMesh mesh = MakeQuad(false);
    mesh.Uvs.Clear();
    CHECK(GenerateTangents(mesh) == Status::Invalid);
    CHECK(mesh.Tangents.IsEmpty());
    CHECK(mesh.GetVertexCount() == 4);
    // Missing normals are generated first.
    IrMesh noNormals = MakeQuad(false);
    noNormals.Normals.Clear();
    REQUIRE(GenerateTangents(noNormals) == Status::Ok);
    CHECK(noNormals.Normals.GetSize() == noNormals.GetVertexCount() * 3);
    CHECK(noNormals.Normals[2] == 1.0F);
}
