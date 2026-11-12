#pragma once

// Ludus::FixedArray<ElementType, Count> — a bounded, owning, contiguous sequence
// whose live elements form a prefix of inline storage. Capacity is a
// compile-time constant; the size varies at run time. It never allocates.
//
// Only occupied slots hold live objects: the storage is an uninitialized union,
// so element types need not be default-constructible and unused capacity costs
// no construction. "Full" is an ordinary status from the Try operations, not an
// allocation failure. Element lifetime uses the same audited helpers as Array
// (detail/contiguous_storage.hpp). See docs/architecture/container-systems.md
// (FixedArray contract) and docs/wiki/guides/containers.md.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/detail/contiguous_storage.hpp>
#include <ludus/foundation/containers/detail/inline_slots.hpp>

#include <type_traits>

namespace ludus::foundation::core
{
/// @brief Bounded contiguous sequence in inline storage; never allocates.
///
/// Elements [0, GetSize()) are live; the remaining Count - GetSize() slots are raw storage. Adding to a full array
/// fails through the Try operations and changes nothing. Ordered insertion and removal invalidate element borrows
/// at and after the affected position; borrows never move otherwise, because the storage is inline. Moving the
/// whole FixedArray moves its elements, so borrows do not follow a move. A moved-from array is empty.
/// @note One owner; synchronize access externally. Element destruction must not throw.
template <typename ElementType, usize Count>
class FixedArray
{
    static_assert(std::is_const_v<ElementType> == false, "FixedArray<const T> is not supported");
    static_assert(std::is_reference_v<ElementType> == false, "FixedArray<T&> is not supported");
    static_assert(std::is_nothrow_destructible_v<ElementType>, "FixedArray<T> requires a nonthrowing destructor");

public:
    /// Stored element type.
    using ValueType = ElementType;
    /// Size and index type.
    using SizeType = usize;
    /// Contiguous mutable iterator into the live prefix.
    using Iterator = ElementType*;
    /// Contiguous read-only iterator into the live prefix.
    using ConstIterator = const ElementType*;

    /// Empty array; no element is constructed.
    FixedArray() noexcept = default;

    /// Copy-construct each of other's live elements.
    FixedArray(const FixedArray& other) noexcept(std::is_nothrow_copy_constructible_v<ElementType>)
        requires std::is_copy_constructible_v<ElementType>
    {
        internal::UninitializedCopy(GetData(), other.GetData(), other.mSize);
        mSize = other.mSize;
    }

    /// @brief Move-construct each of other's live elements, then leave other empty.
    ///
    /// Borrows into other keep referring to other's slots, which no longer hold live elements.
    FixedArray(FixedArray&& other) noexcept
        requires std::is_nothrow_move_constructible_v<ElementType>
    {
        internal::UninitializedRelocate(GetData(), other.GetData(), other.mSize);
        mSize = other.mSize;
        other.mSize = 0;
    }

    /// @brief Replace the contents with copies of other's elements.
    ///
    /// Assigns the common prefix, constructs additional elements and destroys any surplus. Self-assignment does
    /// nothing.
    FixedArray& operator=(const FixedArray& other) noexcept(std::is_nothrow_copy_constructible_v<ElementType> &&
                                                            std::is_nothrow_copy_assignable_v<ElementType>)
        requires std::is_copy_constructible_v<ElementType> && std::is_copy_assignable_v<ElementType>
    {
        if (this != &other)
        {
            assignFrom(other.GetData(), other.mSize);
        }
        return *this;
    }

    /// @brief Replace the contents by moving other's elements, then leave other empty.
    ///
    /// Self-assignment does nothing.
    FixedArray& operator=(FixedArray&& other) noexcept
        requires std::is_nothrow_move_constructible_v<ElementType> && std::is_nothrow_move_assignable_v<ElementType>
    {
        if (this != &other)
        {
            ElementType* destination = GetData();
            ElementType* source = other.GetData();
            const usize common = mSize < other.mSize ? mSize : other.mSize;
            for (usize i = 0; i < common; ++i)
            {
                destination[i] = static_cast<ElementType&&>(source[i]);
            }
            if (other.mSize > mSize)
            {
                internal::UninitializedRelocate(destination + mSize, source + mSize, other.mSize - mSize);
            }
            else
            {
                internal::DestroyRange(destination + other.mSize, destination + mSize);
            }
            internal::DestroyRange(source, source + common);
            mSize = other.mSize;
            other.mSize = 0;
        }
        return *this;
    }

