// Convex polygon partition: every case is checked for exact triangle coverage,
// preserved area and strictly convex polygons of at most seven vertices, the
// input contract of projected solid angle sampling (Peters 2021, section 3.1).

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/geometry/convex_polygon.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>

using namespace ludus::foundation;
using namespace ludus::foundation::geometry;
using math::Vector3;

namespace
{
struct Mesh final
{
    Array<Vector3> Positions;
    Array<uint32> Indices;
};

[[nodiscard]] Mesh MakeMesh(std::initializer_list<Vector3> positions, std::initializer_list<uint32> indices)
{
    Mesh mesh;
    for (const Vector3& position : positions)
    {
        REQUIRE(mesh.Positions.TryAdd(position) == true);
    }
    for (const uint32 index : indices)
    {
        REQUIRE(mesh.Indices.TryAdd(index) == true);
    }
    return mesh;
}

/// A regular polygon of the given vertex count triangulated as a fan around an added center vertex.
[[nodiscard]] Mesh MakeCenterFan(uint32 count, Vector3 offset)
{
    Mesh mesh;
    REQUIRE(mesh.Positions.TryAdd(offset) == true);
    for (uint32 i = 0; i < count; ++i)
    {
        const float32 angle = static_cast<float32>(i) * math::kTauF / static_cast<float32>(count);
        REQUIRE(mesh.Positions.TryAdd(offset + Vector3{math::Cos(angle), math::Sin(angle), 0.0F}) == true);
    }
    for (uint32 i = 0; i < count; ++i)
    {
        REQUIRE(mesh.Indices.TryAdd(0) == true);
        REQUIRE(mesh.Indices.TryAdd(1 + i) == true);
        REQUIRE(mesh.Indices.TryAdd(1 + (i + 1) % count) == true);
    }
    return mesh;
}

/// An n by n grid over the unit square in the plane z = 0, two triangles per cell.
[[nodiscard]] Mesh MakeGrid(uint32 cells)
{
    Mesh mesh;
    for (uint32 y = 0; y <= cells; ++y)
    {
        for (uint32 x = 0; x <= cells; ++x)
        {
            REQUIRE(mesh.Positions.TryAdd(Vector3{static_cast<float32>(x) / static_cast<float32>(cells),
                                                  static_cast<float32>(y) / static_cast<float32>(cells),
                                                  0.0F}) == true);
        }
    }
    for (uint32 y = 0; y < cells; ++y)
    {
        for (uint32 x = 0; x < cells; ++x)
        {
            const uint32 corner = y * (cells + 1) + x;
            for (const uint32 index :
                 {corner, corner + 1, corner + cells + 2, corner, corner + cells + 2, corner + cells + 1})
            {
                REQUIRE(mesh.Indices.TryAdd(index) == true);
            }
        }
    }
    return mesh;
}

[[nodiscard]] float64 ComputeTriangleArea(const Mesh& mesh, usize triangle) noexcept
{
    const Vector3 a = mesh.Positions[mesh.Indices[triangle * 3]];
    const Vector3 b = mesh.Positions[mesh.Indices[triangle * 3 + 1]];
    const Vector3 c = mesh.Positions[mesh.Indices[triangle * 3 + 2]];
    return 0.5 * static_cast<float64>(math::Length(math::Cross(b - a, c - a)));
}

[[nodiscard]] float64 ComputePolygonArea(const ConvexPolygon& polygon) noexcept
{
    Vector3 sum{};
    for (usize i = 1; i + 1 < polygon.VertexCount; ++i)
    {
        sum = sum + math::Cross(polygon.PositionsMesh[i] - polygon.PositionsMesh[0],
                                polygon.PositionsMesh[i + 1] - polygon.PositionsMesh[0]);
    }
    return 0.5 * static_cast<float64>(math::Dot(sum, polygon.NormalMesh));
}

/// Partition and check the contract that every consumer relies on.
[[nodiscard]] ConvexPolygonPartition PartitionAndCheck(const Mesh& mesh)
{
    ConvexPolygonPartition partition;
    REQUIRE(TryPartitionConvexPolygons({mesh.Positions.GetData(), mesh.Positions.GetSize()},
                                       {mesh.Indices.GetData(), mesh.Indices.GetSize()},
                                       partition) == GeometryStatus::Success);
    const usize triangleCount = mesh.Indices.GetSize() / 3;
    Array<uint32> coverage;
    REQUIRE(coverage.TryResize(triangleCount, 0U) == true);
    float64 polygonArea = 0.0;
    usize previousFirstTriangle = 0;
    for (usize p = 0; p < partition.Polygons.GetSize(); ++p)
    {
        const ConvexPolygon& polygon = partition.Polygons[p];
        REQUIRE(polygon.VertexCount >= 3);
        REQUIRE(polygon.VertexCount <= MAX_CONVEX_POLYGON_VERTICES);
        REQUIRE(std::abs(static_cast<float64>(math::Length(polygon.NormalMesh)) - 1.0) <= 1.0e-6);
        REQUIRE(polygon.TriangleCount > 0);
        // Ordered by smallest source triangle, ascending within each group.
        const uint32 smallest = partition.Triangles[polygon.FirstTriangle];
        REQUIRE((p == 0 || smallest > previousFirstTriangle) == true);
        previousFirstTriangle = smallest;
        for (usize i = 0; i < polygon.TriangleCount; ++i)
        {
            const uint32 triangle = partition.Triangles[polygon.FirstTriangle + i];
            REQUIRE((i == 0 || triangle > partition.Triangles[polygon.FirstTriangle + i - 1]) == true);
            ++coverage[triangle];
        }
        // Strictly convex, counterclockwise about the normal, with source positions only.
        for (usize i = 0; i < polygon.VertexCount; ++i)
        {
            const Vector3 previous = polygon.PositionsMesh[(i + polygon.VertexCount - 1) % polygon.VertexCount];
            const Vector3 current = polygon.PositionsMesh[i];
            const Vector3 next = polygon.PositionsMesh[(i + 1) % polygon.VertexCount];
            REQUIRE(math::Dot(math::Cross(current - previous, next - current), polygon.NormalMesh) > 0.0F);
            bool isSourcePosition = false;
            for (usize j = 0; j < mesh.Positions.GetSize(); ++j)
            {
                isSourcePosition = isSourcePosition == true || mesh.Positions[j] == current;
            }
            REQUIRE(isSourcePosition == true);
        }
        polygonArea += ComputePolygonArea(polygon);
    }
    float64 triangleArea = 0.0;
    usize coveredCount = 0;
    for (usize t = 0; t < triangleCount; ++t)
    {
        REQUIRE(coverage[t] <= 1);
        coveredCount += coverage[t];
        triangleArea += coverage[t] == 1 ? ComputeTriangleArea(mesh, t) : 0.0;
    }
    REQUIRE(coveredCount + partition.DegenerateTriangleCount == triangleCount);
    REQUIRE(partition.Triangles.GetSize() == coveredCount);
    REQUIRE(std::abs(polygonArea - triangleArea) <= 1.0e-5 * triangleArea);
    return partition;
}

/// The polygons' projected solid angles must add up to the triangles' as seen from a shading point.
void RequireSameProjectedSolidAngle(const Mesh& mesh, const ConvexPolygonPartition& partition, Vector3 shadingPoint)
{
    float64 fromTriangles = 0.0;
    for (usize t = 0; t < mesh.Indices.GetSize() / 3; ++t)
    {
        math::ProjectedPolygonInput input;
        input.VertexCount = 3;
        for (usize k = 0; k < 3; ++k)
        {
            input.PositionsTangent[k] = mesh.Positions[mesh.Indices[t * 3 + k]] - shadingPoint;
        }
        math::ProjectedSolidAnglePolygon prepared;
        REQUIRE(math::TryPrepareProjectedSolidAnglePolygon(input, prepared) == math::MathStatus::Success);
        fromTriangles += static_cast<float64>(prepared.ProjectedSolidAngle);
    }
    float64 fromPolygons = 0.0;
    for (usize p = 0; p < partition.Polygons.GetSize(); ++p)
    {
        const ConvexPolygon& polygon = partition.Polygons[p];
        math::ProjectedPolygonInput input;
        input.VertexCount = static_cast<uint32>(polygon.VertexCount);
        for (usize k = 0; k < polygon.VertexCount; ++k)
        {
            input.PositionsTangent[k] = polygon.PositionsMesh[k] - shadingPoint;
        }
        math::ProjectedSolidAnglePolygon prepared;
        REQUIRE(math::TryPrepareProjectedSolidAnglePolygon(input, prepared) == math::MathStatus::Success);
        fromPolygons += static_cast<float64>(prepared.ProjectedSolidAngle);
    }
    REQUIRE(fromTriangles > 0.0);
    REQUIRE(std::abs(fromPolygons - fromTriangles) <= 1.0e-4 * fromTriangles);
}
} // namespace

