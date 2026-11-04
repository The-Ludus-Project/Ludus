#include "internal/mesh_silhouette.h"

#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/profiling/profiling.hpp>

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
/// Edges per triangle.
constexpr usize TRIANGLE_EDGES = 3;
/// Bits holding one welded vertex index in an edge key.
constexpr uint64 KEY_VERTEX_BITS = 32;
/// Facing of a triangle whose winding turns towards the viewer, and away from it.
constexpr int8 FRONT_FACING = 1;
constexpr int8 BACK_FACING = -1;
/// Largest welded vertex index an edge key holds.
constexpr uint32 MAXIMUM_VERTEX_INDEX = 0xFFFFFFFFU;

struct EdgeRecord final
{
    uint64 Key = 0;
    uint32 Triangle = 0;
};

bool IsPositionLess(const math::Vector3& left, const math::Vector3& right) noexcept
{
    if (left.X != right.X)
    {
        return left.X < right.X;
    }
    if (left.Y != right.Y)
    {
        return left.Y < right.Y;
    }
    return left.Z < right.Z;
}

bool IsPositionEqual(const math::Vector3& left, const math::Vector3& right) noexcept
{
    return left.X == right.X && left.Y == right.Y && left.Z == right.Z;
}

/// Sign of the homogeneous orientation determinant of three clip positions: positive when the triangle faces
/// the viewer. It equals the triple product of the three points about the eye, so it needs no perspective divide
/// and stays correct for a vertex behind the camera.
int8 ComputeFacing(const math::Vector4& first, const math::Vector4& second, const math::Vector4& third) noexcept
{
    const float32 determinant = first.X * (second.Y * third.W - third.Y * second.W) -
                                first.Y * (second.X * third.W - third.X * second.W) +
                                first.W * (second.X * third.Y - third.X * second.Y);
    return determinant >= 0 ? FRONT_FACING : BACK_FACING;
}
} // namespace

void MeshSilhouette::Clear() noexcept
{
    mTriangles.Clear();
    mEdges.Clear();
    mEdgeTriangles.Clear();
    mVertexCount = 0;
}

