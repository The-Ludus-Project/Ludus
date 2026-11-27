#include "internal/reference_medium.h"

#include <ludus/foundation/base/core.h>

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <cstring>

// Bounded homogeneous absorption for the RT1 CPU oracle; shaders/reference_medium.slang is the GPU twin and
// repeats this float32 arithmetic in the same order.
// Thanks to Julian Fong, Magnus Wrenninge, Christopher Kulla and Ralf Habel, "Production Volume Rendering",
// SIGGRAPH 2017 course, https://doi.org/10.1145/3084873.3084907: section 2.3.1 for the closed-form
// transmittance exp(-sigma_a * d) of a homogeneous absorber, and sections 3.2 and 3.4 for tracking the medium
// across ordered boundary crossings with transmittance accumulated along light-visibility segments.
// Thanks to Sven Woop, Carsten Benthin and Ingo Wald, "Watertight Ray/Triangle Intersection", JCGT 2(1), 2013,
// section 3, https://jcgt.org/published/0002/01/05/paper.pdf, for the permuted and sheared edge test, adapted
// as in ray_bvh_traversal.slang with a direction-based tie rule for exactly zero edge functions in the manner of
// rasterization's top-left rule, so a closed boundary is crossed exactly once where a ray meets a shared edge.
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace math = foundation::math;
using Status = rhi::RasterStatus;
namespace
{
constexpr usize TRIANGLE_CORNERS = REFERENCE_MEDIUM_TRIANGLE_CORNERS;
constexpr usize AXES = 3;
constexpr float32 MAXIMUM_WORLD_COORDINATE = MAX_REFERENCE_MEDIUM_COORDINATE_WORLD;
/// One directed boundary edge keyed by the bit patterns of its two corners.
struct MediumEdge final
{
    StaticArray<uint32, 2 * AXES> Bits{};
};
bool IsEdgeLess(const MediumEdge& a, const MediumEdge& b) noexcept
{
    for (usize i = 0; i < 2 * AXES; ++i)
    {
        if (a.Bits[i] != b.Bits[i])
        {
            return a.Bits[i] < b.Bits[i];
        }
    }
    return false;
}
bool AreEdgesEqual(const MediumEdge& a, const MediumEdge& b) noexcept
{
    return IsEdgeLess(a, b) == false && IsEdgeLess(b, a) == false;
}
uint32 GetCanonicalBits(float32 value) noexcept
{
    // Adding zero turns -0 into +0, so equal coordinates always compare equal.
    const float32 canonical = value + 0.0F;
    uint32 bits = 0;
    std::memcpy(&bits, &canonical, sizeof(bits));
    return bits;
}
MediumEdge MakeEdge(math::Vector3 from, math::Vector3 to) noexcept
{
    MediumEdge edge;
    edge.Bits = {{GetCanonicalBits(from.X),
                  GetCanonicalBits(from.Y),
                  GetCanonicalBits(from.Z),
                  GetCanonicalBits(to.X),
                  GetCanonicalBits(to.Y),
                  GetCanonicalBits(to.Z)}};
    return edge;
}
bool ContainsEdge(const core::Array<MediumEdge>& sorted, const MediumEdge& edge) noexcept
{
    usize low = 0;
    usize high = sorted.GetSize();
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (IsEdgeLess(sorted[middle], edge) == true)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low < sorted.GetSize() && AreEdgesEqual(sorted[low], edge) == true;
}
bool IsCornerAdmitted(math::Vector3 corner) noexcept
{
    return math::IsFinite(corner) == true && math::Abs(corner.X) <= MAXIMUM_WORLD_COORDINATE &&
           math::Abs(corner.Y) <= MAXIMUM_WORLD_COORDINATE && math::Abs(corner.Z) <= MAXIMUM_WORLD_COORDINATE;
}
float32 GetComponent(math::Vector3 value, usize axis) noexcept
{
    return axis == 0 ? value.X : (axis == 1 ? value.Y : value.Z);
}
/// The sheared edge function first.x * second.y - first.y * second.x, evaluated in a canonical corner order
/// so the two triangles sharing an edge compute exactly negated values even with fused multiply-adds.
float32 ComputeEdgeFunction(math::Vector2 first, math::Vector2 second) noexcept
{
    const bool isSwapped = first.X > second.X || (first.X == second.X && first.Y > second.Y);
    const math::Vector2 low = isSwapped == true ? second : first;
    const math::Vector2 high = isSwapped == true ? first : second;
    const float32 value = low.X * high.Y - low.Y * high.X;
    return isSwapped == true ? -value : value;
}
/// A zero edge function admits the hit for one of the two directions of its edge. The triangle that traverses
/// the shared edge from `from` to `to` keeps it exactly when that direction's class matches its winding.
bool IsZeroEdgeAdmitted(math::Vector2 from, math::Vector2 to, bool isPositiveWinding) noexcept
{
    const bool isAscending = from.X < to.X || (from.X == to.X && from.Y < to.Y);
    return isAscending == isPositiveWinding;
}
} // namespace
usize GetReferenceMediumValidationBytes(usize triangles) noexcept
{
    return triangles * TRIANGLE_CORNERS * sizeof(MediumEdge);
}
Status ValidateReferenceMedium(const ReferenceMediumDescription& medium) noexcept
{
    const usize triangles = medium.BoundaryCornersWorld.GetSize() / TRIANGLE_CORNERS;
    if (medium.BoundaryCornersWorld.IsValid() == false ||
        medium.BoundaryCornersWorld.GetSize() % TRIANGLE_CORNERS != 0 || triangles == 0 ||
        triangles > MAX_REFERENCE_MEDIUM_TRIANGLES || math::IsFinite(medium.AbsorptionPerWorld) == false)
    {
        return Status::InvalidDescription;
    }
    for (const float32 channel :
         {medium.AbsorptionPerWorld.X, medium.AbsorptionPerWorld.Y, medium.AbsorptionPerWorld.Z})
    {
        if ((channel >= 0 && channel <= content::scene_format::MAX_MEDIUM_ABSORPTION_PER_WORLD) == false)
        {
            return Status::InvalidDescription;
        }
    }
    core::Array<MediumEdge> edges;
    const usize edgeCount = medium.BoundaryCornersWorld.GetSize();
    if (edges.TryEnsureCapacity(edgeCount) == false || edges.TryResize(edgeCount) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize triangle = 0; triangle < triangles; ++triangle)
    {
        const math::Vector3* corners = medium.BoundaryCornersWorld.GetDataOrNull() + triangle * TRIANGLE_CORNERS;
        for (usize corner = 0; corner < TRIANGLE_CORNERS; ++corner)
        {
            if (IsCornerAdmitted(corners[corner]) == false)
            {
                return Status::InvalidDescription;
            }
        }
        const math::Vector3 normal = math::Cross(corners[1] - corners[0], corners[2] - corners[0]);
        if (math::IsFinite(normal) == false || math::Dot(normal, normal) <= 0)
        {
            return Status::InvalidDescription;
        }
        for (usize corner = 0; corner < TRIANGLE_CORNERS; ++corner)
        {
            edges[triangle * TRIANGLE_CORNERS + corner] =
                MakeEdge(corners[corner], corners[(corner + 1) % TRIANGLE_CORNERS]);
        }
    }
    Sort(edges.GetData(), edges.GetSize(), IsEdgeLess);
    for (usize i = 0; i < edges.GetSize(); ++i)
    {
        // A repeated directed edge is a nonmanifold or inconsistently oriented boundary.
        if (i + 1 < edges.GetSize() && AreEdgesEqual(edges[i], edges[i + 1]) == true)
        {
            return Status::InvalidDescription;
        }
        MediumEdge reverse;
        for (usize axis = 0; axis < AXES; ++axis)
        {
            reverse.Bits[axis] = edges[i].Bits[AXES + axis];
            reverse.Bits[AXES + axis] = edges[i].Bits[axis];
        }
        if (ContainsEdge(edges, reverse) == false)
        {
            return Status::InvalidDescription; // An open boundary encloses no medium.
        }
    }
    return Status::Ready;
}
Status CopyReferenceMedium(const ReferenceMediumDescription& medium, ReferenceMediumData& outMedium) noexcept
{
    ReferenceMediumData copy;
    const usize cornerCount = medium.BoundaryCornersWorld.GetSize();
    if (copy.CornersWorld.TryEnsureCapacity(cornerCount) == false || copy.CornersWorld.TryResize(cornerCount) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize corner = 0; corner < cornerCount; ++corner)
    {
        copy.CornersWorld[corner] = medium.BoundaryCornersWorld[corner];
    }
    copy.AbsorptionPerWorld = medium.AbsorptionPerWorld;
    outMedium = Move(copy);
    return Status::Ready;
}
bool IsSameReferenceMedium(const ReferenceMediumData& retained, const ReferenceMediumDescription* mediumOrNull) noexcept
{
    if (mediumOrNull == nullptr)
    {
        return HasReferenceMedium(retained) == false;
    }
    if (retained.CornersWorld.GetSize() != mediumOrNull->BoundaryCornersWorld.GetSize() ||
        retained.AbsorptionPerWorld != mediumOrNull->AbsorptionPerWorld)
    {
        return false;
    }
    for (usize corner = 0; corner < retained.CornersWorld.GetSize(); ++corner)
    {
        if (retained.CornersWorld[corner] != mediumOrNull->BoundaryCornersWorld[corner])
        {
            return false;
        }
    }
    return true;
}
bool IntersectReferenceMediumTriangle(const math::Vector3* cornersWorld,
                                      const ReferenceMediumRay& ray,
                                      float32& outDistanceWorld) noexcept
{
    const math::Vector3 originWorld = ray.OriginWorld;
    const math::Vector3 directionWorld = ray.DirectionWorld;
    usize z = 0;
    for (usize axis = 1; axis < AXES; ++axis)
    {
        if (math::Abs(GetComponent(directionWorld, axis)) > math::Abs(GetComponent(directionWorld, z)))
        {
            z = axis;
        }
    }
    usize x = (z + 1) % AXES;
    usize y = (x + 1) % AXES;
    if (GetComponent(directionWorld, z) < 0)
    {
        const usize swapped = x;
        x = y;
        y = swapped;
    }
    const float32 shearX = GetComponent(directionWorld, x) / GetComponent(directionWorld, z);
    const float32 shearY = GetComponent(directionWorld, y) / GetComponent(directionWorld, z);
    const float32 shearZ = 1 / GetComponent(directionWorld, z);
    StaticArray<math::Vector2, TRIANGLE_CORNERS> sheared{};
    StaticArray<float32, TRIANGLE_CORNERS> depth{};
    for (usize corner = 0; corner < TRIANGLE_CORNERS; ++corner)
    {
        const float32 dz = GetComponent(cornersWorld[corner], z) - GetComponent(originWorld, z);
        sheared[corner] = {(GetComponent(cornersWorld[corner], x) - GetComponent(originWorld, x)) - shearX * dz,
                           (GetComponent(cornersWorld[corner], y) - GetComponent(originWorld, y)) - shearY * dz};
        depth[corner] = dz * shearZ;
    }
    // Edge i is opposite corner i and runs from corner i + 1 to corner i + 2 in winding order.
    const StaticArray<float32, TRIANGLE_CORNERS> edge{{ComputeEdgeFunction(sheared[2], sheared[1]),
                                                       ComputeEdgeFunction(sheared[0], sheared[2]),
                                                       ComputeEdgeFunction(sheared[1], sheared[0])}};
    const bool hasNegative = edge[0] < 0 || edge[1] < 0 || edge[2] < 0;
    const bool hasPositive = edge[0] > 0 || edge[1] > 0 || edge[2] > 0;
    if (hasNegative == true && hasPositive == true)
    {
        return false;
    }
    const float32 determinant = edge[0] + edge[1] + edge[2];
    if (determinant == 0)
    {
        return false;
    }
    for (usize i = 0; i < TRIANGLE_CORNERS; ++i)
    {
        if (edge[i] == 0 && IsZeroEdgeAdmitted(sheared[(i + 1) % TRIANGLE_CORNERS],
                                               sheared[(i + 2) % TRIANGLE_CORNERS],
                                               determinant > 0) == false)
        {
            return false;
        }
    }
    const float32 distanceWorld = (edge[0] * depth[0] + edge[1] * depth[1] + edge[2] * depth[2]) / determinant;
    if (math::IsFinite(distanceWorld) == false || distanceWorld <= 0)
    {
        return false;
    }
    outDistanceWorld = distanceWorld;
    return true;
}
bool AttenuateReferenceMediumSegment(const ReferenceMediumData& medium,
                                     const ReferenceMediumRay& ray,
                                     float32 lengthWorld,
                                     bool shouldDeriveStart,
                                     uint32& inoutInside,
                                     math::Vector3& outTransmittance) noexcept
{
    // With winding w(t) = w(0) + sum of crossing signs s_i before t, the inside length over [0, L] is
    // w(L) L - sum s_i t_i; that form keeps float32 accuracy for long escaping segments.
    int32 totalSign = 0;
    int32 segmentSign = 0;
    float32 signedDistance = 0;
    const usize triangles = medium.CornersWorld.GetSize() / TRIANGLE_CORNERS;
    for (usize triangle = 0; triangle < triangles; ++triangle)
    {
        const math::Vector3* corners = medium.CornersWorld.GetData() + triangle * TRIANGLE_CORNERS;
        float32 distanceWorld = 0;
        if (IntersectReferenceMediumTriangle(corners, ray, distanceWorld) == false)
        {
            continue;
        }
        const float32 facing =
            math::Dot(math::Cross(corners[1] - corners[0], corners[2] - corners[0]), ray.DirectionWorld);
        if (facing == 0)
        {
            continue;
        }
        // Moving against the outward normal enters the medium.
        const int32 sign = facing < 0 ? 1 : -1;
        totalSign += sign;
        if (distanceWorld < lengthWorld)
        {
            segmentSign += sign;
            signedDistance += static_cast<float32>(sign) * distanceWorld;
        }
    }
    const int32 start = shouldDeriveStart == true ? -totalSign : static_cast<int32>(inoutInside);
    const int32 end = start + segmentSign;
    if (start < 0 || start > 1 || end < 0 || end > 1)
    {
        return false;
    }
    const float32 insideWorld = math::Max(0.0F, static_cast<float32>(end) * lengthWorld - signedDistance);
    outTransmittance = {
        static_cast<float32>(math::Exp(-static_cast<float64>(medium.AbsorptionPerWorld.X * insideWorld))),
        static_cast<float32>(math::Exp(-static_cast<float64>(medium.AbsorptionPerWorld.Y * insideWorld))),
        static_cast<float32>(math::Exp(-static_cast<float64>(medium.AbsorptionPerWorld.Z * insideWorld))),
    };
    inoutInside = static_cast<uint32>(end);
    return true;
}
} // namespace ludus::graphics::renderer::internal
