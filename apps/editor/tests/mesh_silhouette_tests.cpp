#include <catch2/catch_test_macros.hpp>

#include "internal/mesh_silhouette.h"

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace math = ludus::foundation::math;

namespace
{
constexpr usize CUBE_CORNERS = 8;
constexpr usize CUBE_FACES = 6;
constexpr usize CUBE_FACE_CORNERS = 4;
/// A cube seen along no axis shows six silhouette edges, the hexagon around its three visible faces.
constexpr usize CUBE_OBLIQUE_SILHOUETTE_EDGES = 6;
constexpr usize TRIANGLE_EDGES = 3;
constexpr uint32 SPHERE_SEGMENTS = 24;
constexpr uint32 SPHERE_RINGS = 12;
constexpr float32 FIELD_OF_VIEW_RADIANS = 1.0F;
constexpr float32 ASPECT_RATIO = 1.5F;
constexpr float32 NEAR_PLANE = 0.1F;
constexpr float32 FAR_PLANE = 100.0F;
constexpr float32 CAMERA_DISTANCE = 6.0F;
/// Segment bound that no fixture here reaches.
constexpr usize SEGMENT_LIMIT = 100000;

struct Mesh final
{
    Array<math::Vector3> Positions;
    Array<uint32> Indices;
    [[nodiscard]] BorrowedView<const math::Vector3> GetPositions() const noexcept
    {
        return {Positions.GetData(), Positions.GetSize()};
    }
    [[nodiscard]] BorrowedView<const uint32> GetIndices() const noexcept
    {
        return {Indices.GetData(), Indices.GetSize()};
    }
};

math::Matrix4 MakeViewToClip(math::Vector3 eye)
{
    math::Matrix4 view;
    REQUIRE(math::TryLookAt(eye, {0, 0, 0}, {0, 1, 0}, view) == math::MathStatus::Success);
    math::Matrix4 projection;
    REQUIRE(math::TryPerspectiveReverseZ(FIELD_OF_VIEW_RADIANS, ASPECT_RATIO, NEAR_PLANE, FAR_PLANE, projection) ==
            math::MathStatus::Success);
    return projection * view;
}

/// A unit cube with every face's four corners separate, as an importer that splits vertices for normals writes it.
Mesh MakeSplitCube()
{
    Mesh mesh;
    for (usize face = 0; face < CUBE_FACES; ++face)
    {
        const usize axis = face / 2;
        const float32 sign = face % 2 == 0 ? 1.0F : -1.0F;
        const usize tangentAxis = (axis + 1) % 3;
        const usize bitangentAxis = (axis + 2) % 3;
        const uint32 base = static_cast<uint32>(mesh.Positions.GetSize());
        for (usize corner = 0; corner < CUBE_FACE_CORNERS; ++corner)
        {
            float32 components[3] = {0, 0, 0};
            components[axis] = sign;
            components[tangentAxis] = (corner & 1U) != 0 ? 1.0F : -1.0F;
            components[bitangentAxis] = (corner & 2U) != 0 ? 1.0F : -1.0F;
            mesh.Positions.Add({components[0], components[1], components[2]});
        }
        // The tangent and bitangent cross product points along +axis, so a negative face is wound the other way.
        const bool isFlipped = sign < 0;
        const StaticArray<uint32, 6> order =
            isFlipped == true ? StaticArray<uint32, 6>{0, 2, 1, 1, 2, 3} : StaticArray<uint32, 6>{0, 1, 2, 2, 1, 3};
        for (const uint32 index : order)
        {
            mesh.Indices.Add(base + index);
        }
    }
    return mesh;
}

/// A unit-radius UV sphere whose seam and poles split vertices.
Mesh MakeSphere()
{
    Mesh mesh;
    for (uint32 ring = 0; ring <= SPHERE_RINGS; ++ring)
    {
        const float32 polar = math::kPiF * static_cast<float32>(ring) / static_cast<float32>(SPHERE_RINGS);
        const bool isPole = ring == 0 || ring == SPHERE_RINGS;
        for (uint32 segment = 0; segment <= SPHERE_SEGMENTS; ++segment)
        {
            // The seam and the poles repeat exact positions, as an importer that splits vertices writes them.
            const float32 azimuth =
                math::kTauF * static_cast<float32>(segment % SPHERE_SEGMENTS) / static_cast<float32>(SPHERE_SEGMENTS);
            const float32 radius = isPole == true ? 0.0F : math::Sin(polar);
            const float32 height = ring == 0 ? 1.0F : (ring == SPHERE_RINGS ? -1.0F : math::Cos(polar));
            mesh.Positions.Add({radius * math::Cos(azimuth), height, radius * math::Sin(azimuth)});
        }
    }
    const uint32 stride = SPHERE_SEGMENTS + 1;
    for (uint32 ring = 0; ring < SPHERE_RINGS; ++ring)
    {
        for (uint32 segment = 0; segment < SPHERE_SEGMENTS; ++segment)
        {
            const uint32 a = ring * stride + segment;
            const uint32 b = a + stride;
            for (const uint32 index : StaticArray<uint32, 6>{a, a + 1, b, b, a + 1, b + 1})
            {
                mesh.Indices.Add(index);
            }
        }
    }
    return mesh;
}
} // namespace

