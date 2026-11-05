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

/// Smallest clip w treated as in front of the camera, matching the overlay's own projection bound.
constexpr float32 MINIMUM_FRONT_W = 0.000001F;
/// Centre of a coverage pixel, as a fraction of its width.
constexpr float32 PIXEL_CENTRE = 0.5F;
/// Distance, in coverage pixels, from an edge's midpoint to the points tested on its two sides.
constexpr float32 SIDE_SAMPLE_PIXELS = 1.5F;
/// Edges shorter than this, in coverage pixels, have no usable side direction and are kept.
constexpr float32 MINIMUM_EDGE_PIXELS = 0.001F;
/// Coverage value of a pixel under a front-facing triangle.
constexpr uint8 COVERED = 255;

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

bool MeshSilhouette::buildCoverage(uint32 frameWidthPixels, uint32 frameHeightPixels)
{
    LUDUS_PROFILE_SCOPE(EditorSilhouetteCoverage);
    mCoverageWidth = math::Max(uint32{1}, frameWidthPixels / COVERAGE_DOWNSCALE);
    mCoverageHeight = math::Max(uint32{1}, frameHeightPixels / COVERAGE_DOWNSCALE);
    const usize pixelCount = static_cast<usize>(mCoverageWidth) * static_cast<usize>(mCoverageHeight);
    if (mCoverage.TryResize(pixelCount) == false)
    {
        mCoverageWidth = 0;
        mCoverageHeight = 0;
        return false;
    }
    for (uint8& pixel : mCoverage)
    {
        pixel = 0;
    }
    const float32 width = static_cast<float32>(mCoverageWidth);
    const float32 height = static_cast<float32>(mCoverageHeight);
    const float64 lastColumnValue = static_cast<float64>(mCoverageWidth) - 1;
    const float64 lastRowValue = static_cast<float64>(mCoverageHeight) - 1;
    for (usize triangle = 0; triangle < mTriangles.GetSize(); ++triangle)
    {
        if (mFacing[triangle] != FRONT_FACING)
        {
            continue;
        }
        const Triangle& corners = mTriangles[triangle];
        const StaticArray<math::Vector4, TRIANGLE_VERTICES> clip{mClip[corners.First],
                                                                 mClip[corners.Second],
                                                                 mClip[corners.Third]};
        if (clip[0].W <= MINIMUM_FRONT_W || clip[1].W <= MINIMUM_FRONT_W || clip[2].W <= MINIMUM_FRONT_W)
        {
            continue;
        }
        StaticArray<float32, TRIANGLE_VERTICES> xs{};
        StaticArray<float32, TRIANGLE_VERTICES> ys{};
        for (usize corner = 0; corner < TRIANGLE_VERTICES; ++corner)
        {
            xs[corner] = (clip[corner].X / clip[corner].W * PIXEL_CENTRE + PIXEL_CENTRE) * width;
            ys[corner] = (PIXEL_CENTRE - clip[corner].Y / clip[corner].W * PIXEL_CENTRE) * height;
        }
        const float32 minimumY = math::Min(ys[0], math::Min(ys[1], ys[2]));
        const float32 maximumY = math::Max(ys[0], math::Max(ys[1], ys[2]));
        if (math::IsFinite(minimumY) == false || math::IsFinite(maximumY) == false || maximumY < 0 || minimumY > height)
        {
            continue;
        }
        const int32 firstRow = static_cast<int32>(
            math::Clamp(math::Ceil(static_cast<float64>(minimumY - PIXEL_CENTRE)), 0.0, lastRowValue));
        const int32 finalRow = static_cast<int32>(
            math::Clamp(math::Floor(static_cast<float64>(maximumY - PIXEL_CENTRE)), -1.0, lastRowValue));
        for (int32 row = firstRow; row <= finalRow; ++row)
        {
            const float32 centreY = static_cast<float32>(row) + PIXEL_CENTRE;
            float32 minimumX = width;
            float32 maximumX = 0;
            bool hasSpan = false;
            for (usize edge = 0; edge < TRIANGLE_EDGES; ++edge)
            {
                const usize next = (edge + 1) % TRIANGLE_EDGES;
                const float32 startY = ys[edge];
                const float32 endY = ys[next];
                if ((startY <= centreY) == (endY <= centreY))
                {
                    continue;
                }
                const float32 x = xs[edge] + (centreY - startY) * (xs[next] - xs[edge]) / (endY - startY);
                minimumX = hasSpan == true ? math::Min(minimumX, x) : x;
                maximumX = hasSpan == true ? math::Max(maximumX, x) : x;
                hasSpan = true;
            }
            if (hasSpan == false)
            {
                continue;
            }
            if (math::IsFinite(minimumX) == false || math::IsFinite(maximumX) == false)
            {
                continue;
            }
            const int32 firstColumn = static_cast<int32>(
                math::Clamp(math::Ceil(static_cast<float64>(minimumX - PIXEL_CENTRE)), 0.0, lastColumnValue));
            const int32 finalColumn = static_cast<int32>(
                math::Clamp(math::Floor(static_cast<float64>(maximumX - PIXEL_CENTRE)), -1.0, lastColumnValue));
            uint8* rowPixels = mCoverage.GetData() + static_cast<usize>(row) * mCoverageWidth;
            for (int32 column = firstColumn; column <= finalColumn; ++column)
            {
                rowPixels[column] = COVERED;
            }
        }
    }
    return true;
}

