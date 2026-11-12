#include <ludus/physics/collision/triangle_mesh.h>

#include "internal/mesh_arithmetic.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision
{
namespace
{
namespace math = foundation::math;
using foundation::StaticArray;
using internal::GetComponent;
using internal::Widen;

/// Per-ray constants of the watertight test and of the slab test.
struct RayContext final
{
    math::Vector3d OriginMesh{};
    math::Vector3d DirectionMesh{};
    math::Vector3d InverseDirectionMesh{};
    uint32 AxisX = 0;
    uint32 AxisY = 1;
    uint32 AxisZ = internal::Z_AXIS;
    float64 ShearX = 0;
    float64 ShearY = 0;
    float64 ShearZ = 1;
    float64 MaxDistance = 0;
    float64 SlackMeters = 0; ///< Absolute traversal slack from the query's scale.
    TriangleFacePolicy FacePolicy = TriangleFacePolicy::TwoSided;
};

/// One triangle's answer, ordered by distance then source triangle index.
struct RayCandidate final
{
    float64 Distance = 0;
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX; ///< Source index; the ordering key.
    uint32 StoredIndex = 0;                        ///< Position in the cooked triangle array.
    bool HasHit = false;
};

bool IsBetter(const RayCandidate& candidate, const RayCandidate& best) noexcept
{
    if (best.HasHit == false)
    {
        return true;
    }
    return candidate.Distance < best.Distance ||
           (candidate.Distance == best.Distance && candidate.TriangleIndex < best.TriangleIndex);
}

Status ValidateRay(const RaycastInput& input) noexcept
{
    if (math::IsFinite(input.OriginMesh) == false || math::IsFinite(input.DirectionMesh) == false ||
        math::IsFinite(input.MaxDistanceMeters) == false)
    {
        return Status::NonFiniteInput;
    }
    const bool isKnownFacePolicy = input.FacePolicy == TriangleFacePolicy::TwoSided ||
                                   input.FacePolicy == TriangleFacePolicy::FrontFacesOnly ||
                                   input.FacePolicy == TriangleFacePolicy::BackFacesOnly;
    const bool isKnownTraversal =
        input.Traversal == QueryTraversal::Hierarchy || input.Traversal == QueryTraversal::AllTriangles;
    if (isKnownFacePolicy == false || isKnownTraversal == false || input.MaxDistanceMeters <= 0 ||
        math::Abs(math::LengthSquared(input.DirectionMesh) - 1) > UNIT_DIRECTION_SQUARED_TOLERANCE)
    {
        return Status::InvalidArgument;
    }
    if (internal::IsWithinMagnitude(input.OriginMesh, static_cast<float64>(MAX_COORDINATE_METERS)) == false ||
        input.MaxDistanceMeters > MAX_QUERY_DISTANCE_METERS)
    {
        return Status::OutOfRange;
    }
    return Status::Success;
}

// Thanks to Sven Woop, Carsten Benthin and Ingo Wald, "Watertight Ray/Triangle
// Intersection", Journal of Computer Graphics Techniques 2(1), 2013, pages
// 65-82, https://jcgt.org/published/0002/01/05/, for the shear into a ray
// space with the dominant axis as z and the edge functions whose exact sign
// antisymmetry makes shared edges and vertices watertight. Ludus evaluates
// them in float64 from float32 corners, so the paper's double-precision
// fallback for zero edge functions is already the working precision.
RayContext MakeRayContext(const RaycastInput& input, const math::Aabb3& boundsMesh) noexcept
{
    RayContext ray;
    ray.OriginMesh = input.OriginMesh;
    ray.DirectionMesh = input.DirectionMesh;
    ray.MaxDistance = input.MaxDistanceMeters;
    ray.FacePolicy = input.FacePolicy;
    const math::Vector3d magnitude{math::Abs(input.DirectionMesh.X),
                                   math::Abs(input.DirectionMesh.Y),
                                   math::Abs(input.DirectionMesh.Z)};
    ray.AxisZ = magnitude.X > magnitude.Y ? (magnitude.X > magnitude.Z ? 0 : internal::Z_AXIS)
                                          : (magnitude.Y > magnitude.Z ? 1 : internal::Z_AXIS);
    ray.AxisX = (ray.AxisZ + 1) % internal::AXIS_COUNT;
    ray.AxisY = (ray.AxisX + 1) % internal::AXIS_COUNT;
    const float64 dominant = GetComponent(input.DirectionMesh, ray.AxisZ);
    if (dominant < 0)
    {
        // Swapping keeps the winding, and so the edge-function signs, of the triangle.
        const uint32 saved = ray.AxisX;
        ray.AxisX = ray.AxisY;
        ray.AxisY = saved;
    }
    ray.ShearX = GetComponent(input.DirectionMesh, ray.AxisX) / dominant;
    ray.ShearY = GetComponent(input.DirectionMesh, ray.AxisY) / dominant;
    ray.ShearZ = 1 / dominant;
    ray.InverseDirectionMesh = {input.DirectionMesh.X == 0 ? 0 : 1 / input.DirectionMesh.X,
                                input.DirectionMesh.Y == 0 ? 0 : 1 / input.DirectionMesh.Y,
                                input.DirectionMesh.Z == 0 ? 0 : 1 / input.DirectionMesh.Z};
    const float64 scale =
        math::Max(internal::GetMaxMagnitude(input.OriginMesh), internal::GetMaxMagnitude(boundsMesh)) +
        input.MaxDistanceMeters;
    ray.SlackMeters = scale * internal::TRAVERSAL_RELATIVE_ALLOWANCE;
    return ray;
}

float64 RoundUpBound(const RayContext& ray, float64 value) noexcept
{
    return value + math::Abs(value) * internal::TRAVERSAL_RELATIVE_ALLOWANCE + ray.SlackMeters;
}

float64 RoundDownBound(const RayContext& ray, float64 value) noexcept
{
    return value - math::Abs(value) * internal::TRAVERSAL_RELATIVE_ALLOWANCE - ray.SlackMeters;
}

/// Conservative slab test: false only when the box provably lies outside
/// [0, limit] along the ray. A zero direction component is tested exactly.
bool IntersectsBounds(const RayContext& ray, const math::Aabb3& boundsMesh, float64 limit, float64& outEntry) noexcept
{
    float64 entry = 0;
    float64 exit = RoundUpBound(ray, limit);
    for (uint32 axis = 0; axis < internal::AXIS_COUNT; ++axis)
    {
        const float64 origin = GetComponent(ray.OriginMesh, axis);
        const float64 minimum = GetComponent(boundsMesh.Min, axis);
        const float64 maximum = GetComponent(boundsMesh.Max, axis);
        if (GetComponent(ray.DirectionMesh, axis) == 0)
        {
            if (origin < minimum || origin > maximum)
            {
                return false;
            }
            continue;
        }
        const float64 inverse = GetComponent(ray.InverseDirectionMesh, axis);
        const float64 firstTime = (minimum - origin) * inverse;
        const float64 lastTime = (maximum - origin) * inverse;
        entry = math::Max(entry, RoundDownBound(ray, math::Min(firstTime, lastTime)));
        exit = math::Min(exit, RoundUpBound(ray, math::Max(firstTime, lastTime)));
    }
    outEntry = entry;
    return entry <= exit;
}

/// One triangle against the full distance limit. The arithmetic never depends
/// on the current best hit, so hierarchy and brute-force traversal agree.
bool IntersectTriangle(const RayContext& ray, const internal::TriangleCorners& corners, float64& outDistance) noexcept
{
    const math::Vector3d first = Widen(corners.FirstMesh) - ray.OriginMesh;
    const math::Vector3d second = Widen(corners.SecondMesh) - ray.OriginMesh;
    const math::Vector3d third = Widen(corners.ThirdMesh) - ray.OriginMesh;
    const float64 firstZ = GetComponent(first, ray.AxisZ);
    const float64 secondZ = GetComponent(second, ray.AxisZ);
    const float64 thirdZ = GetComponent(third, ray.AxisZ);
    const float64 firstX = GetComponent(first, ray.AxisX) - ray.ShearX * firstZ;
    const float64 firstY = GetComponent(first, ray.AxisY) - ray.ShearY * firstZ;
    const float64 secondX = GetComponent(second, ray.AxisX) - ray.ShearX * secondZ;
    const float64 secondY = GetComponent(second, ray.AxisY) - ray.ShearY * secondZ;
    const float64 thirdX = GetComponent(third, ray.AxisX) - ray.ShearX * thirdZ;
    const float64 thirdY = GetComponent(third, ray.AxisY) - ray.ShearY * thirdZ;
    const float64 edgeU = thirdX * secondY - thirdY * secondX;
    const float64 edgeV = firstX * thirdY - firstY * thirdX;
    const float64 edgeW = secondX * firstY - secondY * firstX;
    const bool hasNegative = edgeU < 0 || edgeV < 0 || edgeW < 0;
    const bool hasPositive = edgeU > 0 || edgeV > 0 || edgeW > 0;
    if (hasNegative == true && hasPositive == true)
    {
        return false;
    }
    const float64 determinant = edgeU + edgeV + edgeW;
    if (determinant == 0)
    {
        return false;
    }
    const float64 scaled =
        edgeU * (ray.ShearZ * firstZ) + edgeV * (ray.ShearZ * secondZ) + edgeW * (ray.ShearZ * thirdZ);
    const float64 sign = determinant < 0 ? -1.0 : 1.0;
    const float64 positiveScaled = scaled * sign;
    const float64 positiveDeterminant = determinant * sign;
    if (positiveScaled < 0 || positiveScaled > ray.MaxDistance * positiveDeterminant)
    {
        return false;
    }
    // A hit at the origin may carry a negative zero; report +0.
    const float64 distance = positiveScaled / positiveDeterminant;
    outDistance = distance > 0 ? math::Min(distance, ray.MaxDistance) : 0.0;
    return true;
}

math::Vector3d ComputeWindingNormal(const internal::TriangleCorners& corners) noexcept
{
    const math::Vector3d normal = math::Cross(Widen(corners.SecondMesh) - Widen(corners.FirstMesh),
                                              Widen(corners.ThirdMesh) - Widen(corners.FirstMesh));
    math::Vector3d unit{};
    // Cooking rejects or skips degenerate triangles, so the normal is nonzero.
    const bool isNormalized = internal::TryNormalize(normal, unit);
    LUDUS_ASSERT(isNormalized == true);
    (void)isNormalized;
    return unit;
}

bool IsFaceAdmitted(TriangleFacePolicy policy, float64 alignment) noexcept
{
    switch (policy)
    {
        case TriangleFacePolicy::FrontFacesOnly:
            return alignment < 0;
        case TriangleFacePolicy::BackFacesOnly:
            return alignment > 0;
        case TriangleFacePolicy::TwoSided:
            break;
    }
    return true;
}
} // namespace

/// Closest raycast traversal for TriangleMesh::RaycastClosest.
class TriangleMeshRaycaster final
{
public:
    static Status Cast(const TriangleMesh& mesh, const RaycastInput& input, RaycastResult& outResult) noexcept;

private:
    struct PendingNode final
    {
        uint32 Node = 0;
        float64 Entry = 0;
    };

    static void
    testTriangle(const TriangleMesh& mesh, const RayContext& ray, uint32 stored, RayCandidate& inoutBest) noexcept;
    static void testLeaf(const TriangleMesh& mesh,
                         const RayContext& ray,
                         const TriangleMesh::Node& node,
                         RayCandidate& inoutBest) noexcept;
    static void traverse(const TriangleMesh& mesh, const RayContext& ray, RayCandidate& inoutBest) noexcept;
    static RaycastResult makeResult(const TriangleMesh& mesh, const RayContext& ray, const RayCandidate& best) noexcept;
};

void TriangleMeshRaycaster::testTriangle(const TriangleMesh& mesh,
                                         const RayContext& ray,
                                         uint32 stored,
                                         RayCandidate& inoutBest) noexcept
{
    const TriangleMesh::Triangle& triangle = mesh.mTriangles[stored];
    const internal::TriangleCorners corners
    {
        .FirstMesh = triangle.FirstMesh,
        .SecondMesh = triangle.SecondMesh,
        .ThirdMesh = triangle.ThirdMesh,
    };
    RayCandidate candidate{ .TriangleIndex = triangle.SourceIndex, .StoredIndex = stored, .HasHit = true };
    if (IntersectTriangle(ray, corners, candidate.Distance) == false || IsBetter(candidate, inoutBest) == false)
    {
        return;
    }
    if (ray.FacePolicy != TriangleFacePolicy::TwoSided)
    {
        const math::Vector3d normal = ComputeWindingNormal(corners);
        if (IsFaceAdmitted(ray.FacePolicy, math::Dot(ray.DirectionMesh, normal)) == false)
        {
            return;
        }
    }
    inoutBest = candidate;
}

void TriangleMeshRaycaster::testLeaf(const TriangleMesh& mesh,
                                     const RayContext& ray,
                                     const TriangleMesh::Node& node,
                                     RayCandidate& inoutBest) noexcept
{
    for (uint32 index = node.First; index < node.First + node.Count; ++index)
    {
        testTriangle(mesh, ray, index, inoutBest);
    }
}

void TriangleMeshRaycaster::traverse(const TriangleMesh& mesh, const RayContext& ray, RayCandidate& inoutBest) noexcept
{
    // Preorder traversal keeps at most one deferred sibling per level.
    StaticArray<PendingNode, MAX_TRIANGLE_MESH_HIERARCHY_DEPTH + 1> pending{};
    usize pendingCount = 0;
    float64 rootEntry = 0;
    if (IntersectsBounds(ray, mesh.mNodes[0].BoundsMesh, ray.MaxDistance, rootEntry) == false)
    {
        return;
    }
    pending[pendingCount++] = { .Node = 0, .Entry = rootEntry };
    while (pendingCount > 0)
    {
        const PendingNode visit = pending[--pendingCount];
        const float64 limit = inoutBest.HasHit == true ? inoutBest.Distance : ray.MaxDistance;
        if (visit.Entry > RoundUpBound(ray, limit))
        {
            continue;
        }
        const TriangleMesh::Node& node = mesh.mNodes[visit.Node];
        if (node.Count > 0)
        {
            testLeaf(mesh, ray, node, inoutBest);
            continue;
        }
        const uint32 leftNode = visit.Node + 1;
        const uint32 rightNode = node.Right;
        float64 leftEntry = 0;
        float64 rightEntry = 0;
        const bool hasLeft = IntersectsBounds(ray, mesh.mNodes[leftNode].BoundsMesh, limit, leftEntry);
        const bool hasRight = IntersectsBounds(ray, mesh.mNodes[rightNode].BoundsMesh, limit, rightEntry);
        LUDUS_ASSERT(pendingCount + 2 <= pending.GetSize());
        // Push the farther child first so the nearer one is visited next.
        const bool isRightFarther = rightEntry >= leftEntry;
        if (hasRight == true && isRightFarther == true)
        {
            pending[pendingCount++] = { .Node = rightNode, .Entry = rightEntry };
        }
        if (hasLeft == true)
        {
            pending[pendingCount++] = { .Node = leftNode, .Entry = leftEntry };
        }
        if (hasRight == true && isRightFarther == false)
        {
            pending[pendingCount++] = { .Node = rightNode, .Entry = rightEntry };
        }
    }
}

RaycastResult
TriangleMeshRaycaster::makeResult(const TriangleMesh& mesh, const RayContext& ray, const RayCandidate& best) noexcept
{
    if (best.HasHit == false)
    {
        return {};
    }
    const TriangleMesh::Triangle& triangle = mesh.mTriangles[best.StoredIndex];
    RaycastResult result;
    result.NormalMesh = ComputeWindingNormal(
        { .FirstMesh = triangle.FirstMesh, .SecondMesh = triangle.SecondMesh, .ThirdMesh = triangle.ThirdMesh });
    result.HasHit = true;
    result.DistanceMeters = best.Distance;
    result.PositionMesh = ray.OriginMesh + ray.DirectionMesh * best.Distance;
    result.TriangleIndex = best.TriangleIndex;
    result.IsFrontFace = math::Dot(ray.DirectionMesh, result.NormalMesh) < 0;
    return result;
}

Status
TriangleMeshRaycaster::Cast(const TriangleMesh& mesh, const RaycastInput& input, RaycastResult& outResult) noexcept
{
    if (mesh.IsCooked() == false)
    {
        return Status::NoGeometry;
    }
    const Status validation = ValidateRay(input);
    if (validation != Status::Success)
    {
        return validation;
    }
    const RayContext ray = MakeRayContext(input, mesh.GetBoundsMesh());
    RayCandidate best;
    if (input.Traversal == QueryTraversal::AllTriangles)
    {
        const uint32 triangleCount = static_cast<uint32>(mesh.mTriangles.GetSize());
        for (uint32 stored = 0; stored < triangleCount; ++stored)
        {
            testTriangle(mesh, ray, stored, best);
        }
    }
    else
    {
        traverse(mesh, ray, best);
    }
    outResult = makeResult(mesh, ray, best);
    return Status::Success;
}

Status TriangleMesh::RaycastClosest(const RaycastInput& input, RaycastResult& outResult) const noexcept
{
    return TriangleMeshRaycaster::Cast(*this, input, outResult);
}
} // namespace ludus::physics::collision
