// Hull mass properties: an incremental convex hull triangulation followed by exact tetrahedron integrals.
//
// The triangulation is the incremental algorithm of Mark de Berg, Otfried Cheong, Marc van Kreveld and Mark
// Overmars, "Computational Geometry: Algorithms and Applications", third edition, Springer, 2008, chapter 11
// ("Convex Hulls"): each point removes the faces it sees and cones the horizon to itself. Ludus keeps the serial
// form without conflict graphs, because hulls hold at most MAX_CONVEX_HULL_POINTS points, and drops points within a
// scale-relative tolerance of the current hull, which leaves the enclosed solid unchanged.
//
// Thanks to Fabio Tonon, "Explicit Exact Formulas for the 3-D Tetrahedron Inertia Tensor in Terms of its Vertex
// Coordinates", Journal of Mathematics and Statistics 1(1), 2005, pages 8-11, https://doi.org/10.3844/jmssp.2005.8.11,
// for the closed-form second moments. Ludus uses the equivalent covariance form
// `integral(x x^T dV) = V / 20 * (sum v v^T + s s^T)` over a tetrahedron with vertices v and vertex sum s, applied to
// signed tetrahedra from an interior reference point to each outward face.

#include "internal/hull_mass.h"

#include "internal/rigid_math.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/physics/collision/convex_shape.h>

