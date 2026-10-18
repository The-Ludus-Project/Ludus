#include <ludus/gameplay/world/entity_registry.h>

#include <ludus/foundation/identity/handle.hpp>
#include <ludus/foundation/identity/lifetime.hpp>

#include <limits>
#include <utility>

// World tokens and slot generations follow the shared FoundationIdentity policy
// (docs/architecture/object-handles-uuid.md, implementation stage 2): one process
// lifetime token per initialized registry, never reused, and generations that
// retire at TERMINAL_GENERATION instead of wrapping.
namespace ludus::gameplay::world
{
using namespace foundation;
// The shared source keeps EntityRegistry's original terminal: the last world token
// is UINT64_MAX - 1, and the next initialization reports IdentityExhausted.
static_assert(identity::LAST_LIFETIME_ID == std::numeric_limits<uint64>::max() - 1);
EntityRegistry::EntityRegistry(EntityRegistry&& other) noexcept
{
    *this = Move(other);
}
EntityRegistry& EntityRegistry::operator=(EntityRegistry&& other) noexcept
{
    if (this != &other)
    {
        mSlots = Move(other.mSlots);
        mFree = Move(other.mFree);
        mWorld = std::exchange(other.mWorld, 0);
        mNextSequence = other.mNextSequence;
        mRevision = other.mRevision;
        mStructural = other.mStructural;
    }
    return *this;
}
Status EntityRegistry::TryInitialize(uint32 capacity) noexcept
{
    if (mWorld != 0 || capacity == 0)
    {
        return Status::InvalidConfiguration;
    }
    core::Array<Slot> slots;
    core::Array<uint32> free;
    if (slots.TryResize(capacity) == false || free.TryEnsureCapacity(capacity) == false)
    {
        return Status::AllocationFailure;
    }
    for (uint32 index = capacity; index > 0; --index)
    {
        [[maybe_unused]] const bool added = free.TryAdd(index - 1);
        LUDUS_ASSERT(added);
    }
    static_assert(Slot{}.Generation == identity::FIRST_GENERATION);
    // Issue the token last so every earlier failure leaves the registry and the
    // process source untouched. The source stops after LAST_LIFETIME_ID, so world
    // tokens are never zero and never wrap.
    uint64 world = 0;
    if (identity::TryIssueLifetimeId(world) == identity::LifetimeIdStatus::IdentityExhausted)
    {
        return Status::IdentityExhausted;
    }
    mSlots = Move(slots);
    mFree = Move(free);
    mWorld = world;
    mNextSequence = 1;
    mRevision = 0;
    mStructural = true;
    return Status::Success;
}
Status EntityRegistry::CheckStructuralBudget(usize creates, usize releases) const noexcept
{
    if (mWorld == 0)
    {
        return Status::InvalidConfiguration;
    }
    if (creates > mFree.GetSize())
    {
        return Status::CapacityExceeded;
    }
    const auto remaining = std::numeric_limits<uint64>::max() - mRevision;
    if (creates > remaining || releases > remaining - creates ||
        creates > std::numeric_limits<uint64>::max() - mNextSequence)
    {
        return Status::IdentityExhausted;
    }
    return Status::Success;
}
Status EntityRegistry::TryCreate(EntityId& output) noexcept
{
    if (CanMutateStructure() == false)
    {
        return Status::InvalidPhase;
    }
    if (mNextSequence == std::numeric_limits<uint64>::max() || mRevision == std::numeric_limits<uint64>::max())
    {
        return Status::IdentityExhausted;
    }
    if (mFree.IsEmpty())
    {
        return Status::CapacityExceeded;
    }
    const auto index = mFree.GetLast();
    mFree.RemoveLast();
    auto& slot = mSlots[index];
    slot.Live = true;
    slot.Pending = false;
    slot.Sequence = mNextSequence++;
    ++mRevision;
    output = {mWorld, index, slot.Generation};
    return Status::Success;
}
bool EntityRegistry::IsAlive(EntityId entity) const noexcept
{
    return entity.World != 0 && entity.World == mWorld && entity.Slot < mSlots.GetSize() && mSlots[entity.Slot].Live &&
           entity.Generation == mSlots[entity.Slot].Generation;
}
bool EntityRegistry::IsActive(EntityId entity) const noexcept
{
    return IsAlive(entity) && mSlots[entity.Slot].Pending == false;
}
uint64 EntityRegistry::GetCreationSequence(EntityId entity) const noexcept
{
    return IsAlive(entity) ? mSlots[entity.Slot].Sequence : 0;
}
Status EntityRegistry::MarkPendingDestroy(EntityId entity) noexcept
{
    if (IsAlive(entity) == false)
    {
        return Status::InvalidEntity;
    }
    mSlots[entity.Slot].Pending = true;
    return Status::Success;
}
Status EntityRegistry::TryRelease(EntityId entity) noexcept
{
    if (CanMutateStructure() == false)
    {
        return Status::InvalidPhase;
    }
    if (IsAlive(entity) == false)
    {
        return Status::InvalidEntity;
    }
    if (mRevision == std::numeric_limits<uint64>::max())
    {
        return Status::IdentityExhausted;
    }
    auto& slot = mSlots[entity.Slot];
    slot.Live = false;
    slot.Pending = false;
    // A slot released at TERMINAL_GENERATION is retired: it keeps its final
    // generation, never returns to the free list, and still counts against capacity.
    const identity::GenerationStatus advanced = identity::AdvanceGeneration(slot.Generation);
    LUDUS_ASSERT(advanced != identity::GenerationStatus::InvalidGeneration);
    if (advanced == identity::GenerationStatus::Advanced)
    {
        [[maybe_unused]] const bool isAdded = mFree.TryAdd(entity.Slot);
        LUDUS_ASSERT(isAdded);
    }
    ++mRevision;
    return Status::Success;
}
} // namespace ludus::gameplay::world
