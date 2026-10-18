#pragma once

// Private open-addressing index from a name to a uint32 value, used by the OBJ
// reader to find mesh groups and materials in constant expected time (a linear
// search made pathological files quadratic). Keys are copied; lookups never
// allocate. Not a general container: it supports only insert and find.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/strings/string.hpp>

#include <string_view>

namespace ludus::content::scene_cook::importers
{
/// Name to value index with insertion and lookup only.
class NameIndex final
{
public:
    /// Value added for name, or NOT_FOUND.
    [[nodiscard]] foundation::core::uint32 Find(std::string_view name) const noexcept;
    /// Add name, which must not be present yet. @return false when memory is exhausted
    /// (the index is unchanged then).
    [[nodiscard]] bool TryAdd(std::string_view name, foundation::core::uint32 value) noexcept;
    /// Number of names added.
    [[nodiscard]] LUDUS_INLINE foundation::core::usize GetSize() const noexcept
    {
        return mNames.GetSize();
    }

    /// Find's result for an absent name.
    static constexpr foundation::core::uint32 NOT_FOUND = ~foundation::core::uint32{0};

private:
    [[nodiscard]] foundation::core::usize findSlot(std::string_view name, foundation::core::uint64 hash) const noexcept;
    [[nodiscard]] bool tryGrow() noexcept;

    /// Entry number per slot, NOT_FOUND when empty; the size is zero or a power of two.
    foundation::Array<foundation::core::uint32> mSlots;
    foundation::Array<foundation::String> mNames;
    foundation::Array<foundation::core::uint64> mHashes;
    foundation::Array<foundation::core::uint32> mValues;
};
} // namespace ludus::content::scene_cook::importers