TEST_CASE("Two triangles of a quad become one quad with the source winding", "[geometry][convex_polygon]")
{
    const Mesh mesh = MakeMesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {0, 1, 2, 0, 2, 3});
    const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
    REQUIRE(partition.Polygons.GetSize() == 1);
    REQUIRE(partition.Polygons[0].VertexCount == 4);
    REQUIRE(partition.Polygons[0].NormalMesh == Vector3{0, 0, 1});
    REQUIRE(partition.Polygons[0].TriangleCount == 2);
    REQUIRE(partition.DegenerateTriangleCount == 0);
    REQUIRE(partition.UnmergeableEdgeCount == 0);
}

TEST_CASE("Fans become one polygon when it fits and convex pieces otherwise", "[geometry][convex_polygon]")
{
    SECTION("A hexagon around a center vertex drops the center")
    {
        const Mesh mesh = MakeCenterFan(6, Vector3{3, -2, 1});
        const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
        REQUIRE(partition.Polygons.GetSize() == 1);
        REQUIRE(partition.Polygons[0].VertexCount == 6);
    }
    SECTION("A heptagon fanned from one vertex")
    {
        Mesh mesh = MakeCenterFan(7, Vector3{});
        mesh.Indices.Clear();
        for (uint32 i = 1; i + 1 < 7; ++i)
        {
            for (const uint32 index : {1U, 1 + i, 2 + i})
            {
                REQUIRE(mesh.Indices.TryAdd(index) == true);
            }
        }
        const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
        REQUIRE(partition.Polygons.GetSize() == 1);
        REQUIRE(partition.Polygons[0].VertexCount == 7);
    }
    SECTION("An octagon exceeds the vertex limit and splits")
    {
        const Mesh mesh = MakeCenterFan(8, Vector3{});
        const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
        REQUIRE(partition.Polygons.GetSize() == 2);
        RequireSameProjectedSolidAngle(mesh, partition, Vector3{0.3F, -0.2F, -0.7F});
    }
}

