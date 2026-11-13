#include <ludus/physics/collision/collision_world.h>

#include "internal/convex_cast.h"
#include "internal/convex_support.h"
#include "internal/mesh_arithmetic.h"
#include "internal/query_validation.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/identity/handle.hpp>
#include <ludus/foundation/identity/lifetime.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision
{
namespace
{
namespace math = foundation::math;
using foundation::StaticArray;
using internal::Widen;

constexpr usize BOX_CORNER_COUNT = 8;
/// Starting value of running minima and maxima; larger than any admitted coordinate or parameter.
constexpr float64 UNBOUNDED = 1.0e300;
/// Bound on a cast's per-collider contact tolerance relative to the Euclidean query scale. A collider-local
/// tolerance uses component magnitudes of its frame, which are at most the Euclidean length.
constexpr float64 COLLIDER_TOLERANCE_ALLOWANCE = 4.0 * CONVEX_CAST_RELATIVE_TOLERANCE;
/// How far a clipped local start may sit outside the coordinate envelope through rounding before it is clamped.
constexpr float64 ENVELOPE_CLAMP_RELATIVE = 1.0e-9;

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

Status ValidatePose(const RigidPose& pose) noexcept
{
    if (math::IsFinite(pose.PositionWorld) == false || IsOrientationFinite(pose.OrientationWorld) == false)
    {
        return Status::NonFiniteInput;
    }
    if (math::Abs(GetNormSquared(pose.OrientationWorld) - 1.0) > UNIT_QUATERNION_SQUARED_TOLERANCE)
    {
        return Status::InvalidArgument;
    }
    return internal::IsWithinMagnitude(pose.PositionWorld, static_cast<float64>(MAX_COORDINATE_METERS)) == true
               ? Status::Success
               : Status::OutOfRange;
}

bool IsFilterValid(const QueryFilter& filter) noexcept
{
    return filter.IgnoredColliders.IsValid() == true &&
           filter.IgnoredColliders.GetSize() <= MAX_QUERY_IGNORED_COLLIDERS;
}

/// An axis-aligned box in one frame.
struct Box final
{
    math::Vector3d Minimum{};
    math::Vector3d Maximum{};

    [[nodiscard]] bool Overlaps(const Box& other) const noexcept
    {
        return Minimum.X <= other.Maximum.X && Maximum.X >= other.Minimum.X && Minimum.Y <= other.Maximum.Y &&
               Maximum.Y >= other.Minimum.Y && Minimum.Z <= other.Maximum.Z && Maximum.Z >= other.Minimum.Z;
    }
};

/// The points `Origin + t * Direction` for real t.
struct Line final
{
    math::Vector3d Origin{};
    math::Vector3d Direction{};
};

/// Parameter interval of a line inside a box.
struct ClipInterval final
{
    bool HasOverlap = false;
    float64 Enter = 0.0;
    float64 Exit = 0.0;
};

ClipInterval ClipToBox(const Line& line, const Box& box) noexcept
{
    float64 enter = -UNBOUNDED;
    float64 exit = UNBOUNDED;
    for (uint32 axis = 0; axis < internal::AXIS_COUNT; ++axis)
    {
        const float64 start = internal::GetComponent(line.Origin, axis);
        const float64 step = internal::GetComponent(line.Direction, axis);
        const float64 low = internal::GetComponent(box.Minimum, axis);
        const float64 high = internal::GetComponent(box.Maximum, axis);
        if (step == 0.0)
        {
            if (start < low || start > high)
            {
                return {};
            }
            continue;
        }
        const float64 toLow = (low - start) / step;
        const float64 toHigh = (high - start) / step;
        enter = math::Max(enter, math::Min(toLow, toHigh));
        exit = math::Min(exit, math::Max(toLow, toHigh));
    }
    if (enter > exit)
    {
        return {};
    }
    return { .HasOverlap = true, .Enter = enter, .Exit = exit };
}

/// Componentwise bounds of two points, widened by `reach` on every side.
Box MakeSweptBox(math::Vector3d start, math::Vector3d end, math::Vector3d reach) noexcept
{
    return
    {
        .Minimum =
            math::Vector3d{math::Min(start.X, end.X), math::Min(start.Y, end.Y), math::Min(start.Z, end.Z)} - reach,
        .Maximum =
            math::Vector3d{math::Max(start.X, end.X), math::Max(start.Y, end.Y), math::Max(start.Z, end.Z)} + reach,
    };
}

math::Vector3d ClampToEnvelope(math::Vector3d value) noexcept
{
    constexpr float64 LIMIT = static_cast<float64>(MAX_COORDINATE_METERS);
    return {math::Clamp(value.X, -LIMIT, LIMIT),
            math::Clamp(value.Y, -LIMIT, LIMIT),
            math::Clamp(value.Z, -LIMIT, LIMIT)};
}

/// One collider's best answer, ordered by distance or fraction, then initial overlap first, then collider slot and
/// generation, then triangle index.
struct WorldCandidate final
{
    float64 Key = 0.0;
    bool HasHit = false;
    bool IsInitiallyOverlapping = false;
    uint32 Slot = 0;
    uint32 Generation = 0;
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX;
};

bool IsBetter(const WorldCandidate& candidate, const WorldCandidate& best) noexcept
{
    if (best.HasHit == false)
    {
        return true;
    }
    if (candidate.Key != best.Key)
    {
        return candidate.Key < best.Key;
    }
    if (candidate.IsInitiallyOverlapping != best.IsInitiallyOverlapping)
    {
        return candidate.IsInitiallyOverlapping == true;
    }
    if (candidate.Slot != best.Slot)
    {
        return candidate.Slot < best.Slot;
    }
    if (candidate.Generation != best.Generation)
    {
        return candidate.Generation < best.Generation;
    }
    return candidate.TriangleIndex < best.TriangleIndex;
}
} // namespace

/// World query implementation with access to collider slots.
class CollisionWorldQueries final
{
public:
    static Status
    Raycast(const CollisionWorld& world, const WorldRaycastInput& input, WorldRaycastResult& outResult) noexcept;
    static Status
    Cast(const CollisionWorld& world, const WorldConvexCastInput& input, WorldConvexCastResult& outResult) noexcept;

private:
    using Slot = CollisionWorld::Slot;

    static math::Vector3d toLocalDirection(const Slot& slot, math::Vector3d directionWorld) noexcept
    {
        return {math::Dot(directionWorld, slot.AxisXWorld),
                math::Dot(directionWorld, slot.AxisYWorld),
                math::Dot(directionWorld, slot.AxisZWorld)};
    }
    static math::Vector3d toLocalPoint(const Slot& slot, math::Vector3d pointWorld) noexcept
    {
        return toLocalDirection(slot, pointWorld - slot.Description.Pose.PositionWorld);
    }
    static math::Vector3d toWorldDirection(const Slot& slot, math::Vector3d directionLocal) noexcept
    {
        return slot.AxisXWorld * directionLocal.X + slot.AxisYWorld * directionLocal.Y +
               slot.AxisZWorld * directionLocal.Z;
    }
    static math::Vector3d toWorldPoint(const Slot& slot, math::Vector3d pointLocal) noexcept
    {
        return slot.Description.Pose.PositionWorld + toWorldDirection(slot, pointLocal);
    }

    static bool isReported(const CollisionWorld& world, uint32 slotIndex, const QueryFilter& filter) noexcept;
};

bool CollisionWorldQueries::isReported(const CollisionWorld& world,
                                       uint32 slotIndex,
                                       const QueryFilter& filter) noexcept
{
    const Slot& slot = world.mSlots[slotIndex];
    if (slot.IsLive == false || slot.Description.IsEnabled == false ||
        slot.Description.Participation.IsQueryable == false ||
        (slot.Description.Filter.CategoryBits & filter.CategoryMask) == 0 ||
        (slot.Description.Participation.IsSensor == true && filter.ShouldIncludeSensors == false))
    {
        return false;
    }
    const ColliderHandle handle{ .Owner = world.mOwner, .Slot = slotIndex, .Generation = slot.Generation };
    for (const ColliderHandle ignored : filter.IgnoredColliders)
    {
        if (ignored == handle)
        {
            return false;
        }
    }
    return true;
}

Status CollisionWorldQueries::Raycast(const CollisionWorld& world,
                                      const WorldRaycastInput& input,
                                      WorldRaycastResult& outResult) noexcept
{
    // Validate in world space with the triangle-mesh rules, so failures match RaycastClosest.
    const RaycastInput worldRay
    {
        .OriginMesh = input.OriginWorld,
        .DirectionMesh = input.DirectionWorld,
        .MaxDistanceMeters = input.MaxDistanceMeters,
        .FacePolicy = input.FacePolicy,
        .Traversal = input.Traversal,
    };
    const Status validation = internal::ValidateRaycast(worldRay);
    if (validation != Status::Success)
    {
        return validation;
    }
    if (IsFilterValid(input.Filter) == false)
    {
        return Status::InvalidArgument;
    }
    const math::Vector3d endWorld = input.OriginWorld + input.DirectionWorld * input.MaxDistanceMeters;
    const float64 slack = (internal::GetMaxMagnitude(input.OriginWorld) + input.MaxDistanceMeters) *
                          internal::TRAVERSAL_RELATIVE_ALLOWANCE;
    const Box path = MakeSweptBox(input.OriginWorld, endWorld, {slack, slack, slack});
    WorldCandidate best;
    RaycastResult bestLocal;
    for (uint32 slotIndex = 0; slotIndex < world.mSlots.GetSize(); ++slotIndex)
    {
        if (isReported(world, slotIndex, input.Filter) == false)
        {
            continue;
        }
        const Slot& slot = world.mSlots[slotIndex];
        const Box bounds{ .Minimum = slot.BoundsMinWorld, .Maximum = slot.BoundsMaxWorld };
        if (input.Traversal == QueryTraversal::Hierarchy && bounds.Overlaps(path) == false)
        {
            continue;
        }
        math::Vector3d originLocal = toLocalPoint(slot, input.OriginWorld);
        const math::Vector3d directionLocal = toLocalDirection(slot, input.DirectionWorld);
        float64 start = 0.0;
        float64 limit = input.MaxDistanceMeters;
        if (internal::IsWithinMagnitude(originLocal, static_cast<float64>(MAX_COORDINATE_METERS)) == false)
        {
            // Start at the mesh bounds: they lie inside the envelope, and nothing before them can be hit.
            const math::Aabb3 boundsLocal = slot.MeshOrNull->GetBoundsMesh();
            const ClipInterval clip = ClipToBox({ .Origin = originLocal, .Direction = directionLocal },
                                                {
                                                    .Minimum = Widen(boundsLocal.Min),
                                                    .Maximum = Widen(boundsLocal.Max),
                                                });
            if (clip.HasOverlap == false || clip.Exit < 0.0 || clip.Enter >= limit)
            {
                continue;
            }
            start = math::Max(0.0, clip.Enter);
            originLocal = ClampToEnvelope(originLocal + directionLocal * start);
            limit -= start;
        }
        const RaycastInput localRay
        {
            .OriginMesh = originLocal,
            .DirectionMesh = directionLocal,
            .MaxDistanceMeters = limit,
            .FacePolicy = input.FacePolicy,
            .Traversal = input.Traversal,
        };
        RaycastResult local;
        const Status status = slot.MeshOrNull->RaycastClosest(localRay, local);
        if (status != Status::Success)
        {
            return status;
        }
        if (local.HasHit == false)
        {
            continue;
        }
        const WorldCandidate candidate
        {
            .Key = start + local.DistanceMeters,
            .HasHit = true,
            .Slot = slotIndex,
            .Generation = slot.Generation,
            .TriangleIndex = local.TriangleIndex,
        };
        if (IsBetter(candidate, best) == true)
        {
            best = candidate;
            bestLocal = local;
        }
    }
    if (best.HasHit == false)
    {
        outResult = {};
        return Status::Success;
    }
    const Slot& hit = world.mSlots[best.Slot];
    WorldRaycastResult result;
    result.HasHit = true;
    result.DistanceMeters = best.Key;
    result.PositionWorld = input.OriginWorld + input.DirectionWorld * best.Key;
    result.NormalWorld = toWorldDirection(hit, bestLocal.NormalMesh);
    result.IsFrontFace = bestLocal.IsFrontFace;
    result.TriangleIndex = bestLocal.TriangleIndex;
    result.Collider = { .Owner = world.mOwner, .Slot = best.Slot, .Generation = best.Generation };
    result.Binding = hit.Description.Binding;
    outResult = result;
    return Status::Success;
}

Status CollisionWorldQueries::Cast(const CollisionWorld& world,
                                   const WorldConvexCastInput& input,
                                   WorldConvexCastResult& outResult) noexcept
{
    const ConvexCastInput worldCast
    {
        .Shape = input.Shape,
        .PositionMesh = input.PositionWorld,
        .OrientationMesh = input.OrientationWorld,
        .TranslationMesh = input.TranslationWorld,
        .OverlapPolicy = input.OverlapPolicy,
        .Traversal = input.Traversal,
    };
    const Status validation = internal::ValidateConvexCast(worldCast);
    if (validation != Status::Success)
    {
        return validation;
    }
    if (IsFilterValid(input.Filter) == false)
    {
        return Status::InvalidArgument;
    }
    internal::PosedConvex shapeAxes;
    const bool isRotated = internal::TrySetRotation(input.OrientationWorld, shapeAxes);
    LUDUS_ASSERT(isRotated == true);
    (void)isRotated;
    const internal::PosedConvex posedWorld = internal::PoseConvexShape(input.Shape, shapeAxes);
    const internal::PosedBounds boundsWorld = internal::GetPosedBounds(posedWorld);
    const math::Vector3d startWorld = input.PositionWorld + boundsWorld.CenterMesh;
    const math::Vector3d endWorld = startWorld + input.TranslationWorld;
    const float64 translationLength = math::Length(input.TranslationWorld);
    const float64 shapeExtent = math::Length(boundsWorld.HalfExtentsMesh) + math::Length(boundsWorld.CenterMesh);
    WorldCandidate best;
    ConvexCastResult bestLocal;
    for (uint32 slotIndex = 0; slotIndex < world.mSlots.GetSize(); ++slotIndex)
    {
        if (isReported(world, slotIndex, input.Filter) == false)
        {
            continue;
        }
        const Slot& slot = world.mSlots[slotIndex];
        // The collider's contact tolerance scales with its bounds relative to the shape; bound it for pruning.
        const float64 meshExtent = math::Max(math::Length(slot.BoundsMinWorld - input.PositionWorld),
                                             math::Length(slot.BoundsMaxWorld - input.PositionWorld));
        const float64 scale = meshExtent + translationLength + shapeExtent;
        const float64 widening = scale * (COLLIDER_TOLERANCE_ALLOWANCE + internal::TRAVERSAL_RELATIVE_ALLOWANCE);
        const math::Vector3d reachWorld = boundsWorld.HalfExtentsMesh + math::Vector3d{widening, widening, widening};
        const Box bounds{ .Minimum = slot.BoundsMinWorld, .Maximum = slot.BoundsMaxWorld };
        if (input.Traversal == QueryTraversal::Hierarchy &&
            bounds.Overlaps(MakeSweptBox(startWorld, endWorld, reachWorld)) == false)
        {
            continue;
        }
        // Compose the shape's rotation into the collider's local axes in float64.
        internal::PosedConvex localAxes;
        localAxes.AxisXMesh = toLocalDirection(slot, shapeAxes.AxisXMesh);
        localAxes.AxisYMesh = toLocalDirection(slot, shapeAxes.AxisYMesh);
        localAxes.AxisZMesh = toLocalDirection(slot, shapeAxes.AxisZMesh);
        const internal::PosedConvex posedLocal = internal::PoseConvexShape(input.Shape, localAxes);
        math::Vector3d positionLocal = toLocalPoint(slot, input.PositionWorld);
        math::Vector3d translationLocal = toLocalDirection(slot, input.TranslationWorld);
        float64 start = 0.0;
        if (internal::IsWithinMagnitude(positionLocal, static_cast<float64>(MAX_COORDINATE_METERS)) == false)
        {
            // Start where the shape's bounds first reach the mesh bounds; nothing earlier can be contacted.
            const internal::PosedBounds boundsLocal = internal::GetPosedBounds(posedLocal);
            const math::Aabb3 meshBounds = slot.MeshOrNull->GetBoundsMesh();
            const math::Vector3d reachLocal =
                boundsLocal.HalfExtentsMesh + math::Vector3d{widening, widening, widening};
            const ClipInterval clip = ClipToBox(
                { .Origin = positionLocal + boundsLocal.CenterMesh, .Direction = translationLocal },
                { .Minimum = Widen(meshBounds.Min) - reachLocal, .Maximum = Widen(meshBounds.Max) + reachLocal });
            if (clip.HasOverlap == false || clip.Exit < 0.0 || clip.Enter >= 1.0)
            {
                continue;
            }
            start = math::Max(0.0, clip.Enter);
            positionLocal = positionLocal + translationLocal * start;
            constexpr float64 CLAMP_LIMIT =
                static_cast<float64>(MAX_COORDINATE_METERS) * (1.0 + ENVELOPE_CLAMP_RELATIVE);
            if (internal::IsWithinMagnitude(positionLocal, CLAMP_LIMIT) == false)
            {
                return Status::OutOfRange;
            }
            positionLocal = ClampToEnvelope(positionLocal);
            translationLocal = translationLocal * (1.0 - start);
        }
        const internal::PosedConvexCast request
        {
            .Shape = posedLocal,
            .PositionMesh = positionLocal,
            .TranslationMesh = translationLocal,
            .OverlapPolicy = input.OverlapPolicy,
            .Traversal = input.Traversal,
        };
        ConvexCastResult local;
        const Status status = internal::CastPosedConvex(*slot.MeshOrNull, request, local);
        if (status != Status::Success)
        {
            return status;
        }
        if (local.HasHit == false)
        {
            continue;
        }
        const WorldCandidate candidate
        {
            .Key = start > 0.0 ? start + (1.0 - start) * local.Fraction : local.Fraction,
            .HasHit = true,
            .IsInitiallyOverlapping = start == 0.0 && local.HasStartedOverlapping == true,
            .Slot = slotIndex,
            .Generation = slot.Generation,
            .TriangleIndex = local.TriangleIndex,
        };
        if (IsBetter(candidate, best) == true)
        {
            best = candidate;
            bestLocal = local;
        }
    }
    if (best.HasHit == false)
    {
        outResult = {};
        return Status::Success;
    }
    const Slot& hit = world.mSlots[best.Slot];
    WorldConvexCastResult result;
    result.HasHit = true;
    result.Fraction = best.Key;
    result.DistanceMeters = best.Key * translationLength;
    result.HasStartedOverlapping = best.IsInitiallyOverlapping;
    result.HasNormal = bestLocal.HasNormal;
    if (result.HasNormal == true)
    {
        result.NormalWorld = toWorldDirection(hit, bestLocal.NormalMesh);
    }
    result.HasWitness = bestLocal.HasWitness;
    if (result.HasWitness == true)
    {
        result.WitnessPositionWorld = toWorldPoint(hit, bestLocal.WitnessPositionMesh);
    }
    result.TriangleIndex = bestLocal.TriangleIndex;
    result.Collider = { .Owner = world.mOwner, .Slot = best.Slot, .Generation = best.Generation };
    result.Binding = hit.Description.Binding;
    outResult = result;
    return Status::Success;
}

CollisionReadView::CollisionReadView(CollisionReadView&& other) noexcept
    : mWorldOrNull(other.mWorldOrNull), mEpoch(other.mEpoch)
{
    other.mWorldOrNull = nullptr;
    other.mEpoch = 0;
}

CollisionReadView& CollisionReadView::operator=(CollisionReadView&& other) noexcept
{
    if (this != &other)
    {
        Release();
        mWorldOrNull = other.mWorldOrNull;
        mEpoch = other.mEpoch;
        other.mWorldOrNull = nullptr;
        other.mEpoch = 0;
    }
    return *this;
}

CollisionReadView::~CollisionReadView() noexcept
{
    Release();
}

void CollisionReadView::Release() noexcept
{
    if (mWorldOrNull != nullptr)
    {
        mWorldOrNull->releaseReader();
        mWorldOrNull = nullptr;
        mEpoch = 0;
    }
}

Status CollisionReadView::RaycastClosest(const WorldRaycastInput& input, WorldRaycastResult& outResult) const noexcept
{
    if (mWorldOrNull == nullptr)
    {
        return Status::InvalidPhase;
    }
    return CollisionWorldQueries::Raycast(*mWorldOrNull, input, outResult);
}

Status CollisionReadView::CastConvexClosest(const WorldConvexCastInput& input,
                                            WorldConvexCastResult& outResult) const noexcept
{
    if (mWorldOrNull == nullptr)
    {
        return Status::InvalidPhase;
    }
    return CollisionWorldQueries::Cast(*mWorldOrNull, input, outResult);
}

CollisionWorld::~CollisionWorld() noexcept
{
    // A view keeps this world's address; it must be released first.
    LUDUS_ASSERT(mReaderCount == 0);
    if (mStoreOrNull == nullptr)
    {
        return;
    }
    for (Slot& slot : mSlots)
    {
        if (slot.IsLive == true)
        {
            const Status released = mStoreOrNull->TryRelease(slot.Description.Shape);
            LUDUS_ASSERT(released == Status::Success);
            (void)released;
        }
    }
    LUDUS_ASSERT(mStoreOrNull->mAttachedWorldCount > 0);
    --mStoreOrNull->mAttachedWorldCount;
}

Status CollisionWorld::TryInitialize(GeometryStore& inoutStore, const CollisionWorldSettings& settings) noexcept
{
    if (IsInitialized() == true || inoutStore.IsInitialized() == false)
    {
        return Status::InvalidPhase;
    }
    if (settings.MaxColliders == 0)
    {
        return Status::InvalidArgument;
    }
    if (settings.MaxColliders > MAX_COLLISION_WORLD_COLLIDERS)
    {
        return Status::CapacityExceeded;
    }
    foundation::Array<Slot> slots;
    foundation::Array<uint32> freeSlots;
    if (slots.TryResize(settings.MaxColliders) == false || freeSlots.TryResize(settings.MaxColliders) == false)
    {
        return Status::AllocationFailure;
    }
    for (uint32 index = 0; index < settings.MaxColliders; ++index)
    {
        freeSlots[index] = settings.MaxColliders - 1 - index;
    }
    uint64 owner = 0;
    if (foundation::identity::TryIssueLifetimeId(owner) != foundation::identity::LifetimeIdStatus::Success)
    {
        return Status::IdentityExhausted;
    }
    mSlots = foundation::Move(slots);
    mFreeSlots = foundation::Move(freeSlots);
    mOwner = owner;
    mStoreOrNull = &inoutStore;
    ++inoutStore.mAttachedWorldCount;
    return Status::Success;
}

Status CollisionWorld::checkMutable() const noexcept
{
    return IsInitialized() == false || mReaderCount > 0 ? Status::InvalidPhase : Status::Success;
}

void CollisionWorld::releaseReader() noexcept
{
    LUDUS_ASSERT(mReaderCount > 0);
    --mReaderCount;
}

const CollisionWorld::Slot* CollisionWorld::findLive(ColliderHandle collider) const noexcept
{
    if (mOwner == 0 || collider.Owner != mOwner || collider.Generation == 0 || collider.Slot >= mSlots.GetSize())
    {
        return nullptr;
    }
    const Slot& slot = mSlots[collider.Slot];
    return slot.IsLive == true && slot.Generation == collider.Generation ? &slot : nullptr;
}

CollisionWorld::Slot* CollisionWorld::findLive(ColliderHandle collider) noexcept
{
    return IsAlive(collider) == true ? &mSlots[collider.Slot] : nullptr;
}

bool CollisionWorld::IsAlive(ColliderHandle collider) const noexcept
{
    return findLive(collider) != nullptr;
}

namespace
{
/// A slot's rotation columns and conservative world bounds.
struct SlotPlacement final
{
    math::Vector3d AxisXWorld{};
    math::Vector3d AxisYWorld{};
    math::Vector3d AxisZWorld{};
    math::Vector3d BoundsMinWorld{};
    math::Vector3d BoundsMaxWorld{};
};

/// Place a mesh by a validated pose.
SlotPlacement PlaceSlot(const RigidPose& pose, const TriangleMesh& mesh) noexcept
{
    internal::PosedConvex rotation;
    const bool isRotated = internal::TrySetRotation(pose.OrientationWorld, rotation);
    LUDUS_ASSERT(isRotated == true);
    (void)isRotated;
    const math::Aabb3 boundsLocal = mesh.GetBoundsMesh();
    const math::Vector3d minimumLocal = Widen(boundsLocal.Min);
    const math::Vector3d maximumLocal = Widen(boundsLocal.Max);
    math::Vector3d minimum{UNBOUNDED, UNBOUNDED, UNBOUNDED};
    math::Vector3d maximum{-UNBOUNDED, -UNBOUNDED, -UNBOUNDED};
    for (uint32 corner = 0; corner < BOX_CORNER_COUNT; ++corner)
    {
        const math::Vector3d cornerLocal{(corner & 1U) == 0 ? minimumLocal.X : maximumLocal.X,
                                         (corner & 2U) == 0 ? minimumLocal.Y : maximumLocal.Y,
                                         (corner & 4U) == 0 ? minimumLocal.Z : maximumLocal.Z};
        const math::Vector3d cornerWorld = pose.PositionWorld + rotation.AxisXMesh * cornerLocal.X +
                                           rotation.AxisYMesh * cornerLocal.Y + rotation.AxisZMesh * cornerLocal.Z;
        minimum = {math::Min(minimum.X, cornerWorld.X),
                   math::Min(minimum.Y, cornerWorld.Y),
                   math::Min(minimum.Z, cornerWorld.Z)};
        maximum = {math::Max(maximum.X, cornerWorld.X),
                   math::Max(maximum.Y, cornerWorld.Y),
                   math::Max(maximum.Z, cornerWorld.Z)};
    }
    // Widen by the rounding of the corner transform so pruning stays conservative.
    const float64 slack = (internal::GetMaxMagnitude(minimum) + internal::GetMaxMagnitude(maximum)) *
                          internal::TRAVERSAL_RELATIVE_ALLOWANCE;
    return
    {
        .AxisXWorld = rotation.AxisXMesh,
        .AxisYWorld = rotation.AxisYMesh,
        .AxisZWorld = rotation.AxisZMesh,
        .BoundsMinWorld = minimum - math::Vector3d{slack, slack, slack},
        .BoundsMaxWorld = maximum + math::Vector3d{slack, slack, slack},
    };
}

} // namespace

Status CollisionWorld::TryCreateCollider(const ColliderDescription& description, ColliderHandle& outCollider) noexcept
{
    const Status phase = checkMutable();
    if (phase != Status::Success)
    {
        return phase;
    }
    const TriangleMesh* meshOrNull = nullptr;
    const Status shape = mStoreOrNull->TryGetTriangleMesh(description.Shape, meshOrNull);
    if (shape != Status::Success)
    {
        return shape;
    }
    const Status pose = ValidatePose(description.Pose);
    if (pose != Status::Success)
    {
        return pose;
    }
    if ((description.Participation.IsSolid == true && description.Participation.IsSensor == true) ||
        description.Filter.CategoryBits == 0)
    {
        return Status::InvalidArgument;
    }
    if (mFreeSlots.IsEmpty() == true)
    {
        return Status::CapacityExceeded;
    }
    const Status lease = mStoreOrNull->TryRetain(description.Shape);
    if (lease != Status::Success)
    {
        return lease;
    }
    const uint32 slotIndex = mFreeSlots[mFreeSlots.GetSize() - 1];
    mFreeSlots.RemoveLast();
    Slot& slot = mSlots[slotIndex];
    slot.Description = description;
    slot.MeshOrNull = meshOrNull;
    const SlotPlacement placement = PlaceSlot(description.Pose, *meshOrNull);
    slot.AxisXWorld = placement.AxisXWorld;
    slot.AxisYWorld = placement.AxisYWorld;
    slot.AxisZWorld = placement.AxisZWorld;
    slot.BoundsMinWorld = placement.BoundsMinWorld;
    slot.BoundsMaxWorld = placement.BoundsMaxWorld;
    slot.IsLive = true;
    ++mLiveCount;
    ++mEpoch;
    outCollider = { .Owner = mOwner, .Slot = slotIndex, .Generation = slot.Generation };
    return Status::Success;
}

Status CollisionWorld::TryDestroyCollider(ColliderHandle collider) noexcept
{
    const Status phase = checkMutable();
    if (phase != Status::Success)
    {
        return phase;
    }
    Slot* slotOrNull = findLive(collider);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    const Status released = mStoreOrNull->TryRelease(slotOrNull->Description.Shape);
    LUDUS_ASSERT(released == Status::Success);
    (void)released;
    const uint32 generation = slotOrNull->Generation;
    *slotOrNull = Slot{};
    slotOrNull->Generation = generation;
    --mLiveCount;
    ++mEpoch;
    // A slot destroyed at the terminal generation is retired: it never returns to the free list.
    if (foundation::identity::AdvanceGeneration(slotOrNull->Generation) ==
        foundation::identity::GenerationStatus::Advanced)
    {
        // The free list's capacity holds every slot, so this cannot allocate.
        const bool isListed = mFreeSlots.TryAdd(collider.Slot);
        LUDUS_ASSERT(isListed == true);
        (void)isListed;
    }
    return Status::Success;
}

Status CollisionWorld::TrySetColliderPose(ColliderHandle collider, const RigidPose& pose) noexcept
{
    const Status phase = checkMutable();
    if (phase != Status::Success)
    {
        return phase;
    }
    Slot* slotOrNull = findLive(collider);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    const Status validation = ValidatePose(pose);
    if (validation != Status::Success)
    {
        return validation;
    }
    slotOrNull->Description.Pose = pose;
    const SlotPlacement placement = PlaceSlot(pose, *slotOrNull->MeshOrNull);
    slotOrNull->AxisXWorld = placement.AxisXWorld;
    slotOrNull->AxisYWorld = placement.AxisYWorld;
    slotOrNull->AxisZWorld = placement.AxisZWorld;
    slotOrNull->BoundsMinWorld = placement.BoundsMinWorld;
    slotOrNull->BoundsMaxWorld = placement.BoundsMaxWorld;
    ++mEpoch;
    return Status::Success;
}

Status CollisionWorld::TrySetColliderEnabled(ColliderHandle collider, bool isEnabled) noexcept
{
    const Status phase = checkMutable();
    if (phase != Status::Success)
    {
        return phase;
    }
    Slot* slotOrNull = findLive(collider);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    slotOrNull->Description.IsEnabled = isEnabled;
    ++mEpoch;
    return Status::Success;
}

Status CollisionWorld::TryGetCollider(ColliderHandle collider, ColliderDescription& outDescription) const noexcept
{
    const Slot* slotOrNull = findLive(collider);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    outDescription = slotOrNull->Description;
    return Status::Success;
}

Status CollisionWorld::TryAcquireReadView(CollisionReadView& outView) noexcept
{
    if (IsInitialized() == false)
    {
        return Status::InvalidPhase;
    }
    if (outView.IsActive() == true)
    {
        return Status::InvalidArgument;
    }
    ++mReaderCount;
    outView.mWorldOrNull = this;
    outView.mEpoch = mEpoch;
    return Status::Success;
}
} // namespace ludus::physics::collision
