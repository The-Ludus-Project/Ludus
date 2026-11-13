#include <ludus/physics/collision/triangle_mesh.h>

#include "internal/convex_cast.h"
#include "internal/convex_support.h"
#include "internal/gjk_simplex.h"
#include "internal/mesh_arithmetic.h"
#include "internal/shape_validation.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

// The general convex cast: a GJK ray cast of the origin along the translation
// against the Minkowski difference of each triangle and the posed shape.
//
// Thanks to Gino van den Bergen, "Ray Casting against General Convex Objects
// with Application to Continuous Collision Detection", 2004 (also Section 4.3
// of "Collision Detection in Interactive 3D Environments", Morgan Kaufmann,
// 2003), for the ray cast that advances along the ray by the separating-plane
// distance while it refines the GJK simplex, and for casting rounded shapes as
// a core plus a margin. Ludus adds an explicit initial-overlap classification,
// a scale-aware contact tolerance and bounded iterations with a reported
// failure, and keeps the per-triangle arithmetic independent of the best hit
// so hierarchy and brute-force traversal agree.

namespace ludus::physics::collision
{
namespace
{
namespace math = foundation::math;
using foundation::StaticArray;
using internal::Widen;

constexpr usize TRIANGLE_CORNER_COUNT = 3;
constexpr float64 CENTROID_WEIGHT = 1.0 / static_cast<float64>(TRIANGLE_CORNER_COUNT);
constexpr float64 HALF = 0.5;
/// A starting core separation below this fraction of the contact tolerance has no reliable direction: it is
/// float64 rounding of an origin that lies on the simplex. The cores meet, so the start counts as overlapping.
/// The ratio sits far above rounding (about 1e-16 of the scale) and far below the tolerance (1e-9 of it).
constexpr float64 DEGENERATE_SEPARATION_RATIO = 1.0e-3;

/// Per-cast constants. Triangle corners are taken relative to PositionMesh.
struct ConvexCastContext final
{
    internal::PosedConvex Shape{};
    math::Vector3d PositionMesh{};
    math::Vector3d TranslationMesh{};
    math::Vector3d BoundsCenterMesh{}; ///< Centre of the posed shape bounds, relative to the position.
    math::Vector3d ReachMesh{};        ///< Posed half extents widened by the tolerance and traversal slack.
    float64 ToleranceMeters = 0.0;
    InitialOverlapPolicy OverlapPolicy = InitialOverlapPolicy::Report;
    bool ShouldStopAtFirstHit = false; ///< Any-hit search; never under InitialOverlapPolicy::Reject.
};

/// One triangle's answer. Ordered by fraction, then initial overlap before a
/// touching contact, then source triangle index.
struct ConvexCandidate final
{
    float64 Fraction = 1;
    math::Vector3d NormalMesh{};  ///< Unnormalized; meaningful only without initial overlap.
    math::Vector3d WitnessMesh{}; ///< Relative to the position; meaningful only when HasWitness.
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX;
    bool HasHit = false;
    bool IsInitiallyOverlapping = false;
    bool HasWitness = false;
};

enum class TriangleOutcome : uint8
{
    Miss,
    Hit,
    NotConverged,
};

/// How the per-triangle iteration ended.
enum class CastStop : uint8
{
    Miss,
    Overlap, ///< The shape starts provably inside the triangle's tolerance.
    Touch,   ///< The shape starts within the tolerance of the triangle without overlapping it.
    Contact, ///< The translating shape reaches the triangle at the current fraction.
};

bool IsBetter(const ConvexCandidate& candidate, const ConvexCandidate& best) noexcept
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

bool IsOrientationFinite(math::Quaternion orientation) noexcept
{
    return math::IsFinite(orientation.X) == true && math::IsFinite(orientation.Y) == true &&
           math::IsFinite(orientation.Z) == true && math::IsFinite(orientation.W) == true;
}

float64 GetNormSquared(math::Quaternion orientation) noexcept
{
    const float64 x = static_cast<float64>(orientation.X);
    const float64 y = static_cast<float64>(orientation.Y);
    const float64 z = static_cast<float64>(orientation.Z);
    const float64 w = static_cast<float64>(orientation.W);
    return x * x + y * y + z * z + w * w;
}

} // namespace

Status internal::ValidateConvexCast(const ConvexCastInput& input) noexcept
{
    if (internal::IsKnownKind(input.Shape.Kind) == false)
    {
        return Status::InvalidArgument;
    }
    if (math::IsFinite(input.PositionMesh) == false || math::IsFinite(input.TranslationMesh) == false ||
        IsOrientationFinite(input.OrientationMesh) == false || internal::AreShapeFieldsFinite(input.Shape) == false)
    {
        return Status::NonFiniteInput;
    }
    const bool isKnownPolicy = input.OverlapPolicy == InitialOverlapPolicy::Report ||
                               input.OverlapPolicy == InitialOverlapPolicy::Ignore ||
                               input.OverlapPolicy == InitialOverlapPolicy::Reject;
    const bool isKnownTraversal =
        input.Traversal == QueryTraversal::Hierarchy || input.Traversal == QueryTraversal::AllTriangles;
    const bool isUnitOrientation =
        math::Abs(GetNormSquared(input.OrientationMesh) - 1.0) <= UNIT_QUATERNION_SQUARED_TOLERANCE;
    if (isKnownPolicy == false || isKnownTraversal == false || isUnitOrientation == false ||
        internal::AreShapeFieldsAdmitted(input.Shape) == false)
    {
        return Status::InvalidArgument;
    }
    if (internal::IsWithinMagnitude(input.PositionMesh, static_cast<float64>(MAX_COORDINATE_METERS)) == false ||
        internal::IsWithinMagnitude(input.TranslationMesh, MAX_QUERY_DISTANCE_METERS) == false ||
        internal::AreShapeFieldsInRange(input.Shape) == false)
    {
        return Status::OutOfRange;
    }
    return Status::Success;
}

namespace
{

/// Bounds of the target geometry in the cast's frame, for the tolerance scale.
struct TargetBounds final
{
    math::Vector3d Minimum{};
    math::Vector3d Maximum{};
};

TargetBounds ToTargetBounds(const math::Aabb3& boundsMesh) noexcept
{
    return { .Minimum = Widen(boundsMesh.Min), .Maximum = Widen(boundsMesh.Max) };
}

ConvexCastContext MakeConvexCastContext(const internal::PosedConvexCast& request,
                                        const TargetBounds& boundsMesh) noexcept
{
    ConvexCastContext cast;
    cast.Shape = request.Shape;
    cast.PositionMesh = request.PositionMesh;
    cast.TranslationMesh = request.TranslationMesh;
    cast.OverlapPolicy = request.OverlapPolicy;
    cast.ShouldStopAtFirstHit =
        request.ShouldStopAtFirstHit == true && request.OverlapPolicy != InitialOverlapPolicy::Reject;
    const internal::PosedBounds bounds = internal::GetPosedBounds(cast.Shape);
    cast.BoundsCenterMesh = bounds.CenterMesh;
    const float64 meshScale = math::Max(internal::GetMaxMagnitude(boundsMesh.Minimum - request.PositionMesh),
                                        internal::GetMaxMagnitude(boundsMesh.Maximum - request.PositionMesh));
    const float64 scale = meshScale + internal::GetMaxMagnitude(request.TranslationMesh) +
                          internal::GetMaxMagnitude(bounds.HalfExtentsMesh) +
                          internal::GetMaxMagnitude(bounds.CenterMesh);
    cast.ToleranceMeters = scale * CONVEX_CAST_RELATIVE_TOLERANCE;
    const float64 widening = cast.ToleranceMeters + scale * internal::TRAVERSAL_RELATIVE_ALLOWANCE;
    cast.ReachMesh = bounds.HalfExtentsMesh + math::Vector3d{widening, widening, widening};
    return cast;
}

/// Conservative test of a node against every shape pose over [0, fraction]:
/// false only when no such pose can come within the tolerance of the node bounds.
bool OverlapsSweep(const ConvexCastContext& cast, const math::Aabb3& boundsMesh, float64 fraction) noexcept
{
    const math::Vector3d startMesh = cast.PositionMesh + cast.BoundsCenterMesh;
    const math::Vector3d endMesh = startMesh + cast.TranslationMesh * fraction;
    const math::Vector3d reachMesh = cast.ReachMesh;
    const math::Vector3d minimumMesh = Widen(boundsMesh.Min);
    const math::Vector3d maximumMesh = Widen(boundsMesh.Max);
    return minimumMesh.X <= math::Max(startMesh.X, endMesh.X) + reachMesh.X &&
           maximumMesh.X >= math::Min(startMesh.X, endMesh.X) - reachMesh.X &&
           minimumMesh.Y <= math::Max(startMesh.Y, endMesh.Y) + reachMesh.Y &&
           maximumMesh.Y >= math::Min(startMesh.Y, endMesh.Y) - reachMesh.Y &&
           minimumMesh.Z <= math::Max(startMesh.Z, endMesh.Z) + reachMesh.Z &&
           maximumMesh.Z >= math::Min(startMesh.Z, endMesh.Z) - reachMesh.Z;
}

/// What a shape is cast against: a triangle, or a convex core rounded by a margin. Positions are relative to the
/// cast's starting position.
struct CastTarget final
{
    StaticArray<math::Vector3d, TRIANGLE_CORNER_COUNT> CornersMesh{}; ///< Triangle corners; unused for convex.
    const internal::PosedConvex* ConvexOrNull = nullptr;              ///< Convex target, or null for a triangle.
    math::Vector3d ConvexOffsetMesh{};                                ///< Convex target origin.
    math::Vector3d InteriorMesh{};                                    ///< A point of the target core.
    float64 MarginMeters = 0.0;                                       ///< Convex target rounding radius.
};

/// The triangle corner farthest along directionMesh; ties keep the lowest corner.
math::Vector3d GetTriangleSupport(const StaticArray<math::Vector3d, TRIANGLE_CORNER_COUNT>& cornersMesh,
                                  math::Vector3d directionMesh) noexcept
{
    usize best = 0;
    float64 bestProjection = math::Dot(cornersMesh[0], directionMesh);
    for (usize corner = 1; corner < TRIANGLE_CORNER_COUNT; ++corner)
    {
        const float64 projection = math::Dot(cornersMesh[corner], directionMesh);
        if (projection > bestProjection)
        {
            bestProjection = projection;
            best = corner;
        }
    }
    return cornersMesh[best];
}

math::Vector3d GetTargetSupport(const CastTarget& target, math::Vector3d directionMesh) noexcept
{
    if (target.ConvexOrNull == nullptr)
    {
        return GetTriangleSupport(target.CornersMesh, directionMesh);
    }
    return target.ConvexOffsetMesh + internal::GetCoreSupport(*target.ConvexOrNull, directionMesh);
}

/// The target's surface point under a core witness: a rounded target's surface lies its margin along the
/// separating direction, from the target toward the shape.
math::Vector3d GetSurfaceWitness(math::Vector3d coreWitnessMesh, float64 margin, math::Vector3d separationMesh) noexcept
{
    math::Vector3d unitMesh;
    if (margin == 0.0 || internal::TryNormalize(separationMesh, unitMesh) == false)
    {
        return coreWitnessMesh;
    }
    return coreWitnessMesh + unitMesh * margin;
}

bool HasDifference(const internal::Simplex& simplex, math::Vector3d differenceMesh) noexcept
{
    for (usize index = 0; index < simplex.Count; ++index)
    {
        if (simplex.Vertices[index].DifferenceMesh == differenceMesh)
        {
            return true;
        }
    }
    return false;
}

/// One target over the whole translation. The arithmetic never depends on
/// the current best hit, so hierarchy and brute-force traversal agree.
TriangleOutcome
CastAgainst(const ConvexCastContext& cast, const CastTarget& target, ConvexCandidate& outCandidate) noexcept
{
    const math::Vector3d translationMesh = cast.TranslationMesh;
    // Both rounding radii add: the cores are cast against each other at their combined margin.
    const float64 margin = cast.Shape.MarginMeters + target.MarginMeters;
    const float64 tolerance = cast.ToleranceMeters;

    // rayPoint = fraction * translation; v = rayPoint - (nearest known point of C = T - A).
    float64 fraction = 0.0;
    math::Vector3d rayPointMesh{};
    math::Vector3d separationMesh = cast.Shape.InteriorMesh - target.InteriorMesh;
    math::Vector3d witnessMesh = target.InteriorMesh;
    math::Vector3d normalMesh{};
    internal::Simplex simplex;
    bool hasAdvanced = false;
    bool hasEnclosedOrigin = false;
    bool isStopped = false;
    CastStop stop = CastStop::Miss;
    for (uint32 iteration = 0; iteration < MAX_CONVEX_CAST_ITERATIONS && isStopped == false; ++iteration)
    {
        const float64 separation = math::Length(separationMesh);
        if (hasEnclosedOrigin == true || separation == 0.0 || simplex.Count == internal::MAX_SIMPLEX_VERTICES)
        {
            stop = hasAdvanced == true ? CastStop::Contact : CastStop::Overlap;
            isStopped = true;
            break;
        }
        if (hasAdvanced == true && separation <= margin + tolerance)
        {
            stop = CastStop::Contact;
            isStopped = true;
            break;
        }
        if (hasAdvanced == false &&
            (separation < margin - tolerance || separation <= tolerance * DEGENERATE_SEPARATION_RATIO))
        {
            stop = CastStop::Overlap;
            isStopped = true;
            break;
        }
        const math::Vector3d targetMesh = GetTargetSupport(target, separationMesh);
        const math::Vector3d differenceMesh = targetMesh - internal::GetCoreSupport(cast.Shape, -separationMesh);
        // separationMesh . (rayPoint - support) / |separationMesh| is a lower bound on the distance.
        const float64 planeDistanceScaled = math::Dot(separationMesh, rayPointMesh - differenceMesh);
        bool hasAdvancedNow = false;
        if (planeDistanceScaled > margin * separation)
        {
            const float64 approach = math::Dot(separationMesh, translationMesh);
            if (approach >= 0.0)
            {
                stop = CastStop::Miss;
                isStopped = true;
                break;
            }
            const float64 advanced = fraction - (planeDistanceScaled - margin * separation) / approach;
            if ((advanced < 1.0) == false)
            {
                stop = CastStop::Miss;
                isStopped = true;
                break;
            }
            fraction = advanced;
            rayPointMesh = translationMesh * fraction;
            normalMesh = separationMesh;
            hasAdvanced = true;
            hasAdvancedNow = true;
        }
        else if (hasAdvanced == false && separation <= margin + tolerance &&
                 planeDistanceScaled >= (margin - tolerance) * separation)
        {
            stop = CastStop::Touch;
            isStopped = true;
            break;
        }
        if (HasDifference(simplex, differenceMesh) == false)
        {
            simplex.Vertices[simplex.Count++] = { .DifferenceMesh = differenceMesh, .TargetMesh = targetMesh };
        }
        else if (hasAdvancedNow == false)
        {
            // No new support point: the estimate cannot improve further.
            if (hasAdvanced == true)
            {
                stop = CastStop::Contact;
            }
            else
            {
                stop = separation < margin - tolerance ? CastStop::Overlap : CastStop::Touch;
            }
            isStopped = true;
            break;
        }
        internal::SimplexClosest closest;
        internal::ReduceToClosest(rayPointMesh, simplex, closest);
        separationMesh = closest.PointMesh;
        witnessMesh = closest.WitnessMesh;
        hasEnclosedOrigin = closest.HasEnclosedOrigin;
    }
    if (isStopped == false)
    {
        return TriangleOutcome::NotConverged;
    }
    switch (stop)
    {
        case CastStop::Miss:
            return TriangleOutcome::Miss;
        case CastStop::Overlap:
            outCandidate.HasHit = true;
            outCandidate.IsInitiallyOverlapping = true;
            outCandidate.Fraction = 0.0;
            return TriangleOutcome::Hit;
        case CastStop::Touch:
            if (math::Dot(separationMesh, translationMesh) >= 0.0)
            {
                // Touching while moving away or parallel is not a contact.
                return TriangleOutcome::Miss;
            }
            outCandidate.HasHit = true;
            outCandidate.Fraction = 0.0;
            outCandidate.NormalMesh = separationMesh;
            outCandidate.WitnessMesh = GetSurfaceWitness(witnessMesh, target.MarginMeters, separationMesh);
            outCandidate.HasWitness = true;
            return TriangleOutcome::Hit;
        case CastStop::Contact:
            outCandidate.HasHit = true;
            // A contact entered at exactly zero may carry a negative zero; report +0.
            outCandidate.Fraction = fraction > 0.0 ? fraction : 0.0;
            outCandidate.NormalMesh = normalMesh;
            outCandidate.WitnessMesh = GetSurfaceWitness(witnessMesh, target.MarginMeters, normalMesh);
            outCandidate.HasWitness = hasEnclosedOrigin == false;
            return TriangleOutcome::Hit;
    }
    return TriangleOutcome::Miss;
}

/// One triangle over the whole translation.
TriangleOutcome CastTriangle(const ConvexCastContext& cast,
                             const internal::TriangleCorners& corners,
                             ConvexCandidate& outCandidate) noexcept
{
    CastTarget target;
    target.CornersMesh = {Widen(corners.FirstMesh) - cast.PositionMesh,
                          Widen(corners.SecondMesh) - cast.PositionMesh,
                          Widen(corners.ThirdMesh) - cast.PositionMesh};
    target.InteriorMesh = (target.CornersMesh[0] + target.CornersMesh[1] + target.CornersMesh[2]) * CENTROID_WEIGHT;
    return CastAgainst(cast, target, outCandidate);
}
} // namespace

/// Closest convex cast traversal for TriangleMesh::CastConvexClosest.
class TriangleMeshConvexCaster final
{
public:
    static Status Cast(const TriangleMesh& mesh, const ConvexCastInput& input, ConvexCastResult& outResult) noexcept;
    static Status
    CastPosed(const TriangleMesh& mesh, const internal::PosedConvexCast& request, ConvexCastResult& outResult) noexcept;
    /// Convert a best candidate into a result in the cast's frame.
    static ConvexCastResult MakeResult(const ConvexCastContext& cast, const ConvexCandidate& best) noexcept
    {
        return makeResult(cast, best);
    }
    static Status Overlap(const TriangleMesh& mesh,
                          const internal::PosedConvexCast& request,
                          internal::MeshOverlap& outOverlap) noexcept;

private:
    /// Visit outcome: Rejected and NotConverged stop the cast.
    enum class Visit : uint8
    {
        Continue,
        Rejected,
        NotConverged,
    };

