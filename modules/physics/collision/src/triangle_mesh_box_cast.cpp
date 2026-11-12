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
using internal::Widen;

constexpr usize TRIANGLE_EDGE_COUNT = 3;
constexpr StaticArray<math::Vector3d, internal::AXIS_COUNT> MESH_AXES{math::Vector3d{1, 0, 0},
                                                                      math::Vector3d{0, 1, 0},
                                                                      math::Vector3d{0, 0, 1}};

/// Per-cast constants. Triangle corners are taken relative to CenterMesh.
struct BoxCastContext final
{
    math::Vector3d CenterMesh{};
    math::Vector3d HalfExtentsMesh{};
    math::Vector3d TranslationMesh{};
    math::Vector3d SlackMesh{}; ///< Absolute traversal slack per axis from the query's scale.
    InitialOverlapPolicy OverlapPolicy = InitialOverlapPolicy::Report;
};

/// One triangle's answer. Ordered by fraction, then initial overlap before a
/// touching contact, then source triangle index.
struct BoxCandidate final
{
    float64 Fraction = 1;
    math::Vector3d SeparatingAxisMesh{}; ///< Unnormalized; meaningful only without initial overlap.
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX;
    bool HasHit = false;
    bool IsInitiallyOverlapping = false;
};

bool IsBetter(const BoxCandidate& candidate, const BoxCandidate& best) noexcept
{
    if (best.HasHit == false)
    {
        return true;
    }
    if (candidate.Fraction != best.Fraction)
    {
        return candidate.Fraction < best.Fraction;
    }
    if (candidate.IsInitiallyOverlapping != best.IsInitiallyOverlapping)
    {
        return candidate.IsInitiallyOverlapping == true;
    }
    return candidate.TriangleIndex < best.TriangleIndex;
}

Status ValidateBoxCast(const BoxCastInput& input) noexcept
{
    if (math::IsFinite(input.CenterMesh) == false || math::IsFinite(input.HalfExtentsMesh) == false ||
        math::IsFinite(input.TranslationMesh) == false)
    {
        return Status::NonFiniteInput;
    }
    const bool isKnownPolicy = input.OverlapPolicy == InitialOverlapPolicy::Report ||
                               input.OverlapPolicy == InitialOverlapPolicy::Ignore ||
                               input.OverlapPolicy == InitialOverlapPolicy::Reject;
    const bool isKnownTraversal =
        input.Traversal == QueryTraversal::Hierarchy || input.Traversal == QueryTraversal::AllTriangles;
    if (isKnownPolicy == false || isKnownTraversal == false || input.HalfExtentsMesh.X <= 0 ||
        input.HalfExtentsMesh.Y <= 0 || input.HalfExtentsMesh.Z <= 0)
    {
        return Status::InvalidArgument;
    }
    if (internal::IsWithinMagnitude(input.CenterMesh, static_cast<float64>(MAX_COORDINATE_METERS)) == false ||
        internal::IsWithinMagnitude(input.HalfExtentsMesh, MAX_QUERY_DISTANCE_METERS) == false ||
        internal::IsWithinMagnitude(input.TranslationMesh, MAX_QUERY_DISTANCE_METERS) == false)
    {
        return Status::OutOfRange;
    }
    return Status::Success;
}

BoxCastContext MakeBoxCastContext(const BoxCastInput& input, const math::Aabb3& boundsMesh) noexcept
{
    const float64 scale = internal::GetMaxMagnitude(boundsMesh) + internal::GetMaxMagnitude(input.CenterMesh) +
                          internal::GetMaxMagnitude(input.TranslationMesh) +
                          internal::GetMaxMagnitude(input.HalfExtentsMesh);
    const float64 slack = scale * internal::TRAVERSAL_RELATIVE_ALLOWANCE;
    return
    {
        .CenterMesh = input.CenterMesh,
        .HalfExtentsMesh = input.HalfExtentsMesh,
        .TranslationMesh = input.TranslationMesh,
        .SlackMesh = {slack, slack, slack},
        .OverlapPolicy = input.OverlapPolicy,
    };
}

/// Conservative test of a node against every box pose over [0, fraction]:
/// false only when no such pose can touch the node bounds.
bool OverlapsSweep(const BoxCastContext& cast, const math::Aabb3& boundsMesh, float64 fraction) noexcept
{
    const math::Vector3d endMesh = cast.CenterMesh + cast.TranslationMesh * fraction;
    const math::Vector3d reachMesh = cast.HalfExtentsMesh + cast.SlackMesh;
    const math::Vector3d minimumMesh = Widen(boundsMesh.Min);
    const math::Vector3d maximumMesh = Widen(boundsMesh.Max);
    return minimumMesh.X <= math::Max(cast.CenterMesh.X, endMesh.X) + reachMesh.X &&
           maximumMesh.X >= math::Min(cast.CenterMesh.X, endMesh.X) - reachMesh.X &&
           minimumMesh.Y <= math::Max(cast.CenterMesh.Y, endMesh.Y) + reachMesh.Y &&
           maximumMesh.Y >= math::Min(cast.CenterMesh.Y, endMesh.Y) - reachMesh.Y &&
           minimumMesh.Z <= math::Max(cast.CenterMesh.Z, endMesh.Z) + reachMesh.Z &&
           maximumMesh.Z >= math::Min(cast.CenterMesh.Z, endMesh.Z) - reachMesh.Z;
}

