#include "name_index.h"

#include <ludus/foundation/hash/hash.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
using foundation::Array;
using foundation::core::uint32;
using foundation::core::uint64;
using foundation::core::uint8;
using foundation::core::usize;

constexpr usize INITIAL_SLOTS = 64;

[[nodiscard]] uint64 HashName(std::string_view name) noexcept
{
    return foundation::TableHash64({reinterpret_cast<const uint8*>(name.data()), name.size()});
}
} // namespace

usize NameIndex::findSlot(std::string_view name, uint64 hash) const noexcept
{
    const usize mask = mSlots.GetSize() - 1;
    usize slot = static_cast<usize>(hash) & mask;
    // Linear probing; the table is at most half full, so an empty slot exists.
    while (mSlots[slot] != NOT_FOUND)
    {
        const uint32 entry = mSlots[slot];
        if (mHashes[entry] == hash && mNames[entry].GetView() == name)
        {
            return slot;
        }
        slot = (slot + 1) & mask;
    }
    return slot;
}

uint32 NameIndex::Find(std::string_view name) const noexcept
{
    if (mSlots.IsEmpty() == true)
    {
        return NOT_FOUND;
    }
    const uint32 entry = mSlots[findSlot(name, HashName(name))];
    return entry == NOT_FOUND ? NOT_FOUND : mValues[entry];
}

bool NameIndex::tryGrow() noexcept
{
    Array<uint32> slots;
    const usize size = mSlots.IsEmpty() == true ? INITIAL_SLOTS : mSlots.GetSize() * 2;
    if (slots.TryResize(size, NOT_FOUND) == false)
    {
        return false;
    }
    mSlots.Swap(slots);
    const usize mask = mSlots.GetSize() - 1;
    for (usize entry = 0; entry < mNames.GetSize(); ++entry)
    {
        usize slot = static_cast<usize>(mHashes[entry]) & mask;
        while (mSlots[slot] != NOT_FOUND)
        {
            slot = (slot + 1) & mask;
        }
        mSlots[slot] = static_cast<uint32>(entry);
    }
    return true;
}

bool NameIndex::TryAdd(std::string_view name, uint32 value) noexcept
{
    if ((mNames.GetSize() + 1) * 2 > mSlots.GetSize() && tryGrow() == false)
    {
        return false;
    }
    foundation::String copy;
    const uint64 hash = HashName(name);
    if (copy.TryAssign(name) != foundation::StringStatus::Ok ||
        mNames.TryEnsureCapacity(mNames.GetSize() + 1) == false ||
        mHashes.TryEnsureCapacity(mHashes.GetSize() + 1) == false ||
        mValues.TryEnsureCapacity(mValues.GetSize() + 1) == false)
    {
        return false;
    }
    const usize slot = findSlot(name, hash);
    mSlots[slot] = static_cast<uint32>(mNames.GetSize());
    (void)mNames.TryAdd(static_cast<foundation::String&&>(copy));
    (void)mHashes.TryAdd(hash);
    (void)mValues.TryAdd(value);
    return true;
}
} // namespace ludus::content::scene_cook::importers
