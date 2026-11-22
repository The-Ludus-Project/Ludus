// Contacts of a RigidWorld: candidate pairs, generation into scratch, the capacity check, persistent identity and
// the plain-value export.
//
// Thanks to Adam Moravanszky and Pierre Terdiman, "Fast Contact Reduction for Dynamics Simulation", Game
// Programming Gems 4, Charles River Media, 2004, section 3.5, pages 253-263, for keying persistent contacts by the
// features that produced them, so that cached solver state follows a contact point from tick to tick. Ludus keys a
// point by the two collider incarnations, their shapes, the mesh triangle and both features, and compares those
// values in full: pairs are kept sorted and joined by their keys, and no hash takes part, so a hash collision can
// never stand for equality. Thanks to David Baraff, "Curved Surfaces and Coherence for Non-Penetrating Rigid Body
// Simulation", Computer Graphics (SIGGRAPH '90) 24(4), 1990, pages 19-28, https://doi.org/10.1145/97880.97881, for
// treating coherence between successive steps as valid only while the contact geometry persists: a cached point
// retires as soon as its geometry, normal, material, settings or body placement stops agreeing.

#include "internal/rigid_world_contacts.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/contacts.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/contacts.h>

namespace ludus::physics::rigid
{
namespace
{
namespace math = foundation::math;

/// Pairs are ordered by their two collider slots. A slot holds one collider at a time, so the order is total.
bool IsBefore(collision::ColliderHandle first,
              collision::ColliderHandle second,
              collision::ColliderHandle otherFirst,
              collision::ColliderHandle otherSecond) noexcept
{
    if (first.Slot != otherFirst.Slot)
    {
        return first.Slot < otherFirst.Slot;
    }
    return second.Slot < otherSecond.Slot;
}

void CountRetired(ContactDiagnostics& inoutDiagnostics, ContactRetirement reason, usize count) noexcept
{
    inoutDiagnostics.RetiredPointCounts[static_cast<usize>(reason)] += count;
}
} // namespace

RigidWorld::ContactSide RigidWorldContacts::describeSide(const RigidWorld& world,
                                                         collision::ColliderHandle collider) noexcept
{
    const RigidWorld::Attachment& attachment = world.mAttachments[collider.Slot];
    const RigidWorld::BodySlot& body = world.mBodies[attachment.Body];
    return
    {
        .Collider = collider,
        .Shape = attachment.Shape,
        .Body = { .Owner = world.mOwner, .Slot = attachment.Body, .Generation = body.Generation },
        .Material = attachment.Material,
        .TeleportRevision = body.TeleportRevision,
    };
}

void RigidWorldContacts::appendPair(const RigidWorld& world,
                                    collision::ColliderHandle first,
                                    collision::ColliderHandle second,
                                    const collision::ContactPairContacts& contacts,
                                    ContactTables& inoutTables) noexcept
{
    inoutTables.Pairs[inoutTables.PairCount++] =
    {
        .First = describeSide(world, first),
        .Second = describeSide(world, second),
        .SettingsRevision = world.mContactSettingsRevision,
        .FirstPatch = static_cast<uint32>(inoutTables.PatchCount),
        .PatchCount = contacts.PatchCount,
    };
    for (uint32 patchIndex = 0; patchIndex < contacts.PatchCount; ++patchIndex)
    {
        const collision::ContactPatch& patch = contacts.Patches[patchIndex];
        inoutTables.Patches[inoutTables.PatchCount++] =
        {
            .NormalWorld = patch.NormalWorld,
            .FirstPoint = static_cast<uint32>(inoutTables.PointCount),
            .PointCount = patch.PointCount,
            .IsApproximate = patch.IsApproximate,
        };
        for (uint32 pointIndex = 0; pointIndex < patch.PointCount; ++pointIndex)
        {
            inoutTables.Points[inoutTables.PointCount++] = { .Point = contacts.Points[patch.FirstPoint + pointIndex] };
        }
    }
}

void RigidWorldContacts::retirePair(const ContactTables& previous,
                                    const ContactPairRecord& pair,
                                    ContactRetirement reason,
                                    ContactDiagnostics& inoutDiagnostics) noexcept
{
    for (uint32 patchIndex = 0; patchIndex < pair.PatchCount; ++patchIndex)
    {
        CountRetired(inoutDiagnostics, reason, previous.Patches[pair.FirstPatch + patchIndex].PointCount);
    }
}

void RigidWorldContacts::matchPair(ContactTables& inoutPrevious,
                                   const ContactPairRecord& previousPair,
                                   ContactTables& inoutCurrent,
                                   const ContactPairRecord& currentPair,
                                   ContactDiagnostics& inoutDiagnostics) noexcept
{
    for (uint32 patchIndex = 0; patchIndex < previousPair.PatchCount; ++patchIndex)
    {
        const ContactPatchRecord& patch = inoutPrevious.Patches[previousPair.FirstPatch + patchIndex];
        for (uint32 pointIndex = 0; pointIndex < patch.PointCount; ++pointIndex)
        {
            inoutPrevious.Points[patch.FirstPoint + pointIndex].IsMatched = false;
        }
    }
    for (uint32 patchIndex = 0; patchIndex < currentPair.PatchCount; ++patchIndex)
    {
        const ContactPatchRecord& patch = inoutCurrent.Patches[currentPair.FirstPatch + patchIndex];
        for (uint32 pointIndex = 0; pointIndex < patch.PointCount; ++pointIndex)
        {
            ContactPointRecord& point = inoutCurrent.Points[patch.FirstPoint + pointIndex];
            // The first unclaimed cached point with the same triangle and the same two features, compared in full.
            bool isResolved = false;
            for (uint32 cachedPatchIndex = 0; cachedPatchIndex < previousPair.PatchCount && isResolved == false;
                 ++cachedPatchIndex)
            {
                const ContactPatchRecord& cachedPatch =
                    inoutPrevious.Patches[previousPair.FirstPatch + cachedPatchIndex];
                for (uint32 cachedIndex = 0; cachedIndex < cachedPatch.PointCount && isResolved == false; ++cachedIndex)
                {
                    ContactPointRecord& cached = inoutPrevious.Points[cachedPatch.FirstPoint + cachedIndex];
                    if (cached.IsMatched == true || cached.Point.FirstFeature != point.Point.FirstFeature ||
                        cached.Point.SecondFeature != point.Point.SecondFeature ||
                        cached.Point.TriangleIndex != point.Point.TriangleIndex)
                    {
                        continue;
                    }
                    cached.IsMatched = true;
                    isResolved = true;
                    if (math::Dot(cachedPatch.NormalWorld, patch.NormalWorld) < collision::CONTACT_PATCH_NORMAL_COSINE)
                    {
                        // The same features now push in another direction: the cached impulses do not apply.
                        CountRetired(inoutDiagnostics, ContactRetirement::NormalChanged, 1);
                        ++inoutDiagnostics.NewPointCount;
                        continue;
                    }
                    point.PersistedTickCount = cached.PersistedTickCount + 1;
                    point.NormalImpulse = cached.NormalImpulse;
                    point.TangentImpulses = cached.TangentImpulses;
                    ++inoutDiagnostics.PersistedPointCount;
                }
            }
            if (isResolved == false)
            {
                ++inoutDiagnostics.NewPointCount;
            }
        }
    }
    for (uint32 patchIndex = 0; patchIndex < previousPair.PatchCount; ++patchIndex)
    {
        const ContactPatchRecord& patch = inoutPrevious.Patches[previousPair.FirstPatch + patchIndex];
        for (uint32 pointIndex = 0; pointIndex < patch.PointCount; ++pointIndex)
        {
            if (inoutPrevious.Points[patch.FirstPoint + pointIndex].IsMatched == false)
            {
                CountRetired(inoutDiagnostics, ContactRetirement::FeatureChanged, 1);
            }
        }
    }
}

void RigidWorldContacts::matchCache(ContactTables& inoutPrevious,
                                    ContactTables& inoutCurrent,
                                    ContactDiagnostics& inoutDiagnostics) noexcept
{
    // Both generations are sorted by pair: one pass joins them.
    usize previousIndex = 0;
    for (usize currentIndex = 0; currentIndex < inoutCurrent.PairCount; ++currentIndex)
    {
        const ContactPairRecord& current = inoutCurrent.Pairs[currentIndex];
        while (previousIndex < inoutPrevious.PairCount && IsBefore(inoutPrevious.Pairs[previousIndex].First.Collider,
                                                                   inoutPrevious.Pairs[previousIndex].Second.Collider,
                                                                   current.First.Collider,
                                                                   current.Second.Collider) == true)
        {
            retirePair(inoutPrevious,
                       inoutPrevious.Pairs[previousIndex],
                       ContactRetirement::Separated,
                       inoutDiagnostics);
            ++previousIndex;
        }
        usize currentPointCount = 0;
        for (uint32 patchIndex = 0; patchIndex < current.PatchCount; ++patchIndex)
        {
            currentPointCount += inoutCurrent.Patches[current.FirstPatch + patchIndex].PointCount;
        }
        if (previousIndex == inoutPrevious.PairCount ||
            inoutPrevious.Pairs[previousIndex].First.Collider.Slot != current.First.Collider.Slot ||
            inoutPrevious.Pairs[previousIndex].Second.Collider.Slot != current.Second.Collider.Slot)
        {
            inoutDiagnostics.NewPointCount += currentPointCount;
            continue;
        }
        const ContactPairRecord& previous = inoutPrevious.Pairs[previousIndex];
        ++previousIndex;
        // The slots agree; the whole identity must. A slot reused by a new collider is another pair.
        ContactRetirement reason = ContactRetirement::Separated;
        bool isCompatible = false;
        if (previous.First.Collider != current.First.Collider || previous.Second.Collider != current.Second.Collider)
        {
            reason = ContactRetirement::Separated;
        }
        else if (previous.First.Shape != current.First.Shape || previous.Second.Shape != current.Second.Shape)
        {
            reason = ContactRetirement::GeometryChanged;
        }
        else if (previous.First.Material != current.First.Material ||
                 previous.Second.Material != current.Second.Material)
        {
            reason = ContactRetirement::MaterialChanged;
        }
        else if (previous.SettingsRevision != current.SettingsRevision)
        {
            reason = ContactRetirement::SettingsChanged;
        }
        else if (previous.First.TeleportRevision != current.First.TeleportRevision ||
                 previous.Second.TeleportRevision != current.Second.TeleportRevision ||
                 previous.First.Body != current.First.Body || previous.Second.Body != current.Second.Body)
        {
            reason = ContactRetirement::Teleported;
        }
        else
        {
            isCompatible = true;
        }
        if (isCompatible == false)
        {
            retirePair(inoutPrevious, previous, reason, inoutDiagnostics);
            inoutDiagnostics.NewPointCount += currentPointCount;
            continue;
        }
        matchPair(inoutPrevious, previous, inoutCurrent, current, inoutDiagnostics);
    }
    for (; previousIndex < inoutPrevious.PairCount; ++previousIndex)
    {
        retirePair(inoutPrevious, inoutPrevious.Pairs[previousIndex], ContactRetirement::Separated, inoutDiagnostics);
    }
}

Status RigidWorldContacts::Detect(RigidWorld& inoutWorld, uint64 tick) noexcept
{
    RigidWorld& world = inoutWorld;
    ContactTables& previous = world.mContactTables[world.mPublishedContacts];
    ContactTables& current = world.mContactTables[1 - world.mPublishedContacts];
    current.PairCount = 0;
    current.PatchCount = 0;
    current.PointCount = 0;
    ContactDiagnostics diagnostics
    {
        .HasDetected = true,
        .Tick = tick,
        .PairHighWater = world.mContactDiagnostics.PairHighWater,
        .PointHighWater = world.mContactDiagnostics.PointHighWater,
    };
    const collision::ContactSettings settings{ .ContactOffsetMeters = world.mSettings.ContactOffsetMeters };
    const foundation::BorrowedView<collision::ColliderHandle> candidates(world.mContactCandidates.GetData(),
                                                                         world.mContactCandidates.GetSize());
    // Every pair is found, generated and counted even when a table is already full, so the required counts are
    // exact; only pairs that fit are written.
    usize requiredPairs = 0;
    usize requiredPoints = 0;
    {
        collision::CollisionReadView view;
        const collision::Status acquired = world.mCollision.TryAcquireReadView(view);
        if (acquired != collision::Status::Success)
        {
            return ToRigidStatus(acquired);
        }
        for (uint32 bodyIndex = 0; bodyIndex < world.mBodies.GetSize(); ++bodyIndex)
        {
            const RigidWorld::BodySlot& body = world.mBodies[bodyIndex];
            // Only pairs with a dynamic body make solid contacts, so only dynamic colliders search for partners.
            if (body.IsLive == false || body.Motion != MotionType::Dynamic)
            {
                continue;
            }
            uint32 colliderSlot = body.FirstCollider;
            for (uint32 colliderIndex = 0; colliderIndex < body.ColliderCount; ++colliderIndex)
            {
                const RigidWorld::Attachment& attachment = world.mAttachments[colliderSlot];
                collision::ContactCandidateResult found;
                const collision::Status search = view.FindContactCandidates(
                    { .Collider = attachment.Collider, .MarginMeters = settings.ContactOffsetMeters },
                    candidates,
                    found);
                if (search != collision::Status::Success)
                {
                    return ToRigidStatus(search);
                }
                // The scratch holds one handle per collider slot, so every candidate was written.
                LUDUS_ASSERT(found.IsComplete == true);
                diagnostics.CandidatePairCount += found.CandidateCount;
                for (usize candidateIndex = 0; candidateIndex < found.WrittenCount; ++candidateIndex)
                {
                    const collision::ColliderHandle other = candidates[candidateIndex];
                    const RigidWorld::Attachment& otherAttachment = world.mAttachments[other.Slot];
                    // Colliders of one body never touch each other. Two dynamic colliders find each other from
                    // both sides: the one with the lower slot reports the pair.
                    if (otherAttachment.Body == bodyIndex ||
                        (world.mBodies[otherAttachment.Body].Motion == MotionType::Dynamic &&
                         other.Slot < colliderSlot))
                    {
                        continue;
                    }
                    const bool isFirst = colliderSlot < other.Slot;
                    const collision::ColliderHandle first = isFirst == true ? attachment.Collider : other;
                    const collision::ColliderHandle second = isFirst == true ? other : attachment.Collider;
                    ++diagnostics.TestedPairCount;
                    collision::ContactPairContacts contacts;
                    const collision::Status generated = view.GenerateContacts(first, second, settings, contacts);
                    if (generated == collision::Status::Unsupported)
                    {
                        ++diagnostics.UnsupportedPairCount;
                        continue;
                    }
                    if (generated != collision::Status::Success)
                    {
                        return ToRigidStatus(generated);
                    }
                    diagnostics.GeneratedPointCount += contacts.GeneratedPointCount;
                    diagnostics.TriangleTestCount += contacts.TriangleTestCount;
                    diagnostics.DiscardedPatchCount += contacts.DiscardedPatchCount;
                    if (contacts.PointCount == 0)
                    {
                        continue;
                    }
                    diagnostics.PatchCount += contacts.PatchCount;
                    diagnostics.ApproximatePatchCount += contacts.ApproximatePatchCount;
                    ++requiredPairs;
                    requiredPoints += contacts.PointCount;
                    if (requiredPairs <= world.mSettings.MaxContactPairs &&
                        requiredPoints <= world.mSettings.MaxContactPoints)
                    {
                        appendPair(world, first, second, contacts, current);
                    }
                }
                colliderSlot = attachment.Next;
            }
        }
    }
    diagnostics.PairCount = requiredPairs;
    diagnostics.PointCount = requiredPoints;
    diagnostics.PairHighWater = math::Max(diagnostics.PairHighWater, requiredPairs);
    diagnostics.PointHighWater = math::Max(diagnostics.PointHighWater, requiredPoints);
    if (requiredPairs > world.mSettings.MaxContactPairs)
    {
        diagnostics.ExhaustedTable = ContactTable::Pairs;
        diagnostics.RequiredCount = requiredPairs;
    }
    else if (requiredPoints > world.mSettings.MaxContactPoints)
    {
        diagnostics.ExhaustedTable = ContactTable::Points;
        diagnostics.RequiredCount = requiredPoints;
    }
    if (diagnostics.ExhaustedTable != ContactTable::None)
    {
        world.mContactDiagnostics = diagnostics;
        return Status::CapacityExceeded;
    }
    foundation::Sort(
        current.Pairs.GetData(),
        current.PairCount,
        [](const ContactPairRecord& first, const ContactPairRecord& second) {
            return IsBefore(first.First.Collider, first.Second.Collider, second.First.Collider, second.Second.Collider);
        });
    matchCache(previous, current, diagnostics);
    world.mContactDiagnostics = diagnostics;
    return Status::Success;
}

void RigidWorldContacts::Publish(RigidWorld& inoutWorld, uint64 tick) noexcept
{
    inoutWorld.mPublishedContacts = 1 - inoutWorld.mPublishedContacts;
    inoutWorld.mContactTick = tick;
}

Status RigidWorld::TryExportContacts(foundation::BorrowedView<ContactPointState> pointBuffer,
                                     ContactExportResult& outResult) const noexcept
{
    if (IsInitialized() == false)
    {
        return Status::InvalidPhase;
    }
    if (pointBuffer.IsValid() == false)
    {
        return Status::InvalidArgument;
    }
    const ContactTables& tables = mContactTables[mPublishedContacts];
    ContactExportResult result
    {
        .PointCount = tables.PointCount,
        .PairCount = tables.PairCount,
        .PatchCount = tables.PatchCount,
        .Tick = mContactTick,
    };
    uint32 exportedPatch = 0;
    for (usize pairIndex = 0; pairIndex < tables.PairCount; ++pairIndex)
    {
        const ContactPairRecord& pair = tables.Pairs[pairIndex];
        for (uint32 patchIndex = 0; patchIndex < pair.PatchCount; ++patchIndex)
        {
            const ContactPatchRecord& patch = tables.Patches[pair.FirstPatch + patchIndex];
            for (uint32 pointIndex = 0; pointIndex < patch.PointCount && result.WrittenCount < pointBuffer.GetSize();
                 ++pointIndex)
            {
                const ContactPointRecord& point = tables.Points[patch.FirstPoint + pointIndex];
                pointBuffer[result.WrittenCount++] =
                {
                    .FirstBody = pair.First.Body,
                    .SecondBody = pair.Second.Body,
                    .FirstCollider = pair.First.Collider,
                    .SecondCollider = pair.Second.Collider,
                    .PatchIndex = exportedPatch,
                    .NormalWorld = patch.NormalWorld,
                    .PositionWorld = point.Point.PositionWorld,
                    .SeparationMeters = point.Point.SeparationMeters,
                    .FirstFeature = point.Point.FirstFeature,
                    .SecondFeature = point.Point.SecondFeature,
                    .TriangleIndex = point.Point.TriangleIndex,
                    .PersistedTickCount = point.PersistedTickCount,
                    .IsApproximate = patch.IsApproximate,
                };
            }
            ++exportedPatch;
        }
    }
    result.IsComplete = result.WrittenCount == result.PointCount;
    outResult = result;
    return Status::Success;
}
} // namespace ludus::physics::rigid
