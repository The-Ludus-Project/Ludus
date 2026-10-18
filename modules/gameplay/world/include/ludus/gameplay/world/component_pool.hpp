#pragma once
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/gameplay/world/entity_registry.h>

#include <span>
#include <type_traits>
#include <utility>
namespace ludus::gameplay::world
{
/// @brief Plain owned state only: construction/move/destruction must not allocate or invoke gameplay.
///
/// Owner/value spans have identical lengths and are phase-local.
template <typename T>
class ComponentPool final
{
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);

public:
    ComponentPool() noexcept = default;
    ComponentPool(const ComponentPool&) = delete;
    ComponentPool& operator=(const ComponentPool&) = delete;
    ComponentPool(ComponentPool&& other) noexcept
    {
        *this = foundation::Move(other);
    }
    ComponentPool& operator=(ComponentPool&& other) noexcept
    {
        if (this != &other)
        {
            mSparse = foundation::Move(other.mSparse);
            mOwners = foundation::Move(other.mOwners);
            mValues = foundation::Move(other.mValues);
            mWorld = std::exchange(other.mWorld, 0);
        }
        return *this;
    }
    [[nodiscard]] Status TryInitialize(const EntityRegistry& registry) noexcept
    {
        if (mWorld != 0 || registry.CanMutateStructure() == false)
        {
            return Status::InvalidConfiguration;
        }
        const auto count = registry.GetCapacity();
        foundation::core::Array<foundation::uint32> sparse;
        foundation::core::Array<EntityId> owners;
        foundation::core::Array<T> values;
        if (sparse.TryResize(count, MissingIndex()) == false || owners.TryEnsureCapacity(count) == false ||
            values.TryEnsureCapacity(count) == false)
        {
            return Status::AllocationFailure;
        }
        mSparse = foundation::Move(sparse);
        mOwners = foundation::Move(owners);
        mValues = foundation::Move(values);
        mWorld = registry.GetWorld();
        return Status::Success;
    }
    [[nodiscard]] const T* Find(EntityId entity) const noexcept
    {
        if (entity.World != mWorld || entity.World == 0 || entity.Generation == 0 || entity.Slot >= mSparse.GetSize())
        {
            return nullptr;
        }
        const auto index = mSparse[entity.Slot];
        return index < mOwners.GetSize() && mOwners[index] == entity ? &mValues[index] : nullptr;
    }
    [[nodiscard]] T* Find(EntityId entity) noexcept
    {
        return const_cast<T*>(static_cast<const ComponentPool&>(*this).Find(entity));
    }
    [[nodiscard]] Status TryInsert(const EntityRegistry& registry, EntityId entity, T value) noexcept
    {
        if (registry.CanMutateStructure() == false)
        {
            return Status::InvalidPhase;
        }
        if (registry.GetWorld() != mWorld || registry.IsAlive(entity) == false)
        {
            return Status::InvalidEntity;
        }
        if (Find(entity) != nullptr)
        {
            return Status::DuplicateComponent;
        }
        if (mSparse[entity.Slot] != MissingIndex() || mValues.GetSize() >= mSparse.GetSize())
        {
            return Status::CapacityExceeded;
        }
        const auto index = static_cast<foundation::uint32>(mValues.GetSize());
        // Initialized capacity covers the full registry limit. Publication never
        // allocates and TryAdd cannot fail after this preflight.
        if (mValues.GetCapacity() <= index || mOwners.GetCapacity() <= index)
        {
            return Status::CapacityExceeded;
        }
        [[maybe_unused]] const bool valueAdded = mValues.TryAdd(value);
        [[maybe_unused]] const bool ownerAdded = mOwners.TryAdd(entity);
        LUDUS_ASSERT(valueAdded && ownerAdded);
        mSparse[entity.Slot] = index;
        return Status::Success;
    }
    [[nodiscard]] Status TryRemove(const EntityRegistry& registry, EntityId entity) noexcept
    {
        if (registry.CanMutateStructure() == false)
        {
            return Status::InvalidPhase;
        }
        if (registry.GetWorld() != mWorld || registry.IsAlive(entity) == false)
        {
            return Status::InvalidEntity;
        }
        if (Find(entity) == nullptr)
        {
            return Status::MissingComponent;
        }
        const auto index = mSparse[entity.Slot];
        const auto moved = mOwners.GetLast();
        mValues.RemoveAtSwap(index);
        mOwners.RemoveAtSwap(index);
        mSparse[moved.Slot] = index;
        mSparse[entity.Slot] = MissingIndex();
        return Status::Success;
    }
    [[nodiscard]] std::span<const EntityId> GetOwners() const noexcept
    {
        return mOwners.AsSpan();
    }
    [[nodiscard]] std::span<const T> GetValues() const noexcept
    {
        return mValues.AsSpan();
    }
    [[nodiscard]] std::span<T> GetValues() noexcept
    {
        return mValues.AsSpan();
    }

private:
    // A slot sentinel needs no storage or per-specialization initialization.
    static constexpr foundation::uint32 MissingIndex() noexcept
    {
        return 0xffffffffU;
    }
    foundation::core::Array<foundation::uint32> mSparse;
    foundation::core::Array<EntityId> mOwners;
    foundation::core::Array<T> mValues;
    foundation::uint64 mWorld = 0;
};
} // namespace ludus::gameplay::world
