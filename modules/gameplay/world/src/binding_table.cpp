// Stage 4 durable binding table. See the header and
// docs/architecture/object-handles-uuid.md ("Resolution and streaming").

#include <ludus/gameplay/world/binding_table.h>

#include <ludus/foundation/identity/uuid.hpp>

namespace ludus::gameplay::world
{
using foundation::identity::PersistentObjectKey;
using foundation::identity::Uuid;

/// The forward mapping type, named so TryAddInPlace's result type is spellable.
using ForwardMap = foundation::core::SortedMap<PersistentObjectKey, EntityId, PersistentKeyLess>;

namespace
{
/// Lexicographic comparison of two UUIDs by their canonical bytes.
/// Returns negative, zero or positive like a three-way compare.
[[nodiscard]] int CompareUuid(const Uuid& left, const Uuid& right) noexcept
{
    for (foundation::usize i = 0; i < foundation::identity::UUID_BYTE_COUNT; ++i)
    {
        if (left.Bytes[i] != right.Bytes[i])
        {
            return left.Bytes[i] < right.Bytes[i] ? -1 : 1;
        }
    }
    return 0;
}
} // namespace

bool PersistentKeyLess::operator()(const PersistentObjectKey& left, const PersistentObjectKey& right) const noexcept
{
    const int instanceOrder = CompareUuid(left.Instance.Value, right.Instance.Value);
    if (instanceOrder != 0)
    {
        return instanceOrder < 0;
    }
    return CompareUuid(left.Object.Value, right.Object.Value) < 0;
}

const char* GetResolveStatusName(ResolveStatus status) noexcept
{
    switch (status)
    {
        case ResolveStatus::Resolved:
            return "Resolved";
        case ResolveStatus::NotResident:
            return "NotResident";
        case ResolveStatus::Missing:
            return "Missing";
        case ResolveStatus::TypeMismatch:
            return "TypeMismatch";
        case ResolveStatus::InvalidReference:
            return "InvalidReference";
        case ResolveStatus::Conflict:
            return "Conflict";
    }
    return "Unknown";
}

Status WorldBindingTable::TryInitialize(foundation::uint32 slotCapacity) noexcept
{
    mReverse.Clear();
    if (mReverse.TryResize(slotCapacity) == false)
    {
        return Status::AllocationFailure;
    }
    mForward.Clear();
    mBindingRevision = 0;
    return Status::Success;
}

Status WorldBindingTable::TryReserve(foundation::usize additional) noexcept
{
    const foundation::usize target = mForward.GetSize() + additional;
    if (mForward.TryEnsureCapacity(target) == false)
    {
        return Status::AllocationFailure;
    }
    return Status::Success;
}

Status WorldBindingTable::TryActivate(const EntityRegistry& registry,
                                      std::span<const BindingCandidate> candidates) noexcept
{
    const foundation::uint64 world = registry.GetWorld();
    const foundation::usize slotCapacity = mReverse.GetSize();

    // First pass validates the whole batch so a failure publishes nothing.
    for (foundation::usize i = 0; i < candidates.size(); ++i)
    {
        const BindingCandidate& candidate = candidates[i];
        if (candidate.Key.Instance.IsNil() == true || candidate.Key.Object.IsNil() == true)
        {
            return Status::InvalidConfiguration;
        }
        if (candidate.Entity.World != world || registry.IsAlive(candidate.Entity) == false)
        {
            return Status::InvalidEntity;
        }
        if (candidate.Entity.Slot >= slotCapacity)
        {
            return Status::InvalidConfiguration;
        }
        // A key already published, or repeated within this batch, is a conflict.
        if (mForward.Contains(candidate.Key) == true)
        {
            return Status::InvalidConfiguration;
        }
        for (foundation::usize j = 0; j < i; ++j)
        {
            if (candidates[j].Key == candidate.Key)
            {
                return Status::InvalidConfiguration;
            }
        }
    }

    // Reserve before any mutation so publication cannot fail partway.
    const Status reserved = TryReserve(candidates.size());
    if (reserved != Status::Success)
    {
        return reserved;
    }

    // Second pass publishes both mappings. Capacity is already reserved, so the
    // sorted-map inserts cannot fail here.
    for (const BindingCandidate& candidate : candidates)
    {
        const ForwardMap::InsertResult insertion = mForward.TryAddInPlace(candidate.Key, candidate.Entity);
        if (insertion.Value == nullptr)
        {
            // Unreachable after a successful reservation; treated as a hard failure.
            return Status::AllocationFailure;
        }
        SlotBinding& reverse = mReverse[candidate.Entity.Slot];
        reverse.Key = candidate.Key;
        reverse.Owner = candidate.Entity.World;
        reverse.Generation = candidate.Entity.Generation;
        reverse.HasBinding = true;
    }

    ++mBindingRevision;
    return Status::Success;
}

Status WorldBindingTable::DetachBinding(const EntityRegistry& registry, EntityId entity) noexcept
{
    if (entity.World != registry.GetWorld())
    {
        return Status::InvalidEntity;
    }
    if (entity.Slot >= mReverse.GetSize())
    {
        return Status::Success;
    }
    SlotBinding& reverse = mReverse[entity.Slot];
    if (reverse.HasBinding == false || reverse.Owner != entity.World || reverse.Generation != entity.Generation)
    {
        // No binding, or a different incarnation already recycled this slot.
        return Status::Success;
    }
    const PersistentObjectKey key = reverse.Key;
    reverse.HasBinding = false;
    reverse.Owner = 0;
    reverse.Generation = 0;
    reverse.Key = PersistentObjectKey{};
    const bool isRemoved = mForward.Remove(key);
    if (isRemoved == true)
    {
        ++mBindingRevision;
    }
    return Status::Success;
}

ResolveStatus WorldBindingTable::TryResolve(const EntityRegistry& registry,
                                            const PersistentObjectKey& key,
                                            EntityId& outEntity) noexcept
{
    if (key.Instance.IsNil() == true || key.Object.IsNil() == true)
    {
        return ResolveStatus::InvalidReference;
    }
    const EntityId* bound = mForward.Find(key);
    if (bound == nullptr)
    {
        return ResolveStatus::Missing;
    }
    if (registry.IsAlive(*bound) == false)
    {
        // The incarnation is gone (unload/reload or destruction). Drop the stale
        // binding and advance the revision so cached results are invalidated; the
        // durable key remains available for a later explicit resolve.
        const PersistentObjectKey stale = key;
        if (mForward.Remove(stale) == true)
        {
            ++mBindingRevision;
        }
        return ResolveStatus::Missing;
    }
    outEntity = *bound;
    return ResolveStatus::Resolved;
}

bool WorldBindingTable::TryGetBinding(EntityId entity, PersistentObjectKey& outKey) const noexcept
{
    if (entity.Slot >= mReverse.GetSize())
    {
        return false;
    }
    const SlotBinding& reverse = mReverse[entity.Slot];
    if (reverse.HasBinding == false || reverse.Owner != entity.World || reverse.Generation != entity.Generation)
    {
        return false;
    }
    outKey = reverse.Key;
    return true;
}
} // namespace ludus::gameplay::world
