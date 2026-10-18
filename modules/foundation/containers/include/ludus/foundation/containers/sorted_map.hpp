#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>

#include <span>
#include <type_traits>

namespace ludus::foundation::core
{
/// Stateless ordering policy. Keys must provide a nonthrowing strict weak order.
struct KeyLess
{
    /// True when `left` orders before `right` by `operator<`.
    template <typename KeyType>
    [[nodiscard]] constexpr bool operator()(const KeyType& left, const KeyType& right) const
        noexcept(noexcept(left < right))
    {
        return left < right;
    }
};

/// @brief One contiguous, sorted allocation.
///
/// O(log n) lookup, O(n) insertion/removal.
/// Keys are exposed only through const entries; Find permits value mutation.
/// Insert/remove invalidate borrows at/after the position, growth invalidates
/// all borrows, failed/duplicate insertion invalidates none. Single-owner.
/// See docs/architecture/container-systems.md for the full lifetime contract.
template <typename KeyType, typename ValueType, typename Compare = KeyLess>
class SortedMap
{
    static_assert(std::is_nothrow_copy_constructible_v<KeyType> && std::is_nothrow_move_constructible_v<KeyType> &&
                  std::is_nothrow_move_assignable_v<KeyType> && std::is_nothrow_destructible_v<KeyType>);
    static_assert(std::is_nothrow_move_constructible_v<ValueType> && std::is_nothrow_move_assignable_v<ValueType> &&
                  std::is_nothrow_destructible_v<ValueType>);
    static_assert(std::is_empty_v<Compare> && std::is_nothrow_default_constructible_v<Compare>);
    static_assert(std::is_nothrow_invocable_r_v<bool, const Compare&, const KeyType&, const KeyType&>);

public:
    /// @brief One stored key/value pair, kept in ascending key order.
    ///
    /// Public access is read-only through AsSpan()/begin(); mutate values through Find().
    struct Entry
    {
        /// Immutable through the map's public access; changing it would break the ordering.
        KeyType Key;
        /// Mapped value.
        ValueType Value;

        /// Copy `key` and construct the value from `args`; construction must not throw.
        template <typename... Args>
        explicit Entry(const KeyType& key, Args&&... args) noexcept : Key(key), Value(static_cast<Args&&>(args)...)
        {
            static_assert(std::is_nothrow_constructible_v<ValueType, Args...>);
        }
    };

    /// @brief Outcome of TryAddInPlace()/AddInPlace().
    ///
    /// Non-null with Inserted=true: new entry. Non-null with Inserted=false: existing key, value and arguments
    /// untouched. Null: allocation or capacity failure, map unchanged.
    struct InsertResult
    {
        ValueType* Value = nullptr; ///< nullptr: allocation/capacity failure.
        bool Inserted = false;      ///< non-null and false: existing key, untouched.
    };

    /// Empty map that owns no storage. Never allocates.
    SortedMap() noexcept = default;
    /// @brief Copy every entry; available when values are nothrow copyable.
    ///
    /// Allocation failure is fatal.
    SortedMap(const SortedMap&) noexcept
        requires std::is_nothrow_copy_constructible_v<ValueType>
    = default;
    /// @brief Replace the entries with copies of other's; available when values are nothrow copyable.
    ///
    /// Allocation failure is fatal.
    SortedMap& operator=(const SortedMap&) noexcept
        requires std::is_nothrow_copy_constructible_v<ValueType>
    = default;
    /// Take other's storage without allocating; other becomes empty.
    SortedMap(SortedMap&&) noexcept = default;
    /// Destroy the current entries, then take other's storage; other becomes empty.
    SortedMap& operator=(SortedMap&&) noexcept = default;
    /// Destroy every entry and free the storage.
    ~SortedMap() = default;

    /// Number of entries.
    [[nodiscard]] usize GetSize() const noexcept
    {
        return mEntries.GetSize();
    }
    /// Number of entries the storage holds before insertion must reallocate.
    [[nodiscard]] usize GetCapacity() const noexcept
    {
        return mEntries.GetCapacity();
    }
    /// True when the map holds no entries; capacity may still be retained.
    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return mEntries.IsEmpty();
    }

    /// Reserve room for at least `capacity` entries; allocation failure is fatal. Growth invalidates all borrows.
    void EnsureCapacity(usize capacity) noexcept
    {
        mEntries.EnsureCapacity(capacity);
    }
    /// @brief Fallible EnsureCapacity().
    ///
    /// @return False on allocation failure or overflow, leaving the map and its borrows unchanged.
    [[nodiscard]] bool TryEnsureCapacity(usize capacity) noexcept
    {
        return mEntries.TryEnsureCapacity(capacity);
    }