TEST_CASE("A cube's outline is the hexagon around its visible faces, with no diagonals", "[editor][silhouette]")
{
    const Mesh cube = MakeSplitCube();
    REQUIRE(cube.Positions.GetSize() == CUBE_FACES * CUBE_FACE_CORNERS);
    MeshSilhouette silhouette;
    REQUIRE(silhouette.IsEmpty() == true);
    REQUIRE(silhouette.Build(cube.GetPositions(), cube.GetIndices()) == true);
    CHECK(silhouette.IsEmpty() == false);
    // Welding the split corners leaves twelve cube edges plus one diagonal in each face.
    CHECK(silhouette.GetEdgeCount() == 12 + CUBE_FACES);
    Array<SilhouetteSegment> segments;
    REQUIRE(
        silhouette.Extract(cube.GetPositions(), MakeViewToClip({3, 2.5F, CAMERA_DISTANCE}), SEGMENT_LIMIT, segments) ==
        true);
    CHECK(segments.GetSize() == CUBE_OBLIQUE_SILHOUETTE_EDGES);
}

TEST_CASE("A cube with shared corners has the same outline as one with split corners", "[editor][silhouette]")
{
    Mesh cube;
    for (usize corner = 0; corner < CUBE_CORNERS; ++corner)
    {
        cube.Positions.Add(
            {(corner & 1U) != 0 ? 1.0F : -1.0F, (corner & 2U) != 0 ? 1.0F : -1.0F, (corner & 4U) != 0 ? 1.0F : -1.0F});
    }
    // Six faces as outward-wound triangle pairs over the eight corners.
    for (const uint32 index : StaticArray<uint32, 36>{1, 3, 5, 5, 3, 7, 0, 4, 2, 2, 4, 6, 2, 6, 3, 3, 6, 7,
                                                      0, 1, 4, 4, 1, 5, 4, 5, 6, 6, 5, 7, 0, 2, 1, 1, 2, 3})
    {
        cube.Indices.Add(index);
    }
    MeshSilhouette silhouette;
    REQUIRE(silhouette.Build(cube.GetPositions(), cube.GetIndices()) == true);
    Array<SilhouetteSegment> segments;
    REQUIRE(
        silhouette.Extract(cube.GetPositions(), MakeViewToClip({3, 2.5F, CAMERA_DISTANCE}), SEGMENT_LIMIT, segments) ==
        true);
    CHECK(segments.GetSize() == CUBE_OBLIQUE_SILHOUETTE_EDGES);
}

TEST_CASE("An open mesh outlines every edge that has one triangle", "[editor][silhouette]")
{
    Mesh triangle;
    triangle.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    triangle.Indices = {0, 1, 2};
    MeshSilhouette silhouette;
    REQUIRE(silhouette.Build(triangle.GetPositions(), triangle.GetIndices()) == true);
    Array<SilhouetteSegment> segments;
    REQUIRE(
        silhouette.Extract(triangle.GetPositions(), MakeViewToClip({0, 0, CAMERA_DISTANCE}), SEGMENT_LIMIT, segments) ==
        true);
    CHECK(segments.GetSize() == TRIANGLE_EDGES);
    // Seen from behind it is the same open boundary.
    segments.Clear();
    REQUIRE(silhouette.Extract(triangle.GetPositions(),
                               MakeViewToClip({0, 0, -CAMERA_DISTANCE}),
                               SEGMENT_LIMIT,
                               segments) == true);
    CHECK(segments.GetSize() == TRIANGLE_EDGES);
}