    /// Trivial destruction when ElementType is trivially destructible.
    ~FixedArray()
        requires std::is_trivially_destructible_v<ElementType>
    = default;
    /// Destroy the live elements.
    ~FixedArray()
    {
        Clear();
    }

    // --- Size / capacity ----------------------------------------------------
    /// Number of live elements.
    [[nodiscard]] LUDUS_INLINE usize GetSize() const noexcept
    {
        return mSize;
    }
    /// Fixed capacity; always Count.
    [[nodiscard]] LUDUS_INLINE static constexpr usize GetCapacity() noexcept
    {
        return Count;
    }
    /// True when no element is live.
    [[nodiscard]] LUDUS_INLINE bool IsEmpty() const noexcept
    {
        return mSize == 0;
    }
    /// True when every slot is live, so the Try add/insert operations fail. Always true for Count 0.
    [[nodiscard]] LUDUS_INLINE bool IsFull() const noexcept
    {
        return mSize == Count;
    }

    /// First slot of the inline storage, or nullptr when Count is 0. Valid for the array's lifetime.
    [[nodiscard]] LUDUS_INLINE ElementType* GetData() noexcept
    {
        if constexpr (Count == 0)
        {
            return nullptr;
        }
        else
        {
            return mSlots.Elements;
        }
    }
    /// Read-only first slot of the inline storage, or nullptr when Count is 0.
    [[nodiscard]] LUDUS_INLINE const ElementType* GetData() const noexcept
    {
        if constexpr (Count == 0)
        {
            return nullptr;
        }
        else
        {
            return mSlots.Elements;
        }
    }

