// Shapes of a contact pair: supports, sizes, closest-point helpers and the support features that are clipped into
// contact points.
//
// Thanks to Christer Ericson, "Real-Time Collision Detection", Morgan Kaufmann, 2005, section 5.1.9 ("Closest
// Points of Two Line Segments"), for the clamped two-parameter solve of FindClosestSegmentPoints, and section 5.1.5
// for the closest point on a triangle, which is evaluated here through the GJK simplex reduction of gjk_simplex.h.
// The support polygon of a hull is found with the gift wrapping of R. A. Jarvis, "On the identification of the
// convex hull of a finite set of points in the plane", Information Processing Letters 2(1), 1973, pages 18-21,
// https://doi.org/10.1016/0020-0190(73)90020-3: hulls store points and no faces, so a face is recovered as the
// planar hull of the points on the support plane.

#include "internal/contact_generation.h"
#include "internal/gjk_simplex.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision::internal
{
namespace
{
using foundation::StaticArray;

constexpr uint32 BOX_FACE_CORNERS = 4;
constexpr uint32 BOX_EDGES_PER_AXIS = 4;
constexpr uint32 HULL_INDEX_BITS = 8;
/// A hull's feature tolerance never exceeds this fraction of its size, so the feature stays close to a real face.
constexpr float64 HULL_FEATURE_EXTENT_RATIO = 0.05;
/// Coordinates of the pair frame carry float64 rounding of about 2^-52 of their size; this share of them keeps
/// every tolerance far above that without inflating it for small shapes far from the frame origin.
constexpr float64 COORDINATE_SCALE_SHARE = 0x1p-10;
/// Two planar directions are collinear when their cross product is below this fraction of their lengths' product.
constexpr float64 COLLINEAR_RATIO = 1.0e-9;

math::Vector3d GetAxis(const PosedConvex& convex, uint32 axis) noexcept
{
    if (axis == 0)
    {
        return convex.AxisXMesh;
    }
    return axis == 1 ? convex.AxisYMesh : convex.AxisZMesh;
}

/// The box corner whose sign bits are `bits`: bit `i` set selects the positive side of axis `i`.
math::Vector3d GetBoxCorner(const ContactShape& shape, uint32 bits) noexcept
{
    math::Vector3d corner = shape.Position;
    for (uint32 axis = 0; axis < AXIS_COUNT; ++axis)
    {
        const float64 halfExtent = GetComponent(shape.Convex.HalfExtentsShape, axis);
        corner = corner + GetAxis(shape.Convex, axis) * (((bits >> axis) & 1U) != 0 ? halfExtent : -halfExtent);
    }
    return corner;
}

/// The feature of the box edge joining two corners that differ in exactly one sign bit.
ContactFeature GetBoxEdgeFeature(uint32 firstBits, uint32 secondBits) noexcept
{
    const uint32 difference = firstBits ^ secondBits;
    uint32 axis = 0;
    while (axis + 1 < AXIS_COUNT && ((difference >> axis) & 1U) == 0)
    {
        ++axis;
    }
    const uint32 nextSide = (firstBits >> ((axis + 1) % AXIS_COUNT)) & 1U;
    const uint32 secondNextSide = (firstBits >> ((axis + 2) % AXIS_COUNT)) & 1U;
    return MakeContactFeature(ContactFeatureKind::Edge, axis * BOX_EDGES_PER_AXIS + nextSide + 2 * secondNextSide);
}

void FindBoxFeature(const ContactShape& shape,
                    math::Vector3d direction,
                    float64 tolerance,
                    FeaturePolygon& outFeature) noexcept
{
    // An axis is free when both of its sides lie within the tolerance of the support plane.
    StaticArray<float64, AXIS_COUNT> alignment{};
    uint32 freeMask = 0;
    uint32 freeCount = 0;
    uint32 supportBits = 0;
    uint32 mostAligned = 0;
    for (uint32 axis = 0; axis < AXIS_COUNT; ++axis)
    {
        alignment[axis] = math::Dot(direction, GetAxis(shape.Convex, axis));
        if (2.0 * GetComponent(shape.Convex.HalfExtentsShape, axis) * math::Abs(alignment[axis]) <= tolerance)
        {
            freeMask |= 1U << axis;
            ++freeCount;
        }
        if (alignment[axis] >= 0.0)
        {
            supportBits |= 1U << axis;
        }
        if (math::Abs(alignment[axis]) > math::Abs(alignment[mostAligned]))
        {
            mostAligned = axis;
        }
    }
    if (freeCount == AXIS_COUNT)
    {
        // The whole box is within the tolerance: keep the face that faces the direction most.
        freeMask &= ~(1U << mostAligned);
        freeCount = 2;
    }
    if (freeCount == 0)
    {
        outFeature.Count = 1;
        outFeature.Vertices[0] = GetBoxCorner(shape, supportBits);
        outFeature.VertexFeatures[0] = MakeContactFeature(ContactFeatureKind::Vertex, supportBits);
        outFeature.EdgeFeatures[0] = outFeature.VertexFeatures[0];
        return;
    }
    if (freeCount == 1)
    {
        const uint32 lowBits = supportBits & ~freeMask;
        const uint32 highBits = supportBits | freeMask;
        outFeature.Count = 2;
        outFeature.Vertices[0] = GetBoxCorner(shape, lowBits);
        outFeature.Vertices[1] = GetBoxCorner(shape, highBits);
        outFeature.VertexFeatures[0] = MakeContactFeature(ContactFeatureKind::Vertex, lowBits);
        outFeature.VertexFeatures[1] = MakeContactFeature(ContactFeatureKind::Vertex, highBits);
        outFeature.EdgeFeatures[0] = GetBoxEdgeFeature(lowBits, highBits);
        outFeature.EdgeFeatures[1] = outFeature.EdgeFeatures[0];
        return;
    }
    uint32 faceAxis = 0;
    while (faceAxis + 1 < AXIS_COUNT && ((freeMask >> faceAxis) & 1U) != 0)
    {
        ++faceAxis;
    }
    const bool isPositive = alignment[faceAxis] >= 0.0;
    const uint32 firstAxis = (faceAxis + 1) % AXIS_COUNT;
    const uint32 secondAxis = (faceAxis + 2) % AXIS_COUNT;
    // Counter-clockwise about +faceAxis; the negative face reverses the order.
    const StaticArray<uint32, BOX_FACE_CORNERS> firstSides{0U, 1U, 1U, 0U};
    const StaticArray<uint32, BOX_FACE_CORNERS> secondSides{0U, 0U, 1U, 1U};
    StaticArray<uint32, BOX_FACE_CORNERS> cornerBits{};
    for (uint32 corner = 0; corner < BOX_FACE_CORNERS; ++corner)
    {
        const uint32 source = isPositive == true ? corner : BOX_FACE_CORNERS - 1 - corner;
        cornerBits[corner] = (isPositive == true ? 1U << faceAxis : 0U) | (firstSides[source] << firstAxis) |
                             (secondSides[source] << secondAxis);
    }
    outFeature.Count = BOX_FACE_CORNERS;
    for (uint32 corner = 0; corner < BOX_FACE_CORNERS; ++corner)
    {
        outFeature.Vertices[corner] = GetBoxCorner(shape, cornerBits[corner]);
        outFeature.VertexFeatures[corner] = MakeContactFeature(ContactFeatureKind::Vertex, cornerBits[corner]);
        outFeature.EdgeFeatures[corner] =
            GetBoxEdgeFeature(cornerBits[corner], cornerBits[(corner + 1) % BOX_FACE_CORNERS]);
    }
    outFeature.FaceFeature =
        MakeContactFeature(ContactFeatureKind::Face, faceAxis * 2 + (isPositive == true ? 1U : 0U));
    outFeature.Normal = isPositive == true ? GetAxis(shape.Convex, faceAxis) : -GetAxis(shape.Convex, faceAxis);
}

void FindCapsuleFeature(const ContactShape& shape,
                        math::Vector3d direction,
                        float64 tolerance,
                        FeaturePolygon& outFeature) noexcept
{
    const math::Vector3d axis = shape.Convex.AxisYMesh;
    const float64 halfHeight = shape.Convex.HalfHeightMeters;
    const float64 alignment = math::Dot(direction, axis);
    const ContactFeature lowFeature = MakeContactFeature(ContactFeatureKind::Vertex, 0);
    const ContactFeature highFeature = MakeContactFeature(ContactFeatureKind::Vertex, 1);
    if (halfHeight > 0.0 && 2.0 * halfHeight * math::Abs(alignment) <= tolerance)
    {
        outFeature.Count = 2;
        outFeature.Vertices[0] = shape.Position - axis * halfHeight;
        outFeature.Vertices[1] = shape.Position + axis * halfHeight;
        outFeature.VertexFeatures[0] = lowFeature;
        outFeature.VertexFeatures[1] = highFeature;
        outFeature.EdgeFeatures[0] = MakeContactFeature(ContactFeatureKind::Edge, 0);
        outFeature.EdgeFeatures[1] = outFeature.EdgeFeatures[0];
        return;
    }
    const bool isHigh = alignment >= 0.0;
    outFeature.Count = 1;
    outFeature.Vertices[0] = shape.Position + axis * (isHigh == true ? halfHeight : -halfHeight);
    outFeature.VertexFeatures[0] = isHigh == true ? highFeature : lowFeature;
    outFeature.EdgeFeatures[0] = outFeature.VertexFeatures[0];
}

/// The triangle edge joining two distinct corners: edge `i` leaves corner `i`.
uint32 GetTriangleEdge(uint32 firstCorner, uint32 secondCorner) noexcept
{
    const uint32 low = math::Min(firstCorner, secondCorner);
    const uint32 high = math::Max(firstCorner, secondCorner);
    if (low == 0)
    {
        return high == 1 ? 0U : 2U;
    }
    return 1;
}

void FindTriangleFeature(const ContactShape& shape,
                         math::Vector3d direction,
                         float64 tolerance,
                         FeaturePolygon& outFeature) noexcept
{
    StaticArray<float64, CONTACT_TRIANGLE_CORNERS> projections{};
    float64 farthest = math::Dot(shape.Corners[0], direction);
    for (usize corner = 0; corner < CONTACT_TRIANGLE_CORNERS; ++corner)
    {
        projections[corner] = math::Dot(shape.Corners[corner], direction);
        farthest = math::Max(farthest, projections[corner]);
    }
    StaticArray<uint32, CONTACT_TRIANGLE_CORNERS> supporting{};
    uint32 supportingCount = 0;
    for (uint32 corner = 0; corner < CONTACT_TRIANGLE_CORNERS; ++corner)
    {
        if (farthest - projections[corner] <= tolerance)
        {
            supporting[supportingCount++] = corner;
        }
    }
    if (supportingCount == CONTACT_TRIANGLE_CORNERS && math::Dot(shape.TriangleNormal, direction) < 0.0)
    {
        // The back of the sheet faces the direction: wind the face the other way.
        supporting = {0U, 2U, 1U};
    }
    outFeature.Count = supportingCount;
    for (uint32 index = 0; index < supportingCount; ++index)
    {
        const uint32 corner = supporting[index];
        const uint32 next = supporting[(index + 1) % supportingCount];
        outFeature.Vertices[index] = shape.Corners[corner];
        outFeature.VertexFeatures[index] = MakeContactFeature(ContactFeatureKind::Vertex, corner);
        outFeature.EdgeFeatures[index] =
            supportingCount == 1 ? outFeature.VertexFeatures[index]
                                 : MakeContactFeature(ContactFeatureKind::Edge, GetTriangleEdge(corner, next));
    }
    if (supportingCount == CONTACT_TRIANGLE_CORNERS)
    {
        outFeature.FaceFeature = MakeContactFeature(ContactFeatureKind::Face, 0);
        outFeature.Normal =
            math::Dot(shape.TriangleNormal, direction) < 0.0 ? -shape.TriangleNormal : shape.TriangleNormal;
    }
}

/// Planar coordinates of the hull points on a support plane, with the point indices they belong to.
struct PlanarPoints final
{
    StaticArray<uint16, MAX_CONVEX_HULL_POINTS> Indices{};
    StaticArray<float64, MAX_CONVEX_HULL_POINTS> First{};
    StaticArray<float64, MAX_CONVEX_HULL_POINTS> Second{};
    uint32 Count = 0;
};

/// @brief Wrap the planar points counter-clockwise, starting from the lowest one, and return the hull's corners as
/// positions in `points`.
///
/// Coincident points keep the lowest index and collinear points keep the farthest, so a set on one line yields its
/// two ends and a single location yields one corner.
uint32 WrapPlanarHull(const PlanarPoints& points, StaticArray<uint16, MAX_CONVEX_HULL_POINTS>& outCorners) noexcept
{
    uint32 start = 0;
    for (uint32 index = 1; index < points.Count; ++index)
    {
        if (points.First[index] < points.First[start] ||
            (points.First[index] == points.First[start] && points.Second[index] < points.Second[start]))
        {
            start = index;
        }
    }
    uint32 cornerCount = 0;
    uint32 current = start;
    // A hull has at most as many corners as points; the bound also stops a wrap that rounding keeps from closing.
    while (cornerCount < points.Count)
    {
        outCorners[cornerCount++] = static_cast<uint16>(current);
        bool hasNext = false;
        uint32 next = current;
        float64 nextFirst = 0.0;
        float64 nextSecond = 0.0;
        for (uint32 index = 0; index < points.Count; ++index)
        {
            const float64 first = points.First[index] - points.First[current];
            const float64 second = points.Second[index] - points.Second[current];
            if (first == 0.0 && second == 0.0)
            {
                continue; // The current corner, or a duplicate of it.
            }
            if (hasNext == false)
            {
                hasNext = true;
                next = index;
                nextFirst = first;
                nextSecond = second;
                continue;
            }
            const float64 turn = nextFirst * second - nextSecond * first;
            const float64 lengths =
                math::Sqrt((nextFirst * nextFirst + nextSecond * nextSecond) * (first * first + second * second));
            const bool isCollinear = math::Abs(turn) <= COLLINEAR_RATIO * lengths;
            const bool isAhead = nextFirst * first + nextSecond * second > 0.0;
            const bool isFarther = first * first + second * second > nextFirst * nextFirst + nextSecond * nextSecond;
            // A point to the right of the candidate edge replaces it; so does a farther point straight ahead.
            if ((isCollinear == false && turn < 0.0) || (isCollinear == true && isAhead == true && isFarther == true))
            {
                next = index;
                nextFirst = first;
                nextSecond = second;
            }
        }
        if (hasNext == false || next == start)
        {
            break;
        }
        const float64 startFirst = points.First[next] - points.First[start];
        const float64 startSecond = points.Second[next] - points.Second[start];
        if (startFirst == 0.0 && startSecond == 0.0)
        {
            break; // Back at a duplicate of the start.
        }
        current = next;
    }
    return cornerCount;
}

void FindHullFeature(const ContactShape& shape,
                     math::Vector3d direction,
                     float64 tolerance,
                     FeaturePolygon& outFeature) noexcept
{
    const foundation::BorrowedView<const math::Vector3> pointsShape = shape.Convex.HullOrNull->GetPointsShape();
    const math::Vector3d directionShape = ToShapeAxes(shape.Convex, direction);
    const float64 cappedTolerance = math::Min(tolerance, HULL_FEATURE_EXTENT_RATIO * GetContactExtent(shape));
    float64 farthest = math::Dot(Widen(pointsShape[0]), directionShape);
    for (usize index = 1; index < pointsShape.GetSize(); ++index)
    {
        farthest = math::Max(farthest, math::Dot(Widen(pointsShape[index]), directionShape));
    }
    const ContactTangents tangents = MakeContactTangents(directionShape);
    PlanarPoints planar;
    for (usize index = 0; index < pointsShape.GetSize(); ++index)
    {
        const math::Vector3d pointShape = Widen(pointsShape[index]);
        if (farthest - math::Dot(pointShape, directionShape) <= cappedTolerance)
        {
            planar.Indices[planar.Count] = static_cast<uint16>(index);
            planar.First[planar.Count] = math::Dot(pointShape, tangents.First);
            planar.Second[planar.Count] = math::Dot(pointShape, tangents.Second);
            ++planar.Count;
        }
    }
    StaticArray<uint16, MAX_CONVEX_HULL_POINTS> corners{};
    const uint32 cornerCount = WrapPlanarHull(planar, corners);
    // A face with more corners than a feature holds keeps an evenly spaced subset: an inscribed polygon.
    const uint32 keptCount = math::Min(cornerCount, static_cast<uint32>(MAX_FEATURE_VERTICES));
    StaticArray<uint32, MAX_FEATURE_VERTICES> pointIndices{};
    for (uint32 kept = 0; kept < keptCount; ++kept)
    {
        const uint32 corner = cornerCount <= MAX_FEATURE_VERTICES ? kept : (kept * cornerCount) / keptCount;
        pointIndices[kept] = planar.Indices[corners[corner]];
        outFeature.Vertices[kept] = shape.Position + ToMeshAxes(shape.Convex, Widen(pointsShape[pointIndices[kept]]));
        outFeature.VertexFeatures[kept] = MakeContactFeature(ContactFeatureKind::Vertex, pointIndices[kept]);
    }
    outFeature.Count = keptCount;
    for (uint32 kept = 0; kept < keptCount; ++kept)
    {
        const uint32 first = pointIndices[kept];
        const uint32 second = pointIndices[(kept + 1) % keptCount];
        outFeature.EdgeFeatures[kept] =
            keptCount == 1
                ? outFeature.VertexFeatures[kept]
                : MakeContactFeature(ContactFeatureKind::Edge,
                                     math::Min(first, second) | (math::Max(first, second) << HULL_INDEX_BITS));
    }
    if (keptCount < CONTACT_TRIANGLE_CORNERS)
    {
        return;
    }
    // A face is named by its three lowest point indices, and its normal is the area vector of its corners.
    StaticArray<uint32, CONTACT_TRIANGLE_CORNERS> lowest{MAX_CONVEX_HULL_POINTS,
                                                         MAX_CONVEX_HULL_POINTS,
                                                         MAX_CONVEX_HULL_POINTS};
    math::Vector3d area{};
    for (uint32 kept = 0; kept < keptCount; ++kept)
    {
        uint32 candidate = pointIndices[kept];
        for (uint32& slot : lowest)
        {
            if (candidate < slot)
            {
                const uint32 displaced = slot;
                slot = candidate;
                candidate = displaced;
            }
        }
        area = area + math::Cross(outFeature.Vertices[kept] - outFeature.Vertices[0],
                                  outFeature.Vertices[(kept + 1) % keptCount] - outFeature.Vertices[0]);
    }
    outFeature.FaceFeature =
        MakeContactFeature(ContactFeatureKind::Face,
                           lowest[0] | (lowest[1] << HULL_INDEX_BITS) | (lowest[2] << (2 * HULL_INDEX_BITS)));
    math::Vector3d normal;
    outFeature.Normal = TryNormalize(area, normal) == true && math::Dot(normal, direction) > 0.0 ? normal : direction;
}
} // namespace

ContactTangents MakeContactTangents(math::Vector3d normal) noexcept
{
    const float64 x = math::Abs(normal.X);
    const float64 y = math::Abs(normal.Y);
    const float64 z = math::Abs(normal.Z);
    math::Vector3d leastAligned{1.0, 0.0, 0.0};
    if (y <= x && y <= z)
    {
        leastAligned = {0.0, 1.0, 0.0};
    }
    else if (z <= x && z <= y)
    {
        leastAligned = {0.0, 0.0, 1.0};
    }
    math::Vector3d first = math::Cross(leastAligned, normal);
    const bool isNormalized = TryNormalize(first, first);
    LUDUS_ASSERT(isNormalized == true);
    (void)isNormalized;
    return { .First = first, .Second = math::Cross(normal, first) };
}

math::Vector3d GetContactSupport(const ContactShape& shape, math::Vector3d direction) noexcept
{
    if (shape.IsTriangle == false)
    {
        return shape.Position + GetCoreSupport(shape.Convex, direction);
    }
    usize best = 0;
    float64 bestProjection = math::Dot(shape.Corners[0], direction);
    for (usize corner = 1; corner < CONTACT_TRIANGLE_CORNERS; ++corner)
    {
        const float64 projection = math::Dot(shape.Corners[corner], direction);
        if (projection > bestProjection)
        {
            bestProjection = projection;
            best = corner;
        }
    }
    return shape.Corners[best];
}

float64 GetContactMargin(const ContactShape& shape) noexcept
{
    return shape.IsTriangle == true ? 0.0 : shape.Convex.MarginMeters;
}

math::Vector3d GetContactInterior(const ContactShape& shape) noexcept
{
    if (shape.IsTriangle == true)
    {
        return (shape.Corners[0] + shape.Corners[1] + shape.Corners[2]) *
               (1.0 / static_cast<float64>(CONTACT_TRIANGLE_CORNERS));
    }
    return shape.Position + shape.Convex.InteriorMesh;
}

float64 GetContactExtent(const ContactShape& shape) noexcept
{
    if (shape.IsTriangle == true)
    {
        const math::Vector3d interior = GetContactInterior(shape);
        return math::Max(
            math::Length(shape.Corners[0] - interior),
            math::Max(math::Length(shape.Corners[1] - interior), math::Length(shape.Corners[2] - interior)));
    }
    switch (shape.Convex.Kind)
    {
        case ConvexShapeKind::Sphere:
            return shape.Convex.MarginMeters;
        case ConvexShapeKind::Capsule:
            return shape.Convex.HalfHeightMeters + shape.Convex.MarginMeters;
        case ConvexShapeKind::Box:
            return math::Length(shape.Convex.HalfExtentsShape);
        case ConvexShapeKind::Hull: {
            const math::Aabb3 bounds = shape.Convex.HullOrNull->GetBoundsShape();
            // The interior point lies inside the bounds, so their diagonal bounds every distance from it.
            return math::Length(Widen(bounds.Max) - Widen(bounds.Min));
        }
    }
    return 0.0;
}

float64 GetPairScale(const ContactShape& first, const ContactShape& second) noexcept
{
    return GetContactExtent(first) + GetContactExtent(second) +
           COORDINATE_SCALE_SHARE *
               (GetMaxMagnitude(GetContactInterior(first)) + GetMaxMagnitude(GetContactInterior(second)));
}

void FindSupportFeature(const ContactShape& shape,
                        math::Vector3d direction,
                        float64 tolerance,
                        FeaturePolygon& outFeature) noexcept
{
    outFeature.Count = 0;
    outFeature.FaceFeature = 0;
    outFeature.Normal = direction;
    if (shape.IsTriangle == true)
    {
        FindTriangleFeature(shape, direction, tolerance, outFeature);
        return;
    }
    switch (shape.Convex.Kind)
    {
        case ConvexShapeKind::Sphere:
            outFeature.Count = 1;
            outFeature.Vertices[0] = shape.Position;
            outFeature.VertexFeatures[0] = MakeContactFeature(ContactFeatureKind::Vertex, 0);
            outFeature.EdgeFeatures[0] = outFeature.VertexFeatures[0];
            return;
        case ConvexShapeKind::Capsule:
            FindCapsuleFeature(shape, direction, tolerance, outFeature);
            return;
        case ConvexShapeKind::Box:
            FindBoxFeature(shape, direction, tolerance, outFeature);
            return;
        case ConvexShapeKind::Hull:
            FindHullFeature(shape, direction, tolerance, outFeature);
            return;
    }
}

SegmentPair FindClosestSegmentPoints(const ContactSegment& first, const ContactSegment& second) noexcept
{
    const math::Vector3d firstEdge = first.End - first.Start;
    const math::Vector3d secondEdge = second.End - second.Start;
    const math::Vector3d offset = first.Start - second.Start;
    const float64 firstLengthSquared = math::Dot(firstEdge, firstEdge);
    const float64 secondLengthSquared = math::Dot(secondEdge, secondEdge);
    const float64 secondOffset = math::Dot(secondEdge, offset);
    SegmentPair pair;
    if (firstLengthSquared <= 0.0 && secondLengthSquared <= 0.0)
    {
        pair.IsParallel = true;
    }
    else if (firstLengthSquared <= 0.0)
    {
        pair.IsParallel = true;
        pair.SecondParameter = math::Clamp(secondOffset / secondLengthSquared, 0.0, 1.0);
    }
    else
    {
        const float64 firstOffset = math::Dot(firstEdge, offset);
        if (secondLengthSquared <= 0.0)
        {
            pair.IsParallel = true;
            pair.FirstParameter = math::Clamp(-firstOffset / firstLengthSquared, 0.0, 1.0);
        }
        else
        {
            const float64 alignment = math::Dot(firstEdge, secondEdge);
            const float64 denominator = firstLengthSquared * secondLengthSquared - alignment * alignment;
            // The squared sine of the angle between the segments decides whether the two-parameter solve is usable.
            pair.IsParallel =
                denominator <= COLLINEAR_RATIO * COLLINEAR_RATIO * firstLengthSquared * secondLengthSquared;
            if (pair.IsParallel == false)
            {
                pair.FirstParameter =
                    math::Clamp((alignment * secondOffset - firstOffset * secondLengthSquared) / denominator, 0.0, 1.0);
            }
            const float64 secondNumerator = alignment * pair.FirstParameter + secondOffset;
            if (secondNumerator < 0.0)
            {
                pair.SecondParameter = 0.0;
                pair.FirstParameter = math::Clamp(-firstOffset / firstLengthSquared, 0.0, 1.0);
            }
            else if (secondNumerator > secondLengthSquared)
            {
                pair.SecondParameter = 1.0;
                pair.FirstParameter = math::Clamp((alignment - firstOffset) / firstLengthSquared, 0.0, 1.0);
            }
            else
            {
                pair.SecondParameter = secondNumerator / secondLengthSquared;
            }
        }
    }
    pair.FirstPoint = first.Start + firstEdge * pair.FirstParameter;
    pair.SecondPoint = second.Start + secondEdge * pair.SecondParameter;
    return pair;
}

math::Vector3d FindClosestTrianglePoint(const StaticArray<math::Vector3d, CONTACT_TRIANGLE_CORNERS>& corners,
                                        math::Vector3d point) noexcept
{
    // The reduction returns the closest point of conv{rayPoint - difference} to the origin with the matching
    // combination of the targets: differences of `-corner` and a ray point of `-point` make that `corner - point`.
    Simplex simplex;
    for (usize corner = 0; corner < CONTACT_TRIANGLE_CORNERS; ++corner)
    {
        simplex.Vertices[corner] = { .DifferenceMesh = -corners[corner], .TargetMesh = corners[corner] };
    }
    simplex.Count = CONTACT_TRIANGLE_CORNERS;
    SimplexClosest closest;
    ReduceToClosest(-point, simplex, closest);
    return closest.WitnessMesh;
}
} // namespace ludus::physics::collision::internal
