// Contact queries of a collision world: candidate pairs from the collider hierarchy and per-pair contact generation.

#include <ludus/physics/collision/collision_world.h>

#include "internal/collider_hierarchy.h"
#include "internal/contact_generation.h"
#include "internal/convex_cast.h"
#include "internal/convex_support.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/contacts.h>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision
{
namespace
{
namespace math = foundation::math;

Status ValidateOffset(float64 offsetMeters) noexcept
{
    if (math::IsFinite(offsetMeters) == false)
    {
        return Status::NonFiniteInput;
    }
    return offsetMeters >= 0.0 && offsetMeters <= MAX_CONTACT_OFFSET_METERS ? Status::Success : Status::OutOfRange;
}

bool AreBoundsOverlapping(math::Vector3d firstMinimum,
                          math::Vector3d firstMaximum,
                          math::Vector3d secondMinimum,
                          math::Vector3d secondMaximum) noexcept
{
    return firstMinimum.X <= secondMaximum.X && firstMaximum.X >= secondMinimum.X &&
           firstMinimum.Y <= secondMaximum.Y && firstMaximum.Y >= secondMinimum.Y &&
           firstMinimum.Z <= secondMaximum.Z && firstMaximum.Z >= secondMinimum.Z;
}
} // namespace

/// Contact query implementation with access to collider slots.
class CollisionWorldContacts final
{
public:
    static Status FindCandidates(const CollisionWorld& world,
                                 const ContactCandidateInput& input,
                                 foundation::BorrowedView<ColliderHandle> candidateBuffer,
                                 ContactCandidateResult& outResult) noexcept;
    static Status Generate(const CollisionWorld& world,
                           ColliderHandle first,
                           ColliderHandle second,
                           const ContactSettings& settings,
                           ContactPairContacts& outContacts) noexcept;

private:
    using Slot = CollisionWorld::Slot;

    /// A pair frame: world axes about an origin, or a mesh collider's local frame.
    struct Frame final
    {
        math::Vector3d Origin{};
        math::Vector3d AxisX{1.0, 0.0, 0.0};
        math::Vector3d AxisY{0.0, 1.0, 0.0};
        math::Vector3d AxisZ{0.0, 0.0, 1.0};
    };

    static math::Vector3d toFrameDirection(const Frame& frame, math::Vector3d directionWorld) noexcept
    {
        return {math::Dot(directionWorld, frame.AxisX),
                math::Dot(directionWorld, frame.AxisY),
                math::Dot(directionWorld, frame.AxisZ)};
    }

    static math::Vector3d toWorldDirection(const Frame& frame, math::Vector3d direction) noexcept
    {
        return frame.AxisX * direction.X + frame.AxisY * direction.Y + frame.AxisZ * direction.Z;
    }

    /// A convex collider's shape posed in a pair frame.
    static internal::ContactShape poseConvex(const Slot& slot, const Frame& frame) noexcept;
    /// Reduce every patch and write it in world space; `isMirrored` swaps the two sides of the pair.
    static void publish(internal::PairAccumulator& inoutPair,
                        const Frame& frame,
                        bool isMirrored,
                        ContactPairContacts& outContacts) noexcept;
};

Status CollisionWorldContacts::FindCandidates(const CollisionWorld& world,
                                              const ContactCandidateInput& input,
                                              foundation::BorrowedView<ColliderHandle> candidateBuffer,
                                              ContactCandidateResult& outResult) noexcept
{
    const Slot* subjectOrNull = world.findLive(input.Collider);
    if (subjectOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    const Status margin = ValidateOffset(input.MarginMeters);
    if (margin != Status::Success)
    {
        return margin;
    }
    if ((input.Traversal != QueryTraversal::Hierarchy && input.Traversal != QueryTraversal::AllTriangles) ||
        candidateBuffer.IsValid() == false)
    {
        return Status::InvalidArgument;
    }
    const Slot& subject = *subjectOrNull;
    ContactCandidateResult result;
    if (subject.Description.IsEnabled == false || subject.Description.Participation.IsSolid == false)
    {
        outResult = result;
        return Status::Success;
    }
    const math::Vector3d widening{input.MarginMeters, input.MarginMeters, input.MarginMeters};
    const math::Vector3d minimum = subject.BoundsMinWorld - widening;
    const math::Vector3d maximum = subject.BoundsMaxWorld + widening;
    // Node bounds are exact unions of their leaves' bounds, so pruning with the same comparison misses nothing.
    const auto canReach = [&](const math::Vector3d& nodeMinimum, const math::Vector3d& nodeMaximum) {
        return AreBoundsOverlapping(minimum, maximum, nodeMinimum, nodeMaximum);
    };
    const auto visitSlot = [&](uint32 slotIndex, HierarchyVisit& outVisit) -> Status {
        (void)outVisit;
        const Slot& other = world.mSlots[slotIndex];
        if (other.IsLive == false || slotIndex == input.Collider.Slot || other.Description.IsEnabled == false ||
            other.Description.Participation.IsSolid == false ||
            ShouldCollide(subject.Description.Filter, other.Description.Filter) == false ||
            AreBoundsOverlapping(minimum, maximum, other.BoundsMinWorld, other.BoundsMaxWorld) == false)
        {
            return Status::Success;
        }
        if (result.WrittenCount < candidateBuffer.GetSize())
        {
            candidateBuffer[result.WrittenCount++] =
            {
                .Owner = world.mOwner,
                .Slot = slotIndex,
                .Generation = other.Generation,
            };
        }
        ++result.CandidateCount;
        return Status::Success;
    };
    Status traversal = Status::Success;
    if (input.Traversal == QueryTraversal::AllTriangles)
    {
        for (uint32 slotIndex = 0; slotIndex < world.mSlots.GetSize() && traversal == Status::Success; ++slotIndex)
        {
            HierarchyVisit visit = HierarchyVisit::Continue;
            traversal = visitSlot(slotIndex, visit);
        }
    }
    else
    {
        traversal = ColliderHierarchy::Traverse(world, canReach, visitSlot);
    }
    if (traversal != Status::Success)
    {
        return traversal;
    }
    // The hierarchy finds candidates in tree order; report them in slot order.
    foundation::Sort(
        candidateBuffer.GetDataOrNull(),
        result.WrittenCount,
        [](const ColliderHandle& first, const ColliderHandle& second) { return first.Slot < second.Slot; });
    result.IsComplete = result.WrittenCount == result.CandidateCount;
    outResult = result;
    return Status::Success;
}

internal::ContactShape CollisionWorldContacts::poseConvex(const Slot& slot, const Frame& frame) noexcept
{
    internal::PosedConvex axes;
    axes.AxisXMesh = toFrameDirection(frame, slot.AxisXWorld);
    axes.AxisYMesh = toFrameDirection(frame, slot.AxisYWorld);
    axes.AxisZMesh = toFrameDirection(frame, slot.AxisZWorld);
    internal::ContactShape shape;
    shape.Convex = internal::PoseConvexShape(slot.Convex, axes);
    shape.Position = toFrameDirection(frame, slot.Description.Pose.PositionWorld - frame.Origin);
    return shape;
}

void CollisionWorldContacts::publish(internal::PairAccumulator& inoutPair,
                                     const Frame& frame,
                                     bool isMirrored,
                                     ContactPairContacts& outContacts) noexcept
{
    ContactPairContacts contacts;
    contacts.GeneratedPointCount = inoutPair.GeneratedPointCount;
    contacts.TriangleTestCount = inoutPair.TriangleTestCount;
    contacts.DiscardedPatchCount = inoutPair.DiscardedPatchCount;
    for (uint32 patchIndex = 0; patchIndex < inoutPair.PatchCount; ++patchIndex)
    {
        internal::PatchAccumulator& patch = inoutPair.Patches[patchIndex];
        patch.Count = internal::ReduceContactPoints(patch.Points.GetData(), patch.Count, patch.Normal, inoutPair.Scale);
        const math::Vector3d normalWorld = toWorldDirection(frame, patch.Normal);
        contacts.Patches[contacts.PatchCount++] =
        {
            .NormalWorld = isMirrored == true ? -normalWorld : normalWorld,
            .FirstPoint = contacts.PointCount,
            .PointCount = patch.Count,
            .IsApproximate = patch.IsApproximate,
        };
        contacts.ApproximatePatchCount += patch.IsApproximate == true ? 1U : 0U;
        for (uint32 pointIndex = 0; pointIndex < patch.Count; ++pointIndex)
        {
            const internal::ManifoldPoint& point = patch.Points[pointIndex];
            contacts.Points[contacts.PointCount++] =
            {
                .PositionWorld = frame.Origin + toWorldDirection(frame, point.Position),
                .SeparationMeters = point.Separation,
                .FirstFeature = isMirrored == true ? point.SecondFeature : point.FirstFeature,
                .SecondFeature = isMirrored == true ? point.FirstFeature : point.SecondFeature,
                .TriangleIndex = point.TriangleIndex,
            };
        }
    }
    outContacts = contacts;
}

Status CollisionWorldContacts::Generate(const CollisionWorld& world,
                                        ColliderHandle first,
                                        ColliderHandle second,
                                        const ContactSettings& settings,
                                        ContactPairContacts& outContacts) noexcept
{
    const Slot* firstOrNull = world.findLive(first);
    const Slot* secondOrNull = world.findLive(second);
    if (firstOrNull == nullptr || secondOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (first.Slot == second.Slot)
    {
        return Status::InvalidArgument;
    }
    const Status offset = ValidateOffset(settings.ContactOffsetMeters);
    if (offset != Status::Success)
    {
        return offset;
    }
    const Slot& firstSlot = *firstOrNull;
    const Slot& secondSlot = *secondOrNull;
    if (firstSlot.MeshOrNull != nullptr && secondSlot.MeshOrNull != nullptr)
    {
        return Status::Unsupported;
    }
    internal::PairAccumulator pair;
    if (firstSlot.MeshOrNull != nullptr || secondSlot.MeshOrNull != nullptr)
    {
        // Mesh pairs run in the mesh's local frame, with the convex shape as the kernels' first shape.
        const bool isMeshFirst = firstSlot.MeshOrNull != nullptr;
        const Slot& meshSlot = isMeshFirst == true ? firstSlot : secondSlot;
        const Slot& convexSlot = isMeshFirst == true ? secondSlot : firstSlot;
        const Frame frame
        {
            .Origin = meshSlot.Description.Pose.PositionWorld,
            .AxisX = meshSlot.AxisXWorld,
            .AxisY = meshSlot.AxisYWorld,
            .AxisZ = meshSlot.AxisZWorld,
        };
        internal::CollectMeshContacts(*meshSlot.MeshOrNull,
                                      poseConvex(convexSlot, frame),
                                      settings.ContactOffsetMeters,
                                      pair);
        publish(pair, frame, isMeshFirst, outContacts);
        return Status::Success;
    }
    // Two convex colliders run in world axes about the first one, so the answer ignores where the pair sits.
    const Frame frame{ .Origin = firstSlot.Description.Pose.PositionWorld };
    const internal::ContactShape firstShape = poseConvex(firstSlot, frame);
    const internal::ContactShape secondShape = poseConvex(secondSlot, frame);
    internal::Penetration penetration;
    internal::FindPenetration(firstShape, secondShape, settings.ContactOffsetMeters, penetration);
    if (penetration.HasContact == true)
    {
        internal::Manifold manifold;
        internal::BuildManifold(firstShape, secondShape, penetration, settings.ContactOffsetMeters, false, manifold);
        internal::AddManifold(manifold, internal::GetPairScale(firstShape, secondShape), pair);
    }
    publish(pair, frame, false, outContacts);
    return Status::Success;
}

Status CollisionReadView::FindContactCandidates(const ContactCandidateInput& input,
                                                foundation::BorrowedView<ColliderHandle> candidateBuffer,
                                                ContactCandidateResult& outResult) const noexcept
{
    if (mWorldOrNull == nullptr)
    {
        return Status::InvalidPhase;
    }
    return CollisionWorldContacts::FindCandidates(*mWorldOrNull, input, candidateBuffer, outResult);
}

Status CollisionReadView::GenerateContacts(ColliderHandle first,
                                           ColliderHandle second,
                                           const ContactSettings& settings,
                                           ContactPairContacts& outContacts) const noexcept
{
    if (mWorldOrNull == nullptr)
    {
        return Status::InvalidPhase;
    }
    return CollisionWorldContacts::Generate(*mWorldOrNull, first, second, settings, outContacts);
}
} // namespace ludus::physics::collision
