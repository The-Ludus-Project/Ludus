// The general convex contact path: GJK distance on the polytope cores, then expanding-polytope penetration when
// the cores overlap.
//
// Thanks to Elmer G. Gilbert, Daniel W. Johnson and S. Sathiya Keerthi, "A Fast Procedure for Computing the
// Distance Between Complex Objects in Three-Dimensional Space", IEEE Journal on Robotics and Automation 4(2), 1988,
// pages 193-203, https://doi.org/10.1109/56.2083, for the distance iteration, which reuses the simplex reduction of
// the convex cast (gjk_simplex.h). Thanks to Gino van den Bergen, "Proximity Queries and Penetration Depth
// Computation on 3D Game Objects", Game Developers Conference 2001, and section 4.3.8 of "Collision Detection in
// Interactive 3D Environments", Morgan Kaufmann, 2003, for the expanding polytope algorithm and for treating
// spheres and capsules as cores with a margin, so that most rounded contacts never need a penetration search.
//
// Ludus departs from the published algorithm in three ways. The polytope is seeded from four support points of
// the difference instead of the final GJK simplex, and each step expands the face with the smallest *signed*
// distance to the origin, so a seed that does not yet contain the origin grows toward it; a touching pair then
// reports a small positive gap rather than failing. Every bound is explicit: vertices, faces, horizon edges and
// steps are capped, and a search that reaches a cap returns its best face, a lower bound of the depth, flagged as
// approximate. Cores are polytopes, so the search ends exactly when a support point repeats.

