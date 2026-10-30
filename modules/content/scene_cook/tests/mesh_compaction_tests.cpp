#include "importers/mesh_compaction.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
using namespace ludus::content::scene_cook;
using namespace ludus::content::scene_cook::importers;
using ludus::content::Status;

IrMesh MakeSplitQuad()
{
    IrMesh mesh;
    mesh.Positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0};
    mesh.Normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    mesh.Uvs = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
    mesh.Tangents = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    mesh.Indices = {0, 1, 2, 3, 4, 5};
    return mesh;
}

void CheckCorners(const IrMesh& original, const IrMesh& compacted)
{
    REQUIRE(compacted.Indices.GetSize() == original.Indices.GetSize());
    for (usize corner = 0; corner < original.Indices.GetSize(); ++corner)
    {
        const usize before = original.Indices[corner];
        const usize after = compacted.Indices[corner];
        const auto check = [&](const Array<float32>& source, const Array<float32>& destination, usize width) {
            if (source.IsEmpty() == false)
            {
                REQUIRE(destination.GetSize() >= (after + 1) * width);
                CHECK(std::memcmp(source.GetData() + before * width,
                                  destination.GetData() + after * width,
                                  width * sizeof(float32)) == 0);
            }
        };
        check(original.Positions, compacted.Positions, 3);
        check(original.Normals, compacted.Normals, 3);
        check(original.Uvs, compacted.Uvs, 2);
        check(original.Tangents, compacted.Tangents, 4);
    }
}
} // namespace

TEST_CASE("exact mesh compaction preserves every corner and attribute seam", "[scene_cook][compaction]")
{
    IrMesh mesh = MakeSplitQuad();
    usize expectedVertices = 4;
    SECTION("identical records share the first occurrence")
    {
        IrMesh compacted = mesh;
        REQUIRE(CompactMesh(compacted, nullptr) == Status::Ok);
        const Array<uint32> expected = {0, 1, 2, 0, 2, 3};
        REQUIRE(compacted.Indices.GetSize() == expected.GetSize());
        CHECK(std::memcmp(compacted.Indices.GetData(), expected.GetData(), expected.GetSize() * sizeof(uint32)) == 0);
    }
    SECTION("normal seams stay split")
    {
        mesh.Normals[9] = 1;
        expectedVertices = 5;
    }
    SECTION("UV seams stay split")
    {
        mesh.Uvs[6] = .5F;
        expectedVertices = 5;
    }
    SECTION("tangent handedness seams stay split")
    {
        mesh.Tangents[15] = -1;
        expectedVertices = 5;
    }
    SECTION("signed zeros remain byte distinct")
    {
        mesh.Positions[9] = -0.0F;
        expectedVertices = 5;
    }
    SECTION("optional attributes can be absent")
    {
        mesh.Uvs.Clear();
        mesh.Tangents.Clear();
    }
    const IrMesh original = mesh;
    REQUIRE(CompactMesh(mesh, nullptr) == Status::Ok);
    CHECK(mesh.GetVertexCount() == expectedVertices);
    CHECK(mesh.SourceVertexCount == original.GetVertexCount());
    CheckCorners(original, mesh);
    const IrMesh once = mesh;
    REQUIRE(CompactMesh(mesh, nullptr) == Status::Ok);
    CHECK(mesh.SourceVertexCount == original.GetVertexCount());
    REQUIRE(mesh.Indices.GetSize() == once.Indices.GetSize());
    CHECK(std::memcmp(mesh.Indices.GetData(), once.Indices.GetData(), mesh.Indices.GetSize() * sizeof(uint32)) == 0);
    CheckCorners(once, mesh);
}

TEST_CASE("mesh compaction keeps generated-normal topology and validates before mutation", "[scene_cook][compaction]")
{
    IrMesh mesh = MakeSplitQuad();
    SECTION("missing normals preserve independent smoothing vertices")
    {
        mesh.Normals.Clear();
        const IrMesh original = mesh;
        REQUIRE(CompactMesh(mesh, nullptr) == Status::Ok);
        CHECK(mesh.GetVertexCount() == original.GetVertexCount());
        CheckCorners(original, mesh);
    }
    SECTION("malformed attributes fail unchanged")
    {
        mesh.Tangents.RemoveLast();
        CHECK(CompactMesh(mesh, nullptr) == Status::Invalid);
        CHECK(mesh.GetVertexCount() == 6);
        CHECK(mesh.Tangents.GetSize() == 23);
        CHECK(mesh.SourceVertexCount == 0);
    }
    SECTION("invalid indices fail unchanged")
    {
        mesh.Indices[0] = 6;
        CHECK(CompactMesh(mesh, nullptr) == Status::Invalid);
        CHECK(mesh.GetVertexCount() == 6);
        CHECK(mesh.Indices[0] == 6);
        CHECK(mesh.SourceVertexCount == 0);
    }
    SECTION("cancellation after sorting preserves all inputs")
    {
        usize calls = 0;
        Cancellation cancellation;
        cancellation.ContextOrNull = &calls;
        cancellation.Requested = [](void* contextOrNull) noexcept {
            usize& count = *static_cast<usize*>(contextOrNull);
            ++count;
            return count > 1;
        };
        const IrMesh original = mesh;
        CHECK(CompactMesh(mesh, &cancellation) == Status::Cancelled);
        CHECK(mesh.GetVertexCount() == original.GetVertexCount());
        CHECK(mesh.SourceVertexCount == 0);
        CheckCorners(original, mesh);
    }
}