namespace ludus::physics::rigid::internal
{
namespace
{
using foundation::StaticArray;
using foundation::uint16;

constexpr usize MAX_POINTS = collision::MAX_CONVEX_HULL_POINTS;
/// Live faces of a closed triangulated hull of n vertices number 2n - 4; one insertion adds at most n more before
/// the visible faces are removed.
constexpr usize MAX_FACES = 3 * MAX_POINTS;
/// The horizon of one insertion is a simple cycle through distinct hull vertices.
constexpr usize MAX_HORIZON_EDGES = MAX_POINTS;
constexpr usize TETRAHEDRON_VERTEX_COUNT = 4;
/// Index of the seed tetrahedron vertex farthest from the plane of the other three.
constexpr usize APEX_VERTEX = 3;
/// Signed distance, relative to the hull's largest extent, beyond which a face counts as visible from a point.
constexpr float64 VISIBILITY_RELATIVE_TOLERANCE = 1.0e-10;
constexpr float64 TETRAHEDRON_VOLUME_FROM_DETERMINANT = 1.0 / 6.0;
constexpr float64 TETRAHEDRON_SECOND_MOMENT_FROM_DETERMINANT = 1.0 / 120.0;
constexpr float64 TETRAHEDRON_CENTROID_WEIGHT = 0.25;
/// Euler's formula for a closed triangulated sphere: F = 2V - 4.
constexpr usize EULER_FACE_OFFSET = 4;

/// One outward triangle, counterclockwise when seen from outside.
struct Face final
{
    uint16 A = 0;
    uint16 B = 0;
    uint16 C = 0;
    bool IsVisible = false;
};

/// A directed horizon edge.
struct Edge final
{
    uint32 From = 0;
    uint32 To = 0;
};

struct Triangulation final
{
    StaticArray<math::Vector3d, MAX_POINTS> Points{};
    usize PointCount = 0;
    StaticArray<Face, MAX_FACES> Faces{};
    usize FaceCount = 0;
    StaticArray<Edge, MAX_HORIZON_EDGES> Horizon{};
    usize HorizonCount = 0;
    float64 Tolerance = 0.0;
};

math::Vector3d WidenPoint(math::Vector3 point) noexcept
{
    return {static_cast<float64>(point.X), static_cast<float64>(point.Y), static_cast<float64>(point.Z)};
}

math::Vector3d GetNormal(const Triangulation& hull, const Face& face) noexcept
{
    return math::Cross(hull.Points[face.B] - hull.Points[face.A], hull.Points[face.C] - hull.Points[face.A]);
}

/// Whether `point` lies beyond the tolerance on the outer side of `face`.
bool IsVisible(const Triangulation& hull, const Face& face, math::Vector3d point) noexcept
{
    const math::Vector3d normal = GetNormal(hull, face);
    const float64 normalLength = math::Length(normal);
    return normalLength > 0.0 && math::Dot(normal, point - hull.Points[face.A]) > hull.Tolerance * normalLength;
}

bool AddFace(Triangulation& inoutHull, uint32 a, uint32 b, uint32 c) noexcept
{
    if (inoutHull.FaceCount == MAX_FACES)
    {
        return false;
    }
    inoutHull.Faces[inoutHull.FaceCount] =
    {
        .A = static_cast<uint16>(a),
        .B = static_cast<uint16>(b),
        .C = static_cast<uint16>(c),
    };
    ++inoutHull.FaceCount;
    return true;
}

/// Indices of the four cooking extremes: farthest pair along the widest axis, farthest from their line and farthest
/// from their plane. Returns false when the points are collinear or coplanar.
bool TryFindTetrahedron(const Triangulation& hull, StaticArray<uint32, TETRAHEDRON_VERTEX_COUNT>& outVertices) noexcept
{
    float64 widest = -1.0;
    StaticArray<uint32, TETRAHEDRON_VERTEX_COUNT> vertices{};
    for (uint32 candidate = 0; candidate < AXIS_COUNT; ++candidate)
    {
        uint32 low = 0;
        uint32 high = 0;
        for (uint32 index = 1; index < hull.PointCount; ++index)
        {
            const float64 value = GetComponent(hull.Points[index], candidate);
            low = value < GetComponent(hull.Points[low], candidate) ? index : low;
            high = value > GetComponent(hull.Points[high], candidate) ? index : high;
        }
        const float64 extent = GetComponent(hull.Points[high], candidate) - GetComponent(hull.Points[low], candidate);
        if (extent > widest)
        {
            widest = extent;
            vertices[0] = low;
            vertices[1] = high;
        }
    }
    const math::Vector3d origin = hull.Points[vertices[0]];
    const math::Vector3d direction = hull.Points[vertices[1]] - origin;
    float64 farthestFromLine = 0.0;
    for (uint32 index = 0; index < hull.PointCount; ++index)
    {
        const float64 distance = math::LengthSquared(math::Cross(hull.Points[index] - origin, direction));
        if (distance > farthestFromLine)
        {
            farthestFromLine = distance;
            vertices[2] = index;
        }
    }
    const math::Vector3d normal = math::Cross(direction, hull.Points[vertices[2]] - origin);
    float64 farthestFromPlane = 0.0;
    for (uint32 index = 0; index < hull.PointCount; ++index)
    {
        const float64 distance = math::Abs(math::Dot(hull.Points[index] - origin, normal));
        if (distance > farthestFromPlane)
        {
            farthestFromPlane = distance;
            vertices[APEX_VERTEX] = index;
        }
    }
    if (widest <= 0.0 || farthestFromLine <= 0.0 || farthestFromPlane <= 0.0)
    {
        return false;
    }
    outVertices = vertices;
    return true;
}

/// Whether some visible face holds the directed edge `from -> to`.
bool IsVisibleEdge(const Triangulation& hull, uint32 from, uint32 to) noexcept
{
    for (usize faceIndex = 0; faceIndex < hull.FaceCount; ++faceIndex)
    {
        const Face& face = hull.Faces[faceIndex];
        if (face.IsVisible == true &&
            ((face.A == from && face.B == to) || (face.B == from && face.C == to) || (face.C == from && face.A == to)))
        {
            return true;
        }
    }
    return false;
}

/// Collect the horizon of the visible faces: their edges whose twin belongs to a hidden face.
bool TryCollectHorizon(Triangulation& inoutHull) noexcept
{
    inoutHull.HorizonCount = 0;
    for (usize faceIndex = 0; faceIndex < inoutHull.FaceCount; ++faceIndex)
    {
        const Face face = inoutHull.Faces[faceIndex];
        if (face.IsVisible == false)
        {
            continue;
        }
        const StaticArray<Edge, AXIS_COUNT> edges{Edge{face.A, face.B}, Edge{face.B, face.C}, Edge{face.C, face.A}};
        for (const Edge& edge : edges)
        {
            if (IsVisibleEdge(inoutHull, edge.To, edge.From) == true)
            {
                continue;
            }
            if (inoutHull.HorizonCount == MAX_HORIZON_EDGES)
            {
                return false;
            }
            inoutHull.Horizon[inoutHull.HorizonCount] = edge;
            ++inoutHull.HorizonCount;
        }
    }
    // A simple cycle leaves each vertex once; rounding that splits the visible region breaks this.
    for (usize first = 0; first < inoutHull.HorizonCount; ++first)
    {
        for (usize second = first + 1; second < inoutHull.HorizonCount; ++second)
        {
            if (inoutHull.Horizon[first].From == inoutHull.Horizon[second].From)
            {
                return false;
            }
        }
    }
    return inoutHull.HorizonCount >= AXIS_COUNT;
}

/// Add `pointIndex` to the hull. Returns false when the triangulation would stop being a closed surface.
bool TryInsertPoint(Triangulation& inoutHull, uint32 pointIndex) noexcept
{
    const math::Vector3d point = inoutHull.Points[pointIndex];
    bool isOutside = false;
    for (usize faceIndex = 0; faceIndex < inoutHull.FaceCount; ++faceIndex)
    {
        Face& face = inoutHull.Faces[faceIndex];
        face.IsVisible = IsVisible(inoutHull, face, point);
        isOutside = isOutside == true || face.IsVisible == true;
    }
    if (isOutside == false)
    {
        return true;
    }
    if (TryCollectHorizon(inoutHull) == false)
    {
        return false;
    }
    usize kept = 0;
    for (usize faceIndex = 0; faceIndex < inoutHull.FaceCount; ++faceIndex)
    {
        if (inoutHull.Faces[faceIndex].IsVisible == false)
        {
            inoutHull.Faces[kept] = inoutHull.Faces[faceIndex];
            ++kept;
        }
    }
    inoutHull.FaceCount = kept;
    for (usize edgeIndex = 0; edgeIndex < inoutHull.HorizonCount; ++edgeIndex)
    {
        const Edge edge = inoutHull.Horizon[edgeIndex];
        if (AddFace(inoutHull, edge.From, edge.To, pointIndex) == false)
        {
            return false;
        }
    }
    return true;
}

bool TryTriangulate(Triangulation& inoutHull, math::Vector3d& outReference) noexcept
{
    StaticArray<uint32, TETRAHEDRON_VERTEX_COUNT> vertices{};
    if (TryFindTetrahedron(inoutHull, vertices) == false)
    {
        return false;
    }
    uint32 a = vertices[0];
    uint32 b = vertices[1];
    uint32 c = vertices[2];
    const uint32 d = vertices[APEX_VERTEX];
    const math::Vector3d baseNormal =
        math::Cross(inoutHull.Points[b] - inoutHull.Points[a], inoutHull.Points[c] - inoutHull.Points[a]);
    if (math::Dot(baseNormal, inoutHull.Points[d] - inoutHull.Points[a]) > 0.0)
    {
        // Orient the base away from the apex.
        const uint32 swapped = b;
        b = c;
        c = swapped;
    }
    const bool isSeeded = AddFace(inoutHull, a, b, c) == true && AddFace(inoutHull, a, d, b) == true &&
                          AddFace(inoutHull, b, d, c) == true && AddFace(inoutHull, c, d, a) == true;
    LUDUS_ASSERT(isSeeded == true);
    (void)isSeeded;
    outReference = (inoutHull.Points[a] + inoutHull.Points[b] + inoutHull.Points[c] + inoutHull.Points[d]) *
                   TETRAHEDRON_CENTROID_WEIGHT;
    for (uint32 index = 0; index < inoutHull.PointCount; ++index)
    {
        if (index == a || index == b || index == c || index == d)
        {
            continue;
        }
        if (TryInsertPoint(inoutHull, index) == false)
        {
            return false;
        }
    }
    StaticArray<bool, MAX_POINTS> isUsed{};
    usize usedCount = 0;
    for (usize faceIndex = 0; faceIndex < inoutHull.FaceCount; ++faceIndex)
    {
        const Face& face = inoutHull.Faces[faceIndex];
        const StaticArray<uint32, AXIS_COUNT> corners{face.A, face.B, face.C};
        for (const uint32 corner : corners)
        {
            usedCount += isUsed[corner] == true ? 0 : 1;
            isUsed[corner] = true;
        }
    }
    return inoutHull.FaceCount + EULER_FACE_OFFSET == 2 * usedCount;
}
} // namespace

Status ComputeHullUnitMass(foundation::BorrowedView<const math::Vector3> pointsShape, UnitDensityMass& outMass) noexcept
{
    if (pointsShape.IsValid() == false || pointsShape.GetSize() < TETRAHEDRON_VERTEX_COUNT ||
        pointsShape.GetSize() > MAX_POINTS)
    {
        return Status::DegenerateShape;
    }
    // The scratch is large; keep it in one object so that its size is visible in one place.
    Triangulation hull;
    hull.PointCount = pointsShape.GetSize();
    math::Vector3d minimum = WidenPoint(pointsShape[0]);
    math::Vector3d maximum = minimum;
    for (const math::Vector3& point : pointsShape)
    {
        const math::Vector3d wide = WidenPoint(point);
        minimum = {math::Min(minimum.X, wide.X), math::Min(minimum.Y, wide.Y), math::Min(minimum.Z, wide.Z)};
        maximum = {math::Max(maximum.X, wide.X), math::Max(maximum.Y, wide.Y), math::Max(maximum.Z, wide.Z)};
    }
    // Work relative to the bounds centre so that products keep their precision far from the shape origin.
    const math::Vector3d centre = (minimum + maximum) * 0.5;
    for (usize index = 0; index < hull.PointCount; ++index)
    {
        hull.Points[index] = WidenPoint(pointsShape[index]) - centre;
    }
    hull.Tolerance = VISIBILITY_RELATIVE_TOLERANCE * GetMaxMagnitude(maximum - minimum);
    math::Vector3d reference{};
    if (TryTriangulate(hull, reference) == false)
    {
        return Status::DegenerateShape;
    }
    float64 volume = 0.0;
    math::Vector3d firstMoment{};
    Matrix3x3 secondMoment{};
    for (usize faceIndex = 0; faceIndex < hull.FaceCount; ++faceIndex)
    {
        const Face& face = hull.Faces[faceIndex];
        const math::Vector3d a = hull.Points[face.A] - reference;
        const math::Vector3d b = hull.Points[face.B] - reference;
        const math::Vector3d c = hull.Points[face.C] - reference;
        const float64 determinant = math::Dot(a, math::Cross(b, c));
        const float64 tetrahedronVolume = determinant * TETRAHEDRON_VOLUME_FROM_DETERMINANT;
        const math::Vector3d sum = a + b + c;
        volume += tetrahedronVolume;
        firstMoment = firstMoment + sum * (tetrahedronVolume * TETRAHEDRON_CENTROID_WEIGHT);
        const Matrix3x3 vertexSum = Add(Add(MakeOuterProduct(a, a), MakeOuterProduct(b, b)),
                                        Add(MakeOuterProduct(c, c), MakeOuterProduct(sum, sum)));
        secondMoment = Add(secondMoment, Scale(vertexSum, determinant * TETRAHEDRON_SECOND_MOMENT_FROM_DETERMINANT));
    }
    if (math::IsFinite(volume) == false || volume <= 0.0)
    {
        return Status::DegenerateShape;
    }
    const math::Vector3d centerOffset = firstMoment * (1.0 / volume);
    // Shift the second moment from the reference point to the COM, then convert covariance to inertia.
    const Matrix3x3 covariance = Add(secondMoment, Scale(MakeOuterProduct(centerOffset, centerOffset), -volume));
    const Matrix3x3 inertia =
        Add(MakeDiagonal({GetTrace(covariance), GetTrace(covariance), GetTrace(covariance)}), Scale(covariance, -1.0));
    outMass =
    {
        .VolumeCubicMeters = volume,
        .CenterOfMassShape = centre + reference + centerOffset,
        .UnitInertia = ToTensor(inertia),
    };
    return Status::Success;
}
} // namespace ludus::physics::rigid::internal