TEST_CASE("A sphere's outline is a closed curve far shorter than its mesh", "[editor][silhouette]")
{
    const Mesh sphere = MakeSphere();
    MeshSilhouette silhouette;
    REQUIRE(silhouette.Build(sphere.GetPositions(), sphere.GetIndices()) == true);
    for (const math::Vector3 eye :
         StaticArray<math::Vector3, 3>{{{0, 0, CAMERA_DISTANCE}, {4, 3, 4}, {0, CAMERA_DISTANCE, 0.5F}}})
    {
        Array<SilhouetteSegment> segments;
        REQUIRE(silhouette.Extract(sphere.GetPositions(), MakeViewToClip(eye), SEGMENT_LIMIT, segments) == true);
        REQUIRE(segments.GetSize() > SPHERE_RINGS / 2);
        // Far fewer edges than the mesh has: the outline marks the form, not the tessellation.
        CHECK(segments.GetSize() < silhouette.GetEdgeCount() / 4);
        // Closed: front and back triangles meet along a cycle, so an even number of segments meets at every endpoint.
        for (usize first = 0; first < segments.GetSize(); ++first)
        {
            usize shared = 0;
            for (usize second = 0; second < segments.GetSize(); ++second)
            {
                shared += segments[second].FirstClip == segments[first].FirstClip ? 1 : 0;
                shared += segments[second].SecondClip == segments[first].FirstClip ? 1 : 0;
            }
            CHECK(shared % 2 == 0);
        }
    }
}

TEST_CASE("Extraction honours the segment bound and rejects another mesh", "[editor][silhouette]")
{
    const Mesh sphere = MakeSphere();
    MeshSilhouette silhouette;
    REQUIRE(silhouette.Build(sphere.GetPositions(), sphere.GetIndices()) == true);
    constexpr usize LIMIT = 5;
    Array<SilhouetteSegment> segments;
    REQUIRE(silhouette.Extract(sphere.GetPositions(), MakeViewToClip({4, 3, 4}), LIMIT, segments) == true);
    CHECK(segments.GetSize() == LIMIT);
    const Mesh cube = MakeSplitCube();
    segments.Clear();
    CHECK(silhouette.Extract(cube.GetPositions(), MakeViewToClip({4, 3, 4}), SEGMENT_LIMIT, segments) == false);
    CHECK(segments.IsEmpty() == true);
    silhouette.Clear();
    CHECK(silhouette.IsEmpty() == true);
    CHECK(silhouette.Extract(sphere.GetPositions(), MakeViewToClip({4, 3, 4}), SEGMENT_LIMIT, segments) == false);
}

TEST_CASE("Meshes without usable triangles or beyond the bound build no adjacency", "[editor][silhouette]")
{
    MeshSilhouette silhouette;
    Mesh empty;
    CHECK(silhouette.Build(empty.GetPositions(), empty.GetIndices()) == false);
    Mesh degenerate;
    degenerate.Positions = {{0, 0, 0}, {0, 0, 0}, {1, 0, 0}};
    degenerate.Indices = {0, 1, 2, 0, 0, 5};
    CHECK(silhouette.Build(degenerate.GetPositions(), degenerate.GetIndices()) == false);
    Mesh invalid;
    const float32 notANumber = math::Sqrt(-1.0F);
    invalid.Positions = {{0, 0, 0}, {1, 0, 0}, {notANumber, 1, 0}};
    invalid.Indices = {0, 1, 2};
    CHECK(silhouette.Build(invalid.GetPositions(), invalid.GetIndices()) == false);
    Mesh large;
    large.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (usize triangle = 0; triangle <= MeshSilhouette::MAX_TRIANGLES; ++triangle)
    {
        for (const uint32 index : StaticArray<uint32, 3>{0, 1, 2})
        {
            large.Indices.Add(index);
        }
    }
    CHECK(silhouette.Build(large.GetPositions(), large.GetIndices()) == false);
    CHECK(silhouette.IsEmpty() == true);
}

TEST_CASE("A cube behind the camera and one straddling its plane extract without failing", "[editor][silhouette]")
{
    const Mesh cube = MakeSplitCube();
    MeshSilhouette silhouette;
    REQUIRE(silhouette.Build(cube.GetPositions(), cube.GetIndices()) == true);
    // The camera sits inside the cube: every vertex is in front or behind, and facing stays defined.
    Array<SilhouetteSegment> segments;
    CHECK(silhouette.Extract(cube.GetPositions(), MakeViewToClip({0.2F, 0.1F, 0.3F}), SEGMENT_LIMIT, segments) == true);
    // The camera is beyond the cube looking away from it.
    segments.Clear();
    math::Matrix4 view;
    REQUIRE(math::TryLookAt({0, 0, CAMERA_DISTANCE}, {0, 0, 2 * CAMERA_DISTANCE}, {0, 1, 0}, view) ==
            math::MathStatus::Success);
    CHECK(silhouette.Extract(cube.GetPositions(), view, SEGMENT_LIMIT, segments) == true);
}