#include "internal/contact_generation.h"
#include "internal/gjk_simplex.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision::internal
{
namespace
{
using foundation::StaticArray;

/// Most support steps of one distance search; a search that reaches it keeps its last estimate, flagged.
constexpr uint32 MAX_DISTANCE_ITERATIONS = MAX_CONVEX_CAST_ITERATIONS;
/// Most vertices of the expanding polytope, and so most expansion steps.
constexpr usize MAX_POLYTOPE_VERTICES = 64;
/// A convex polytope with V vertices has at most 2V - 4 triangular faces.
constexpr usize MAX_POLYTOPE_FACES = 2 * MAX_POLYTOPE_VERTICES;
/// And at most 3V - 6 edges, which bounds the horizon of one expansion.
constexpr usize MAX_HORIZON_EDGES = 3 * MAX_POLYTOPE_VERTICES;
constexpr usize FACE_CORNER_COUNT = 3;
constexpr usize SEED_VERTEX_COUNT = 4;
constexpr float64 SEED_CENTROID_WEIGHT = 1.0 / static_cast<float64>(SEED_VERTEX_COUNT);
/// The distance search stops when a support point improves the squared distance by less than this fraction.
constexpr float64 DISTANCE_RELATIVE_PROGRESS = 1.0e-12;
/// A seed tetrahedron thinner than this fraction of the pair scale has no usable volume.
constexpr float64 MIN_SEED_THICKNESS_RATIO = 1.0e-9;

/// One vertex of the difference `first - second`, with the point of the first core it came from.
struct PolytopeVertex final
{
    math::Vector3d Difference{};
    math::Vector3d FirstPoint{};
};

/// One triangular face of the polytope, with its outward plane.
struct PolytopeFace final
{
    StaticArray<uint8, FACE_CORNER_COUNT> Corners{};
    math::Vector3d Normal{}; ///< Outward unit normal.
    float64 Distance = 0.0;  ///< Signed distance of the plane from the origin along Normal.
    bool IsLive = false;
};

struct HorizonEdge final
{
    uint8 Start = 0;
    uint8 End = 0;
    bool IsLive = false;
};

/// The expanding polytope: an inscribed approximation of the difference of two cores.
struct Polytope final
{
    StaticArray<PolytopeVertex, MAX_POLYTOPE_VERTICES> Vertices{};
    StaticArray<PolytopeFace, MAX_POLYTOPE_FACES> Faces{};
    StaticArray<HorizonEdge, MAX_HORIZON_EDGES> Horizon{};
    usize VertexCount = 0;
    usize FaceCount = 0; ///< Slots in use, live or not.
    usize HorizonCount = 0;
    math::Vector3d Interior{}; ///< A point strictly inside the seed, and so inside every later polytope.
    float64 MinimumArea = 0.0; ///< Twice the smallest face area that still has a usable normal.
};

PolytopeVertex
GetDifferenceSupport(const ContactShape& first, const ContactShape& second, math::Vector3d direction) noexcept
{
    const math::Vector3d firstPoint = GetContactSupport(first, direction);
    return { .Difference = firstPoint - GetContactSupport(second, -direction), .FirstPoint = firstPoint };
}

/// Add the face (a, b, c) oriented away from the interior point; false when it has no area or no slot is free.
bool TryAddFace(Polytope& inoutPolytope, uint8 firstCorner, uint8 secondCorner, uint8 thirdCorner) noexcept
{
    const math::Vector3d origin = inoutPolytope.Vertices[firstCorner].Difference;
    math::Vector3d area = math::Cross(inoutPolytope.Vertices[secondCorner].Difference - origin,
                                      inoutPolytope.Vertices[thirdCorner].Difference - origin);
    const float64 areaLength = math::Length(area);
    if (areaLength <= inoutPolytope.MinimumArea || math::IsFinite(areaLength) == false)
    {
        return false;
    }
    usize slot = 0;
    while (slot < inoutPolytope.FaceCount && inoutPolytope.Faces[slot].IsLive == true)
    {
        ++slot;
    }
    if (slot == MAX_POLYTOPE_FACES)
    {
        return false;
    }
    area = area * (1.0 / areaLength);
    PolytopeFace face{ .Corners = {firstCorner, secondCorner, thirdCorner}, .Normal = area, .IsLive = true };
    if (math::Dot(area, origin - inoutPolytope.Interior) < 0.0)
    {
        face.Corners = {firstCorner, thirdCorner, secondCorner};
        face.Normal = -area;
    }
    face.Distance = math::Dot(face.Normal, origin);
    inoutPolytope.Faces[slot] = face;
    inoutPolytope.FaceCount = math::Max(inoutPolytope.FaceCount, slot + 1);
    return true;
}

/// Record one edge of a removed face. An edge seen from both of its faces is interior to the removed region and
/// cancels; the edges that remain bound it.
bool TryAddHorizonEdge(Polytope& inoutPolytope, uint8 start, uint8 end) noexcept
{
    for (usize index = 0; index < inoutPolytope.HorizonCount; ++index)
    {
        HorizonEdge& edge = inoutPolytope.Horizon[index];
        if (edge.IsLive == true && edge.Start == end && edge.End == start)
        {
            edge.IsLive = false;
            return true;
        }
    }
    if (inoutPolytope.HorizonCount == MAX_HORIZON_EDGES)
    {
        return false;
    }
    inoutPolytope.Horizon[inoutPolytope.HorizonCount++] = { .Start = start, .End = end, .IsLive = true };
    return true;
}

/// Seed the polytope with four support points that span a volume: the two farthest apart along the frame's first
/// axis, the one farthest from their line and the one farthest from the plane of those three.
bool TrySeedPolytope(const ContactShape& first,
                     const ContactShape& second,
                     float64 scale,
                     Polytope& inoutPolytope) noexcept
{
    const float64 thickness = scale * MIN_SEED_THICKNESS_RATIO;
    StaticArray<PolytopeVertex, SEED_VERTEX_COUNT> seed{};
    seed[0] = GetDifferenceSupport(first, second, {1.0, 0.0, 0.0});
    seed[1] = GetDifferenceSupport(first, second, {-1.0, 0.0, 0.0});
    math::Vector3d line;
    if (TryNormalize(seed[1].Difference - seed[0].Difference, line) == false ||
        math::Length(seed[1].Difference - seed[0].Difference) <= thickness)
    {
        return false;
    }
    const ContactTangents tangents = MakeContactTangents(line);
    const StaticArray<math::Vector3d, SEED_VERTEX_COUNT> sideDirections{tangents.First,
                                                                        -tangents.First,
                                                                        tangents.Second,
                                                                        -tangents.Second};
    float64 bestOffLine = 0.0;
    for (const math::Vector3d& direction : sideDirections)
    {
        const PolytopeVertex candidate = GetDifferenceSupport(first, second, direction);
        const math::Vector3d offset = candidate.Difference - seed[0].Difference;
        const float64 offLine = math::Length(offset - line * math::Dot(offset, line));
        if (offLine > bestOffLine)
        {
            bestOffLine = offLine;
            seed[2] = candidate;
        }
    }
    math::Vector3d plane;
    if (bestOffLine <= thickness ||
        TryNormalize(math::Cross(seed[1].Difference - seed[0].Difference, seed[2].Difference - seed[0].Difference),
                     plane) == false)
    {
        return false;
    }
    float64 bestOffPlane = 0.0;
    for (uint32 side = 0; side < 2; ++side)
    {
        const PolytopeVertex candidate = GetDifferenceSupport(first, second, side == 0 ? plane : -plane);
        const float64 offPlane = math::Abs(math::Dot(candidate.Difference - seed[0].Difference, plane));
        if (offPlane > bestOffPlane)
        {
            bestOffPlane = offPlane;
            seed[SEED_VERTEX_COUNT - 1] = candidate;
        }
    }
    if (bestOffPlane <= thickness)
    {
        return false;
    }
    inoutPolytope.VertexCount = SEED_VERTEX_COUNT;
    inoutPolytope.Interior = {};
    for (usize index = 0; index < SEED_VERTEX_COUNT; ++index)
    {
        inoutPolytope.Vertices[index] = seed[index];
        inoutPolytope.Interior = inoutPolytope.Interior + seed[index].Difference * SEED_CENTROID_WEIGHT;
    }
    inoutPolytope.MinimumArea = thickness * thickness;
    const StaticArray<StaticArray<uint8, FACE_CORNER_COUNT>, SEED_VERTEX_COUNT> faces{
        StaticArray<uint8, FACE_CORNER_COUNT>{0, 1, 2},
        StaticArray<uint8, FACE_CORNER_COUNT>{0, 1, 3},
        StaticArray<uint8, FACE_CORNER_COUNT>{0, 2, 3},
        StaticArray<uint8, FACE_CORNER_COUNT>{1, 2, 3}};
    for (const StaticArray<uint8, FACE_CORNER_COUNT>& face : faces)
    {
        if (TryAddFace(inoutPolytope, face[0], face[1], face[2]) == false)
        {
            return false;
        }
    }
    return true;
}

/// The live face whose plane is nearest the origin by signed distance; the lowest slot wins ties.
usize FindNearestFace(const Polytope& polytope) noexcept
{
    usize best = MAX_POLYTOPE_FACES;
    for (usize index = 0; index < polytope.FaceCount; ++index)
    {
        if (polytope.Faces[index].IsLive == true &&
            (best == MAX_POLYTOPE_FACES || polytope.Faces[index].Distance < polytope.Faces[best].Distance))
        {
            best = index;
        }
    }
    return best;
}

/// Replace the faces that see `vertex` by a cone from their horizon to it; false when a bound or a degenerate face
/// stops the expansion, which leaves the polytope unusable for further steps.
bool TryExpand(Polytope& inoutPolytope, const PolytopeVertex& vertex) noexcept
{
    if (inoutPolytope.VertexCount == MAX_POLYTOPE_VERTICES)
    {
        return false;
    }
    inoutPolytope.HorizonCount = 0;
    for (usize index = 0; index < inoutPolytope.FaceCount; ++index)
    {
        PolytopeFace& face = inoutPolytope.Faces[index];
        if (face.IsLive == false || math::Dot(face.Normal, vertex.Difference) - face.Distance <= 0.0)
        {
            continue;
        }
        face.IsLive = false;
        for (usize corner = 0; corner < FACE_CORNER_COUNT; ++corner)
        {
            if (TryAddHorizonEdge(inoutPolytope,
                                  face.Corners[corner],
                                  face.Corners[(corner + 1) % FACE_CORNER_COUNT]) == false)
            {
                return false;
            }
        }
    }
    const uint8 added = static_cast<uint8>(inoutPolytope.VertexCount);
    inoutPolytope.Vertices[inoutPolytope.VertexCount++] = vertex;
    for (usize index = 0; index < inoutPolytope.HorizonCount; ++index)
    {
        const HorizonEdge& edge = inoutPolytope.Horizon[index];
        if (edge.IsLive == true && TryAddFace(inoutPolytope, edge.Start, edge.End, added) == false)
        {
            return false;
        }
    }
    return true;
}

/// The point of the first core under the origin's projection onto a face.
math::Vector3d GetFaceWitness(const Polytope& polytope, const PolytopeFace& face) noexcept
{
    const PolytopeVertex& first = polytope.Vertices[face.Corners[0]];
    const PolytopeVertex& second = polytope.Vertices[face.Corners[1]];
    const PolytopeVertex& third = polytope.Vertices[face.Corners[2]];
    const math::Vector3d projection = face.Normal * face.Distance;
    const math::Vector3d firstEdge = second.Difference - first.Difference;
    const math::Vector3d secondEdge = third.Difference - first.Difference;
    const math::Vector3d offset = projection - first.Difference;
    const float64 firstSquared = math::Dot(firstEdge, firstEdge);
    const float64 alignment = math::Dot(firstEdge, secondEdge);
    const float64 secondSquared = math::Dot(secondEdge, secondEdge);
    const float64 denominator = firstSquared * secondSquared - alignment * alignment;
    if (denominator <= 0.0 || math::IsFinite(denominator) == false)
    {
        return first.FirstPoint;
    }
    const float64 firstOffset = math::Dot(offset, firstEdge);
    const float64 secondOffset = math::Dot(offset, secondEdge);
    // Clamped barycentric coordinates: a projection outside the face still names a point of the first core.
    const float64 secondWeight =
        math::Clamp((secondSquared * firstOffset - alignment * secondOffset) / denominator, 0.0, 1.0);
    const float64 thirdWeight =
        math::Clamp((firstSquared * secondOffset - alignment * firstOffset) / denominator, 0.0, 1.0 - secondWeight);
    return first.FirstPoint * (1.0 - secondWeight - thirdWeight) + second.FirstPoint * secondWeight +
           third.FirstPoint * thirdWeight;
}

/// Expanding-polytope penetration of two overlapping cores.
void FindCorePenetration(const ContactShape& first,
                         const ContactShape& second,
                         float64 contactOffset,
                         Penetration& outPenetration) noexcept
{
    const float64 scale = GetPairScale(first, second);
    const float64 marginSum = GetContactMargin(first) + GetContactMargin(second);
    const float64 tolerance = scale * CONTACT_RELATIVE_TOLERANCE;
    Polytope polytope;
    if (TrySeedPolytope(first, second, scale, polytope) == false)
    {
        // The difference has no volume to expand: separate along the line of interior points at the margins' depth.
        math::Vector3d normal;
        if (TryNormalize(GetContactInterior(second) - GetContactInterior(first), normal) == false)
        {
            normal = {0.0, 1.0, 0.0};
        }
        outPenetration.Normal = normal;
        outPenetration.Separation = -marginSum;
        outPenetration.FirstPoint = GetContactSupport(first, normal);
        outPenetration.SecondPoint = outPenetration.FirstPoint;
        outPenetration.HasContact = outPenetration.Separation <= contactOffset;
        outPenetration.IsApproximate = true;
        return;
    }
    usize nearest = FindNearestFace(polytope);
    PolytopeFace best = polytope.Faces[nearest];
    bool isExact = false;
    // Every step adds one vertex, so the vertex bound also bounds the steps.
    for (usize step = 0; step < MAX_POLYTOPE_VERTICES; ++step)
    {
        best = polytope.Faces[nearest];
        const PolytopeVertex support = GetDifferenceSupport(first, second, best.Normal);
        bool isKnown = math::Dot(best.Normal, support.Difference) - best.Distance <= tolerance;
        for (usize index = 0; index < polytope.VertexCount && isKnown == false; ++index)
        {
            isKnown = polytope.Vertices[index].Difference == support.Difference;
        }
        if (isKnown == true)
        {
            isExact = true; // The face lies on the boundary of the difference.
            break;
        }
        if (TryExpand(polytope, support) == false)
        {
            break;
        }
        nearest = FindNearestFace(polytope);
        if (nearest == MAX_POLYTOPE_FACES)
        {
            break;
        }
    }
    // The difference is `first - second`: its boundary lies `Distance` from the origin along the direction in which
    // the second shape must move to separate the pair.
    outPenetration.Normal = best.Normal;
    outPenetration.Separation = -best.Distance - marginSum;
    outPenetration.FirstPoint = GetFaceWitness(polytope, best);
    outPenetration.SecondPoint = outPenetration.FirstPoint - best.Normal * best.Distance;
    outPenetration.HasContact = outPenetration.Separation <= contactOffset;
    outPenetration.IsApproximate = isExact == false;
}

bool HasDifference(const Simplex& simplex, math::Vector3d difference) noexcept
{
    for (usize index = 0; index < simplex.Count; ++index)
    {
        if (simplex.Vertices[index].DifferenceMesh == difference)
        {
            return true;
        }
    }
    return false;
}
} // namespace

void FindGeneralPenetration(const ContactShape& first,
                            const ContactShape& second,
                            float64 contactOffset,
                            Penetration& outPenetration) noexcept
{
    outPenetration = {};
    const float64 scale = GetPairScale(first, second);
    const float64 marginSum = GetContactMargin(first) + GetContactMargin(second);
    const float64 reach = marginSum + contactOffset;
    // The simplex of the cast stores `target - shape` and reduces `rayPoint - difference`. With the first core as
    // target, the second as shape and a ray point at the origin, the reduced point is the vector from the first
    // core toward the second, and the witness is a point of the first core.
    math::Vector3d separation = GetContactInterior(second) - GetContactInterior(first);
    if (math::Dot(separation, separation) <= 0.0)
    {
        separation = {1.0, 0.0, 0.0};
    }
    math::Vector3d witness = GetContactInterior(first);
    Simplex simplex;
    bool isOverlapping = false;
    bool isConverged = false;
    for (uint32 iteration = 0; iteration < MAX_DISTANCE_ITERATIONS; ++iteration)
    {
        const math::Vector3d firstPoint = GetContactSupport(first, separation);
        const math::Vector3d secondPoint = GetContactSupport(second, -separation);
        const math::Vector3d difference = firstPoint - secondPoint;
        if (simplex.Count > 0)
        {
            const float64 separationSquared = math::Dot(separation, separation);
            // `separation . (second - first)` over the separation's length bounds the distance from below.
            const float64 bound = math::Dot(separation, secondPoint - firstPoint);
            if (bound > 0.0 && bound * bound > reach * reach * separationSquared)
            {
                return; // The cores are provably further apart than the margins and the offset reach.
            }
            if (separationSquared - bound <= DISTANCE_RELATIVE_PROGRESS * separationSquared ||
                HasDifference(simplex, difference) == true)
            {
                isConverged = true;
                break;
            }
        }
        simplex.Vertices[simplex.Count++] = { .DifferenceMesh = difference, .TargetMesh = firstPoint };
        SimplexClosest closest;
        ReduceToClosest({}, simplex, closest);
        if (closest.HasEnclosedOrigin == true)
        {
            isOverlapping = true;
            break;
        }
        separation = closest.PointMesh;
        witness = closest.WitnessMesh;
        if (math::Length(separation) <= scale * CONTACT_RELATIVE_TOLERANCE)
        {
            isOverlapping = true; // The cores touch: no direction is reliable, so the polytope search decides.
            break;
        }
    }
    if (isOverlapping == true)
    {
        FindCorePenetration(first, second, contactOffset, outPenetration);
        return;
    }
    const float64 distance = math::Length(separation);
    outPenetration.Normal = separation * (1.0 / distance);
    outPenetration.Separation = distance - marginSum;
    outPenetration.FirstPoint = witness;
    outPenetration.SecondPoint = witness + separation;
    outPenetration.HasContact = outPenetration.Separation <= contactOffset;
    outPenetration.IsApproximate = isConverged == false;
}
} // namespace ludus::physics::collision::internal