bool MeshSilhouette::Build(BorrowedView<const math::Vector3> positionsMesh, BorrowedView<const uint32> indices)
{
    LUDUS_PROFILE_SCOPE(EditorSilhouetteBuild);
    Clear();
    const usize vertexCount = positionsMesh.GetSize();
    const usize triangleCount = indices.GetSize() / TRIANGLE_VERTICES;
    if (positionsMesh.IsValid() == false || indices.IsValid() == false || triangleCount == 0 ||
        triangleCount > MAX_TRIANGLES || vertexCount > static_cast<usize>(MAXIMUM_VERTEX_INDEX))
    {
        return false;
    }
    for (usize vertex = 0; vertex < vertexCount; ++vertex)
    {
        if (math::IsFinite(positionsMesh[vertex]) == false)
        {
            return false;
        }
    }
    // Weld vertices at one position to the first of them, by sorting indices on position.
    Array<uint32> order;
    Array<uint32> welded;
    if (order.TryResize(vertexCount) == false || welded.TryResize(vertexCount) == false)
    {
        return false;
    }
    for (usize vertex = 0; vertex < vertexCount; ++vertex)
    {
        order[vertex] = static_cast<uint32>(vertex);
    }
    Sort(order.GetData(), vertexCount, [positionsMesh](uint32 left, uint32 right) noexcept {
        const math::Vector3& leftPosition = positionsMesh[left];
        const math::Vector3& rightPosition = positionsMesh[right];
        return IsPositionLess(leftPosition, rightPosition) ||
               (IsPositionEqual(leftPosition, rightPosition) == true && left < right);
    });
    for (usize rank = 0; rank < vertexCount; ++rank)
    {
        const bool isFirstOfPosition =
            rank == 0 || IsPositionEqual(positionsMesh[order[rank]], positionsMesh[order[rank - 1]]) == false;
        welded[order[rank]] = isFirstOfPosition == true ? order[rank] : welded[order[rank - 1]];
    }
    Array<EdgeRecord> records;
    if (mTriangles.TryEnsureCapacity(triangleCount) == false ||
        records.TryEnsureCapacity(triangleCount * TRIANGLE_EDGES) == false)
    {
        return false;
    }
    for (usize triangle = 0; triangle < triangleCount; ++triangle)
    {
        const uint32 first = indices[triangle * TRIANGLE_VERTICES];
        const uint32 second = indices[triangle * TRIANGLE_VERTICES + 1];
        const uint32 third = indices[triangle * TRIANGLE_VERTICES + 2];
        if (first >= vertexCount || second >= vertexCount || third >= vertexCount)
        {
            continue;
        }
        const StaticArray<uint32, TRIANGLE_VERTICES> corners{welded[first], welded[second], welded[third]};
        if (corners[0] == corners[1] || corners[1] == corners[2] || corners[0] == corners[2])
        {
            continue;
        }
        const uint32 triangleIndex = static_cast<uint32>(mTriangles.GetSize());
        mTriangles.Add({first, second, third});
        for (usize edge = 0; edge < TRIANGLE_EDGES; ++edge)
        {
            const uint32 start = corners[edge];
            const uint32 end = corners[(edge + 1) % TRIANGLE_EDGES];
            const uint32 low = math::Min(start, end);
            const uint32 high = math::Max(start, end);
            records.Add({(static_cast<uint64>(low) << KEY_VERTEX_BITS) | high, triangleIndex});
        }
    }
    if (mTriangles.IsEmpty() == true)
    {
        Clear();
        return false;
    }
    Sort(records.GetData(), records.GetSize(), [](const EdgeRecord& left, const EdgeRecord& right) noexcept {
        return left.Key < right.Key || (left.Key == right.Key && left.Triangle < right.Triangle);
    });
    if (mEdges.TryEnsureCapacity(records.GetSize()) == false || mEdgeTriangles.TryResize(records.GetSize()) == false)
    {
        Clear();
        return false;
    }
    for (usize record = 0; record < records.GetSize(); ++record)
    {
        mEdgeTriangles[record] = records[record].Triangle;
        if (record > 0 && records[record].Key == records[record - 1].Key)
        {
            ++mEdges.GetLast().TriangleCount;
            continue;
        }
        const uint64 key = records[record].Key;
        mEdges.Add({static_cast<uint32>(key >> KEY_VERTEX_BITS),
                    static_cast<uint32>(key & MAXIMUM_VERTEX_INDEX),
                    static_cast<uint32>(record),
                    1});
    }
    mVertexCount = vertexCount;
    return true;
}

bool MeshSilhouette::Extract(BorrowedView<const math::Vector3> positionsMesh,
                             const math::Matrix4& meshToClip,
                             usize maximumSegments,
                             Array<SilhouetteSegment>& outSegments)
{
    LUDUS_PROFILE_SCOPE(EditorSilhouetteExtract);
    if (mEdges.IsEmpty() == true || positionsMesh.GetSize() != mVertexCount || positionsMesh.IsValid() == false ||
        mClip.TryResize(mVertexCount) == false || mFacing.TryResize(mTriangles.GetSize()) == false)
    {
        return false;
    }
    for (usize vertex = 0; vertex < mVertexCount; ++vertex)
    {
        const math::Vector3& position = positionsMesh[vertex];
        mClip[vertex] = meshToClip * math::Vector4{position.X, position.Y, position.Z, 1};
    }
    for (usize triangle = 0; triangle < mTriangles.GetSize(); ++triangle)
    {
        const Triangle& corners = mTriangles[triangle];
        mFacing[triangle] = ComputeFacing(mClip[corners.First], mClip[corners.Second], mClip[corners.Third]);
    }
    for (const Edge& edge : mEdges)
    {
        if (outSegments.GetSize() >= maximumSegments)
        {
            break;
        }
        bool isFront = false;
        bool isBack = false;
        for (uint32 member = 0; member < edge.TriangleCount; ++member)
        {
            const int8 facing = mFacing[mEdgeTriangles[edge.FirstTriangle + member]];
            isFront = isFront == true || facing == FRONT_FACING;
            isBack = isBack == true || facing == BACK_FACING;
        }
        const bool isBoundary = edge.TriangleCount == 1;
        if ((isBoundary == true || (isFront == true && isBack == true)) &&
            outSegments.TryAdd({mClip[edge.First], mClip[edge.Second]}) == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace ludus::editor