    static Visit testTriangle(const ConvexCastContext& cast,
                              const TriangleMesh::Triangle& triangle,
                              ConvexCandidate& inoutBest) noexcept;
    static Visit traverse(const TriangleMesh& mesh, const ConvexCastContext& cast, ConvexCandidate& inoutBest) noexcept;
    static ConvexCastResult makeResult(const ConvexCastContext& cast, const ConvexCandidate& best) noexcept;
};

TriangleMeshConvexCaster::Visit TriangleMeshConvexCaster::testTriangle(const ConvexCastContext& cast,
                                                                       const TriangleMesh::Triangle& triangle,
                                                                       ConvexCandidate& inoutBest) noexcept
{
    ConvexCandidate candidate{ .TriangleIndex = triangle.SourceIndex };
    const internal::TriangleCorners corners
    {
        .FirstMesh = triangle.FirstMesh,
        .SecondMesh = triangle.SecondMesh,
        .ThirdMesh = triangle.ThirdMesh,
    };
    const TriangleOutcome outcome = CastTriangle(cast, corners, candidate);
    if (outcome == TriangleOutcome::NotConverged)
    {
        return Visit::NotConverged;
    }
    if (outcome == TriangleOutcome::Miss)
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

TriangleMeshConvexCaster::Visit TriangleMeshConvexCaster::traverse(const TriangleMesh& mesh,
                                                                   const ConvexCastContext& cast,
                                                                   ConvexCandidate& inoutBest) noexcept
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
            const Visit visit = testTriangle(cast, mesh.mTriangles[index], inoutBest);
            if (visit != Visit::Continue)
            {
                return visit;
            }
            if (cast.ShouldStopAtFirstHit == true && inoutBest.HasHit == true)
            {
                return Visit::Continue;
            }
        }
    }
    return Visit::Continue;
}