bool MeshSilhouette::isCovered(float32 xCoverage, float32 yCoverage) const noexcept
{
    if (xCoverage < 0 || yCoverage < 0)
    {
        return false;
    }
    const usize column = static_cast<usize>(xCoverage);
    const usize row = static_cast<usize>(yCoverage);
    return column < mCoverageWidth && row < mCoverageHeight && mCoverage[row * mCoverageWidth + column] == COVERED;
}

bool MeshSilhouette::isInside(const SilhouetteSegment& segment) const noexcept
{
    if (segment.FirstClip.W <= MINIMUM_FRONT_W || segment.SecondClip.W <= MINIMUM_FRONT_W)
    {
        return false;
    }
    const float32 width = static_cast<float32>(mCoverageWidth);
    const float32 height = static_cast<float32>(mCoverageHeight);
    const float32 firstX = (segment.FirstClip.X / segment.FirstClip.W * PIXEL_CENTRE + PIXEL_CENTRE) * width;
    const float32 firstY = (PIXEL_CENTRE - segment.FirstClip.Y / segment.FirstClip.W * PIXEL_CENTRE) * height;
    const float32 secondX = (segment.SecondClip.X / segment.SecondClip.W * PIXEL_CENTRE + PIXEL_CENTRE) * width;
    const float32 secondY = (PIXEL_CENTRE - segment.SecondClip.Y / segment.SecondClip.W * PIXEL_CENTRE) * height;
    const float32 directionX = secondX - firstX;
    const float32 directionY = secondY - firstY;
    const float32 length = math::Sqrt(directionX * directionX + directionY * directionY);
    if (math::IsFinite(length) == false || length < MINIMUM_EDGE_PIXELS)
    {
        return false;
    }
    const float32 sideX = -directionY / length * SIDE_SAMPLE_PIXELS;
    const float32 sideY = directionX / length * SIDE_SAMPLE_PIXELS;
    const float32 middleX = (firstX + secondX) * PIXEL_CENTRE;
    const float32 middleY = (firstY + secondY) * PIXEL_CENTRE;
    return isCovered(middleX + sideX, middleY + sideY) == true && isCovered(middleX - sideX, middleY - sideY) == true;
}

bool MeshSilhouette::Extract(BorrowedView<const math::Vector3> positionsMesh,
                             const math::Matrix4& meshToClip,
                             usize maximumSegments,
                             uint32 frameWidthPixels,
                             uint32 frameHeightPixels,
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
    const bool hasCoverage =
        frameWidthPixels > 0 && frameHeightPixels > 0 && buildCoverage(frameWidthPixels, frameHeightPixels) == true;
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
        if (isBoundary == false && (isFront == false || isBack == false))
        {
            continue;
        }
        const SilhouetteSegment segment{mClip[edge.First], mClip[edge.Second]};
        if (hasCoverage == true && isInside(segment) == true)
        {
            continue;
        }
        if (outSegments.TryAdd(segment) == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace ludus::editor
