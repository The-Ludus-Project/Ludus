#include "internal/gjk_simplex.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision::internal
{
namespace
{
using foundation::StaticArray;

constexpr usize SEGMENT_VERTICES = 2;
constexpr usize TRIANGLE_VERTICES = 3;
constexpr usize TETRAHEDRON_FACES = 4;
/// Slot of a tetrahedron face's opposite vertex in each face row below.
constexpr usize OPPOSITE_VERTEX = 3;

using Weights = StaticArray<float64, MAX_SIMPLEX_VERTICES>;

/// Parameter t in [0, 1] of the point `first + t * (second - first)` closest to the origin. A zero-length segment
/// is its first point.
float64 FindSegmentParameter(math::Vector3d first, math::Vector3d second) noexcept
{
    const math::Vector3d edge = second - first;
    const float64 lengthSquared = math::Dot(edge, edge);
    return lengthSquared > 0.0 ? math::Clamp(-math::Dot(first, edge) / lengthSquared, 0.0, 1.0) : 0.0;
}

/// Barycentric weights of the point of triangle (first, second, third) closest to the origin, found by Voronoi
/// regions (Ericson, section 5.1.5). Edge regions use the clamped segment projection, and a triangle with no area
/// falls back to its nearest edge, so no region divides by zero.
void FindTriangleWeights(math::Vector3d first,
                         math::Vector3d second,
                         math::Vector3d third,
                         StaticArray<float64, TRIANGLE_VERTICES>& outWeights) noexcept
{
    outWeights = {0.0, 0.0, 0.0};
    const math::Vector3d firstEdge = second - first;
    const math::Vector3d secondEdge = third - first;
    const float64 d1 = -math::Dot(firstEdge, first);
    const float64 d2 = -math::Dot(secondEdge, first);
    if (d1 <= 0.0 && d2 <= 0.0)
    {
        outWeights[0] = 1.0;
        return;
    }
    const float64 d3 = -math::Dot(firstEdge, second);
    const float64 d4 = -math::Dot(secondEdge, second);
    if (d3 >= 0.0 && d4 <= d3)
    {
        outWeights[1] = 1.0;
        return;
    }
    const float64 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        const float64 t = FindSegmentParameter(first, second);
        outWeights[0] = 1.0 - t;
        outWeights[1] = t;
        return;
    }
    const float64 d5 = -math::Dot(firstEdge, third);
    const float64 d6 = -math::Dot(secondEdge, third);
    if (d6 >= 0.0 && d5 <= d6)
    {
        outWeights[2] = 1.0;
        return;
    }
    const float64 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        const float64 t = FindSegmentParameter(first, third);
        outWeights[0] = 1.0 - t;
        outWeights[2] = t;
        return;
    }
    const float64 va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0)
    {
        const float64 t = FindSegmentParameter(second, third);
        outWeights[1] = 1.0 - t;
        outWeights[2] = t;
        return;
    }
    const float64 denominator = va + vb + vc;
    if (denominator > 0.0 && math::IsFinite(denominator) == true)
    {
        outWeights[1] = vb / denominator;
        outWeights[2] = vc / denominator;
        outWeights[0] = 1.0 - outWeights[1] - outWeights[2];
        return;
    }
    // No area: the nearest of the three edges.
    StaticArray<float64, TRIANGLE_VERTICES> edgeWeights{};
    float64 bestDistanceSquared = 0.0;
    bool hasBest = false;
    const StaticArray<StaticArray<usize, SEGMENT_VERTICES>, TRIANGLE_VERTICES> edges{
        StaticArray<usize, SEGMENT_VERTICES>{0, 1},
        StaticArray<usize, SEGMENT_VERTICES>{0, 2},
        StaticArray<usize, SEGMENT_VERTICES>{1, 2}};
    const StaticArray<math::Vector3d, TRIANGLE_VERTICES> corners{first, second, third};
    for (const StaticArray<usize, SEGMENT_VERTICES>& edge : edges)
    {
        const float64 t = FindSegmentParameter(corners[edge[0]], corners[edge[1]]);
        const float64 startWeight = 1.0 - t;
        const float64 endWeight = t;
        const math::Vector3d point = corners[edge[0]] * startWeight + corners[edge[1]] * endWeight;
        const float64 distanceSquared = math::Dot(point, point);
        if (hasBest == false || distanceSquared < bestDistanceSquared)
        {
            hasBest = true;
            bestDistanceSquared = distanceSquared;
            edgeWeights = {0.0, 0.0, 0.0};
            edgeWeights[edge[0]] = startWeight;
            edgeWeights[edge[1]] = endWeight;
        }
    }
    outWeights = edgeWeights;
}

