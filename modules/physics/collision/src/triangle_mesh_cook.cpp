#include <ludus/physics/collision/triangle_mesh.h>

#include "internal/mesh_arithmetic.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision
{
namespace
{
namespace math = foundation::math;
using foundation::StaticArray;
using internal::GetComponent;
using internal::Widen;

constexpr usize TRIANGLE_CORNER_COUNT = 3;
constexpr uint32 TRIANGLES_PER_LEAF = 8;
// A partition leaving either side below a quarter of the node is replaced by
// a split by count, bounding depth (see MAX_TRIANGLE_MESH_HIERARCHY_DEPTH).
constexpr uint32 MIN_PARTITION_DIVISOR = 4;
constexpr float64 CENTROID_WEIGHT = 1.0 / static_cast<float64>(TRIANGLE_CORNER_COUNT);

/// An edge record packs its cooked triangle index and the edge's slot in that triangle.
constexpr uint32 EDGE_SLOT_BITS = 2;
constexpr uint32 EDGE_SLOT_MASK = (1U << EDGE_SLOT_BITS) - 1U;
/// Sides of a triangle on which a feature is active.
constexpr uint8 NO_SIDE = 0;
constexpr uint8 FRONT_SIDE = 1;
constexpr uint8 BACK_SIDE = 2;
constexpr uint8 BOTH_SIDES = FRONT_SIDE | BACK_SIDE;

/// One triangle edge, filed under the lower of its two source vertex indices.
struct EdgeRecord final
{
    uint32 High = 0;            ///< The higher source vertex index of the edge.
    uint32 TriangleAndEdge = 0; ///< `cooked triangle index << EDGE_SLOT_BITS | edge`.
};

/// Where the flags of one kind of feature start in TriangleFeatureFlags, for each side.
struct FeatureShifts final
{
    uint32 Front = 0;
    uint32 Back = 0;
};

constexpr FeatureShifts EDGE_SHIFTS{ .Front = TRIANGLE_FRONT_EDGE_SHIFT, .Back = TRIANGLE_BACK_EDGE_SHIFT };
constexpr FeatureShifts CORNER_SHIFTS{ .Front = TRIANGLE_FRONT_CORNER_SHIFT, .Back = TRIANGLE_BACK_CORNER_SHIFT };

/// The feature bits of edge or corner `slot`, active on `sides`.
TriangleFeatureFlags MakeFeatureBits(uint8 sides, const FeatureShifts& shifts, uint32 slot) noexcept
{
    uint32 bits = 0;
    if ((sides & FRONT_SIDE) != 0)
    {
        bits |= 1U << (shifts.Front + slot);
    }
    if ((sides & BACK_SIDE) != 0)
    {
        bits |= 1U << (shifts.Back + slot);
    }
    return static_cast<TriangleFeatureFlags>(bits);
}

enum class TriangleCheck : uint8
{
    Admitted,
    Degenerate,
    Failed,
};

void IncludePoint(math::Aabb3& inoutBoundsMesh, math::Vector3 pointMesh) noexcept
{
    inoutBoundsMesh.Min = {math::Min(inoutBoundsMesh.Min.X, pointMesh.X),
                           math::Min(inoutBoundsMesh.Min.Y, pointMesh.Y),
                           math::Min(inoutBoundsMesh.Min.Z, pointMesh.Z)};
    inoutBoundsMesh.Max = {math::Max(inoutBoundsMesh.Max.X, pointMesh.X),
                           math::Max(inoutBoundsMesh.Max.Y, pointMesh.Y),
                           math::Max(inoutBoundsMesh.Max.Z, pointMesh.Z)};
}

Status CheckPosition(math::Vector3 positionMesh) noexcept
{
    if (math::IsFinite(positionMesh) == false)
    {
        return Status::NonFiniteInput;
    }
    return internal::IsWithinMagnitude(Widen(positionMesh), static_cast<float64>(MAX_COORDINATE_METERS)) == true
               ? Status::Success
               : Status::OutOfRange;
}

/// Twice the area against the longest edge squared; scale free, so the same
/// shape is degenerate or not at any admitted size.
bool IsDegenerate(const internal::TriangleCorners& corners) noexcept
{
    const math::Vector3d firstEdgeMesh = Widen(corners.SecondMesh) - Widen(corners.FirstMesh);
    const math::Vector3d secondEdgeMesh = Widen(corners.ThirdMesh) - Widen(corners.FirstMesh);
    const math::Vector3d thirdEdgeMesh = Widen(corners.ThirdMesh) - Widen(corners.SecondMesh);
    const float64 longestEdgeSquared =
        math::Max(math::LengthSquared(firstEdgeMesh),
                  math::Max(math::LengthSquared(secondEdgeMesh), math::LengthSquared(thirdEdgeMesh)));
    const float64 bound = MIN_TRIANGLE_SHAPE_RATIO * longestEdgeSquared;
    return math::LengthSquared(math::Cross(firstEdgeMesh, secondEdgeMesh)) <= bound * bound;
}
} // namespace

/// Validation, copying and hierarchy construction for TriangleMesh::TryCook.
class TriangleMeshBuilder final
{
public:
    static TriangleMeshCookResult
    Cook(const TriangleMeshSource& source, const TriangleMeshCookSettings& settings, TriangleMesh& outMesh) noexcept;

private:
    struct PendingNode final
    {
        uint32 First = 0;
        uint32 Count = 0;
        uint32 Parent = INVALID_TRIANGLE_INDEX; ///< Node whose Right this node fills; INVALID for a left child.
        uint32 Depth = 0;
    };

    static TriangleCheck checkTriangle(const TriangleMeshSource& source,
                                       const TriangleMeshCookSettings& settings,
                                       usize triangle,
                                       Status& outStatus) noexcept;
    static TriangleMeshCookResult validate(const TriangleMeshSource& source,
                                           const TriangleMeshCookSettings& settings,
                                           uint32& outAdmittedCount) noexcept;
    [[nodiscard]] static bool copyTriangles(const TriangleMeshSource& source, TriangleMesh& inoutMesh) noexcept;
    [[nodiscard]] static math::Vector3d getCorner(const TriangleMesh::Triangle& triangle, uint32 corner) noexcept;
    [[nodiscard]] static uint8 classifySharedEdge(const TriangleMeshSource& source,
                                                  const TriangleMesh& mesh,
                                                  const EdgeRecord& first,
                                                  const EdgeRecord& second,
                                                  float64 activeEdgeCosine) noexcept;
    [[nodiscard]] static Status classifyEdges(const TriangleMeshSource& source,
                                              const TriangleMeshCookSettings& settings,
                                              TriangleMesh& inoutMesh) noexcept;
    static uint32
    partition(TriangleMesh& inoutMesh, const math::Aabb3& boundsMesh, uint32 first, uint32 count) noexcept;
    [[nodiscard]] static Status buildHierarchy(TriangleMesh& inoutMesh) noexcept;
};

TriangleCheck TriangleMeshBuilder::checkTriangle(const TriangleMeshSource& source,
                                                 const TriangleMeshCookSettings& settings,
                                                 usize triangle,
                                                 Status& outStatus) noexcept
{
    const usize positionCount = source.PositionsMesh.GetSize();
    StaticArray<math::Vector3, TRIANGLE_CORNER_COUNT> cornersMesh{};
    for (usize corner = 0; corner < TRIANGLE_CORNER_COUNT; ++corner)
    {
        const uint32 index = source.Indices[triangle * TRIANGLE_CORNER_COUNT + corner];
        if (index >= positionCount)
        {
            outStatus = Status::IndexOutOfRange;
            return TriangleCheck::Failed;
        }
        cornersMesh[corner] = source.PositionsMesh[index];
        const Status positionStatus = CheckPosition(cornersMesh[corner]);
        if (positionStatus != Status::Success)
        {
            outStatus = positionStatus;
            return TriangleCheck::Failed;
        }
    }
    if (IsDegenerate(
    {
        .FirstMesh = cornersMesh[0],
        .SecondMesh = cornersMesh[1],
        .ThirdMesh = cornersMesh[2],
    }) == false)
    {
        return TriangleCheck::Admitted;
    }
    if (settings.DegeneratePolicy == DegenerateTrianglePolicy::Skip)
    {
        return TriangleCheck::Degenerate;
    }
    outStatus = Status::DegenerateTriangle;
    return TriangleCheck::Failed;
}

TriangleMeshCookResult TriangleMeshBuilder::validate(const TriangleMeshSource& source,
                                                     const TriangleMeshCookSettings& settings,
                                                     uint32& outAdmittedCount) noexcept
{
    if (source.PositionsMesh.IsValid() == false || source.Indices.IsValid() == false ||
        source.Indices.GetSize() % TRIANGLE_CORNER_COUNT != 0 ||
        (settings.DegeneratePolicy != DegenerateTrianglePolicy::Reject &&
         settings.DegeneratePolicy != DegenerateTrianglePolicy::Skip) ||
        (settings.EdgePolicy != InternalEdgePolicy::Classify && settings.EdgePolicy != InternalEdgePolicy::KeepAll) ||
        math::IsFinite(settings.ActiveEdgeCosine) == false || math::Abs(settings.ActiveEdgeCosine) > 1.0)
    {
        return { .Result = Status::InvalidArgument };
    }
    const usize triangleCount = source.Indices.GetSize() / TRIANGLE_CORNER_COUNT;
    if (triangleCount == 0)
    {
        return { .Result = Status::NoGeometry };
    }
    if (triangleCount > MAX_TRIANGLE_MESH_TRIANGLES)
    {
        return { .Result = Status::CapacityExceeded };
    }
    uint32 admittedCount = 0;
    for (usize triangle = 0; triangle < triangleCount; ++triangle)
    {
        Status status = Status::Success;
        const TriangleCheck check = checkTriangle(source, settings, triangle, status);
        if (check == TriangleCheck::Failed)
        {
            return { .Result = status, .TriangleIndex = static_cast<uint32>(triangle) };
        }
        if (check == TriangleCheck::Admitted)
        {
            ++admittedCount;
        }
    }
    if (admittedCount == 0)
    {
        return { .Result = Status::NoGeometry };
    }
    outAdmittedCount = admittedCount;
    return {};
}

bool TriangleMeshBuilder::copyTriangles(const TriangleMeshSource& source, TriangleMesh& inoutMesh) noexcept
{
    const usize triangleCount = source.Indices.GetSize() / TRIANGLE_CORNER_COUNT;
    for (usize triangle = 0; triangle < triangleCount; ++triangle)
    {
        const usize base = triangle * TRIANGLE_CORNER_COUNT;
        const TriangleMesh::Triangle copy
        {
            .FirstMesh = source.PositionsMesh[source.Indices[base]],
            .SecondMesh = source.PositionsMesh[source.Indices[base + 1]],
            .ThirdMesh = source.PositionsMesh[source.Indices[base + 2]],
            .SourceIndex = static_cast<uint32>(triangle),
        };
        if (IsDegenerate({ .FirstMesh = copy.FirstMesh, .SecondMesh = copy.SecondMesh, .ThirdMesh = copy.ThirdMesh }) ==
            true)
        {
            ++inoutMesh.mSkippedTriangleCount;
            continue;
        }
        // Capacity was reserved for every admitted triangle, so this never allocates.
        if (inoutMesh.mTriangles.TryAdd(copy) == false)
        {
            return false;
        }
    }
    return true;
}

math::Vector3d TriangleMeshBuilder::getCorner(const TriangleMesh::Triangle& triangle, uint32 corner) noexcept
{
    if (corner == 0)
    {
        return Widen(triangle.FirstMesh);
    }
    return Widen(corner == 1 ? triangle.SecondMesh : triangle.ThirdMesh);
}

// The sides on which an edge shared by exactly two triangles is active, seen from the first triangle. Consistently
// wound neighbours traverse the edge in opposite directions and share their front side.
uint8 TriangleMeshBuilder::classifySharedEdge(const TriangleMeshSource& source,
                                              const TriangleMesh& mesh,
                                              const EdgeRecord& first,
                                              const EdgeRecord& second,
                                              float64 activeEdgeCosine) noexcept
{
    const TriangleMesh::Triangle& firstTriangle = mesh.mTriangles[first.TriangleAndEdge >> EDGE_SLOT_BITS];
    const TriangleMesh::Triangle& secondTriangle = mesh.mTriangles[second.TriangleAndEdge >> EDGE_SLOT_BITS];
    const uint32 firstEdge = first.TriangleAndEdge & EDGE_SLOT_MASK;
    const uint32 secondEdge = second.TriangleAndEdge & EDGE_SLOT_MASK;
    const usize firstBase = static_cast<usize>(firstTriangle.SourceIndex) * TRIANGLE_CORNER_COUNT;
    const usize secondBase = static_cast<usize>(secondTriangle.SourceIndex) * TRIANGLE_CORNER_COUNT;
    if (source.Indices[firstBase + firstEdge] == source.Indices[secondBase + secondEdge])
    {
        // Both triangles traverse the edge the same way: their fronts face opposite sides of the surface.
        return BOTH_SIDES;
    }
    const math::Vector3d firstOrigin = getCorner(firstTriangle, 0);
    const math::Vector3d secondOrigin = getCorner(secondTriangle, 0);
    const math::Vector3d firstArea =
        math::Cross(getCorner(firstTriangle, 1) - firstOrigin, getCorner(firstTriangle, 2) - firstOrigin);
    const math::Vector3d secondArea =
        math::Cross(getCorner(secondTriangle, 1) - secondOrigin, getCorner(secondTriangle, 2) - secondOrigin);
    // Admitted triangles pass MIN_TRIANGLE_SHAPE_RATIO, so neither area vector is zero.
    const float64 cosine = math::Dot(firstArea, secondArea) / (math::Length(firstArea) * math::Length(secondArea));
    if (cosine >= activeEdgeCosine)
    {
        return NO_SIDE;
    }
    const uint32 oppositeCorner = (secondEdge + 2) % static_cast<uint32>(TRIANGLE_CORNER_COUNT);
    const float64 oppositeSide =
        math::Dot(getCorner(secondTriangle, oppositeCorner) - getCorner(firstTriangle, firstEdge), firstArea);
    if (oppositeSide < 0.0)
    {
        return FRONT_SIDE; // A ridge: the neighbour falls away behind the first triangle's plane.
    }
    return oppositeSide > 0.0 ? BACK_SIDE : BOTH_SIDES;
}

// Adjacency by source indices, in linear time: edge records are counted and scattered into one bucket per lower
// vertex index, each bucket is sorted by the higher index, and equal neighbours in a bucket share an edge.
Status TriangleMeshBuilder::classifyEdges(const TriangleMeshSource& source,
                                          const TriangleMeshCookSettings& settings,
                                          TriangleMesh& inoutMesh) noexcept
{
    if (settings.EdgePolicy == InternalEdgePolicy::KeepAll)
    {
        return Status::Success; // Triangles are copied with every feature active.
    }
    const usize triangleCount = inoutMesh.mTriangles.GetSize();
    const uint32 cornerCount = static_cast<uint32>(TRIANGLE_CORNER_COUNT);
    usize vertexCount = 0;
    for (const TriangleMesh::Triangle& triangle : inoutMesh.mTriangles)
    {
        const usize base = static_cast<usize>(triangle.SourceIndex) * TRIANGLE_CORNER_COUNT;
        for (usize corner = 0; corner < TRIANGLE_CORNER_COUNT; ++corner)
        {
            vertexCount = math::Max(vertexCount, static_cast<usize>(source.Indices[base + corner]) + 1);
        }
    }
    foundation::Array<uint32> bucketEnds;
    foundation::Array<EdgeRecord> records;
    foundation::Array<uint8> vertexSides;
    if (bucketEnds.TryResize(vertexCount + 1, 0U) == false ||
        records.TryResize(triangleCount * TRIANGLE_CORNER_COUNT, EdgeRecord{}) == false ||
        vertexSides.TryResize(vertexCount, NO_SIDE) == false)
    {
        return Status::AllocationFailure;
    }
    // Count each bucket one slot ahead, then turn the counts into bucket starts.
    for (usize index = 0; index < triangleCount; ++index)
    {
        const usize base = static_cast<usize>(inoutMesh.mTriangles[index].SourceIndex) * TRIANGLE_CORNER_COUNT;
        for (uint32 edge = 0; edge < cornerCount; ++edge)
        {
            const uint32 start = source.Indices[base + edge];
            const uint32 end = source.Indices[base + (edge + 1) % cornerCount];
            ++bucketEnds[static_cast<usize>(math::Min(start, end)) + 1];
        }
        inoutMesh.mTriangles[index].Features = 0;
    }
    for (usize vertex = 0; vertex < vertexCount; ++vertex)
    {
        bucketEnds[vertex + 1] += bucketEnds[vertex];
    }
    // Scatter: each bucket's start advances to its end, which is the next bucket's start.
    for (usize index = 0; index < triangleCount; ++index)
    {
        const usize base = static_cast<usize>(inoutMesh.mTriangles[index].SourceIndex) * TRIANGLE_CORNER_COUNT;
        for (uint32 edge = 0; edge < cornerCount; ++edge)
        {
            const uint32 start = source.Indices[base + edge];
            const uint32 end = source.Indices[base + (edge + 1) % cornerCount];
            records[bucketEnds[math::Min(start, end)]++] =
            {
                .High = math::Max(start, end),
                .TriangleAndEdge = (static_cast<uint32>(index) << EDGE_SLOT_BITS) | edge,
            };
        }
    }
    usize bucketStart = 0;
    for (usize low = 0; low < vertexCount; ++low)
    {
        const usize bucketEnd = bucketEnds[low];
        foundation::Sort(records.GetData() + bucketStart,
                         bucketEnd - bucketStart,
                         [](const EdgeRecord& first, const EdgeRecord& second) {
                             if (first.High != second.High)
                             {
                                 return first.High < second.High;
                             }
                             return first.TriangleAndEdge < second.TriangleAndEdge;
                         });
        usize groupStart = bucketStart;
        while (groupStart < bucketEnd)
        {
            usize groupEnd = groupStart + 1;
            while (groupEnd < bucketEnd && records[groupEnd].High == records[groupStart].High)
            {
                ++groupEnd;
            }
            // One triangle is a boundary and three or more are nonmanifold: both stay active on both sides.
            const uint8 sides = groupEnd - groupStart == 2 ? classifySharedEdge(source,
                                                                                inoutMesh,
                                                                                records[groupStart],
                                                                                records[groupStart + 1],
                                                                                settings.ActiveEdgeCosine)
                                                           : BOTH_SIDES;
            for (usize member = groupStart; member < groupEnd; ++member)
            {
                TriangleMesh::Triangle& triangle =
                    inoutMesh.mTriangles[records[member].TriangleAndEdge >> EDGE_SLOT_BITS];
                triangle.Features |=
                    MakeFeatureBits(sides, EDGE_SHIFTS, records[member].TriangleAndEdge & EDGE_SLOT_MASK);
            }
            vertexSides[low] |= sides;
            vertexSides[records[groupStart].High] |= sides;
            groupStart = groupEnd;
        }
        bucketStart = bucketEnd;
    }
    for (TriangleMesh::Triangle& triangle : inoutMesh.mTriangles)
    {
        const usize base = static_cast<usize>(triangle.SourceIndex) * TRIANGLE_CORNER_COUNT;
        for (uint32 corner = 0; corner < cornerCount; ++corner)
        {
            triangle.Features |= MakeFeatureBits(vertexSides[source.Indices[base + corner]], CORNER_SHIFTS, corner);
        }
    }
    return Status::Success;
}

// The partition, its axis choice and the split-by-count fallback are lifted
// from the Explore as Player preview (apps/editor/src/viewport_walk_collision.cpp).
uint32 TriangleMeshBuilder::partition(TriangleMesh& inoutMesh,
                                      const math::Aabb3& boundsMesh,
                                      uint32 first,
                                      uint32 count) noexcept
{
    const math::Vector3d spanMesh = Widen(boundsMesh.Max) - Widen(boundsMesh.Min);
    const uint32 horizontalAxis = spanMesh.X >= spanMesh.Z ? 0 : internal::Z_AXIS;
    const uint32 axis = spanMesh.Y > math::Max(spanMesh.X, spanMesh.Z) ? 1 : horizontalAxis;
    const float64 pivot = 0.5 * (GetComponent(boundsMesh.Min, axis) + GetComponent(boundsMesh.Max, axis));
    uint32 middle = first;
    for (uint32 index = first; index < first + count; ++index)
    {
        const TriangleMesh::Triangle& triangle = inoutMesh.mTriangles[index];
        const float64 centroid = (GetComponent(triangle.FirstMesh, axis) + GetComponent(triangle.SecondMesh, axis) +
                                  GetComponent(triangle.ThirdMesh, axis)) *
                                 CENTROID_WEIGHT;
        if (centroid < pivot)
        {
            const TriangleMesh::Triangle saved = triangle;
            inoutMesh.mTriangles[index] = inoutMesh.mTriangles[middle];
            inoutMesh.mTriangles[middle] = saved;
            ++middle;
        }
    }
    if (middle - first < count / MIN_PARTITION_DIVISOR || first + count - middle < count / MIN_PARTITION_DIVISOR)
    {
        middle = first + count / 2;
    }
    return middle;
}

// Preorder construction with an explicit stack: a node's left child is the
// next node, its right child is patched in when that child is created. The
// stack holds at most one waiting right sibling per level plus the two
// children just pushed, so MAX_TRIANGLE_MESH_HIERARCHY_DEPTH + 1 entries suffice.
Status TriangleMeshBuilder::buildHierarchy(TriangleMesh& inoutMesh) noexcept
{
    StaticArray<PendingNode, MAX_TRIANGLE_MESH_HIERARCHY_DEPTH + 1> pending{};
    usize pendingCount = 0;
    pending[pendingCount++] = { .First = 0, .Count = static_cast<uint32>(inoutMesh.mTriangles.GetSize()), .Depth = 1 };
    while (pendingCount > 0)
    {
        const PendingNode node = pending[--pendingCount];
        const uint32 nodeIndex = static_cast<uint32>(inoutMesh.mNodes.GetSize());
        if (node.Parent != INVALID_TRIANGLE_INDEX)
        {
            inoutMesh.mNodes[node.Parent].Right = nodeIndex;
        }
        math::Aabb3 boundsMesh = math::EmptyAabb3();
        for (uint32 index = node.First; index < node.First + node.Count; ++index)
        {
            const TriangleMesh::Triangle& triangle = inoutMesh.mTriangles[index];
            IncludePoint(boundsMesh, triangle.FirstMesh);
            IncludePoint(boundsMesh, triangle.SecondMesh);
            IncludePoint(boundsMesh, triangle.ThirdMesh);
        }
        // Capacity was reserved for the node bound, so this never allocates.
        if (inoutMesh.mNodes.TryAdd({ .BoundsMesh = boundsMesh, .First = node.First, .Count = node.Count }) == false)
        {
            return Status::AllocationFailure;
        }
        inoutMesh.mHierarchyDepth = math::Max(inoutMesh.mHierarchyDepth, node.Depth);
        if (node.Count <= TRIANGLES_PER_LEAF)
        {
            continue;
        }
        if (node.Depth >= MAX_TRIANGLE_MESH_HIERARCHY_DEPTH)
        {
            return Status::CapacityExceeded;
        }
        const uint32 middle = partition(inoutMesh, boundsMesh, node.First, node.Count);
        inoutMesh.mNodes[nodeIndex].Count = 0;
        LUDUS_ASSERT(pendingCount + 2 <= pending.GetSize());
        pending[pendingCount++] =
        {
            .First = middle,
            .Count = node.First + node.Count - middle,
            .Parent = nodeIndex,
            .Depth = node.Depth + 1,
        };
        pending[pendingCount++] = { .First = node.First, .Count = middle - node.First, .Depth = node.Depth + 1 };
    }
    return Status::Success;
}

TriangleMeshCookResult TriangleMeshBuilder::Cook(const TriangleMeshSource& source,
                                                 const TriangleMeshCookSettings& settings,
                                                 TriangleMesh& outMesh) noexcept
{
    uint32 admittedCount = 0;
    const TriangleMeshCookResult validation = validate(source, settings, admittedCount);
    if (validation.Result != Status::Success)
    {
        return validation;
    }
    // Leaves split only above TRIANGLES_PER_LEAF and both halves keep at least
    // two triangles, so a mesh never has more nodes than triangles.
    TriangleMesh candidate;
    if (candidate.mTriangles.TryEnsureCapacity(admittedCount) == false ||
        candidate.mNodes.TryEnsureCapacity(admittedCount) == false)
    {
        return { .Result = Status::AllocationFailure };
    }
    if (copyTriangles(source, candidate) == false)
    {
        return { .Result = Status::AllocationFailure };
    }
    // Edges are classified in source order, before the hierarchy reorders the triangles.
    const Status edgeStatus = classifyEdges(source, settings, candidate);
    if (edgeStatus != Status::Success)
    {
        return { .Result = edgeStatus };
    }
    const Status hierarchyStatus = buildHierarchy(candidate);
    if (hierarchyStatus != Status::Success)
    {
        return { .Result = hierarchyStatus };
    }
    outMesh = foundation::Move(candidate);
    return {};
}

Status TriangleMesh::TryGetTriangleFeatures(uint32 sourceTriangleIndex, TriangleFeatureFlags& outFlags) const noexcept
{
    if (IsCooked() == false)
    {
        return Status::NoGeometry;
    }
    for (const Triangle& triangle : mTriangles)
    {
        if (triangle.SourceIndex == sourceTriangleIndex)
        {
            outFlags = triangle.Features;
            return Status::Success;
        }
    }
    return Status::IndexOutOfRange;
}

TriangleMeshCookResult TriangleMesh::TryCook(const TriangleMeshSource& source,
                                             const TriangleMeshCookSettings& settings,
                                             TriangleMesh& outMesh) noexcept
{
    return TriangleMeshBuilder::Cook(source, settings, outMesh);
}
} // namespace ludus::physics::collision
