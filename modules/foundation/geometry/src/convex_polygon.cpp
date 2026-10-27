// Convex polygon partition of triangle meshes; see convex_polygon.hpp for the
// Peters 2021 input contract and the Hertel-Mehlhorn merge it adapts. Geometry
// tests run in float64 on the float32 source positions, which are copied
// bitwise into the output.

#include <ludus/foundation/geometry/convex_polygon.hpp>

#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::foundation::geometry
{
namespace
{
using math::Vector3;
using math::Vector3d;

// A merged boundary keeps collinear vertices until the end so shared edges stay
// visible; this bounds it before they are removed.
constexpr usize MAX_LOOP_VERTICES = 2 * MAX_CONVEX_POLYGON_VERTICES + 2;
// Unit normals whose dot product is at least this are coplanar (about 0.26 degrees).
constexpr float64 COPLANAR_NORMAL_COSINE = 1.0 - 1.0e-5;
// Plane distance allowed while merging, relative to the merged polygon's extent.
constexpr float64 PLANE_DISTANCE_RELATIVE_TOLERANCE = 1.0e-5;
// Plane distance floor relative to the coordinate magnitude, about eight float32 ulps.
constexpr float64 COORDINATE_RELATIVE_TOLERANCE = 1.0e-6;
// |sin(turn)| at or below this makes a vertex straight; it also rejects sliver triangles.
constexpr float64 COLLINEAR_SINE_TOLERANCE = 1.0e-5;
// The merged area must match the sum of its parts within this relative tolerance, which rejects folded loops.
constexpr float64 AREA_RELATIVE_TOLERANCE = 1.0e-5;
// Merge passes over the shared edges; a later pass can join pieces whose earlier union was not convex.
constexpr usize MAX_MERGE_PASSES = 16;
constexpr usize TRIANGLE_CORNERS = 3;
// Corner, triangle and position ids are stored as uint32.
constexpr usize MAX_CORNER_COUNT = 0xffffffffU;

struct CornerKey final
{
    Vector3 Position{};
    uint32 Corner = 0;
};

struct SharedEdge final
{
    uint32 Low = 0;  ///< Smaller position id.
    uint32 High = 0; ///< Larger position id.
    uint32 Triangle = 0;
    bool IsLowToHigh = false; ///< Whether the triangle traverses the edge from Low to High.
};

struct MergeCandidate final
{
    uint32 FirstTriangle = 0;
    uint32 SecondTriangle = 0;
    uint32 Start = 0; ///< The edge runs Start -> End in FirstTriangle and End -> Start in SecondTriangle.
    uint32 End = 0;
};

struct Piece final
{
    StaticArray<uint32, MAX_LOOP_VERTICES> Loop{}; ///< Position ids in winding order, collinear vertices included.
    usize LoopCount = 0;
    Vector3d Normal{};
    float64 Area = 0.0;
    uint32 Parent = 0;
    bool IsValid = false;
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
[[nodiscard]] uint32 FindRoot(Array<Piece>& inoutPieces, uint32 piece) noexcept
{
    while (inoutPieces[piece].Parent != piece)
    {
        inoutPieces[piece].Parent = inoutPieces[inoutPieces[piece].Parent].Parent;
        piece = inoutPieces[piece].Parent;
    }
    return piece;
}

/// Signed sine of the turn at each vertex, relative to the normal; also reports zero-length edges.
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

/// Whether the loop is convex and simple enough to sample, and how many vertices remain without collinear ones.
[[nodiscard]] bool IsConvexLoop(const Array<Vector3d>& positions,
                                const uint32* loop,
                                usize loopCount,
                                Vector3d normal,
                                usize& outCornerCount) noexcept
{
    outCornerCount = 0;
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
        outCornerCount += sine > COLLINEAR_SINE_TOLERANCE ? 1 : 0;
    }
    return outCornerCount >= math::MIN_PROJECTED_POLYGON_VERTICES;
}

/// Signed area of a planar loop along the normal.
[[nodiscard]] float64
ComputeLoopArea(const Array<Vector3d>& positions, const uint32* loop, usize loopCount, Vector3d normal) noexcept
{
    Vector3d sum{};
    const Vector3d origin = positions[loop[0]];
    for (usize i = 1; i + 1 < loopCount; ++i)
    {
        sum = sum + Cross(positions[loop[i]] - origin, positions[loop[i + 1]] - origin);
    }
    return 0.5 * Dot(sum, normal);
}

/// Try to merge piece second into piece first across the shared edge start -> end of first, extending the shared
/// boundary in both directions. Writes the merged loop only when the union is a valid polygon.
[[nodiscard]] bool TryMergePieces(const Array<Vector3d>& positions,
                                  const Piece& first,
                                  const Piece& second,
                                  uint32 start,
                                  uint32 end,
                                  Piece& outMerged) noexcept
{
    if (Dot(first.Normal, second.Normal) < COPLANAR_NORMAL_COSINE)
    {
        return false;
    }
    const usize firstCount = first.LoopCount;
    const usize secondCount = second.LoopCount;
    usize firstStart = firstCount;
    usize secondStart = secondCount;
    for (usize i = 0; i < firstCount; ++i)
    {
        firstStart = first.Loop[i] == start && first.Loop[(i + 1) % firstCount] == end ? i : firstStart;
    }
    for (usize i = 0; i < secondCount; ++i)
    {
        secondStart = second.Loop[i] == end && second.Loop[(i + 1) % secondCount] == start ? i : secondStart;
    }
    if (firstStart == firstCount || secondStart == secondCount)
    {
        return false;
    }
    // The shared chain runs from first.Loop[chainBegin] to first.Loop[chainBegin + chainEdges] in first's order.
    usize chainBegin = firstStart;
    usize chainEdges = 1;
    usize secondChainEnd = (secondStart + 1) % secondCount; // second's copy of first.Loop[chainBegin]
    while (chainEdges + 1 < firstCount && chainEdges + 1 < secondCount &&
           first.Loop[(chainBegin + firstCount - 1) % firstCount] == second.Loop[(secondChainEnd + 1) % secondCount])
    {
        chainBegin = (chainBegin + firstCount - 1) % firstCount;
        secondChainEnd = (secondChainEnd + 1) % secondCount;
        ++chainEdges;
    }
    usize secondChainBegin = secondStart; // second's copy of the chain's last vertex
    while (chainEdges + 1 < firstCount && chainEdges + 1 < secondCount &&
           first.Loop[(chainBegin + chainEdges + 1) % firstCount] ==
               second.Loop[(secondChainBegin + secondCount - 1) % secondCount])
    {
        secondChainBegin = (secondChainBegin + secondCount - 1) % secondCount;
        ++chainEdges;
    }
    const usize mergedCount = firstCount + secondCount - 2 * chainEdges;
    if (mergedCount < math::MIN_PROJECTED_POLYGON_VERTICES || mergedCount > MAX_LOOP_VERTICES)
    {
        return false;
    }
    // First from the chain's last vertex around to its first vertex, then second's vertices off the chain.
    Piece merged;
    const usize chainEnd = (chainBegin + chainEdges) % firstCount;
    for (usize i = 0; i <= firstCount - chainEdges; ++i)
    {
        merged.Loop[merged.LoopCount] = first.Loop[(chainEnd + i) % firstCount];
        ++merged.LoopCount;
    }
    for (usize i = 1; i < secondCount - chainEdges; ++i)
    {
        merged.Loop[merged.LoopCount] = second.Loop[(secondChainEnd + i) % secondCount];
        ++merged.LoopCount;
    }
    if (merged.LoopCount != mergedCount)
    {
        return false;
    }
    // Coplanarity relative to the merged extent, with a floor for float32 coordinates.
    const Vector3d origin = positions[merged.Loop[0]];
    float64 extent = 0.0;
    float64 magnitude = 0.0;
    for (usize i = 0; i < merged.LoopCount; ++i)
    {
        extent = math::Max(extent, GetMagnitude(positions[merged.Loop[i]] - origin));
        magnitude = math::Max(magnitude, GetMagnitude(positions[merged.Loop[i]]));
    }
    const float64 planeTolerance =
        PLANE_DISTANCE_RELATIVE_TOLERANCE * extent + COORDINATE_RELATIVE_TOLERANCE * magnitude;
    for (usize i = 0; i < merged.LoopCount; ++i)
    {
        if (math::Abs(Dot(first.Normal, positions[merged.Loop[i]] - origin)) > planeTolerance)
        {
            return false;
        }
    }
    // The output classifies corners with this same normal, so the vertex count checked here is the one emitted.
    Vector3d normal = first.Normal * first.Area + second.Normal * second.Area;
    normal = normal * (1.0 / GetLength(normal));
    usize cornerCount = 0;
    if (IsConvexLoop(positions, merged.Loop.GetData(), merged.LoopCount, normal, cornerCount) == false ||
        cornerCount > MAX_CONVEX_POLYGON_VERTICES)
    {
        return false;
    }
    // A convex loop that winds more than once has all left turns; its area exposes it.
    const float64 area = ComputeLoopArea(positions, merged.Loop.GetData(), merged.LoopCount, normal);
    if (math::Abs(area - (first.Area + second.Area)) > AREA_RELATIVE_TOLERANCE * (first.Area + second.Area))
    {
        return false;
    }
    merged.Normal = normal;
    merged.Area = first.Area + second.Area;
    merged.IsValid = true;
    outMerged = merged;
    return true;
}
} // namespace

GeometryStatus TryPartitionConvexPolygons(BorrowedView<const math::Vector3> positionsMesh,
                                          BorrowedView<const uint32> indices,
                                          ConvexPolygonPartition& outPartition) noexcept
{
    if (positionsMesh.IsValid() == false || indices.IsValid() == false || indices.GetSize() % TRIANGLE_CORNERS != 0 ||
        indices.GetSize() > MAX_CORNER_COUNT)
    {
        return GeometryStatus::InvalidArgument;
    }
    const usize cornerCount = indices.GetSize();
    const usize triangleCount = cornerCount / TRIANGLE_CORNERS;
    for (usize i = 0; i < cornerCount; ++i)
    {
        if (indices[i] >= positionsMesh.GetSize())
        {
            return GeometryStatus::InvalidTopology;
        }
        if (math::IsFinite(positionsMesh[indices[i]]) == false)
        {
            return GeometryStatus::NonFiniteInput;
        }
    }

    // Identify corners by position so that split vertices (normal or UV seams) still share edges.
    Array<CornerKey> corners;
    Array<uint32> positionIds;
    Array<Vector3d> positions;
    Array<math::Vector3> uniquePositions;
    if (corners.TryResize(cornerCount) == false || positionIds.TryResize(cornerCount) == false ||
        positions.TryEnsureCapacity(cornerCount) == false || uniquePositions.TryEnsureCapacity(cornerCount) == false)
    {
        return GeometryStatus::OutOfMemory;
    }
    for (usize i = 0; i < cornerCount; ++i)
    {
        corners[i] = CornerKey{ .Position = positionsMesh[indices[i]], .Corner = static_cast<uint32>(i) };
    }
    Sort(corners.GetData(), cornerCount, [](const CornerKey& a, const CornerKey& b) noexcept {
        return IsPositionLess(a.Position, b.Position) || (a.Position == b.Position && a.Corner < b.Corner);
    });
    for (usize i = 0; i < cornerCount; ++i)
    {
        if (i == 0 || (corners[i].Position == corners[i - 1].Position) == false)
        {
            static_cast<void>(uniquePositions.TryAdd(corners[i].Position));
            static_cast<void>(positions.TryAdd(Widen(corners[i].Position)));
        }
        positionIds[corners[i].Corner] = static_cast<uint32>(positions.GetSize() - 1);
    }

    // Each nondegenerate triangle starts as its own piece.
    ConvexPolygonPartition result;
    Array<Piece> pieces;
    if (pieces.TryResize(triangleCount) == false)
    {
        return GeometryStatus::OutOfMemory;
    }
    for (usize t = 0; t < triangleCount; ++t)
    {
        Piece& piece = pieces[t];
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
        // The smallest angle's sine is at least |cross| / longest^2; slivers would break the no-collinear contract.
        if (a == b || b == c || c == a || (crossLength > COLLINEAR_SINE_TOLERANCE * longest * longest) == false)
        {
            ++result.DegenerateTriangleCount;
            continue;
        }
        piece.Loop[0] = a;
        piece.Loop[1] = b;
        piece.Loop[2] = c;
        piece.LoopCount = TRIANGLE_CORNERS;
        piece.Normal = cross * (1.0 / crossLength);
        piece.Area = 0.5 * crossLength;
        piece.IsValid = true;
    }

    // Shared edges, sorted so that merge order is deterministic.
    Array<SharedEdge> edges;
    if (edges.TryEnsureCapacity(cornerCount) == false)
    {
        return GeometryStatus::OutOfMemory;
    }
    for (usize t = 0; t < triangleCount; ++t)
    {
        for (usize k = 0; k < TRIANGLE_CORNERS && pieces[t].IsValid == true; ++k)
        {
            const uint32 start = positionIds[t * TRIANGLE_CORNERS + k];
            const uint32 end = positionIds[t * TRIANGLE_CORNERS + (k + 1) % TRIANGLE_CORNERS];
            static_cast<void>(edges.TryAdd(SharedEdge
            {
                .Low = math::Min(start, end),
                .High = math::Max(start, end),
                .Triangle = static_cast<uint32>(t),
                .IsLowToHigh = start < end,
            }));
        }
    }
    Sort(edges.GetData(), edges.GetSize(), [](const SharedEdge& a, const SharedEdge& b) noexcept {
        return a.Low < b.Low || (a.Low == b.Low && (a.High < b.High || (a.High == b.High && a.Triangle < b.Triangle)));
    });
    Array<MergeCandidate> candidates;
    if (candidates.TryEnsureCapacity(edges.GetSize() / 2) == false)
    {
        return GeometryStatus::OutOfMemory;
    }
    for (usize first = 0; first < edges.GetSize();)
    {
        usize end = first + 1;
        while (end < edges.GetSize() && edges[end].Low == edges[first].Low && edges[end].High == edges[first].High)
        {
            ++end;
        }
        if (end - first == 2 && edges[first].IsLowToHigh != edges[first + 1].IsLowToHigh)
        {
            const SharedEdge& edge = edges[first];
            static_cast<void>(candidates.TryAdd(MergeCandidate
            {
                .FirstTriangle = edge.Triangle,
                .SecondTriangle = edges[first + 1].Triangle,
                .Start = edge.IsLowToHigh == true ? edge.Low : edge.High,
                .End = edge.IsLowToHigh == true ? edge.High : edge.Low,
            }));
        }
        else if (end - first > 1)
        {
            ++result.UnmergeableEdgeCount;
        }
        first = end;
    }

    // Greedy merging; the smaller root survives so polygons are named by their smallest triangle.
    bool hasMerged = true;
    for (usize pass = 0; pass < MAX_MERGE_PASSES && hasMerged == true; ++pass)
    {
        hasMerged = false;
        for (usize i = 0; i < candidates.GetSize(); ++i)
        {
            const MergeCandidate& candidate = candidates[i];
            const uint32 firstRoot = FindRoot(pieces, candidate.FirstTriangle);
            const uint32 secondRoot = FindRoot(pieces, candidate.SecondTriangle);
            if (firstRoot == secondRoot)
            {
                continue;
            }
            Piece merged;
            if (TryMergePieces(positions,
                               pieces[firstRoot],
                               pieces[secondRoot],
                               candidate.Start,
                               candidate.End,
                               merged) == false)
            {
                continue;
            }
            const uint32 root = math::Min(firstRoot, secondRoot);
            const uint32 child = math::Max(firstRoot, secondRoot);
            merged.Parent = root;
            pieces[root] = merged;
            pieces[child].Parent = root;
            pieces[child].IsValid = false;
            hasMerged = true;
        }
    }

    // Emit polygons by root in ascending order, with their triangles grouped by a counting sort.
    Array<usize> polygonOfRoot;
    if (polygonOfRoot.TryResize(triangleCount, triangleCount) == false ||
        result.Triangles.TryResize(triangleCount - result.DegenerateTriangleCount) == false)
    {
        return GeometryStatus::OutOfMemory;
    }
    for (usize t = 0; t < triangleCount; ++t)
    {
        if (pieces[t].IsValid == true && pieces[t].Parent == t)
        {
            const Piece& piece = pieces[t];
            ConvexPolygon polygon;
            polygon.NormalMesh = math::Vector3{static_cast<float32>(piece.Normal.X),
                                               static_cast<float32>(piece.Normal.Y),
                                               static_cast<float32>(piece.Normal.Z)};
            for (usize i = 0; i < piece.LoopCount; ++i)
            {
                bool isReversal = false;
                const float64 sine = ComputeTurnSine(positions[piece.Loop[(i + piece.LoopCount - 1) % piece.LoopCount]],
                                                     positions[piece.Loop[i]],
                                                     positions[piece.Loop[(i + 1) % piece.LoopCount]],
                                                     piece.Normal,
                                                     isReversal);
                if (sine > COLLINEAR_SINE_TOLERANCE && polygon.VertexCount < MAX_CONVEX_POLYGON_VERTICES)
                {
                    polygon.PositionsMesh[polygon.VertexCount] = uniquePositions[piece.Loop[i]];
                    ++polygon.VertexCount;
                }
            }
            polygonOfRoot[t] = result.Polygons.GetSize();
            if (result.Polygons.TryAdd(polygon) == false)
            {
                return GeometryStatus::OutOfMemory;
            }
        }
    }
    for (usize t = 0; t < triangleCount; ++t)
    {
        if (pieces[t].LoopCount != 0)
        {
            ++result.Polygons[polygonOfRoot[FindRoot(pieces, static_cast<uint32>(t))]].TriangleCount;
        }
    }
    usize next = 0;
    for (usize p = 0; p < result.Polygons.GetSize(); ++p)
    {
        result.Polygons[p].FirstTriangle = next;
        next += result.Polygons[p].TriangleCount;
        result.Polygons[p].TriangleCount = 0;
    }
    for (usize t = 0; t < triangleCount; ++t)
    {
        if (pieces[t].LoopCount != 0)
        {
            ConvexPolygon& polygon = result.Polygons[polygonOfRoot[FindRoot(pieces, static_cast<uint32>(t))]];
            result.Triangles[polygon.FirstTriangle + polygon.TriangleCount] = static_cast<uint32>(t);
            ++polygon.TriangleCount;
        }
    }
    outPartition = Move(result);
    return GeometryStatus::Success;
}
} // namespace ludus::foundation::geometry