// Thanks to David Eberly, "Intersection of Convex Objects: The Method of
// Separating Axes", Geometric Tools, sections 4 and 6, for intersecting time
// intervals on the face normals and edge cross products of two convex shapes.
// https://www.geometrictools.com/Documentation/MethodOfSeparatingAxes.pdf
// Here one shape is a stationary triangle and the other a translating AABB.
// Lifted from the Explore as Player preview (apps/editor/src/viewport_walk_collision.cpp),
// with the box half extents as a parameter instead of a fixed player size.
struct SweepInterval final
{
    float64 Enter = -1;
    float64 Exit = 1;
    math::Vector3d NormalMesh{};
};

bool ClipAxis(math::Vector3d axisMesh,
              math::Vector3d firstMesh,
              math::Vector3d secondMesh,
              math::Vector3d thirdMesh,
              math::Vector3d halfExtentsMesh,
              math::Vector3d movementMesh,
              SweepInterval& inoutInterval) noexcept
{
    if (math::LengthSquared(axisMesh) == 0)
    {
        return true;
    }
    const float64 first = math::Dot(axisMesh, firstMesh);
    const float64 second = math::Dot(axisMesh, secondMesh);
    const float64 third = math::Dot(axisMesh, thirdMesh);
    const float64 minimum = math::Min(first, math::Min(second, third));
    const float64 maximum = math::Max(first, math::Max(second, third));
    const float64 radius = math::Abs(axisMesh.X) * halfExtentsMesh.X + math::Abs(axisMesh.Y) * halfExtentsMesh.Y +
                           math::Abs(axisMesh.Z) * halfExtentsMesh.Z;
    const float64 projectedMovement = math::Dot(axisMesh, movementMesh);
    if (projectedMovement == 0)
    {
        // A stationary axis that merely touches allows parallel movement.
        return minimum < radius && maximum > -radius;
    }
    const float64 firstTime = (minimum - radius) / projectedMovement;
    const float64 lastTime = (maximum + radius) / projectedMovement;
    const float64 enter = math::Min(firstTime, lastTime);
    if (enter > inoutInterval.Enter)
    {
        inoutInterval.Enter = enter;
        inoutInterval.NormalMesh = projectedMovement > 0 ? -axisMesh : axisMesh;
    }
    inoutInterval.Exit = math::Min(inoutInterval.Exit, math::Max(firstTime, lastTime));
    return inoutInterval.Exit > 0 && inoutInterval.Enter < inoutInterval.Exit;
}

/// One triangle over the whole translation. The arithmetic never depends on
/// the current best hit, so hierarchy and brute-force traversal agree.
bool CastTriangle(const BoxCastContext& cast,
                  math::Vector3 firstCornerMesh,
                  math::Vector3 secondCornerMesh,
                  math::Vector3 thirdCornerMesh,
                  BoxCandidate& outCandidate) noexcept
{
    const math::Vector3d firstMesh = Widen(firstCornerMesh) - cast.CenterMesh;
    const math::Vector3d secondMesh = Widen(secondCornerMesh) - cast.CenterMesh;
    const math::Vector3d thirdMesh = Widen(thirdCornerMesh) - cast.CenterMesh;
    const StaticArray<math::Vector3d, TRIANGLE_EDGE_COUNT> edgesMesh{secondMesh - firstMesh,
                                                                     thirdMesh - secondMesh,
                                                                     firstMesh - thirdMesh};
    const math::Vector3d faceMesh = math::Cross(edgesMesh[0], edgesMesh[1]);
    SweepInterval interval;
    if (ClipAxis(faceMesh, firstMesh, secondMesh, thirdMesh, cast.HalfExtentsMesh, cast.TranslationMesh, interval) ==
        false)
    {
        return false;
    }
    for (const math::Vector3d axisMesh : MESH_AXES)
    {
        if (ClipAxis(axisMesh,
                     firstMesh,
                     secondMesh,
                     thirdMesh,
                     cast.HalfExtentsMesh,
                     cast.TranslationMesh,
                     interval) == false)
        {
            return false;
        }
        for (const math::Vector3d edgeMesh : edgesMesh)
        {
            if (ClipAxis(math::Cross(axisMesh, edgeMesh),
                         firstMesh,
                         secondMesh,
                         thirdMesh,
                         cast.HalfExtentsMesh,
                         cast.TranslationMesh,
                         interval) == false)
            {
                return false;
            }
        }
    }
    outCandidate.HasHit = true;
    outCandidate.IsInitiallyOverlapping = interval.Enter < 0;
    // A contact entered at exactly zero may carry a negative zero; report +0.
    outCandidate.Fraction = interval.Enter > 0 ? interval.Enter : 0.0;
    outCandidate.SeparatingAxisMesh =
        outCandidate.IsInitiallyOverlapping == true ? math::Vector3d{} : interval.NormalMesh;
    return true;
}
} // namespace

