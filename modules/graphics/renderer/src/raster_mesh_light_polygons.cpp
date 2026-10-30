// Constant-emission region merging for raster mesh-light records; see raster_mesh_light_polygons.h.
// Welding by position and pairing triangles across shared edges follow FoundationGeometry's convex partition
// (convex_polygon.cpp), which adapts Hertel and Mehlhorn's removal of inessential diagonals. Here a whole joined
// region is accepted or rejected at once, so fine grids collapse to their convex outline without a loop-size bound,
// and an outline longer than one record is fanned into pieces that share chords. Geometry tests run in float64 on the
// float32 mesh positions, which are copied bitwise into the output.

#include "internal/raster_mesh_light_polygons.h"

#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace
{
using math::Vector3;
using math::Vector3d;
constexpr usize TRIANGLE_CORNERS = 3;
constexpr usize MIN_POLYGON_CORNERS = 3;
// Unit normals of edge-adjacent triangles whose dot product is at least this are coplanar (about 0.26 degrees).
constexpr float64 COPLANAR_NORMAL_COSINE = 1.0 - 1.0e-5;
// Distance of a region vertex from the region's plane, relative to the region's extent.
constexpr float64 PLANE_DISTANCE_RELATIVE_TOLERANCE = 1.0e-5;
// Plane distance floor relative to the coordinate magnitude, about eight float32 ulps.
constexpr float64 COORDINATE_RELATIVE_TOLERANCE = 1.0e-6;
// |sin(turn)| at or below this makes a boundary vertex straight; it also keeps sliver triangles unmerged.
constexpr float64 COLLINEAR_SINE_TOLERANCE = 1.0e-5;
// The convex outline must enclose the region's summed triangle area within this relative tolerance.
constexpr float64 AREA_RELATIVE_TOLERANCE = 1.0e-5;
// Longest run of triangle edges on one welded edge that is paired by exhaustive comparison; longer runs stay apart.
constexpr usize MAX_EDGE_RUN = 8;
constexpr uint32 NO_EDGE = 0xffffffffU;

struct CornerKey final
{
    Vector3 Position{};
    uint32 Corner = 0;
};

/// The edge from corner Corner to the next corner of its triangle, by welded position id.
struct EmitterEdge final
{
    uint32 Low = 0;  ///< Smaller position id.
    uint32 High = 0; ///< Larger position id.
    uint32 Corner = 0;
    bool IsLowToHigh = false;
};

struct EmitterTriangle final
{
    Vector3d Normal{};
    float64 Area = 0.0;
    uint32 Parent = 0;
    bool IsMergeable = false;
};

struct BoundaryEdge final
{
    uint32 Start = 0;
    uint32 End = 0;
};

[[nodiscard]] bool IsPositionLess(Vector3 a, Vector3 b) noexcept
{
    return a.X < b.X || (a.X == b.X && (a.Y < b.Y || (a.Y == b.Y && a.Z < b.Z)));
}
[[nodiscard]] Vector3d Widen(Vector3 value) noexcept
{
    return Vector3d{static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
[[nodiscard]] float64 GetLength(Vector3d value) noexcept
{
    return math::Sqrt(Dot(value, value));
}
[[nodiscard]] float64 GetMagnitude(Vector3d value) noexcept
{
    return math::Max(math::Abs(value.X), math::Max(math::Abs(value.Y), math::Abs(value.Z)));
}
[[nodiscard]] uint32 FindRoot(Array<EmitterTriangle>& inoutTriangles, uint32 triangle) noexcept
{
    while (inoutTriangles[triangle].Parent != triangle)
    {
        inoutTriangles[triangle].Parent = inoutTriangles[inoutTriangles[triangle].Parent].Parent;
        triangle = inoutTriangles[triangle].Parent;
    }
    return triangle;
}
/// Signed sine of the turn at current relative to normal; a zero-length edge or a doubling back is a reversal.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): vertices in loop order.
[[nodiscard]] float64
ComputeTurnSine(Vector3d previous, Vector3d current, Vector3d next, Vector3d normal, bool& outIsReversal) noexcept
{
    const Vector3d incoming = current - previous;
    const Vector3d outgoing = next - current;
    const float64 lengths = GetLength(incoming) * GetLength(outgoing);
    if ((lengths > 0.0) == false)
    {
        outIsReversal = true;
        return 0.0;
    }
    const float64 sine = Dot(Cross(incoming, outgoing), normal) / lengths;
    outIsReversal = math::Abs(sine) <= COLLINEAR_SINE_TOLERANCE && Dot(incoming, outgoing) < 0.0;
    return sine;
}
// NOLINTEND(bugprone-easily-swappable-parameters)
/// Index of the boundary edge starting at start in edges sorted by Start, or NO_EDGE.
[[nodiscard]] uint32 FindBoundaryEdge(const Array<BoundaryEdge>& edges, uint32 start) noexcept
{
    usize low = 0;
    usize high = edges.GetSize();
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (edges[middle].Start < start)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low < edges.GetSize() && edges[low].Start == start ? static_cast<uint32>(low) : NO_EDGE;
}

/// Scratch reused across the regions of one mesh.
struct RegionScratch final
{
    Array<BoundaryEdge> Boundary;
    Array<uint32> Loop;
    Array<uint32> Corners;
};

/// Write the region's strictly convex outline to inoutScratch.Corners and return true, or return false when the
/// region is not a planar convex disk within tolerance. outIsOutOfMemory reports allocation failure separately.
[[nodiscard]] bool TryFindConvexOutline(const Array<Vector3d>& positions,
                                        const Array<uint32>& positionIds,
                                        const Array<bool>& isInteriorEdge,
                                        BorrowedView<const uint32> members,
                                        Vector3d normal,
                                        float64 area,
                                        RegionScratch& inoutScratch,
                                        bool& outIsOutOfMemory) noexcept
{
    outIsOutOfMemory = false;
    Array<BoundaryEdge>& boundary = inoutScratch.Boundary;
    boundary.Clear();
    for (usize member = 0; member < members.GetSize(); ++member)
    {
        const uint32 triangle = members[member];
        for (usize k = 0; k < TRIANGLE_CORNERS; ++k)
        {
            const usize corner = triangle * TRIANGLE_CORNERS + k;
            if (isInteriorEdge[corner] == false &&
                boundary.TryAdd(BoundaryEdge
                {
                    .Start = positionIds[corner],
                    .End = positionIds[triangle * TRIANGLE_CORNERS + (k + 1) % TRIANGLE_CORNERS],
                }) == false)
            {
                outIsOutOfMemory = true;
                return false;
            }
        }
    }
    // Every region vertex lies on the plane through a boundary vertex, relative to the region's extent.
    const Vector3d origin = positions[boundary[0].Start];
    float64 extent = 0.0;
    float64 magnitude = 0.0;
    for (usize member = 0; member < members.GetSize(); ++member)
    {
        const uint32 triangle = members[member];
        for (usize k = 0; k < TRIANGLE_CORNERS; ++k)
        {
            const Vector3d position = positions[positionIds[triangle * TRIANGLE_CORNERS + k]];
            extent = math::Max(extent, GetMagnitude(position - origin));
            magnitude = math::Max(magnitude, GetMagnitude(position));
        }
    }
    const float64 planeTolerance =
        PLANE_DISTANCE_RELATIVE_TOLERANCE * extent + COORDINATE_RELATIVE_TOLERANCE * magnitude;
    for (usize member = 0; member < members.GetSize(); ++member)
    {
        const uint32 triangle = members[member];
        for (usize k = 0; k < TRIANGLE_CORNERS; ++k)
        {
            if (math::Abs(Dot(normal, positions[positionIds[triangle * TRIANGLE_CORNERS + k]] - origin)) >
                planeTolerance)
            {
                return false;
            }
        }
    }
    // A disk has one boundary loop through distinct starts.
    Sort(boundary.GetData(), boundary.GetSize(), [](const BoundaryEdge& a, const BoundaryEdge& b) noexcept {
        return a.Start < b.Start;
    });
    for (usize i = 1; i < boundary.GetSize(); ++i)
    {
        if (boundary[i].Start == boundary[i - 1].Start)
        {
            return false;
        }
    }
    Array<uint32>& loop = inoutScratch.Loop;
    loop.Clear();
    uint32 edge = 0;
    for (usize step = 0; step < boundary.GetSize(); ++step)
    {
        if (loop.TryAdd(boundary[edge].Start) == false)
        {
            outIsOutOfMemory = true;
            return false;
        }
        edge = FindBoundaryEdge(boundary, boundary[edge].End);
        if (edge == NO_EDGE || (edge == 0 && step + 1 < boundary.GetSize()))
        {
            return false;
        }
    }
    if (edge != 0)
    {
        return false;
    }
    // Strictly convex corners after dropping straight vertices; any reflex turn or doubling back rejects.
    Array<uint32>& corners = inoutScratch.Corners;
    corners.Clear();
    const usize loopCount = loop.GetSize();
    for (usize i = 0; i < loopCount; ++i)
    {
        bool isReversal = false;
        const float64 sine = ComputeTurnSine(positions[loop[(i + loopCount - 1) % loopCount]],
                                             positions[loop[i]],
                                             positions[loop[(i + 1) % loopCount]],
                                             normal,
                                             isReversal);
        if (isReversal == true || sine < -COLLINEAR_SINE_TOLERANCE)
        {
            return false;
        }
        if (sine > COLLINEAR_SINE_TOLERANCE && corners.TryAdd(loop[i]) == false)
        {
            outIsOutOfMemory = true;
            return false;
        }
    }
    if (corners.GetSize() < MIN_POLYGON_CORNERS)
    {
        return false;
    }
    // All-left turns can still wind more than once; the enclosed area exposes it and bounds dropped slivers.
    Vector3d sum{};
    const Vector3d first = positions[corners[0]];
    for (usize i = 1; i + 1 < corners.GetSize(); ++i)
    {
        sum = sum + Cross(positions[corners[i]] - first, positions[corners[i + 1]] - first);
    }
    return math::Abs(0.5 * Dot(sum, normal) - area) <= AREA_RELATIVE_TOLERANCE * area;
}
} // namespace

bool TryPartitionRasterEmitterRegions(const Vector3* positionsMesh,
                                      const uint32* indices,
                                      BorrowedView<const uint32> triangles,
                                      RasterEmitterPartition& outPartition) noexcept
{
    const usize triangleCount = triangles.GetSize();
    const usize cornerCount = triangleCount * TRIANGLE_CORNERS;
    RasterEmitterPartition result;
    if (result.IsMerged.TryResize(triangleCount, false) == false)
    {
        return false;
    }
    if (triangleCount < 2)
    {
        outPartition = Move(result);
        return true;
    }

    // Identify corners by position so that split vertices (normal or UV seams) still share edges.
    Array<CornerKey> keys;
    Array<uint32> positionIds;
    Array<Vector3d> positions;
    Array<Vector3> uniquePositions;
    if (keys.TryResize(cornerCount) == false || positionIds.TryResize(cornerCount) == false ||
        positions.TryEnsureCapacity(cornerCount) == false || uniquePositions.TryEnsureCapacity(cornerCount) == false)
    {
        return false;
    }
    for (usize i = 0; i < cornerCount; ++i)
    {
        const uint32 vertex = indices[triangles[i / TRIANGLE_CORNERS] * TRIANGLE_CORNERS + i % TRIANGLE_CORNERS];
        keys[i] = CornerKey{ .Position = positionsMesh[vertex], .Corner = static_cast<uint32>(i) };
    }
    Sort(keys.GetData(), cornerCount, [](const CornerKey& a, const CornerKey& b) noexcept {
        return IsPositionLess(a.Position, b.Position) || (a.Position == b.Position && a.Corner < b.Corner);
    });
    for (usize i = 0; i < cornerCount; ++i)
    {
        if (i == 0 || (keys[i].Position == keys[i - 1].Position) == false)
        {
            static_cast<void>(uniquePositions.TryAdd(keys[i].Position));
            static_cast<void>(positions.TryAdd(Widen(keys[i].Position)));
        }
        positionIds[keys[i].Corner] = static_cast<uint32>(positions.GetSize() - 1);
    }

    // Slivers stay unmerged: they would break the no-collinear-corner contract of the outline.
    Array<EmitterTriangle> pieces;
    if (pieces.TryResize(triangleCount) == false)
    {
        return false;
    }
    for (usize t = 0; t < triangleCount; ++t)
    {
        EmitterTriangle& piece = pieces[t];
        piece.Parent = static_cast<uint32>(t);
        const uint32 a = positionIds[t * TRIANGLE_CORNERS];
        const uint32 b = positionIds[t * TRIANGLE_CORNERS + 1];
        const uint32 c = positionIds[t * TRIANGLE_CORNERS + 2];
        const Vector3d ab = positions[b] - positions[a];
        const Vector3d bc = positions[c] - positions[b];
        const Vector3d ca = positions[a] - positions[c];
        const Vector3d cross = Cross(ab, positions[c] - positions[a]);
        const float64 crossLength = GetLength(cross);
        const float64 longest = math::Max(GetLength(ab), math::Max(GetLength(bc), GetLength(ca)));
        if (a == b || b == c || c == a || (crossLength > COLLINEAR_SINE_TOLERANCE * longest * longest) == false)
        {
            continue;
        }
        piece.Normal = cross * (1.0 / crossLength);
        piece.Area = 0.5 * crossLength;
        piece.IsMergeable = true;
    }

    // Join across welded edges traversed in opposite directions by exactly one coplanar partner each, so that
    // coincident or back-to-back copies never join and non-manifold fins stay apart.
    Array<EmitterEdge> edges;
    Array<bool> isInteriorEdge;
    if (edges.TryEnsureCapacity(cornerCount) == false || isInteriorEdge.TryResize(cornerCount, false) == false)
    {
        return false;
    }
    for (usize corner = 0; corner < cornerCount; ++corner)
    {
        if (pieces[corner / TRIANGLE_CORNERS].IsMergeable == true)
        {
            const uint32 start = positionIds[corner];
            const uint32 end =
                positionIds[corner - corner % TRIANGLE_CORNERS + (corner % TRIANGLE_CORNERS + 1) % TRIANGLE_CORNERS];
            static_cast<void>(edges.TryAdd(EmitterEdge
            {
                .Low = math::Min(start, end),
                .High = math::Max(start, end),
                .Corner = static_cast<uint32>(corner),
                .IsLowToHigh = start < end,
            }));
        }
    }
    Sort(edges.GetData(), edges.GetSize(), [](const EmitterEdge& a, const EmitterEdge& b) noexcept {
        return a.Low < b.Low || (a.Low == b.Low && (a.High < b.High || (a.High == b.High && a.Corner < b.Corner)));
    });
    for (usize first = 0; first < edges.GetSize();)
    {
        usize end = first + 1;
        while (end < edges.GetSize() && edges[end].Low == edges[first].Low && edges[end].High == edges[first].High)
        {
            ++end;
        }
        StaticArray<uint32, MAX_EDGE_RUN> partners{};
        const usize runCount = end - first;
        for (usize i = 0; i < runCount && runCount <= MAX_EDGE_RUN; ++i)
        {
            partners[i] = NO_EDGE;
            const EmitterEdge& edge = edges[first + i];
            for (usize j = 0; j < runCount; ++j)
            {
                const EmitterEdge& other = edges[first + j];
                if (edge.IsLowToHigh != other.IsLowToHigh &&
                    Dot(pieces[edge.Corner / TRIANGLE_CORNERS].Normal,
                        pieces[other.Corner / TRIANGLE_CORNERS].Normal) >= COPLANAR_NORMAL_COSINE)
                {
                    // A second partner makes the pairing ambiguous.
                    partners[i] = partners[i] == NO_EDGE ? static_cast<uint32>(j) : static_cast<uint32>(runCount);
                }
            }
        }
        for (usize i = 0; i < runCount && runCount <= MAX_EDGE_RUN; ++i)
        {
            const uint32 j = partners[i];
            if (j < runCount && j > i && partners[j] == i)
            {
                const uint32 firstRoot = FindRoot(pieces, edges[first + i].Corner / TRIANGLE_CORNERS);
                const uint32 secondRoot = FindRoot(pieces, edges[first + j].Corner / TRIANGLE_CORNERS);
                // The smaller root survives so a region is named by its smallest triangle.
                pieces[math::Max(firstRoot, secondRoot)].Parent = math::Min(firstRoot, secondRoot);
                isInteriorEdge[edges[first + i].Corner] = true;
                isInteriorEdge[edges[first + j].Corner] = true;
            }
        }
        first = end;
    }

    // Group members by root with a counting sort; roots are each region's smallest triangle.
    Array<uint32> roots;
    Array<uint32> regionStart;
    Array<uint32> regionFill;
    Array<uint32> members;
    if (roots.TryResize(triangleCount) == false || regionStart.TryResize(triangleCount + 1, 0) == false ||
        regionFill.TryResize(triangleCount, 0) == false || members.TryResize(triangleCount) == false)
    {
        return false;
    }
    for (usize t = 0; t < triangleCount; ++t)
    {
        roots[t] = FindRoot(pieces, static_cast<uint32>(t));
        ++regionStart[roots[t] + 1];
    }
    for (usize t = 0; t < triangleCount; ++t)
    {
        regionStart[t + 1] += regionStart[t];
    }
    for (usize t = 0; t < triangleCount; ++t)
    {
        members[regionStart[roots[t]] + regionFill[roots[t]]] = static_cast<uint32>(t);
        ++regionFill[roots[t]];
    }

    RegionScratch scratch;
    for (usize root = 0; root < triangleCount; ++root)
    {
        const usize memberCount = regionStart[root + 1] - regionStart[root];
        if (memberCount < 2)
        {
            continue;
        }
        const BorrowedView<const uint32> region{members.GetData() + regionStart[root], memberCount};
        Vector3d normalSum{};
        float64 area = 0.0;
        for (usize member = 0; member < region.GetSize(); ++member)
        {
            const uint32 triangle = region[member];
            normalSum = normalSum + pieces[triangle].Normal * pieces[triangle].Area;
            area += pieces[triangle].Area;
        }
        const float64 normalLength = GetLength(normalSum);
        if ((normalLength > 0.0) == false)
        {
            continue;
        }
        const Vector3d normal = normalSum * (1.0 / normalLength);
        bool isOutOfMemory = false;
        if (TryFindConvexOutline(positions,
                                 positionIds,
                                 isInteriorEdge,
                                 region,
                                 normal,
                                 area,
                                 scratch,
                                 isOutOfMemory) == false)
        {
            if (isOutOfMemory == true)
            {
                return false;
            }
            continue;
        }
        // Fan from the first corner; each piece shares its first chord with the previous one.
        const Array<uint32>& corners = scratch.Corners;
        const usize outlineCount = corners.GetSize();
        for (usize start = 1; start + 1 < outlineCount;)
        {
            const usize last = math::Min(start + MAX_MESH_LIGHT_POLYGON_CORNERS - 2, outlineCount - 1);
            RasterEmitterPolygon polygon;
            polygon.NormalMesh =
                Vector3{static_cast<float32>(normal.X), static_cast<float32>(normal.Y), static_cast<float32>(normal.Z)};
            polygon.FirstTriangle = triangles[root];
            polygon.PositionsMesh[0] = uniquePositions[corners[0]];
            polygon.CornerCount = 1;
            for (usize corner = start; corner <= last; ++corner)
            {
                polygon.PositionsMesh[polygon.CornerCount] = uniquePositions[corners[corner]];
                ++polygon.CornerCount;
            }
            if (result.Polygons.TryAdd(polygon) == false)
            {
                return false;
            }
            start = last;
        }
        for (usize member = 0; member < region.GetSize(); ++member)
        {
            const uint32 triangle = region[member];
            result.IsMerged[triangle] = true;
        }
    }
    outPartition = Move(result);
    return true;
}
} // namespace ludus::graphics::renderer::internal
