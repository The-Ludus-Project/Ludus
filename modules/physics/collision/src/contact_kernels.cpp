// Specialized contact kernels for spheres, capsules, boxes and triangles, and the dispatch by shape kind.
//
// Thanks to Christer Ericson, "Real-Time Collision Detection", Morgan Kaufmann, 2005: section 4.4.1 for the
// separating axis test of two oriented boxes on their 15 axes, section 5.2.9 for the 13 axes of a box and a
// triangle, and section 5.1 for the closest-point queries the rounded kernels are built from. Thanks to Dirk
// Gregorius, "The Separating Axis Test between Convex Polyhedra", Game Developers Conference 2013, for preferring a
// face axis over an edge axis of nearly equal separation, which keeps the contact features of a resting pair
// stable from tick to tick. Ludus reports the separation along the axis of greatest separation for boxes that are
// apart, and the exact distance for every rounded pair.

#include "internal/contact_generation.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision::internal
{
namespace
{
using foundation::StaticArray;

constexpr uint32 BOX_CORNER_COUNT = 8;
constexpr uint32 SEGMENT_END_COUNT = 2;
/// An edge axis replaces the best face axis only when its separation is larger by this fraction of the pair scale.
constexpr float64 FACE_AXIS_PREFERENCE_RATIO = 1.0e-4;
/// Two unit edge directions whose cross product is shorter than this are parallel: their axis is skipped.
constexpr float64 PARALLEL_EDGE_SINE = 1.0e-6;

math::Vector3d GetAxis(const ContactShape& shape, uint32 axis) noexcept
{
    if (axis == 0)
    {
        return shape.Convex.AxisXMesh;
    }
    return axis == 1 ? shape.Convex.AxisYMesh : shape.Convex.AxisZMesh;
}

/// The core segment of a sphere or capsule; a sphere's is a point.
ContactSegment GetSegment(const ContactShape& shape) noexcept
{
    const math::Vector3d half = shape.Convex.AxisYMesh * shape.Convex.HalfHeightMeters;
    return { .Start = shape.Position - half, .End = shape.Position + half };
}

/// Fill a penetration from matching core points: `coreGap` is their signed distance along `normal`.
void SetPenetration(const ContactShape& first,
                    const ContactShape& second,
                    math::Vector3d normal,
                    float64 coreGap,
                    math::Vector3d firstPoint,
                    float64 contactOffset,
                    Penetration& outPenetration) noexcept
{
    outPenetration.Normal = normal;
    outPenetration.Separation = coreGap - GetContactMargin(first) - GetContactMargin(second);
    outPenetration.FirstPoint = firstPoint;
    outPenetration.SecondPoint = firstPoint + normal * coreGap;
    outPenetration.HasContact = outPenetration.Separation <= contactOffset;
    outPenetration.IsApproximate = false;
}

/// Half the width of a box's projection onto a unit axis.
float64 GetBoxRadius(const ContactShape& box, math::Vector3d axis) noexcept
{
    return box.Convex.HalfExtentsShape.X * math::Abs(math::Dot(axis, box.Convex.AxisXMesh)) +
           box.Convex.HalfExtentsShape.Y * math::Abs(math::Dot(axis, box.Convex.AxisYMesh)) +
           box.Convex.HalfExtentsShape.Z * math::Abs(math::Dot(axis, box.Convex.AxisZMesh));
}

/// A point in a box's own axes, relative to its centre.
math::Vector3d ToBoxAxes(const ContactShape& box, math::Vector3d point) noexcept
{
    return ToShapeAxes(box.Convex, point - box.Position);
}

math::Vector3d ClampToBox(const ContactShape& box, math::Vector3d pointBox) noexcept
{
    const math::Vector3d half = box.Convex.HalfExtentsShape;
    return {math::Clamp(pointBox.X, -half.X, half.X),
            math::Clamp(pointBox.Y, -half.Y, half.Y),
            math::Clamp(pointBox.Z, -half.Z, half.Z)};
}

/// The point of a box core closest to `point`, in the pair frame.
math::Vector3d FindClosestBoxPoint(const ContactShape& box, math::Vector3d point) noexcept
{
    return box.Position + ToMeshAxes(box.Convex, ClampToBox(box, ToBoxAxes(box, point)));
}

/// Spheres and capsules against each other: the distance between their core segments, in closed form.
void FindRoundedPenetration(const ContactShape& first,
                            const ContactShape& second,
                            float64 contactOffset,
                            Penetration& outPenetration) noexcept
{
    const ContactSegment firstSegment = GetSegment(first);
    const ContactSegment secondSegment = GetSegment(second);
    const SegmentPair pair = FindClosestSegmentPoints(firstSegment, secondSegment);
    const math::Vector3d delta = pair.SecondPoint - pair.FirstPoint;
    const float64 distance = math::Length(delta);
    if (distance > GetPairScale(first, second) * CONTACT_RELATIVE_TOLERANCE)
    {
        SetPenetration(first,
                       second,
                       delta * (1.0 / distance),
                       distance,
                       pair.FirstPoint,
                       contactOffset,
                       outPenetration);
        return;
    }
    // The cores meet: no direction separates them. Crossing segments separate along their common perpendicular,
    // anything else along the line of centres, and coincident centres along a fixed perpendicular.
    const math::Vector3d centers = second.Position - first.Position;
    math::Vector3d normal = math::Cross(first.Convex.AxisYMesh, second.Convex.AxisYMesh);
    const bool hasBothSegments = first.Convex.HalfHeightMeters > 0.0 && second.Convex.HalfHeightMeters > 0.0;
    if (hasBothSegments == false || math::Length(normal) <= PARALLEL_EDGE_SINE || TryNormalize(normal, normal) == false)
    {
        if (TryNormalize(centers, normal) == false)
        {
            normal = MakeContactTangents(first.Convex.AxisYMesh).First;
        }
    }
    else if (math::Dot(normal, centers) < 0.0)
    {
        normal = -normal;
    }
    SetPenetration(first, second, normal, 0.0, pair.FirstPoint, contactOffset, outPenetration);
}

/// A sphere against a box: the closest point of the box, or the nearest face when the centre is inside.
void FindSphereBoxPenetration(const ContactShape& sphere,
                              const ContactShape& box,
                              float64 contactOffset,
                              Penetration& outPenetration) noexcept
{
    const math::Vector3d centerBox = ToBoxAxes(box, sphere.Position);
    const math::Vector3d closest = box.Position + ToMeshAxes(box.Convex, ClampToBox(box, centerBox));
    const math::Vector3d delta = closest - sphere.Position;
    const float64 distance = math::Length(delta);
    if (distance > GetPairScale(sphere, box) * CONTACT_RELATIVE_TOLERANCE)
    {
        SetPenetration(sphere, box, delta * (1.0 / distance), distance, sphere.Position, contactOffset, outPenetration);
        return;
    }
    // The centre is inside the box: it leaves through the nearest face, lowest axis first on ties.
    uint32 bestAxis = 0;
    float64 bestDepth = box.Convex.HalfExtentsShape.X - math::Abs(centerBox.X);
    for (uint32 axis = 1; axis < AXIS_COUNT; ++axis)
    {
        const float64 depth =
            GetComponent(box.Convex.HalfExtentsShape, axis) - math::Abs(GetComponent(centerBox, axis));
        if (depth < bestDepth)
        {
            bestDepth = depth;
            bestAxis = axis;
        }
    }
    const math::Vector3d outward =
        GetComponent(centerBox, bestAxis) >= 0.0 ? GetAxis(box, bestAxis) : -GetAxis(box, bestAxis);
    SetPenetration(sphere, box, -outward, -math::Max(bestDepth, 0.0), sphere.Position, contactOffset, outPenetration);
}

/// One candidate axis of a separating axis test.
struct AxisCandidate final
{
    math::Vector3d Normal{};   ///< Unit axis, oriented from the first shape toward the second.
    float64 Separation = 0.0;  ///< Signed gap of the cores along Normal.
    bool HasCandidate = false; ///< Whether any axis was recorded.
};

void KeepLargerSeparation(math::Vector3d normal, float64 separation, AxisCandidate& inoutBest) noexcept
{
    if (inoutBest.HasCandidate == false || separation > inoutBest.Separation)
    {
        inoutBest = { .Normal = normal, .Separation = separation, .HasCandidate = true };
    }
}

/// The axis the test settles on: the best face axis unless an edge axis separates clearly more.
AxisCandidate ChooseAxis(const AxisCandidate& face, const AxisCandidate& edge, float64 scale) noexcept
{
    if (edge.HasCandidate == true && edge.Separation > face.Separation + FACE_AXIS_PREFERENCE_RATIO * scale)
    {
        return edge;
    }
    return face;
}

/// A box against a capsule. Apart, the distance is the smallest over the capsule ends against the box and the
/// capsule segment against the box's twelve edges; overlapping, the six axes of a box and a segment give the depth.
void FindBoxCapsulePenetration(const ContactShape& box,
                               const ContactShape& capsule,
                               float64 contactOffset,
                               Penetration& outPenetration) noexcept
{
    const float64 scale = GetPairScale(box, capsule);
    const ContactSegment segment = GetSegment(capsule);
    const math::Vector3d segmentAxis = capsule.Convex.AxisYMesh;
    const math::Vector3d centers = capsule.Position - box.Position;
    AxisCandidate face;
    AxisCandidate edge;
    for (uint32 axis = 0; axis < AXIS_COUNT; ++axis)
    {
        const math::Vector3d faceAxis = GetAxis(box, axis);
        const float64 faceOffset = math::Dot(faceAxis, centers);
        KeepLargerSeparation(faceOffset >= 0.0 ? faceAxis : -faceAxis,
                             math::Abs(faceOffset) - GetComponent(box.Convex.HalfExtentsShape, axis) -
                                 capsule.Convex.HalfHeightMeters * math::Abs(math::Dot(faceAxis, segmentAxis)),
                             face);
        math::Vector3d edgeAxis = math::Cross(segmentAxis, faceAxis);
        if (capsule.Convex.HalfHeightMeters <= 0.0 || math::Length(edgeAxis) <= PARALLEL_EDGE_SINE ||
            TryNormalize(edgeAxis, edgeAxis) == false)
        {
            continue;
        }
        const float64 edgeOffset = math::Dot(edgeAxis, centers);
        // The segment is perpendicular to this axis, so only the box has a radius along it.
        KeepLargerSeparation(edgeOffset >= 0.0 ? edgeAxis : -edgeAxis,
                             math::Abs(edgeOffset) - GetBoxRadius(box, edgeAxis),
                             edge);
    }
    const AxisCandidate best = ChooseAxis(face, edge, scale);
    if (best.Separation <= scale * CONTACT_RELATIVE_TOLERANCE)
    {
        SetPenetration(box,
                       capsule,
                       best.Normal,
                       best.Separation,
                       GetContactSupport(box, best.Normal),
                       contactOffset,
                       outPenetration);
        return;
    }
    if (best.Separation - capsule.Convex.MarginMeters > contactOffset)
    {
        outPenetration.HasContact = false; // An axis already separates the pair by more than the offset.
        return;
    }
    math::Vector3d bestBoxPoint = FindClosestBoxPoint(box, segment.Start);
    math::Vector3d bestSegmentPoint = segment.Start;
    float64 bestDistanceSquared = math::LengthSquared(bestSegmentPoint - bestBoxPoint);
    const math::Vector3d endBoxPoint = FindClosestBoxPoint(box, segment.End);
    if (math::LengthSquared(segment.End - endBoxPoint) < bestDistanceSquared)
    {
        bestDistanceSquared = math::LengthSquared(segment.End - endBoxPoint);
        bestBoxPoint = endBoxPoint;
        bestSegmentPoint = segment.End;
    }
    StaticArray<math::Vector3d, BOX_CORNER_COUNT> corners{};
    for (uint32 bits = 0; bits < BOX_CORNER_COUNT; ++bits)
    {
        const math::Vector3d half = box.Convex.HalfExtentsShape;
        corners[bits] = box.Position + ToMeshAxes(box.Convex,
                                                  {(bits & 1U) != 0 ? half.X : -half.X,
                                                   ((bits >> 1U) & 1U) != 0 ? half.Y : -half.Y,
                                                   ((bits >> 2U) & 1U) != 0 ? half.Z : -half.Z});
    }
    for (uint32 bits = 0; bits < BOX_CORNER_COUNT; ++bits)
    {
        for (uint32 axis = 0; axis < AXIS_COUNT; ++axis)
        {
            if (((bits >> axis) & 1U) != 0)
            {
                continue; // Each edge is visited once, from its negative end.
            }
            const SegmentPair pair =
                FindClosestSegmentPoints({ .Start = corners[bits], .End = corners[bits | (1U << axis)] }, segment);
            const float64 distanceSquared = math::LengthSquared(pair.SecondPoint - pair.FirstPoint);
            if (distanceSquared < bestDistanceSquared)
            {
                bestDistanceSquared = distanceSquared;
                bestBoxPoint = pair.FirstPoint;
                bestSegmentPoint = pair.SecondPoint;
            }
        }
    }
    const float64 distance = math::Sqrt(bestDistanceSquared);
    math::Vector3d normal;
    if (TryNormalize(bestSegmentPoint - bestBoxPoint, normal) == false)
    {
        SetPenetration(box,
                       capsule,
                       best.Normal,
                       best.Separation,
                       GetContactSupport(box, best.Normal),
                       contactOffset,
                       outPenetration);
        return;
    }
    SetPenetration(box, capsule, normal, distance, bestBoxPoint, contactOffset, outPenetration);
}

/// Two boxes: the separating axis test on three face axes of each box and the nine pairs of their edges.
void FindBoxBoxPenetration(const ContactShape& first,
                           const ContactShape& second,
                           float64 contactOffset,
                           Penetration& outPenetration) noexcept
{
    const math::Vector3d centers = second.Position - first.Position;
    AxisCandidate face;
    AxisCandidate edge;
    for (uint32 shape = 0; shape < 2; ++shape)
    {
        for (uint32 axis = 0; axis < AXIS_COUNT; ++axis)
        {
            const math::Vector3d faceAxis = GetAxis(shape == 0 ? first : second, axis);
            const float64 offset = math::Dot(faceAxis, centers);
            KeepLargerSeparation(offset >= 0.0 ? faceAxis : -faceAxis,
                                 math::Abs(offset) - GetBoxRadius(first, faceAxis) - GetBoxRadius(second, faceAxis),
                                 face);
        }
    }
    for (uint32 firstAxis = 0; firstAxis < AXIS_COUNT; ++firstAxis)
    {
        for (uint32 secondAxis = 0; secondAxis < AXIS_COUNT; ++secondAxis)
        {
            math::Vector3d edgeAxis = math::Cross(GetAxis(first, firstAxis), GetAxis(second, secondAxis));
            if (math::Length(edgeAxis) <= PARALLEL_EDGE_SINE || TryNormalize(edgeAxis, edgeAxis) == false)
            {
                continue;
            }
            const float64 offset = math::Dot(edgeAxis, centers);
            KeepLargerSeparation(offset >= 0.0 ? edgeAxis : -edgeAxis,
                                 math::Abs(offset) - GetBoxRadius(first, edgeAxis) - GetBoxRadius(second, edgeAxis),
                                 edge);
        }
    }
    const AxisCandidate best = ChooseAxis(face, edge, GetPairScale(first, second));
    SetPenetration(first,
                   second,
                   best.Normal,
                   best.Separation,
                   GetContactSupport(first, best.Normal),
                   contactOffset,
                   outPenetration);
}

/// Whether the convex shape's interior point is on the front of the triangle; a point in the plane counts as front.
bool IsOnFrontSide(const ContactShape& convex, const ContactShape& triangle) noexcept
{
    return math::Dot(GetContactInterior(convex) - triangle.Corners[0], triangle.TriangleNormal) >= 0.0;
}

/// The normal from a convex shape toward the triangle face under it.
math::Vector3d GetFaceApproach(const ContactShape& convex, const ContactShape& triangle) noexcept
{
    return IsOnFrontSide(convex, triangle) == true ? -triangle.TriangleNormal : triangle.TriangleNormal;
}

void FindSphereTrianglePenetration(const ContactShape& sphere,
                                   const ContactShape& triangle,
                                   float64 contactOffset,
                                   Penetration& outPenetration) noexcept
{
    const math::Vector3d closest = FindClosestTrianglePoint(triangle.Corners, sphere.Position);
    const math::Vector3d delta = closest - sphere.Position;
    const float64 distance = math::Length(delta);
    if (distance > GetPairScale(sphere, triangle) * CONTACT_RELATIVE_TOLERANCE)
    {
        SetPenetration(sphere,
                       triangle,
                       delta * (1.0 / distance),
                       distance,
                       sphere.Position,
                       contactOffset,
                       outPenetration);
        return;
    }
    SetPenetration(sphere,
                   triangle,
                   GetFaceApproach(sphere, triangle),
                   0.0,
                   sphere.Position,
                   contactOffset,
                   outPenetration);
}

/// A capsule against a triangle. Apart, the distance is the smallest over the capsule ends against the triangle and
/// the capsule segment against the triangle's edges; a segment that passes through the triangle is pushed out along
/// the face normal on the side of the capsule's centre.
void FindCapsuleTrianglePenetration(const ContactShape& capsule,
                                    const ContactShape& triangle,
                                    float64 contactOffset,
                                    Penetration& outPenetration) noexcept
{
    const float64 tolerance = GetPairScale(capsule, triangle) * CONTACT_RELATIVE_TOLERANCE;
    const ContactSegment segment = GetSegment(capsule);
    const math::Vector3d approach = GetFaceApproach(capsule, triangle);
    // Heights of the ends above the plane, on the side of the capsule's centre.
    const float64 startHeight = -math::Dot(segment.Start - triangle.Corners[0], approach);
    const float64 endHeight = -math::Dot(segment.End - triangle.Corners[0], approach);
    if (math::Min(startHeight, endHeight) <= 0.0 && startHeight != endHeight)
    {
        const float64 crossing = startHeight / (startHeight - endHeight);
        const math::Vector3d pierce = segment.Start + (segment.End - segment.Start) * math::Clamp(crossing, 0.0, 1.0);
        if (math::Length(FindClosestTrianglePoint(triangle.Corners, pierce) - pierce) <= tolerance)
        {
            const bool isStartLower = startHeight <= endHeight;
            SetPenetration(capsule,
                           triangle,
                           approach,
                           math::Min(startHeight, endHeight),
                           isStartLower == true ? segment.Start : segment.End,
                           contactOffset,
                           outPenetration);
            return;
        }
    }
    const StaticArray<math::Vector3d, SEGMENT_END_COUNT> ends{segment.Start, segment.End};
    math::Vector3d bestSegmentPoint = segment.Start;
    math::Vector3d bestTrianglePoint = FindClosestTrianglePoint(triangle.Corners, segment.Start);
    float64 bestDistanceSquared = math::LengthSquared(bestTrianglePoint - bestSegmentPoint);
    const math::Vector3d endTrianglePoint = FindClosestTrianglePoint(triangle.Corners, ends[1]);
    if (math::LengthSquared(endTrianglePoint - ends[1]) < bestDistanceSquared)
    {
        bestDistanceSquared = math::LengthSquared(endTrianglePoint - ends[1]);
        bestSegmentPoint = ends[1];
        bestTrianglePoint = endTrianglePoint;
    }
    for (usize corner = 0; corner < CONTACT_TRIANGLE_CORNERS; ++corner)
    {
        const SegmentPair pair =
            FindClosestSegmentPoints(segment,
                                     {
                                         .Start = triangle.Corners[corner],
                                         .End = triangle.Corners[(corner + 1) % CONTACT_TRIANGLE_CORNERS],
                                     });
        const float64 distanceSquared = math::LengthSquared(pair.SecondPoint - pair.FirstPoint);
        if (distanceSquared < bestDistanceSquared)
        {
            bestDistanceSquared = distanceSquared;
            bestSegmentPoint = pair.FirstPoint;
            bestTrianglePoint = pair.SecondPoint;
        }
    }
    const float64 distance = math::Sqrt(bestDistanceSquared);
    if (distance > tolerance)
    {
        SetPenetration(capsule,
                       triangle,
                       (bestTrianglePoint - bestSegmentPoint) * (1.0 / distance),
                       distance,
                       bestSegmentPoint,
                       contactOffset,
                       outPenetration);
        return;
    }
    SetPenetration(capsule, triangle, approach, 0.0, bestSegmentPoint, contactOffset, outPenetration);
}

/// The gap between a box and a triangle along one unit axis, oriented from the box toward the triangle.
AxisCandidate MeasureBoxTriangleAxis(const ContactShape& box,
                                     const StaticArray<math::Vector3d, CONTACT_TRIANGLE_CORNERS>& corners,
                                     math::Vector3d axis) noexcept
{
    const float64 boxCenter = math::Dot(box.Position, axis);
    const float64 boxRadius = GetBoxRadius(box, axis);
    float64 lowest = math::Dot(corners[0], axis);
    float64 highest = lowest;
    for (usize corner = 1; corner < CONTACT_TRIANGLE_CORNERS; ++corner)
    {
        const float64 projection = math::Dot(corners[corner], axis);
        lowest = math::Min(lowest, projection);
        highest = math::Max(highest, projection);
    }
    const float64 ahead = lowest - (boxCenter + boxRadius);   // Triangle beyond the box along +axis.
    const float64 behind = (boxCenter - boxRadius) - highest; // Triangle beyond the box along -axis.
    if (ahead >= behind)
    {
        return { .Normal = axis, .Separation = ahead, .HasCandidate = true };
    }
    return { .Normal = -axis, .Separation = behind, .HasCandidate = true };
}

/// A box against a triangle: the separating axis test on the triangle normal, the box's face axes and the nine
/// pairs of their edges. The triangle normal is measured first, so a box lying flat reports the face.
void FindBoxTrianglePenetration(const ContactShape& box,
                                const ContactShape& triangle,
                                float64 contactOffset,
                                Penetration& outPenetration) noexcept
{
    AxisCandidate face = MeasureBoxTriangleAxis(box, triangle.Corners, triangle.TriangleNormal);
    AxisCandidate edge;
    for (uint32 axis = 0; axis < AXIS_COUNT; ++axis)
    {
        const AxisCandidate candidate = MeasureBoxTriangleAxis(box, triangle.Corners, GetAxis(box, axis));
        KeepLargerSeparation(candidate.Normal, candidate.Separation, face);
        for (usize corner = 0; corner < CONTACT_TRIANGLE_CORNERS; ++corner)
        {
            math::Vector3d triangleEdge;
            math::Vector3d edgeAxis;
            if (TryNormalize(triangle.Corners[(corner + 1) % CONTACT_TRIANGLE_CORNERS] - triangle.Corners[corner],
                             triangleEdge) == false)
            {
                continue;
            }
            edgeAxis = math::Cross(GetAxis(box, axis), triangleEdge);
            if (math::Length(edgeAxis) <= PARALLEL_EDGE_SINE || TryNormalize(edgeAxis, edgeAxis) == false)
            {
                continue;
            }
            const AxisCandidate edgeCandidate = MeasureBoxTriangleAxis(box, triangle.Corners, edgeAxis);
            KeepLargerSeparation(edgeCandidate.Normal, edgeCandidate.Separation, edge);
        }
    }
    const AxisCandidate best = ChooseAxis(face, edge, GetPairScale(box, triangle));
    SetPenetration(box,
                   triangle,
                   best.Normal,
                   best.Separation,
                   GetContactSupport(box, best.Normal),
                   contactOffset,
                   outPenetration);
}

/// The same penetration seen with the two shapes swapped.
void MirrorPenetration(Penetration& inoutPenetration) noexcept
{
    const math::Vector3d firstPoint = inoutPenetration.FirstPoint;
    inoutPenetration.Normal = -inoutPenetration.Normal;
    inoutPenetration.FirstPoint = inoutPenetration.SecondPoint;
    inoutPenetration.SecondPoint = firstPoint;
}

bool IsRounded(const ContactShape& shape) noexcept
{
    return shape.Convex.Kind == ConvexShapeKind::Sphere || shape.Convex.Kind == ConvexShapeKind::Capsule;
}
} // namespace

void FindPenetration(const ContactShape& first,
                     const ContactShape& second,
                     float64 contactOffset,
                     Penetration& outPenetration) noexcept
{
    LUDUS_ASSERT(first.IsTriangle == false);
    outPenetration = {};
    const ConvexShapeKind firstKind = first.Convex.Kind;
    if (second.IsTriangle == true)
    {
        switch (firstKind)
        {
            case ConvexShapeKind::Sphere:
                FindSphereTrianglePenetration(first, second, contactOffset, outPenetration);
                return;
            case ConvexShapeKind::Capsule:
                FindCapsuleTrianglePenetration(first, second, contactOffset, outPenetration);
                return;
            case ConvexShapeKind::Box:
                FindBoxTrianglePenetration(first, second, contactOffset, outPenetration);
                return;
            case ConvexShapeKind::Hull:
                FindGeneralPenetration(first, second, contactOffset, outPenetration);
                return;
        }
        return;
    }
    const ConvexShapeKind secondKind = second.Convex.Kind;
    if (firstKind == ConvexShapeKind::Hull || secondKind == ConvexShapeKind::Hull)
    {
        FindGeneralPenetration(first, second, contactOffset, outPenetration);
        return;
    }
    if (IsRounded(first) == true && IsRounded(second) == true)
    {
        FindRoundedPenetration(first, second, contactOffset, outPenetration);
        return;
    }
    if (firstKind == ConvexShapeKind::Box && secondKind == ConvexShapeKind::Box)
    {
        FindBoxBoxPenetration(first, second, contactOffset, outPenetration);
        return;
    }
    // One box and one rounded shape: each kernel is written for one order and mirrored for the other.
    const bool isBoxFirst = firstKind == ConvexShapeKind::Box;
    const ContactShape& box = isBoxFirst == true ? first : second;
    const ContactShape& rounded = isBoxFirst == true ? second : first;
    if (rounded.Convex.Kind == ConvexShapeKind::Sphere)
    {
        FindSphereBoxPenetration(rounded, box, contactOffset, outPenetration);
        if (isBoxFirst == true)
        {
            MirrorPenetration(outPenetration);
        }
        return;
    }
    FindBoxCapsulePenetration(box, rounded, contactOffset, outPenetration);
    if (isBoxFirst == false)
    {
        MirrorPenetration(outPenetration);
    }
}
} // namespace ludus::physics::collision::internal