TEST_CASE("Collinear boundary vertices are removed", "[geometry][convex_polygon]")
{
    // A T-junction on the bottom edge (paper supplement A.2 forbids three collinear vertices).
    const Mesh mesh = MakeMesh({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}}, {0, 1, 4, 1, 2, 3, 1, 3, 4});
    const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
    REQUIRE(partition.Polygons.GetSize() == 1);
    REQUIRE(partition.Polygons[0].VertexCount == 4);

    // Interior grid vertices disappear along shared chains.
    const Mesh grid = MakeGrid(4);
    const ConvexPolygonPartition gridPartition = PartitionAndCheck(grid);
    REQUIRE(gridPartition.Polygons.GetSize() == 1);
    REQUIRE(gridPartition.Polygons[0].VertexCount == 4);
}

TEST_CASE("Concave shapes and holes split into convex polygons", "[geometry][convex_polygon]")
{
    SECTION("An L shape")
    {
        const Mesh mesh = MakeMesh({{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}, {1, 2, 0}, {0, 2, 0}},
                                   {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5});
        const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
        REQUIRE(partition.Polygons.GetSize() == 2);
        RequireSameProjectedSolidAngle(mesh, partition, Vector3{0.5F, 0.5F, -1.0F});
    }
    SECTION("A square frame around a hole")
    {
        const Mesh mesh =
            MakeMesh({{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}, {1, 1, 0}, {3, 1, 0}, {3, 3, 0}, {1, 3, 0}},
                     {0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7});
        const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
        REQUIRE(partition.Polygons.GetSize() >= 4);
        RequireSameProjectedSolidAngle(mesh, partition, Vector3{2.0F, 2.0F, -0.5F});
    }
}