ConvexCastResult TriangleMeshConvexCaster::makeResult(const ConvexCastContext& cast,
                                                      const ConvexCandidate& best) noexcept
{
    if (best.HasHit == false)
    {
        return {};
    }
    ConvexCastResult result;
    result.HasHit = true;
    result.Fraction = best.Fraction;
    result.DistanceMeters = best.Fraction * math::Length(cast.TranslationMesh);
    result.HasStartedOverlapping = best.IsInitiallyOverlapping;
    result.HasNormal =
        best.IsInitiallyOverlapping == false && internal::TryNormalize(best.NormalMesh, result.NormalMesh) == true;
    result.HasWitness = best.IsInitiallyOverlapping == false && best.HasWitness == true;
    if (result.HasWitness == true)
    {
        result.WitnessPositionMesh = best.WitnessMesh + cast.PositionMesh;
    }
    result.TriangleIndex = best.TriangleIndex;
    return result;
}

Status TriangleMeshConvexCaster::Cast(const TriangleMesh& mesh,
                                      const ConvexCastInput& input,
                                      ConvexCastResult& outResult) noexcept
{
    if (mesh.IsCooked() == false)
    {
        return Status::NoGeometry;
    }
    const Status validation = internal::ValidateConvexCast(input);
    if (validation != Status::Success)
    {
        return validation;
    }
    internal::PosedConvex rotation;
    const bool isRotated = internal::TrySetRotation(input.OrientationMesh, rotation);
    LUDUS_ASSERT(isRotated == true);
    (void)isRotated;
    const internal::PosedConvexCast request
    {
        .Shape = internal::PoseConvexShape(input.Shape, rotation),
        .PositionMesh = input.PositionMesh,
        .TranslationMesh = input.TranslationMesh,
        .OverlapPolicy = input.OverlapPolicy,
        .Traversal = input.Traversal,
    };
    return CastPosed(mesh, request, outResult);
}