/// Closest box cast traversal for TriangleMesh::CastBoxClosest.
class TriangleMeshBoxCaster final
{
public:
    static Status Cast(const TriangleMesh& mesh, const BoxCastInput& input, BoxCastResult& outResult) noexcept;

private:
    /// Visit outcome: Reject stops the cast as soon as any initial overlap is found.
    enum class Visit : uint8
    {
        Continue,
        Rejected,
    };

    static Visit
    testTriangle(const BoxCastContext& cast, const TriangleMesh::Triangle& triangle, BoxCandidate& inoutBest) noexcept;
    static Visit traverse(const TriangleMesh& mesh, const BoxCastContext& cast, BoxCandidate& inoutBest) noexcept;
    static BoxCastResult makeResult(const BoxCastContext& cast, const BoxCandidate& best) noexcept;
};

TriangleMeshBoxCaster::Visit TriangleMeshBoxCaster::testTriangle(const BoxCastContext& cast,
                                                                 const TriangleMesh::Triangle& triangle,
                                                                 BoxCandidate& inoutBest) noexcept
{
    BoxCandidate candidate{ .TriangleIndex = triangle.SourceIndex };
    if (CastTriangle(cast, triangle.FirstMesh, triangle.SecondMesh, triangle.ThirdMesh, candidate) == false)
    {
        return Visit::Continue;
    }
    if (candidate.IsInitiallyOverlapping == true)
    {
        if (cast.OverlapPolicy == InitialOverlapPolicy::Reject)
        {
            return Visit::Rejected;
        }
        if (cast.OverlapPolicy == InitialOverlapPolicy::Ignore)
        {
            return Visit::Continue;
        }
    }
    if (IsBetter(candidate, inoutBest) == true)
    {
        inoutBest = candidate;
    }
    return Visit::Continue;
}

TriangleMeshBoxCaster::Visit
TriangleMeshBoxCaster::traverse(const TriangleMesh& mesh, const BoxCastContext& cast, BoxCandidate& inoutBest) noexcept
{
    // Preorder traversal keeps at most one deferred sibling per level.
    StaticArray<uint32, MAX_TRIANGLE_MESH_HIERARCHY_DEPTH + 1> pending{};
    usize pendingCount = 0;
    pending[pendingCount++] = 0;
    while (pendingCount > 0)
    {
        const uint32 nodeIndex = pending[--pendingCount];
        const TriangleMesh::Node& node = mesh.mNodes[nodeIndex];
        const float64 fraction = inoutBest.HasHit == true ? inoutBest.Fraction : 1.0;
        if (OverlapsSweep(cast, node.BoundsMesh, fraction) == false)
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
            if (testTriangle(cast, mesh.mTriangles[index], inoutBest) == Visit::Rejected)
            {
                return Visit::Rejected;
            }
        }
    }
    return Visit::Continue;
}

BoxCastResult TriangleMeshBoxCaster::makeResult(const BoxCastContext& cast, const BoxCandidate& best) noexcept
{
    if (best.HasHit == false)
    {
        return {};
    }
    BoxCastResult result;
    result.HasHit = true;
    result.Fraction = best.Fraction;
    result.DistanceMeters = best.Fraction * math::Length(cast.TranslationMesh);
    result.HasStartedOverlapping = best.IsInitiallyOverlapping;
    result.HasNormal = best.IsInitiallyOverlapping == false &&
                       internal::TryNormalize(best.SeparatingAxisMesh, result.NormalMesh) == true;
    result.TriangleIndex = best.TriangleIndex;
    return result;
}

Status
TriangleMeshBoxCaster::Cast(const TriangleMesh& mesh, const BoxCastInput& input, BoxCastResult& outResult) noexcept
{
    if (mesh.IsCooked() == false)
    {
        return Status::NoGeometry;
    }
    const Status validation = ValidateBoxCast(input);
    if (validation != Status::Success)
    {
        return validation;
    }
    const BoxCastContext cast = MakeBoxCastContext(input, mesh.GetBoundsMesh());
    BoxCandidate best;
    Visit visit = Visit::Continue;
    if (input.Traversal == QueryTraversal::AllTriangles)
    {
        for (const TriangleMesh::Triangle& triangle : mesh.mTriangles)
        {
            visit = testTriangle(cast, triangle, best);
            if (visit == Visit::Rejected)
            {
                break;
            }
        }
    }
    else
    {
        visit = traverse(mesh, cast, best);
    }
    if (visit == Visit::Rejected)
    {
        return Status::InitialOverlap;
    }
    outResult = makeResult(cast, best);
    return Status::Success;
}

Status TriangleMesh::CastBoxClosest(const BoxCastInput& input, BoxCastResult& outResult) const noexcept
{
    return TriangleMeshBoxCaster::Cast(*this, input, outResult);
}
} // namespace ludus::physics::collision