/// Whether the origin lies strictly on the far side of a tetrahedron face from its opposite vertex. `face` holds the
/// three face corners and then the opposite vertex. A flat tetrahedron (opposite in the face plane) counts every face
/// as outside, so the reduction falls back to faces.
bool IsOriginOutsideFace(const StaticArray<math::Vector3d, MAX_SIMPLEX_VERTICES>& points,
                         const StaticArray<usize, MAX_SIMPLEX_VERTICES>& face) noexcept
{
    const math::Vector3d first = points[face[0]];
    const math::Vector3d normal = math::Cross(points[face[1]] - first, points[face[2]] - first);
    const float64 originSide = -math::Dot(first, normal);
    const float64 oppositeSide = math::Dot(points[face[OPPOSITE_VERTEX]] - first, normal);
    return oppositeSide == 0.0 || (originSide > 0.0 && oppositeSide < 0.0) || (originSide < 0.0 && oppositeSide > 0.0);
}

/// Barycentric weights of the closest point to the origin; false when a full tetrahedron encloses the origin.
bool FindWeights(const StaticArray<math::Vector3d, MAX_SIMPLEX_VERTICES>& points,
                 usize count,
                 Weights& outWeights) noexcept
{
    outWeights = {0.0, 0.0, 0.0, 0.0};
    if (count == 1)
    {
        outWeights[0] = 1.0;
        return true;
    }
    if (count == SEGMENT_VERTICES)
    {
        const float64 t = FindSegmentParameter(points[0], points[1]);
        outWeights[0] = 1.0 - t;
        outWeights[1] = t;
        return true;
    }
    StaticArray<float64, TRIANGLE_VERTICES> triangleWeights{};
    if (count == TRIANGLE_VERTICES)
    {
        FindTriangleWeights(points[0], points[1], points[2], triangleWeights);
        outWeights = {triangleWeights[0], triangleWeights[1], triangleWeights[2], 0.0};
        return true;
    }
    // Tetrahedron (Ericson, section 5.1.6): the nearest face the origin lies outside of.
    const StaticArray<StaticArray<usize, MAX_SIMPLEX_VERTICES>, TETRAHEDRON_FACES> faces{
        StaticArray<usize, MAX_SIMPLEX_VERTICES>{0, 1, 2, 3},
        StaticArray<usize, MAX_SIMPLEX_VERTICES>{0, 2, 3, 1},
        StaticArray<usize, MAX_SIMPLEX_VERTICES>{0, 3, 1, 2},
        StaticArray<usize, MAX_SIMPLEX_VERTICES>{1, 3, 2, 0}};
    bool hasOutsideFace = false;
    float64 bestDistanceSquared = 0.0;
    for (const StaticArray<usize, MAX_SIMPLEX_VERTICES>& face : faces)
    {
        if (IsOriginOutsideFace(points, face) == false)
        {
            continue;
        }
        FindTriangleWeights(points[face[0]], points[face[1]], points[face[2]], triangleWeights);
        const math::Vector3d point = points[face[0]] * triangleWeights[0] + points[face[1]] * triangleWeights[1] +
                                     points[face[2]] * triangleWeights[2];
        const float64 distanceSquared = math::Dot(point, point);
        if (hasOutsideFace == false || distanceSquared < bestDistanceSquared)
        {
            hasOutsideFace = true;
            bestDistanceSquared = distanceSquared;
            outWeights = {0.0, 0.0, 0.0, 0.0};
            outWeights[face[0]] = triangleWeights[0];
            outWeights[face[1]] = triangleWeights[1];
            outWeights[face[2]] = triangleWeights[2];
        }
    }
    return hasOutsideFace;
}
} // namespace

void ReduceToClosest(math::Vector3d rayPointMesh, Simplex& inoutSimplex, SimplexClosest& outClosest) noexcept
{
    LUDUS_ASSERT(inoutSimplex.Count >= 1 && inoutSimplex.Count <= MAX_SIMPLEX_VERTICES);
    StaticArray<math::Vector3d, MAX_SIMPLEX_VERTICES> points{};
    for (usize index = 0; index < inoutSimplex.Count; ++index)
    {
        points[index] = rayPointMesh - inoutSimplex.Vertices[index].DifferenceMesh;
    }
    Weights weights{};
    if (FindWeights(points, inoutSimplex.Count, weights) == false)
    {
        outClosest = { .PointMesh = {}, .WitnessMesh = {}, .HasEnclosedOrigin = true };
        return;
    }
    SimplexClosest closest;
    Simplex reduced;
    for (usize index = 0; index < inoutSimplex.Count; ++index)
    {
        if (weights[index] > 0.0)
        {
            closest.PointMesh = closest.PointMesh + points[index] * weights[index];
            closest.WitnessMesh = closest.WitnessMesh + inoutSimplex.Vertices[index].TargetMesh * weights[index];
            reduced.Vertices[reduced.Count++] = inoutSimplex.Vertices[index];
        }
    }
    inoutSimplex = reduced;
    outClosest = closest;
}
} // namespace ludus::physics::collision::internal