Status TriangleMeshConvexCaster::CastPosed(const TriangleMesh& mesh,
                                           const internal::PosedConvexCast& request,
                                           ConvexCastResult& outResult) noexcept
{
    if (mesh.IsCooked() == false)
    {
        return Status::NoGeometry;
    }
    if (internal::IsWithinMagnitude(request.PositionMesh, static_cast<float64>(MAX_COORDINATE_METERS)) == false ||
        internal::IsWithinMagnitude(request.TranslationMesh, internal::MAX_LOCAL_TRANSLATION_METERS) == false)
    {
        return Status::OutOfRange;
    }
    const ConvexCastContext cast = MakeConvexCastContext(request, ToTargetBounds(mesh.GetBoundsMesh()));
    ConvexCandidate best;
    Visit visit = Visit::Continue;
    if (request.Traversal == QueryTraversal::AllTriangles)
    {
        for (const TriangleMesh::Triangle& triangle : mesh.mTriangles)
        {
            visit = testTriangle(cast, triangle, best);
            if (visit != Visit::Continue || (cast.ShouldStopAtFirstHit == true && best.HasHit == true))
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
    if (visit == Visit::NotConverged)
    {
        return Status::NotConverged;
    }
    outResult = makeResult(cast, best);
    return Status::Success;
}

Status TriangleMeshConvexCaster::Overlap(const TriangleMesh& mesh,
                                         const internal::PosedConvexCast& request,
                                         internal::MeshOverlap& outOverlap) noexcept
{
    if (mesh.IsCooked() == false)
    {
        return Status::NoGeometry;
    }
    if (internal::IsWithinMagnitude(request.PositionMesh, static_cast<float64>(MAX_COORDINATE_METERS)) == false)
    {
        return Status::OutOfRange;
    }
    internal::PosedConvexCast stationary = request;
    stationary.TranslationMesh = {};
    stationary.OverlapPolicy = InitialOverlapPolicy::Report;
    const ConvexCastContext cast = MakeConvexCastContext(stationary, ToTargetBounds(mesh.GetBoundsMesh()));
    internal::MeshOverlap overlap;
    // False when the triangle did not converge.
    const auto visit = [&cast, &overlap](const TriangleMesh::Triangle& triangle) {
        ConvexCandidate candidate;
        const internal::TriangleCorners corners
        {
            .FirstMesh = triangle.FirstMesh,
            .SecondMesh = triangle.SecondMesh,
            .ThirdMesh = triangle.ThirdMesh,
        };
        const TriangleOutcome outcome = CastTriangle(cast, corners, candidate);
        if (outcome == TriangleOutcome::Hit && candidate.IsInitiallyOverlapping == true)
        {
            ++overlap.TriangleCount;
            overlap.LowestTriangleIndex = math::Min(overlap.LowestTriangleIndex, triangle.SourceIndex);
        }
        return outcome != TriangleOutcome::NotConverged;
    };
    if (request.Traversal == QueryTraversal::AllTriangles)
    {
        for (const TriangleMesh::Triangle& triangle : mesh.mTriangles)
        {
            if (visit(triangle) == false)
            {
                return Status::NotConverged;
            }
        }
        outOverlap = overlap;
        return Status::Success;
    }
    StaticArray<uint32, MAX_TRIANGLE_MESH_HIERARCHY_DEPTH + 1> pending{};
    usize pendingCount = 0;
    pending[pendingCount++] = 0;
    while (pendingCount > 0)
    {
        const uint32 nodeIndex = pending[--pendingCount];
        const TriangleMesh::Node& node = mesh.mNodes[nodeIndex];
        if (OverlapsSweep(cast, node.BoundsMesh, 0.0) == false)
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
            if (visit(mesh.mTriangles[index]) == false)
            {
                return Status::NotConverged;
            }
        }
    }
    outOverlap = overlap;
    return Status::Success;
}

Status TriangleMesh::CastConvexClosest(const ConvexCastInput& input, ConvexCastResult& outResult) const noexcept
{
    return TriangleMeshConvexCaster::Cast(*this, input, outResult);
}

namespace internal
{
PosedConvex PoseConvexShape(const ConvexShape& shape, const PosedConvex& rotation) noexcept
{
    PosedConvex posed;
    posed.Kind = shape.Kind;
    posed.AxisXMesh = rotation.AxisXMesh;
    posed.AxisYMesh = rotation.AxisYMesh;
    posed.AxisZMesh = rotation.AxisZMesh;
    switch (shape.Kind)
    {
        case ConvexShapeKind::Sphere:
            posed.MarginMeters = shape.RadiusMeters;
            break;
        case ConvexShapeKind::Capsule:
            posed.MarginMeters = shape.RadiusMeters;
            posed.HalfHeightMeters = shape.HalfHeightMeters;
            break;
        case ConvexShapeKind::Box:
            posed.HalfExtentsShape = shape.HalfExtentsMeters;
            break;
        case ConvexShapeKind::Hull:
            posed.HullOrNull = shape.HullOrNull;
            posed.InteriorMesh = ToMeshAxes(posed, shape.HullOrNull->GetInteriorPointShape());
            break;
    }
    return posed;
}

PosedBounds GetPosedBounds(const PosedConvex& shape) noexcept
{
    const StaticArray<math::Vector3d, AXIS_COUNT> axes{math::Vector3d{1, 0, 0},
                                                       math::Vector3d{0, 1, 0},
                                                       math::Vector3d{0, 0, 1}};
    StaticArray<float64, AXIS_COUNT> centers{};
    StaticArray<float64, AXIS_COUNT> halves{};
    for (uint32 axis = 0; axis < AXIS_COUNT; ++axis)
    {
        const float64 maximum = GetComponent(GetCoreSupport(shape, axes[axis]), axis);
        const float64 minimum = GetComponent(GetCoreSupport(shape, -axes[axis]), axis);
        centers[axis] = HALF * (maximum + minimum);
        halves[axis] = HALF * (maximum - minimum) + shape.MarginMeters;
    }
    return
    {
        .CenterMesh = {centers[0], centers[1], centers[Z_AXIS]},
        .HalfExtentsMesh = {halves[0], halves[1], halves[Z_AXIS]},
    };
}

Status CastPosedConvex(const TriangleMesh& mesh, const PosedConvexCast& request, ConvexCastResult& outResult) noexcept
{
    return TriangleMeshConvexCaster::CastPosed(mesh, request, outResult);
}

Status
OverlapPosedConvexWithMesh(const TriangleMesh& mesh, const PosedConvexCast& request, MeshOverlap& outOverlap) noexcept
{
    return TriangleMeshConvexCaster::Overlap(mesh, request, outOverlap);
}

namespace
{
/// The cast context and target for a convex target posed at the origin.
struct ConvexTargetCast final
{
    ConvexCastContext Context{};
    CastTarget Target{};
};

ConvexTargetCast MakeConvexTargetCast(const PosedConvex& target, const PosedConvexCast& request) noexcept
{
    const PosedBounds bounds = GetPosedBounds(target);
    const TargetBounds targetBounds
    {
        .Minimum = bounds.CenterMesh - bounds.HalfExtentsMesh,
        .Maximum = bounds.CenterMesh + bounds.HalfExtentsMesh,
    };
    ConvexTargetCast cast{ .Context = MakeConvexCastContext(request, targetBounds) };
    cast.Target.ConvexOrNull = &target;
    cast.Target.ConvexOffsetMesh = -request.PositionMesh;
    cast.Target.InteriorMesh = target.InteriorMesh - request.PositionMesh;
    cast.Target.MarginMeters = target.MarginMeters;
    return cast;
}
} // namespace

Status CastPosedConvexAgainstConvex(const PosedConvex& target,
                                    const PosedConvexCast& request,
                                    ConvexCastResult& outResult) noexcept
{
    if (IsWithinMagnitude(request.PositionMesh, static_cast<float64>(MAX_COORDINATE_METERS)) == false ||
        IsWithinMagnitude(request.TranslationMesh, MAX_LOCAL_TRANSLATION_METERS) == false)
    {
        return Status::OutOfRange;
    }
    const ConvexTargetCast cast = MakeConvexTargetCast(target, request);
    ConvexCandidate candidate;
    const TriangleOutcome outcome = CastAgainst(cast.Context, cast.Target, candidate);
    if (outcome == TriangleOutcome::NotConverged)
    {
        return Status::NotConverged;
    }
    if (outcome == TriangleOutcome::Hit && candidate.IsInitiallyOverlapping == true)
    {
        if (request.OverlapPolicy == InitialOverlapPolicy::Reject)
        {
            return Status::InitialOverlap;
        }
        if (request.OverlapPolicy == InitialOverlapPolicy::Ignore)
        {
            candidate = {};
        }
    }
    outResult = TriangleMeshConvexCaster::MakeResult(cast.Context, candidate);
    return Status::Success;
}

Status IsOverlappingConvex(const PosedConvex& target, const PosedConvexCast& request, bool& outIsOverlapping) noexcept
{
    if (IsWithinMagnitude(request.PositionMesh, static_cast<float64>(MAX_COORDINATE_METERS)) == false)
    {
        return Status::OutOfRange;
    }
    PosedConvexCast stationary = request;
    stationary.TranslationMesh = {};
    const ConvexTargetCast cast = MakeConvexTargetCast(target, stationary);
    ConvexCandidate candidate;
    const TriangleOutcome outcome = CastAgainst(cast.Context, cast.Target, candidate);
    if (outcome == TriangleOutcome::NotConverged)
    {
        return Status::NotConverged;
    }
    outIsOverlapping = outcome == TriangleOutcome::Hit && candidate.IsInitiallyOverlapping == true;
    return Status::Success;
}
} // namespace internal
} // namespace ludus::physics::collision
