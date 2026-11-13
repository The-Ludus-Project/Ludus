#include <ludus/physics/collision/geometry_store.h>

#include "internal/shape_validation.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/identity/handle.hpp>
#include <ludus/foundation/identity/lifetime.hpp>

namespace ludus::physics::collision
{
GeometryStore::~GeometryStore() noexcept
{
    // A world keeps this store's address and leases; it must be destroyed first.
    LUDUS_ASSERT(mAttachedWorldCount == 0);
}

Status GeometryStore::TryInitialize(const GeometryStoreSettings& settings) noexcept
{
    if (IsInitialized() == true)
    {
        return Status::InvalidPhase;
    }
    if (settings.MaxShapes == 0)
    {
        return Status::InvalidArgument;
    }
    if (settings.MaxShapes > MAX_GEOMETRY_STORE_SHAPES)
    {
        return Status::CapacityExceeded;
    }
    foundation::Array<Slot> slots;
    foundation::Array<uint32> freeSlots;
    if (slots.TryResize(settings.MaxShapes) == false || freeSlots.TryResize(settings.MaxShapes) == false)
    {
        return Status::AllocationFailure;
    }
    // LIFO: slot zero is issued first.
    for (uint32 index = 0; index < settings.MaxShapes; ++index)
    {
        freeSlots[index] = settings.MaxShapes - 1 - index;
    }
    uint64 owner = 0;
    if (foundation::identity::TryIssueLifetimeId(owner) != foundation::identity::LifetimeIdStatus::Success)
    {
        return Status::IdentityExhausted;
    }
    mSlots = foundation::Move(slots);
    mFreeSlots = foundation::Move(freeSlots);
    mOwner = owner;
    return Status::Success;
}

Status GeometryStore::reserveSlot(uint32& outSlot) const noexcept
{
    if (IsInitialized() == false || mReaderCount > 0)
    {
        return Status::InvalidPhase;
    }
    if (mFreeSlots.IsEmpty() == true)
    {
        return Status::CapacityExceeded;
    }
    outSlot = mFreeSlots[mFreeSlots.GetSize() - 1];
    return Status::Success;
}

ShapeHandle GeometryStore::publish(uint32 slot, ShapeKind kind) noexcept
{
    LUDUS_ASSERT(mFreeSlots.IsEmpty() == false && mFreeSlots[mFreeSlots.GetSize() - 1] == slot);
    mFreeSlots.RemoveLast();
    Slot& record = mSlots[slot];
    record.Kind = kind;
    record.LeaseCount = 1;
    ++mLiveCount;
    return { .Owner = mOwner, .Slot = slot, .Generation = record.Generation };
}

Status GeometryStore::TryAddTriangleMesh(TriangleMesh&& mesh, ShapeHandle& outShape) noexcept
{
    uint32 slot = 0;
    const Status reserved = reserveSlot(slot);
    if (reserved != Status::Success)
    {
        return reserved;
    }
    if (mesh.IsCooked() == false)
    {
        return Status::NoGeometry;
    }
    mSlots[slot].Mesh = foundation::Move(mesh);
    outShape = publish(slot, ShapeKind::TriangleMesh);
    return Status::Success;
}

Status GeometryStore::TryAddConvexHull(ConvexHull&& hull, ShapeHandle& outShape) noexcept
{
    uint32 slot = 0;
    const Status reserved = reserveSlot(slot);
    if (reserved != Status::Success)
    {
        return reserved;
    }
    if (hull.IsCooked() == false)
    {
        return Status::NoGeometry;
    }
    Slot& record = mSlots[slot];
    record.Hull = foundation::Move(hull);
    record.Primitive = MakeHullShape(record.Hull);
    outShape = publish(slot, ShapeKind::Hull);
    return Status::Success;
}

Status GeometryStore::TryAddPrimitive(const ConvexShape& shape, ShapeHandle& outShape) noexcept
{
    uint32 slot = 0;
    const Status reserved = reserveSlot(slot);
    if (reserved != Status::Success)
    {
        return reserved;
    }
    if (shape.Kind == ConvexShapeKind::Hull)
    {
        return Status::InvalidArgument;
    }
    const Status validation = internal::ValidateConvexShape(shape);
    if (validation != Status::Success)
    {
        return validation;
    }
    ShapeKind kind = ShapeKind::Sphere;
    if (shape.Kind == ConvexShapeKind::Box)
    {
        kind = ShapeKind::Box;
    }
    else if (shape.Kind == ConvexShapeKind::Capsule)
    {
        kind = ShapeKind::Capsule;
    }
    mSlots[slot].Primitive = shape;
    outShape = publish(slot, kind);
    return Status::Success;
}

GeometryStore::Slot* GeometryStore::findLive(ShapeHandle shape) noexcept
{
    return IsAlive(shape) == true ? &mSlots[shape.Slot] : nullptr;
}

const GeometryStore::Slot* GeometryStore::findLive(ShapeHandle shape) const noexcept
{
    if (mOwner == 0 || shape.Owner != mOwner || shape.Generation == 0 || shape.Slot >= mSlots.GetSize())
    {
        return nullptr;
    }
    const Slot& record = mSlots[shape.Slot];
    return record.LeaseCount > 0 && record.Generation == shape.Generation ? &record : nullptr;
}

Status GeometryStore::TryRetain(ShapeHandle shape) noexcept
{
    if (mReaderCount > 0)
    {
        return Status::InvalidPhase;
    }
    Slot* recordOrNull = findLive(shape);
    if (recordOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (recordOrNull->LeaseCount >= MAX_SHAPE_LEASES)
    {
        return Status::CapacityExceeded;
    }
    ++recordOrNull->LeaseCount;
    return Status::Success;
}

Status GeometryStore::TryRelease(ShapeHandle shape) noexcept
{
    if (mReaderCount > 0)
    {
        return Status::InvalidPhase;
    }
    Slot* recordOrNull = findLive(shape);
    if (recordOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (--recordOrNull->LeaseCount > 0)
    {
        return Status::Success;
    }
    recordOrNull->Mesh = TriangleMesh{};
    recordOrNull->Hull = ConvexHull{};
    recordOrNull->Primitive = ConvexShape{};
    --mLiveCount;
    // A slot released at the terminal generation is retired: it never returns to the free list.
    if (foundation::identity::AdvanceGeneration(recordOrNull->Generation) ==
        foundation::identity::GenerationStatus::Advanced)
    {
        // The free list's capacity holds every slot, so this cannot allocate.
        const bool isListed = mFreeSlots.TryAdd(shape.Slot);
        LUDUS_ASSERT(isListed == true);
        (void)isListed;
    }
    return Status::Success;
}

bool GeometryStore::IsAlive(ShapeHandle shape) const noexcept
{
    return findLive(shape) != nullptr;
}

Status GeometryStore::TryGetKind(ShapeHandle shape, ShapeKind& outKind) const noexcept
{
    const Slot* recordOrNull = findLive(shape);
    if (recordOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    outKind = recordOrNull->Kind;
    return Status::Success;
}

Status GeometryStore::TryGetLeaseCount(ShapeHandle shape, uint32& outLeaseCount) const noexcept
{
    const Slot* recordOrNull = findLive(shape);
    if (recordOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    outLeaseCount = recordOrNull->LeaseCount;
    return Status::Success;
}

Status GeometryStore::TryGetTriangleMesh(ShapeHandle shape, const TriangleMesh*& outMeshOrNull) const noexcept
{
    const Slot* recordOrNull = findLive(shape);
    if (recordOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (recordOrNull->Kind != ShapeKind::TriangleMesh)
    {
        return Status::Unsupported;
    }
    outMeshOrNull = &recordOrNull->Mesh;
    return Status::Success;
}

Status GeometryStore::TryGetConvexShape(ShapeHandle shape, ConvexShape& outShape) const noexcept
{
    const Slot* recordOrNull = findLive(shape);
    if (recordOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (recordOrNull->Kind == ShapeKind::TriangleMesh)
    {
        return Status::Unsupported;
    }
    outShape = recordOrNull->Primitive;
    return Status::Success;
}
} // namespace ludus::physics::collision