    /// @brief Mutable value for `key`, or nullptr when absent. O(log n); never allocates.
    ///
    /// The pointer is invalidated by insertion or removal at or before the entry, growth, Clear(), Reset() and
    /// destruction.
    [[nodiscard]] ValueType* Find(const KeyType& key) noexcept
    {
        const usize index = LowerBound(key);
        return Matches(index, key) ? &mEntries[index].Value : nullptr;
    }
    /// Read-only value for `key`, or nullptr when absent. O(log n); never allocates.
    [[nodiscard]] const ValueType* Find(const KeyType& key) const noexcept
    {
        const usize index = LowerBound(key);
        return Matches(index, key) ? &mEntries[index].Value : nullptr;
    }
    /// True when an entry equivalent to `key` exists. O(log n).
    [[nodiscard]] bool Contains(const KeyType& key) const noexcept
    {
        return Find(key) != nullptr;
    }

    /// @brief Insert `key` with a value constructed from `args` unless an equivalent key exists.
    ///
    /// A duplicate consumes nothing and changes nothing. On allocation failure the map is unchanged and `args`,
    /// including moved or aliased values, are untouched. O(log n) search plus O(n) moves; growth invalidates all
    /// borrows.
    template <typename... Args>
        requires std::is_nothrow_constructible_v<ValueType, Args...>
    [[nodiscard]] InsertResult TryAddInPlace(const KeyType& key, Args&&... args) noexcept
    {
        const usize index = LowerBound(key);
        if (Matches(index, key))
        {
            return { .Value = &mEntries[index].Value, .Inserted = false };
        }
        Entry* entry = mEntries.TryInsertAtInPlace(index, key, static_cast<Args&&>(args)...);
        return { .Value = entry == nullptr ? nullptr : &entry->Value, .Inserted = entry != nullptr };
    }

    /// Infallible TryAddInPlace(): allocation failure is fatal, so Value is never null.
    template <typename... Args>
        requires std::is_nothrow_constructible_v<ValueType, Args...>
    InsertResult AddInPlace(const KeyType& key, Args&&... args) noexcept
    {
        InsertResult result = TryAddInPlace(key, static_cast<Args&&>(args)...);
        if (result.Value == nullptr)
        {
            detail::OnAllocationFailure();
        }
        return result;
    }

    /// @brief Remove the entry equivalent to `key`.
    ///
    /// @return False when absent. Keeps the capacity and invalidates borrows at and after the removed position.
    [[nodiscard]] bool Remove(const KeyType& key) noexcept
    {
        const usize index = LowerBound(key);
        if (Matches(index, key) == false)
        {
            return false;
        }
        mEntries.RemoveAt(index);
        return true;
    }

    /// Destroy every entry and keep the capacity. Never allocates.
    void Clear() noexcept
    {
        mEntries.Clear();
    }
    /// Destroy every entry and release the storage. Never allocates.
    void Reset() noexcept
    {
        mEntries.Clear();
        mEntries.TrimCapacity();
    }
    /// Exchange storage with `other` without allocating. Borrows follow their storage.
    void Swap(SortedMap& other) noexcept
    {
        mEntries.Swap(other.mEntries);
    }

    /// Entries in ascending key order. Read-only so keys cannot be changed; invalidated by any mutation.
    [[nodiscard]] std::span<const Entry> AsSpan() const noexcept
    {
        return mEntries.AsSpan();
    }
    /// Range-for: first entry in ascending key order (null when no storage is owned).
    [[nodiscard]] const Entry* begin() const noexcept
    {
        return mEntries.GetData();
    }
    /// Range-for: one past the last entry.
    [[nodiscard]] const Entry* end() const noexcept
    {
        // Avoid pointer arithmetic on the null data of a default/moved map.
        return IsEmpty() ? mEntries.GetData() : mEntries.GetData() + GetSize();
    }

private:
    [[nodiscard]] usize LowerBound(const KeyType& key) const noexcept
    {
        usize first = 0;
        usize count = GetSize();
        while (count != 0)
        {
            const usize step = count / 2;
            const usize middle = first + step;
            if (Compare{}(mEntries[middle].Key, key))
            {
                first = middle + 1;
                count -= step + 1;
            }
            else
            {
                count = step;
            }
        }
        return first;
    }
    [[nodiscard]] bool Matches(usize index, const KeyType& key) const noexcept
    {
        // LowerBound already proves stored key is not less than the query.
        return index != GetSize() && Compare{}(key, mEntries[index].Key) == false;
    }

    Array<Entry> mEntries;
};

/// ADL swap: exchanges storage without allocating.
template <typename KeyType, typename ValueType, typename Compare>
void Swap(SortedMap<KeyType, ValueType, Compare>& left, SortedMap<KeyType, ValueType, Compare>& right) noexcept
{
    left.Swap(right);
}
} // namespace ludus::foundation::core

namespace ludus::foundation
{
using core::KeyLess;
/// Engine-namespace alias for core::SortedMap.
template <typename KeyType, typename ValueType, typename Compare = KeyLess>
using SortedMap = core::SortedMap<KeyType, ValueType, Compare>;
} // namespace ludus::foundation