    // --- Element access -----------------------------------------------------
    /// Element at `index`. @pre index < GetSize() (asserted in checked builds).
    [[nodiscard]] LUDUS_INLINE ElementType& operator[](usize index) noexcept
    {
        LUDUS_ASSERT(index < mSize);
        return GetData()[index];
    }
    /// Read-only element at `index`. @pre index < GetSize() (asserted in checked builds).
    [[nodiscard]] LUDUS_INLINE const ElementType& operator[](usize index) const noexcept
    {
        LUDUS_ASSERT(index < mSize);
        return GetData()[index];
    }
    /// First element. @pre The array is not empty.
    [[nodiscard]] LUDUS_INLINE ElementType& GetFirst() noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return GetData()[0];
    }
    /// Read-only first element. @pre The array is not empty.
    [[nodiscard]] LUDUS_INLINE const ElementType& GetFirst() const noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return GetData()[0];
    }
    /// Last element. @pre The array is not empty.
    [[nodiscard]] LUDUS_INLINE ElementType& GetLast() noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return GetData()[mSize - 1];
    }
    /// Read-only last element. @pre The array is not empty.
    [[nodiscard]] LUDUS_INLINE const ElementType& GetLast() const noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return GetData()[mSize - 1];
    }

    // --- Iterators (lowercase for range-for / standard interop) -------------
    /// Range-for/standard interop: first live element.
    [[nodiscard]] LUDUS_INLINE Iterator begin() noexcept
    {
        return GetData();
    }
    /// Range-for/standard interop: read-only first live element.
    [[nodiscard]] LUDUS_INLINE ConstIterator begin() const noexcept
    {
        return GetData();
    }
    /// Standard interop: read-only first live element.
    [[nodiscard]] LUDUS_INLINE ConstIterator cbegin() const noexcept
    {
        return GetData();
    }
    /// Range-for/standard interop: one past the last live element.
    [[nodiscard]] LUDUS_INLINE Iterator end() noexcept
    {
        return endPointer();
    }
    /// Range-for/standard interop: read-only one past the last live element.
    [[nodiscard]] LUDUS_INLINE ConstIterator end() const noexcept
    {
        return endPointer();
    }
    /// Standard interop: read-only one past the last live element.
    [[nodiscard]] LUDUS_INLINE ConstIterator cend() const noexcept
    {
        return endPointer();
    }
    /// Standard interop spelling of GetData().
    [[nodiscard]] LUDUS_INLINE ElementType* data() noexcept
    {
        return GetData();
    }
    /// Standard interop spelling of the read-only GetData().
    [[nodiscard]] LUDUS_INLINE const ElementType* data() const noexcept
    {
        return GetData();
    }
    /// Standard interop spelling of GetSize().
    [[nodiscard]] LUDUS_INLINE usize size() const noexcept
    {
        return mSize;
    }

    // --- Adding elements ----------------------------------------------------
    /// @brief Append a copy of `value` unless the array is full.
    ///
    /// `value` may refer to an element of this array.
    /// @return False when full; the array and `value` are unchanged.
    [[nodiscard]] bool TryAdd(const ElementType& value) noexcept(std::is_nothrow_copy_constructible_v<ElementType>)
    {
        return TryAddInPlace(value) != nullptr;
    }
    /// @brief Append by moving `value` unless the array is full.
    ///
    /// @return False when full; the array is unchanged and `value` is not moved from.
    [[nodiscard]] bool TryAdd(ElementType&& value) noexcept(std::is_nothrow_move_constructible_v<ElementType>)
    {
        return TryAddInPlace(static_cast<ElementType&&>(value)) != nullptr;
    }

    /// @brief Append an element constructed from `args` unless the array is full.
    ///
    /// No existing element moves, so `args` may refer to elements of this array.
    /// @return The new element, or nullptr when full with the array and `args` untouched.
    template <typename... Args>
    [[nodiscard]] ElementType*
    TryAddInPlace(Args&&... args) noexcept(std::is_nothrow_constructible_v<ElementType, Args...>)
    {
        if (mSize == Count)
        {
            return nullptr;
        }
        ElementType* slot = internal::ConstructAt(GetData() + mSize, static_cast<Args&&>(args)...);
        ++mSize;
        return slot;
    }

    /// @brief Append copies of every element of `items`, or none of them.
    ///
    /// @pre `items` does not overlap this array's storage (asserted).
    /// @return False when `items` is invalid (null with a nonzero extent) or the elements do not all fit;
    /// the array is unchanged.
    [[nodiscard]] bool
    TryAddRange(BorrowedView<const ElementType> items) noexcept(std::is_nothrow_copy_constructible_v<ElementType>)
    {
        if (items.IsValid() == false)
        {
            return false;
        }
        const usize count = items.GetSize();
        if (count > Count - mSize)
        {
            return false;
        }
        if (count == 0)
        {
            return true;
        }
        const ElementType* source = items.GetDataOrNull();
        LUDUS_ASSERT(source + count <= GetData() || source >= GetData() + Count);
        internal::UninitializedCopy(GetData() + mSize, source, count);
        mSize += count;
        return true;
    }

    /// @brief Insert a copy of `value` at `index`, shifting later elements, unless the array is full.
    ///
    /// @pre index <= GetSize(). `value` may refer to an element of this array.
    /// @return The inserted element, or nullptr when full with the array unchanged.
    [[nodiscard]] ElementType*
    TryInsertAt(usize index, const ElementType& value) noexcept(std::is_nothrow_copy_constructible_v<ElementType> &&
                                                                std::is_nothrow_move_constructible_v<ElementType> &&
                                                                std::is_nothrow_move_assignable_v<ElementType>)
    {
        return TryInsertAtInPlace(index, value);
    }
    /// @brief Move `value` into `index`, shifting later elements, unless the array is full.
    ///
    /// @pre index <= GetSize().
    /// @return The inserted element, or nullptr when full with the array unchanged and `value` not moved from.
    [[nodiscard]] ElementType*
    TryInsertAt(usize index, ElementType&& value) noexcept(std::is_nothrow_move_constructible_v<ElementType> &&
                                                           std::is_nothrow_move_assignable_v<ElementType>)
    {
        return TryInsertAtInPlace(index, static_cast<ElementType&&>(value));
    }

    /// @brief Insert an element constructed from `args` at `index`, shifting later elements.
    ///
    /// The element is materialized before any element moves, so `args` may refer to elements of this array.
    /// Invalidates borrows at and after `index`. @pre index <= GetSize().
    /// @return The inserted element, or nullptr when full with the array and `args` untouched.
    template <typename... Args>
    [[nodiscard]] ElementType*
    TryInsertAtInPlace(usize index, Args&&... args) noexcept(std::is_nothrow_constructible_v<ElementType, Args...> &&
                                                             std::is_nothrow_move_constructible_v<ElementType> &&
                                                             std::is_nothrow_move_assignable_v<ElementType>)
    {
        LUDUS_ASSERT(index <= mSize);
        if (mSize == Count)
        {
            return nullptr;
        }
        ElementType* elements = GetData();
        if (index == mSize)
        {
            internal::ConstructAt(elements + index, static_cast<Args&&>(args)...);
        }
        else
        {
            ElementType value(static_cast<Args&&>(args)...);
            internal::ShiftRightByOne(elements + index, elements + mSize);
            elements[index] = static_cast<ElementType&&>(value);
        }
        ++mSize;
        return elements + index;
    }

    // --- Removing elements --------------------------------------------------
    /// Destroy the last element. @pre The array is not empty.
    void RemoveLast() noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        --mSize;
        internal::DestroyAt(GetData() + mSize);
    }

    /// @brief Order-preserving removal at `index`; later elements shift left. O(n).
    ///
    /// Invalidates borrows at and after `index`. @pre index < GetSize().
    void RemoveAt(usize index) noexcept
    {
        LUDUS_ASSERT(index < mSize);
        ElementType* elements = GetData();
        internal::ShiftLeftByOne(elements + index, elements + mSize);
        --mSize;
        internal::DestroyAt(elements + mSize);
    }

    /// @brief O(1) removal that moves the last element into `index`; order is not preserved.
    ///
    /// @pre index < GetSize().
    void RemoveAtSwap(usize index) noexcept
    {
        LUDUS_ASSERT(index < mSize);
        ElementType* elements = GetData();
        const usize lastIndex = mSize - 1;
        if (index != lastIndex)
        {
            elements[index] = static_cast<ElementType&&>(elements[lastIndex]);
        }
        --mSize;
        internal::DestroyAt(elements + mSize);
    }

    /// Destroy every live element. The capacity is unchanged.
    void Clear() noexcept
    {
        ElementType* elements = GetData();
        internal::DestroyRange(elements, elements + mSize);
        mSize = 0;
    }

    // --- Borrowed views -----------------------------------------------------
    /// Borrow the live elements as a mutable view; invalidated with element borrows.
    [[nodiscard]] operator BorrowedView<ElementType>() noexcept
    {
        return BorrowedView<ElementType>(GetData(), mSize);
    }
    /// Borrow the live elements as a read-only view; invalidated with element borrows.
    [[nodiscard]] operator BorrowedView<const ElementType>() const noexcept
    {
        return BorrowedView<const ElementType>(GetData(), mSize);
    }
    /// Mutable view over the live elements; invalidated with element borrows.
    [[nodiscard]] BorrowedView<ElementType> AsView() noexcept
    {
        return BorrowedView<ElementType>(GetData(), mSize);
    }
    /// Read-only view over the live elements; invalidated with element borrows.
    [[nodiscard]] BorrowedView<const ElementType> AsView() const noexcept
    {
        return BorrowedView<const ElementType>(GetData(), mSize);
    }

    // --- Comparison ---------------------------------------------------------
    /// Element-wise equality of the live elements; arrays of different sizes are unequal.
    [[nodiscard]] friend bool operator==(const FixedArray& left, const FixedArray& right)
    {
        if (left.mSize != right.mSize)
        {
            return false;
        }
        for (usize i = 0; i < left.mSize; ++i)
        {
            if ((left[i] == right[i]) == false)
            {
                return false;
            }
        }
        return true;
    }

private:
    // Avoid pointer arithmetic on the null data of a zero-capacity array.
    [[nodiscard]] ElementType* endPointer() noexcept
    {
        return mSize == 0 ? GetData() : GetData() + mSize;
    }
    [[nodiscard]] const ElementType* endPointer() const noexcept
    {
        return mSize == 0 ? GetData() : GetData() + mSize;
    }

    // Copy-assign [source, source + count) over the current contents.
    void assignFrom(const ElementType* source, usize count)
    {
        ElementType* destination = GetData();
        const usize common = mSize < count ? mSize : count;
        for (usize i = 0; i < common; ++i)
        {
            destination[i] = source[i];
        }
        if (count > mSize)
        {
            internal::UninitializedCopy(destination + mSize, source + mSize, count - mSize);
        }
        else
        {
            internal::DestroyRange(destination + count, destination + mSize);
        }
        mSize = count;
    }

    detail::InlineSlots<ElementType, Count> mSlots;
    usize mSize = 0;
};
} // namespace ludus::foundation::core

namespace ludus::foundation
{
/// Engine-namespace alias for core::FixedArray.
template <typename ElementType, core::usize Count>
using FixedArray = core::FixedArray<ElementType, Count>;
} // namespace ludus::foundation
