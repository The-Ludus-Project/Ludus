// Contact manifolds: clipping the support features of two shapes into contact points, and reducing patches.
//
// Thanks to Ivan E. Sutherland and Gary W. Hodgman, "Reentrant Polygon Clipping", Communications of the ACM 17(1),
// 1974, pages 32-42, https://doi.org/10.1145/360767.360802, for clipping the incident feature against the side
// planes of the reference face. Thanks to Dirk Gregorius, "Robust Contact Creation for Physics Simulations", Game
// Developers Conference 2015, for the reference and incident face formulation and for labelling each clipped point
// with the features that produced it. Thanks to Adam Moravanszky and Pierre Terdiman, "Fast Contact Reduction for
// Dynamics Simulation", Game Programming Gems 4, Charles River Media, 2004, section 3.5, pages 253-263, for
// reducing a patch to the deepest point plus the points that span the most area. Ludus keeps four points per patch
// rather than the chapter's five, compares full feature values instead of a feature hash, and never merges patches
// whose normals differ by more than CONTACT_PATCH_NORMAL_COSINE.
//
// Following Kokichi Sugihara, "A Robust and Consistent Algorithm for Intersecting Convex Polyhedra", Computer
// Graphics Forum 13(3) (Eurographics '94), 1994, sections 2 to 5, pages C-46 to C-53, topological decisions are
// kept apart from coordinates: which feature a clipped point belongs to follows from which side-plane tests it
// passed, never from comparing the coordinates that those tests produced, and the tolerance of a test never moves
// a point. A. Agrawal and A. A. G. Requicha, "A Paradigm for the Robust Design of Algorithms for Geometric
// Modeling", same issue, sections 1 and 3, pages C-33 to C-38, draw the same line between numerical error in
// coordinates and inconsistent topology.