TEST_CASE("Only coplanar, consistently wound, manifold neighbors merge", "[geometry][convex_polygon]")
{
    SECTION("Disjoint quads stay separate and ordered by their first triangle")
    {
        const Mesh mesh =
            MakeMesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {5, 0, 0}, {6, 0, 0}, {6, 1, 0}, {5, 1, 0}},
                     {4, 5, 6, 0, 1, 2, 0, 2, 3, 4, 6, 7});
        const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
        REQUIRE(partition.Polygons.GetSize() == 2);
        REQUIRE(partition.Triangles[partition.Polygons[0].FirstTriangle] == 0);
        REQUIRE(partition.Triangles[partition.Polygons[1].FirstTriangle] == 1);
    }
    SECTION("A folded pair")
    {
        const Mesh mesh = MakeMesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 1}}, {0, 1, 2, 0, 2, 3});
        REQUIRE(PartitionAndCheck(mesh).Polygons.GetSize() == 2);
    }
    SECTION("Opposite winding")
    {
        const Mesh mesh = MakeMesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {0, 1, 2, 0, 3, 2});
        const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
        REQUIRE(partition.Polygons.GetSize() == 2);
        REQUIRE(partition.UnmergeableEdgeCount == 1);
    }
    SECTION("A nonmanifold edge")
    {
        const Mesh mesh =
            MakeMesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0.5F, 0.5F, 1}}, {0, 1, 2, 0, 2, 3, 2, 0, 4});
        const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
        REQUIRE(partition.Polygons.GetSize() == 3);
        REQUIRE(partition.UnmergeableEdgeCount == 1);
    }
    SECTION("Split vertices with equal positions still share edges")
    {
        const Mesh mesh =
            MakeMesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {0, 1, 2, 3, 4, 5});
        REQUIRE(PartitionAndCheck(mesh).Polygons.GetSize() == 1);
    }
    SECTION("Far from the origin, float32 rounding of a tilted quad does not prevent merging")
    {
        // The plane z = 0.3x + 0.2y; rounding z near 2.5e4 moves the corners off it by up to 1e-3.
        const Vector3 offset{1.0e5F, -3.0e4F, 2.5e4F};
        const Mesh mesh =
            MakeMesh({offset, offset + Vector3{1, 0, 0.3F}, offset + Vector3{1, 1, 0.5F}, offset + Vector3{0, 1, 0.2F}},
                     {0, 1, 2, 0, 2, 3});
        REQUIRE(PartitionAndCheck(mesh).Polygons.GetSize() == 1);
        // Rounding moves a unit fan's center visibly off the straight line, so its pieces stay apart but valid.
        static_cast<void>(PartitionAndCheck(MakeCenterFan(5, offset)));
    }
}

TEST_CASE("Degenerate triangles are counted and left out", "[geometry][convex_polygon]")
{
    const Mesh mesh =
        MakeMesh({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}}, {0, 1, 2, 0, 2, 3, 0, 1, 4, 1, 1, 2});
    const ConvexPolygonPartition partition = PartitionAndCheck(mesh);
    REQUIRE(partition.DegenerateTriangleCount == 2);
    REQUIRE(partition.Polygons.GetSize() == 1);
    REQUIRE(partition.Triangles.GetSize() == 2);
}

TEST_CASE("Invalid meshes leave the partition unchanged", "[geometry][convex_polygon]")
{
    ConvexPolygonPartition partition;
    partition.DegenerateTriangleCount = 42;
    const Vector3 positions[] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
    const uint32 twoIndices[] = {0, 1};
    REQUIRE(TryPartitionConvexPolygons(positions, twoIndices, partition) == GeometryStatus::InvalidArgument);
    const uint32 outOfRange[] = {0, 1, 3};
    REQUIRE(TryPartitionConvexPolygons(positions, outOfRange, partition) == GeometryStatus::InvalidTopology);
    const Vector3 nonFinite[] = {{0, 0, 0}, {1, 0, 0}, {1, std::numeric_limits<float32>::quiet_NaN(), 0}};
    const uint32 triangle[] = {0, 1, 2};
    REQUIRE(TryPartitionConvexPolygons(nonFinite, triangle, partition) == GeometryStatus::NonFiniteInput);
    REQUIRE(partition.DegenerateTriangleCount == 42);
    REQUIRE(partition.Polygons.GetSize() == 0);
}
