// Contacts of a convex shape against a cooked triangle mesh, honoring the mesh's internal-edge classification.
//
// Thanks to Adam Moravanszky and Pierre Terdiman, "Fast Contact Reduction for Dynamics Simulation", Game
// Programming Gems 4, Charles River Media, 2004, section 3.5, pages 253-263, for preprocessing mesh features so
// that contacts against the interior seams of a surface cannot push along a seam's own direction. Ludus classifies
// edges per side of the surface at cook time (TriangleMeshCookSettings::EdgePolicy) and, at an inactive edge or
// corner, replaces the contact normal by the face normal and keeps the contact only where the shape is over the
// triangle itself; the neighbouring triangle reports the rest. Boundaries, creases and nonmanifold edges stay
// active, so no real geometry is removed.

#include "internal/contact_generation.h"
#include "internal/convex_cast.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision
{
namespace
{
namespace math = foundation::math;
using foundation::StaticArray;
using internal::Widen;

/// An axis-aligned box in the mesh frame, as a centre and half extents.
struct Reach final
{
    math::Vector3d Center{};
    math::Vector3d HalfExtents{};
};

bool Overlaps(const Reach& reach, math::Vector3d minimum, math::Vector3d maximum) noexcept
{
    return minimum.X <= reach.Center.X + reach.HalfExtents.X && maximum.X >= reach.Center.X - reach.HalfExtents.X &&
           minimum.Y <= reach.Center.Y + reach.HalfExtents.Y && maximum.Y >= reach.Center.Y - reach.HalfExtents.Y &&
           minimum.Z <= reach.Center.Z + reach.HalfExtents.Z && maximum.Z >= reach.Center.Z - reach.HalfExtents.Z;
}

math::Vector3d GetMinimum(math::Vector3d first, math::Vector3d second, math::Vector3d third) noexcept
{
    return {math::Min(first.X, math::Min(second.X, third.X)),
            math::Min(first.Y, math::Min(second.Y, third.Y)),
            math::Min(first.Z, math::Min(second.Z, third.Z))};
}

math::Vector3d GetMaximum(math::Vector3d first, math::Vector3d second, math::Vector3d third) noexcept
{
    return {math::Max(first.X, math::Max(second.X, third.X)),
            math::Max(first.Y, math::Max(second.Y, third.Y)),
            math::Max(first.Z, math::Max(second.Z, third.Z))};
}
} // namespace

/// Contact traversal of a cooked mesh, with access to its triangles and hierarchy.
class TriangleMeshContacts final
{
public:
    static void Collect(const TriangleMesh& mesh,
                        const internal::ContactShape& convex,
                        float64 contactOffset,
                        internal::PairAccumulator& inoutPair) noexcept;

private:
    static void testTriangle(const TriangleMesh::Triangle& triangle,
                             const internal::ContactShape& convex,
                             const Reach& reach,
                             float64 contactOffset,
                             internal::PairAccumulator& inoutPair) noexcept;
};

void TriangleMeshContacts::testTriangle(const TriangleMesh::Triangle& triangle,
                                        const internal::ContactShape& convex,
                                        const Reach& reach,
                                        float64 contactOffset,
                                        internal::PairAccumulator& inoutPair) noexcept
{
    internal::ContactShape shape;
    shape.IsTriangle = true;
    shape.Corners = {Widen(triangle.FirstMesh), Widen(triangle.SecondMesh), Widen(triangle.ThirdMesh)};
    if (Overlaps(reach,
                 GetMinimum(shape.Corners[0], shape.Corners[1], shape.Corners[2]),
                 GetMaximum(shape.Corners[0], shape.Corners[1], shape.Corners[2])) == false)
    {
        return;
    }
    // Admitted triangles pass MIN_TRIANGLE_SHAPE_RATIO, so the winding normal exists.
    const bool hasNormal =
        internal::TryNormalize(math::Cross(shape.Corners[1] - shape.Corners[0], shape.Corners[2] - shape.Corners[0]),
                               shape.TriangleNormal);
    LUDUS_ASSERT(hasNormal == true);
    (void)hasNormal;
    ++inoutPair.TriangleTestCount;
    internal::Penetration penetration;
    internal::FindPenetration(convex, shape, contactOffset, penetration);
    if (penetration.HasContact == false)
    {
        return;
    }
    const float64 scale = internal::GetPairScale(convex, shape);
    const bool isFrontSide =
        math::Dot(internal::GetContactInterior(convex) - shape.Corners[0], shape.TriangleNormal) >= 0.0;
    // Which part of the triangle faces the shape along the contact normal: its face, an edge or a corner.
    internal::FeaturePolygon touched;
    internal::FindSupportFeature(shape, -penetration.Normal, scale * internal::CONTACT_RELATIVE_TOLERANCE, touched);
    bool isFaceContact = touched.Count == internal::CONTACT_TRIANGLE_CORNERS;
    if (isFaceContact == false)
    {
        const ContactFeature feature = touched.Count == 2 ? touched.EdgeFeatures[0] : touched.VertexFeatures[0];
        const bool isActive =
            touched.Count == 2
                ? IsTriangleEdgeActive(triangle.Features, GetContactFeatureIndex(feature), isFrontSide)
                : IsTriangleCornerActive(triangle.Features, GetContactFeatureIndex(feature), isFrontSide);
        if (isActive == false)
        {
            // An interior seam: push along the face instead, measured from the shape's lowest point over the plane.
            const math::Vector3d approach = isFrontSide == true ? -shape.TriangleNormal : shape.TriangleNormal;
            const math::Vector3d lowest = internal::GetContactSupport(convex, approach);
            const float64 coreGap = math::Dot(shape.Corners[0] - lowest, approach);
            penetration.Normal = approach;
            penetration.Separation = coreGap - internal::GetContactMargin(convex);
            penetration.FirstPoint = lowest;
            penetration.SecondPoint = lowest + approach * coreGap;
            penetration.IsApproximate = false;
            if (penetration.Separation > contactOffset)
            {
                return;
            }
            isFaceContact = true;
        }
    }
    // A face contact exists only where the shape is over the triangle: beyond its outline, the neighbouring
    // triangle or the edge itself answers.
    internal::Manifold manifold;
    internal::BuildManifold(convex, shape, penetration, contactOffset, isFaceContact, manifold);
    for (uint32 index = 0; index < manifold.Count; ++index)
    {
        manifold.Points[index].TriangleIndex = triangle.SourceIndex;
    }
    internal::AddManifold(manifold, scale, inoutPair);
}

void TriangleMeshContacts::Collect(const TriangleMesh& mesh,
                                   const internal::ContactShape& convex,
                                   float64 contactOffset,
                                   internal::PairAccumulator& inoutPair) noexcept
{
    if (mesh.IsCooked() == false)
    {
        return;
    }
    const internal::PosedBounds bounds = internal::GetPosedBounds(convex.Convex);
    const math::Vector3d center = convex.Position + bounds.CenterMesh;
    // The reach covers the contact offset plus rounding of the bounds arithmetic.
    const float64 widening =
        contactOffset + (internal::GetMaxMagnitude(center) + internal::GetMaxMagnitude(bounds.HalfExtentsMesh)) *
                            internal::TRAVERSAL_RELATIVE_ALLOWANCE;
    const Reach reach
    {
        .Center = center,
        .HalfExtents = bounds.HalfExtentsMesh + math::Vector3d{widening, widening, widening},
    };
    // Depth-first with an explicit stack: one waiting sibling per level plus the two children just pushed.
    StaticArray<uint32, MAX_TRIANGLE_MESH_HIERARCHY_DEPTH + 1> pending{};
    usize pendingCount = 0;
    pending[pendingCount++] = 0;
    while (pendingCount > 0)
    {
        const uint32 nodeIndex = pending[--pendingCount];
        const TriangleMesh::Node& node = mesh.mNodes[nodeIndex];
        if (Overlaps(reach, Widen(node.BoundsMesh.Min), Widen(node.BoundsMesh.Max)) == false)
        {
            continue;
        }
        if (node.Count == 0)
        {
            LUDUS_ASSERT(pendingCount + 2 <= pending.GetSize());
            pending[pendingCount++] = node.Right;
            pending[pendingCount++] = nodeIndex + 1;
            continue;
        }
        for (uint32 index = node.First; index < node.First + node.Count; ++index)
        {
            testTriangle(mesh.mTriangles[index], convex, reach, contactOffset, inoutPair);
        }
    }
}

void internal::CollectMeshContacts(const TriangleMesh& mesh,
                                   const ContactShape& convex,
                                   float64 contactOffset,
                                   PairAccumulator& inoutPair) noexcept
{
    TriangleMeshContacts::Collect(mesh, convex, contactOffset, inoutPair);
}
} // namespace ludus::physics::collision