#include "internal/contact_generation.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision::internal
{
namespace
{
using foundation::StaticArray;

/// Marks a clip edge that is a piece of an original incident edge rather than of a reference side plane.
constexpr uint32 NO_REFERENCE_EDGE = 0xFFFFFFFFU;
/// A reference face whose normal is this close to perpendicular to the contact normal cannot be projected onto.
constexpr float64 MIN_REFERENCE_ALIGNMENT = 0.1;
/// Two edges whose directions differ by less than this sine share a two-point contact instead of one point.
constexpr float64 PARALLEL_FEATURE_SINE = 1.0e-3;
/// Points closer together than this fraction of the pair scale are one contact point.
constexpr float64 POINT_MERGE_RATIO = 1.0e-6;
constexpr uint32 TRIANGLE_EDGE_COUNT = 3;

/// One vertex of the incident feature while it is clipped.
struct ClipVertex final
{
    math::Vector3d Position{};
    ContactFeature IncidentFeature = 0;       ///< Feature of the incident shape the vertex lies on.
    ContactFeature ReferenceFeature = 0;      ///< Feature of the reference shape above or below the vertex.
    ContactFeature EdgeFeature = 0;           ///< Incident feature of the edge leaving this vertex.
    uint32 ReferenceEdge = NO_REFERENCE_EDGE; ///< Reference edge the leaving edge lies on, if it is a clip edge.
};

struct ClipPolygon final
{
    StaticArray<ClipVertex, MAX_MANIFOLD_POINTS> Vertices{};
    uint32 Count = 0;
};

/// One side plane of the reference face: points with a non-negative distance are inside.
struct SidePlane final
{
    math::Vector3d Point{};
    math::Vector3d Inward{}; ///< Unit normal pointing into the face.
    bool IsUsable = false;   ///< False for a zero-length edge.
};

SidePlane MakeSidePlane(const FeaturePolygon& reference, uint32 edge) noexcept
{
    const math::Vector3d start = reference.Vertices[edge];
    const math::Vector3d end = reference.Vertices[(edge + 1) % reference.Count];
    SidePlane plane{ .Point = start };
    plane.IsUsable = TryNormalize(math::Cross(reference.Normal, end - start), plane.Inward);
    return plane;
}

/// The reference feature where the reference edges `first` and `second` meet: their shared vertex when they are
/// neighbours, the later edge otherwise.
ContactFeature GetReferenceCorner(const FeaturePolygon& reference, uint32 first, uint32 second) noexcept
{
    if ((first + 1) % reference.Count == second)
    {
        return reference.VertexFeatures[second];
    }
    if ((second + 1) % reference.Count == first)
    {
        return reference.VertexFeatures[first];
    }
    return reference.EdgeFeatures[second];
}

/// Where a segment meets a side plane, as a parameter in [0, 1], from the two ends' distances to the plane itself.
/// The slack only decides which end is inside; a crossing is never moved off the plane by it.
float64 GetCrossing(float64 startDistance, float64 endDistance) noexcept
{
    const float64 span = startDistance - endDistance;
    return span != 0.0 ? math::Clamp(startDistance / span, 0.0, 1.0) : 0.0;
}

/// The two features of a clip, named so that they cannot be exchanged by position.
struct ClipFeatures final
{
    const FeaturePolygon& Incident;  ///< The feature being clipped.
    const FeaturePolygon& Reference; ///< The face whose side planes clip it.
};

/// The point at `parameter` along the edge leaving `start`, where it crosses the side plane of `referenceEdge`,
/// labelled by the two features that meet there.
ClipVertex MakeCrossing(const ClipVertex& start,
                        math::Vector3d position,
                        const ClipFeatures& features,
                        uint32 referenceEdge) noexcept
{
    ClipVertex crossing;
    crossing.Position = position;
    if (start.ReferenceEdge == NO_REFERENCE_EDGE)
    {
        crossing.IncidentFeature = start.EdgeFeature;
        crossing.ReferenceFeature = features.Reference.EdgeFeatures[referenceEdge];
    }
    else
    {
        // The edge lies on an earlier side plane: two reference edges meet here, inside the incident face.
        crossing.IncidentFeature = features.Incident.FaceFeature;
        crossing.ReferenceFeature = GetReferenceCorner(features.Reference, start.ReferenceEdge, referenceEdge);
    }
    return crossing;
}

/// Clip a face (three or more vertices) against one side plane.
void ClipFace(const ClipPolygon& input,
              const SidePlane& plane,
              float64 slack,
              const ClipFeatures& features,
              uint32 referenceEdge,
              ClipPolygon& outPolygon) noexcept
{
    outPolygon.Count = 0;
    for (uint32 index = 0; index < input.Count; ++index)
    {
        const ClipVertex& start = input.Vertices[index];
        const ClipVertex& end = input.Vertices[(index + 1) % input.Count];
        const float64 startDistance = math::Dot(start.Position - plane.Point, plane.Inward);
        const float64 endDistance = math::Dot(end.Position - plane.Point, plane.Inward);
        // The slack widens the inside, so a vertex on the boundary is inside for both faces that share it.
        const bool isStartInside = startDistance + slack >= 0.0;
        const bool isEndInside = endDistance + slack >= 0.0;
        const math::Vector3d crossingPosition =
            start.Position + (end.Position - start.Position) * GetCrossing(startDistance, endDistance);
        if (isStartInside == true)
        {
            if (outPolygon.Count < MAX_MANIFOLD_POINTS)
            {
                outPolygon.Vertices[outPolygon.Count++] = start;
            }
            if (isEndInside == false && outPolygon.Count < MAX_MANIFOLD_POINTS)
            {
                ClipVertex crossing = MakeCrossing(start, crossingPosition, features, referenceEdge);
                crossing.EdgeFeature = features.Incident.FaceFeature;
                crossing.ReferenceEdge = referenceEdge; // The next edge runs along this side plane.
                outPolygon.Vertices[outPolygon.Count++] = crossing;
            }
        }
        else if (isEndInside == true && outPolygon.Count < MAX_MANIFOLD_POINTS)
        {
            ClipVertex crossing = MakeCrossing(start, crossingPosition, features, referenceEdge);
            crossing.EdgeFeature = start.EdgeFeature; // The rest of the cut edge follows.
            crossing.ReferenceEdge = start.ReferenceEdge;
            outPolygon.Vertices[outPolygon.Count++] = crossing;
        }
    }
}

/// Clip an edge (two vertices) against every side plane of the reference face.
void ClipEdge(const FeaturePolygon& incident,
              const FeaturePolygon& reference,
              float64 slack,
              ClipPolygon& outPolygon) noexcept
{
    outPolygon.Count = 0;
    float64 startParameter = 0.0;
    float64 endParameter = 1.0;
    ContactFeature startReference = reference.FaceFeature;
    ContactFeature endReference = reference.FaceFeature;
    for (uint32 edge = 0; edge < reference.Count; ++edge)
    {
        const SidePlane plane = MakeSidePlane(reference, edge);
        if (plane.IsUsable == false)
        {
            continue;
        }
        const float64 startDistance = math::Dot(incident.Vertices[0] - plane.Point, plane.Inward);
        const float64 endDistance = math::Dot(incident.Vertices[1] - plane.Point, plane.Inward);
        const bool isStartInside = startDistance + slack >= 0.0;
        const bool isEndInside = endDistance + slack >= 0.0;
        if (isStartInside == false && isEndInside == false)
        {
            return;
        }
        if (isStartInside == false)
        {
            const float64 parameter = GetCrossing(startDistance, endDistance);
            if (parameter > startParameter)
            {
                startParameter = parameter;
                startReference = reference.EdgeFeatures[edge];
            }
        }
        else if (isEndInside == false)
        {
            const float64 parameter = GetCrossing(startDistance, endDistance);
            if (parameter < endParameter)
            {
                endParameter = parameter;
                endReference = reference.EdgeFeatures[edge];
            }
        }
    }
    if (startParameter > endParameter)
    {
        return;
    }
    const math::Vector3d span = incident.Vertices[1] - incident.Vertices[0];
    outPolygon.Vertices[0] =
    {
        .Position = incident.Vertices[0] + span * startParameter,
        .IncidentFeature = startParameter > 0.0 ? incident.EdgeFeatures[0] : incident.VertexFeatures[0],
        .ReferenceFeature = startReference,
    };
    outPolygon.Vertices[1] =
    {
        .Position = incident.Vertices[0] + span * endParameter,
        .IncidentFeature = endParameter < 1.0 ? incident.EdgeFeatures[0] : incident.VertexFeatures[1],
        .ReferenceFeature = endReference,
    };
    outPolygon.Count = 2;
}

/// Clip the incident feature against the side planes of the reference face.
void ClipAgainstReference(const FeaturePolygon& incident,
                          const FeaturePolygon& reference,
                          float64 slack,
                          ClipPolygon& outPolygon) noexcept
{
    if (incident.Count == 2)
    {
        ClipEdge(incident, reference, slack, outPolygon);
        return;
    }
    ClipPolygon current;
    current.Count = incident.Count;
    for (uint32 index = 0; index < incident.Count; ++index)
    {
        current.Vertices[index] =
        {
            .Position = incident.Vertices[index],
            .IncidentFeature = incident.VertexFeatures[index],
            .ReferenceFeature = reference.FaceFeature,
            .EdgeFeature = incident.EdgeFeatures[index],
        };
    }
    for (uint32 edge = 0; edge < reference.Count && current.Count > 0; ++edge)
    {
        const SidePlane plane = MakeSidePlane(reference, edge);
        if (plane.IsUsable == false)
        {
            continue;
        }
        if (incident.Count == 1)
        {
            if (math::Dot(current.Vertices[0].Position - plane.Point, plane.Inward) + slack < 0.0)
            {
                current.Count = 0;
            }
            continue;
        }
        ClipPolygon clipped;
        ClipFace(current, plane, slack, { .Incident = incident, .Reference = reference }, edge, clipped);
        current = clipped;
    }
    outPolygon = current;
}

/// Everything a manifold point needs besides its two core points.
struct PointContext final
{
    math::Vector3d Normal{};
    float64 FirstMargin = 0.0;
    float64 SecondMargin = 0.0;
    float64 ContactOffset = 0.0;
};

/// Add the contact between two core points, unless they are further apart than the contact offset.
void AddPoint(const PointContext& context,
              math::Vector3d firstPoint,
              math::Vector3d secondPoint,
              ContactFeature firstFeature,
              ContactFeature secondFeature,
              Manifold& inoutManifold) noexcept
{
    const float64 separation =
        math::Dot(secondPoint - firstPoint, context.Normal) - context.FirstMargin - context.SecondMargin;
    if (separation > context.ContactOffset || inoutManifold.Count == MAX_MANIFOLD_POINTS)
    {
        return;
    }
    const math::Vector3d firstSurface = firstPoint + context.Normal * context.FirstMargin;
    const math::Vector3d secondSurface = secondPoint - context.Normal * context.SecondMargin;
    inoutManifold.Points[inoutManifold.Count++] =
    {
        .Position = (firstSurface + secondSurface) * 0.5,
        .Separation = separation,
        .FirstFeature = firstFeature,
        .SecondFeature = secondFeature,
    };
}

/// The feature a vertex, edge or face polygon stands for as a whole.
ContactFeature GetWholeFeature(const FeaturePolygon& feature) noexcept
{
    if (feature.Count == 1)
    {
        return feature.VertexFeatures[0];
    }
    return feature.Count == 2 ? feature.EdgeFeatures[0] : feature.FaceFeature;
}

/// The feature of an edge at a parameter along it: an end vertex at exactly zero or one, the edge between.
ContactFeature GetEdgeFeatureAt(const FeaturePolygon& edge, float64 parameter) noexcept
{
    if (edge.Count == 1 || parameter <= 0.0)
    {
        return edge.VertexFeatures[0];
    }
    return parameter >= 1.0 ? edge.VertexFeatures[1] : edge.EdgeFeatures[0];
}

/// Contacts of two features that are both vertices or edges.
void AddEdgeContacts(const PointContext& context,
                     const FeaturePolygon& first,
                     const FeaturePolygon& second,
                     Manifold& inoutManifold) noexcept
{
    const math::Vector3d firstStart = first.Vertices[0];
    const math::Vector3d firstEnd = first.Vertices[first.Count - 1];
    const math::Vector3d secondStart = second.Vertices[0];
    const math::Vector3d secondEnd = second.Vertices[second.Count - 1];
    const math::Vector3d firstSpan = firstEnd - firstStart;
    const math::Vector3d secondSpan = secondEnd - secondStart;
    const float64 firstSquared = math::Dot(firstSpan, firstSpan);
    const float64 secondSquared = math::Dot(secondSpan, secondSpan);
    if (first.Count == 2 && second.Count == 2 && firstSquared > 0.0 && secondSquared > 0.0 &&
        math::LengthSquared(math::Cross(firstSpan, secondSpan)) <=
            PARALLEL_FEATURE_SINE * PARALLEL_FEATURE_SINE * firstSquared * secondSquared)
    {
        // Parallel edges touch along the part of the first edge that the second one covers.
        const float64 startCover = math::Dot(secondStart - firstStart, firstSpan) / firstSquared;
        const float64 endCover = math::Dot(secondEnd - firstStart, firstSpan) / firstSquared;
        const float64 low = math::Max(0.0, math::Min(startCover, endCover));
        const float64 high = math::Min(1.0, math::Max(startCover, endCover));
        if (high > low)
        {
            const StaticArray<float64, 2> parameters{low, high};
            for (const float64 parameter : parameters)
            {
                const math::Vector3d firstPoint = firstStart + firstSpan * parameter;
                const float64 secondParameter =
                    math::Clamp(math::Dot(firstPoint - secondStart, secondSpan) / secondSquared, 0.0, 1.0);
                AddPoint(context,
                         firstPoint,
                         secondStart + secondSpan * secondParameter,
                         GetEdgeFeatureAt(first, parameter),
                         GetEdgeFeatureAt(second, secondParameter),
                         inoutManifold);
            }
            return;
        }
    }
    const SegmentPair pair =
        FindClosestSegmentPoints({ .Start = firstStart, .End = firstEnd }, { .Start = secondStart, .End = secondEnd });
    AddPoint(context,
             pair.FirstPoint,
             pair.SecondPoint,
             GetEdgeFeatureAt(first, pair.FirstParameter),
             GetEdgeFeatureAt(second, pair.SecondParameter),
             inoutManifold);
}

/// Twice the area of a face feature.
float64 GetFeatureArea(const FeaturePolygon& feature) noexcept
{
    math::Vector3d area{};
    for (uint32 index = 1; index + 1 < feature.Count; ++index)
    {
        area = area + math::Cross(feature.Vertices[index] - feature.Vertices[0],
                                  feature.Vertices[index + 1] - feature.Vertices[0]);
    }
    return math::Length(area);
}

/// Twice the signed area of a triangle of points, seen against the patch normal.
float64 GetSignedArea(math::Vector3d first, math::Vector3d second, math::Vector3d third, math::Vector3d normal) noexcept
{
    return math::Dot(math::Cross(second - first, third - first), normal);
}
} // namespace

void BuildManifold(const ContactShape& first,
                   const ContactShape& second,
                   const Penetration& penetration,
                   float64 contactOffset,
                   bool shouldDropWhenClippedAway,
                   Manifold& outManifold) noexcept
{
    outManifold.Normal = penetration.Normal;
    outManifold.Count = 0;
    outManifold.IsApproximate = penetration.IsApproximate;
    const float64 slack = GetPairScale(first, second) * CONTACT_RELATIVE_TOLERANCE;
    // A corner belongs to the contact when it can still be within the offset: as far behind the deepest point as
    // the deepest point is inside the offset.
    const float64 featureTolerance = math::Max(contactOffset - penetration.Separation, 0.0) + slack;
    const PointContext context
    {
        .Normal = penetration.Normal,
        .FirstMargin = GetContactMargin(first),
        .SecondMargin = GetContactMargin(second),
        .ContactOffset = contactOffset,
    };
    FeaturePolygon firstFeature;
    FeaturePolygon secondFeature;
    FindSupportFeature(first, penetration.Normal, featureTolerance, firstFeature);
    FindSupportFeature(second, -penetration.Normal, featureTolerance, secondFeature);
    const float64 firstAlignment = firstFeature.Count >= CONTACT_TRIANGLE_CORNERS
                                       ? math::Abs(math::Dot(firstFeature.Normal, penetration.Normal))
                                       : 0.0;
    const float64 secondAlignment = secondFeature.Count >= CONTACT_TRIANGLE_CORNERS
                                        ? math::Abs(math::Dot(secondFeature.Normal, penetration.Normal))
                                        : 0.0;
    if (math::Max(firstAlignment, secondAlignment) >= MIN_REFERENCE_ALIGNMENT)
    {
        // The face that faces the contact normal most squarely is the reference. Two parallel faces tie: the
        // larger one is then the reference whichever shape comes first, and the first shape wins equal areas.
        bool isFirstReference = firstAlignment >= secondAlignment;
        if (math::Abs(firstAlignment - secondAlignment) <= CONTACT_RELATIVE_TOLERANCE)
        {
            isFirstReference = GetFeatureArea(firstFeature) >= GetFeatureArea(secondFeature);
        }
        const FeaturePolygon& reference = isFirstReference == true ? firstFeature : secondFeature;
        const FeaturePolygon& incident = isFirstReference == true ? secondFeature : firstFeature;
        const float64 projection = math::Dot(penetration.Normal, reference.Normal);
        ClipPolygon clipped;
        ClipAgainstReference(incident, reference, slack, clipped);
        for (uint32 index = 0; index < clipped.Count; ++index)
        {
            const ClipVertex& vertex = clipped.Vertices[index];
            // Slide the incident point along the contact normal onto the reference plane.
            const math::Vector3d onReference =
                vertex.Position +
                penetration.Normal *
                    (math::Dot(reference.Vertices[0] - vertex.Position, reference.Normal) / projection);
            if (isFirstReference == true)
            {
                AddPoint(context,
                         onReference,
                         vertex.Position,
                         vertex.ReferenceFeature,
                         vertex.IncidentFeature,
                         outManifold);
            }
            else
            {
                AddPoint(context,
                         vertex.Position,
                         onReference,
                         vertex.IncidentFeature,
                         vertex.ReferenceFeature,
                         outManifold);
            }
        }
    }
    else if (firstFeature.Count < CONTACT_TRIANGLE_CORNERS && secondFeature.Count < CONTACT_TRIANGLE_CORNERS)
    {
        AddEdgeContacts(context, firstFeature, secondFeature, outManifold);
    }
    if (outManifold.Count == 0 && shouldDropWhenClippedAway == false)
    {
        AddPoint(context,
                 penetration.FirstPoint,
                 penetration.SecondPoint,
                 GetWholeFeature(firstFeature),
                 GetWholeFeature(secondFeature),
                 outManifold);
    }
}

uint32 ReduceContactPoints(ManifoldPoint* points, uint32 count, math::Vector3d normal, float64 scale) noexcept
{
    const float64 mergeDistance = scale * POINT_MERGE_RATIO;
    const float64 depthTie = scale * CONTACT_RELATIVE_TOLERANCE;
    // Merge coincident points; of two, the earlier stays unless the later is clearly deeper.
    uint32 merged = 0;
    for (uint32 index = 0; index < count; ++index)
    {
        bool isDuplicate = false;
        for (uint32 kept = 0; kept < merged && isDuplicate == false; ++kept)
        {
            if (math::LengthSquared(points[index].Position - points[kept].Position) <= mergeDistance * mergeDistance)
            {
                isDuplicate = true;
                if (points[index].Separation < points[kept].Separation - depthTie)
                {
                    points[kept] = points[index];
                }
            }
        }
        if (isDuplicate == false)
        {
            points[merged++] = points[index];
        }
    }
    // Even a few points go through the selection below: points inside the outline of the others, such as the
    // crossings of a capsule's segment with the seams of the triangles under it, add no support and are dropped.
    if (merged <= 2)
    {
        return merged;
    }
    const ContactTangents tangents = MakeContactTangents(normal);
    const math::Vector3d firstTangent = tangents.First;
    const math::Vector3d secondTangent = tangents.Second;
    float64 deepest = points[0].Separation;
    for (uint32 index = 1; index < merged; ++index)
    {
        deepest = math::Min(deepest, points[index].Separation);
    }
    // First point: the deepest; among equally deep points, an extreme one, which is a corner of their outline.
    uint32 first = merged;
    for (uint32 index = 0; index < merged; ++index)
    {
        if (points[index].Separation > deepest + depthTie)
        {
            continue;
        }
        if (first == merged)
        {
            first = index;
            continue;
        }
        const float64 along = math::Dot(points[index].Position - points[first].Position, firstTangent);
        const float64 across = math::Dot(points[index].Position - points[first].Position, secondTangent);
        if (along > depthTie || (along >= -depthTie && across > depthTie))
        {
            first = index;
        }
    }
    // Second point: the farthest from the first.
    uint32 second = first;
    float64 farthest = 0.0;
    for (uint32 index = 0; index < merged; ++index)
    {
        const float64 distance = math::LengthSquared(points[index].Position - points[first].Position);
        if (distance > farthest)
        {
            farthest = distance;
            second = index;
        }
    }
    // Third point: the one that spans the largest triangle with the first two.
    uint32 third = first;
    float64 largestArea = 0.0;
    for (uint32 index = 0; index < merged; ++index)
    {
        const float64 area =
            math::Abs(GetSignedArea(points[first].Position, points[second].Position, points[index].Position, normal));
        if (area > largestArea)
        {
            largestArea = area;
            third = index;
        }
    }
    StaticArray<uint32, MAX_CONTACT_PATCH_POINTS> chosen{first, second, third, first};
    uint32 chosenCount = TRIANGLE_EDGE_COUNT;
    if (second == first)
    {
        chosenCount = 1;
    }
    else if (third == first || largestArea <= mergeDistance * math::Sqrt(farthest))
    {
        chosenCount = 2; // Every point lies within the merge distance of one line.
    }
    else
    {
        // Fourth point: the one furthest outside an edge of the triangle, which adds the most area to it.
        StaticArray<uint32, TRIANGLE_EDGE_COUNT> corners{first, second, third};
        if (GetSignedArea(points[first].Position, points[second].Position, points[third].Position, normal) < 0.0)
        {
            corners = {first, third, second};
        }
        uint32 fourth = first;
        float64 largestGain = 0.0;
        for (uint32 index = 0; index < merged; ++index)
        {
            for (uint32 edge = 0; edge < TRIANGLE_EDGE_COUNT; ++edge)
            {
                const math::Vector3d start = points[corners[edge]].Position;
                const math::Vector3d end = points[corners[(edge + 1) % TRIANGLE_EDGE_COUNT]].Position;
                const float64 gain = -GetSignedArea(start, end, points[index].Position, normal);
                // A point counts only when it lies further outside the edge than the merge distance.
                if (gain > largestGain && gain > mergeDistance * math::Length(end - start))
                {
                    largestGain = gain;
                    fourth = index;
                }
            }
        }
        if (fourth != first)
        {
            chosen[chosenCount++] = fourth;
        }
    }
    // Keep the chosen points in their original order.
    uint32 written = 0;
    for (uint32 index = 0; index < merged; ++index)
    {
        bool isChosen = false;
        for (uint32 slot = 0; slot < chosenCount; ++slot)
        {
            isChosen = isChosen == true || chosen[slot] == index;
        }
        if (isChosen == true)
        {
            points[written++] = points[index];
        }
    }
    return written;
}

uint32 KeepExtremeContactPoints(ManifoldPoint* points, uint32 count, math::Vector3d normal, float64 scale) noexcept
{
    if (count <= PATCH_EXTREME_POINTS)
    {
        return count;
    }
    const float64 tie = scale * CONTACT_RELATIVE_TOLERANCE;
    const ContactTangents tangents = MakeContactTangents(normal);
    const math::Vector3d firstTangent = tangents.First;
    const math::Vector3d secondTangent = tangents.Second;
    const float64 diagonal = math::Sqrt(0.5);
    // Eight directions, 45 degrees apart: a corner of any rectangle is the farthest point along at least one.
    const StaticArray<math::Vector3d, PATCH_EXTREME_DIRECTIONS> directions{firstTangent,
                                                                           (firstTangent + secondTangent) * diagonal,
                                                                           secondTangent,
                                                                           (secondTangent - firstTangent) * diagonal,
                                                                           -firstTangent,
                                                                           (firstTangent + secondTangent) * -diagonal,
                                                                           -secondTangent,
                                                                           (firstTangent - secondTangent) * diagonal};
    StaticArray<uint32, PATCH_EXTREME_POINTS> chosen{};
    for (usize direction = 0; direction < PATCH_EXTREME_DIRECTIONS; ++direction)
    {
        uint32 best = 0;
        for (uint32 index = 1; index < count; ++index)
        {
            if (math::Dot(points[index].Position - points[best].Position, directions[direction]) > tie)
            {
                best = index;
            }
        }
        chosen[direction] = best;
    }
    uint32 deepest = 0;
    for (uint32 index = 1; index < count; ++index)
    {
        if (points[index].Separation < points[deepest].Separation - tie)
        {
            deepest = index;
        }
    }
    chosen[PATCH_EXTREME_DIRECTIONS] = deepest;
    uint32 written = 0;
    for (uint32 index = 0; index < count; ++index)
    {
        bool isChosen = false;
        for (const uint32 slot : chosen)
        {
            isChosen = isChosen == true || slot == index;
        }
        if (isChosen == true)
        {
            points[written++] = points[index];
        }
    }
    return written;
}

void AddManifold(const Manifold& manifold, float64 scale, PairAccumulator& inoutPair) noexcept
{
    inoutPair.GeneratedPointCount += manifold.Count;
    inoutPair.Scale = math::Max(inoutPair.Scale, scale);
    if (manifold.Count == 0)
    {
        return;
    }
    float64 deepest = manifold.Points[0].Separation;
    for (uint32 index = 1; index < manifold.Count; ++index)
    {
        deepest = math::Min(deepest, manifold.Points[index].Separation);
    }
    uint32 patchIndex = 0;
    while (patchIndex < inoutPair.PatchCount &&
           math::Dot(inoutPair.Patches[patchIndex].Normal, manifold.Normal) < CONTACT_PATCH_NORMAL_COSINE)
    {
        ++patchIndex;
    }
    if (patchIndex == inoutPair.PatchCount)
    {
        if (inoutPair.PatchCount < MAX_CONTACT_PAIR_PATCHES)
        {
            ++inoutPair.PatchCount;
        }
        else
        {
            // Every patch slot is taken: the shallowest patch gives way to a deeper newcomer.
            ++inoutPair.DiscardedPatchCount;
            uint32 shallowest = 0;
            for (uint32 index = 1; index < inoutPair.PatchCount; ++index)
            {
                if (inoutPair.Patches[index].DeepestSeparation > inoutPair.Patches[shallowest].DeepestSeparation)
                {
                    shallowest = index;
                }
            }
            if (deepest >= inoutPair.Patches[shallowest].DeepestSeparation)
            {
                return;
            }
            patchIndex = shallowest;
        }
        inoutPair.Patches[patchIndex] = { .Normal = manifold.Normal, .DeepestSeparation = deepest };
    }
    PatchAccumulator& patch = inoutPair.Patches[patchIndex];
    StaticArray<ManifoldPoint, MAX_MANIFOLD_POINTS> points = manifold.Points;
    uint32 count = manifold.Count;
    if (patch.Count + count > PATCH_SCRATCH_POINTS)
    {
        // Thin the patch, and the newcomer too if it is crowded: both then fit.
        patch.Count = KeepExtremeContactPoints(patch.Points.GetData(), patch.Count, patch.Normal, inoutPair.Scale);
        count = KeepExtremeContactPoints(points.GetData(), count, patch.Normal, inoutPair.Scale);
    }
    for (uint32 index = 0; index < count; ++index)
    {
        patch.Points[patch.Count++] = points[index];
    }
    patch.DeepestSeparation = math::Min(patch.DeepestSeparation, deepest);
    patch.IsApproximate = patch.IsApproximate == true || manifold.IsApproximate == true;
}
} // namespace ludus::physics::collision::internal
