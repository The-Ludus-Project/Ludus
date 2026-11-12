#pragma once

// Ludus::RingBuffer<ElementType, Count> — a bounded, single-owner FIFO in inline
// storage. Capacity is a compile-time constant; it never allocates.
//
// Live elements occupy a possibly wrapped run of slots starting at the head.
// Only those slots hold objects; the rest is uninitialized storage shared with
// FixedArray's design (detail/inline_slots.hpp). Wrapped storage is exposed as
// two contiguous borrowed views rather than pretending it is one. "Full" is an ordinary
// status from the Try operations. This is not a concurrent queue: the existing
// SPSC/MPSC queues keep their own publication protocols. See
// docs/architecture/container-systems.md (RingBuffer contract).

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/detail/contiguous_storage.hpp>
#include <ludus/foundation/containers/detail/inline_slots.hpp>

#include <type_traits>

namespace ludus::foundation::core
{
/// @brief Bounded first-in, first-out queue in inline storage; never allocates.
///
/// TryAdd appends at the back and RemoveFirst removes from the front. Index 0 is always the oldest element. Adding
/// to a full ring fails and changes nothing. Elements never move while they are live, so a borrow stays valid until
/// that element is removed or the ring is cleared, copied over, moved from or destroyed. Moving the whole ring
/// moves its elements; a moved-from ring is empty.
/// @note One owner; synchronize access externally. This is not a concurrent queue. Element destruction must not
/// throw.
template <typename ElementType, usize Count>
class RingBuffer
{
    static_assert(std::is_const_v<ElementType> == false, "RingBuffer<const T> is not supported");
    static_assert(std::is_reference_v<ElementType> == false, "RingBuffer<T&> is not supported");
    static_assert(std::is_nothrow_destructible_v<ElementType>, "RingBuffer<T> requires a nonthrowing destructor");

public:
    /// Stored element type.
    using ValueType = ElementType;
    /// Size and logical index type.
    using SizeType = usize;

    /// Empty ring; no element is constructed.
    RingBuffer() noexcept = default;

    /// Copy-construct other's elements in FIFO order. The copy starts at the first slot.
    RingBuffer(const RingBuffer& other) noexcept(std::is_nothrow_copy_constructible_v<ElementType>)
        requires std::is_copy_constructible_v<ElementType>
    {
        copyFrom(other);
    }

    /// Move-construct other's elements in FIFO order, then leave other empty.
    RingBuffer(RingBuffer&& other) noexcept
        requires std::is_nothrow_move_constructible_v<ElementType>
    {
        takeFrom(other);
    }

    /// @brief Replace the contents with copies of other's elements in FIFO order.
    ///
    /// The current elements are destroyed first. Self-assignment does nothing.
    RingBuffer& operator=(const RingBuffer& other) noexcept(std::is_nothrow_copy_constructible_v<ElementType>)
        requires std::is_copy_constructible_v<ElementType>
    {
        if (this != &other)
        {
            Clear();
            copyFrom(other);
        }
        return *this;
    }

    /// @brief Destroy the current elements, then move other's elements in and leave other empty.
    ///
    /// Self-assignment does nothing.
    RingBuffer& operator=(RingBuffer&& other) noexcept
        requires std::is_nothrow_move_constructible_v<ElementType>
    {
        if (this != &other)
        {
            Clear();
            takeFrom(other);
        }
        return *this;
    }

    /// Trivial destruction when ElementType is trivially destructible.
    ~RingBuffer()
        requires std::is_trivially_destructible_v<ElementType>
    = default;
    /// Destroy the live elements.
    ~RingBuffer()
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
    /// True when every slot is live, so TryAdd fails. Always true for Count 0.
    [[nodiscard]] LUDUS_INLINE bool IsFull() const noexcept
    {
        return mSize == Count;
    }

    // --- Element access (logical FIFO order) --------------------------------
    /// Element `index` positions after the oldest. @pre index < GetSize() (asserted in checked builds).
    [[nodiscard]] LUDUS_INLINE ElementType& operator[](usize index) noexcept
    {
        LUDUS_ASSERT(index < mSize);
        return *slotAt(index);
    }
    /// Read-only element `index` positions after the oldest. @pre index < GetSize() (asserted in checked builds).
    [[nodiscard]] LUDUS_INLINE const ElementType& operator[](usize index) const noexcept
    {
        LUDUS_ASSERT(index < mSize);
        return *slotAt(index);
    }
    /// Oldest element, the next one RemoveFirst removes. @pre The ring is not empty.
    [[nodiscard]] LUDUS_INLINE ElementType& GetFirst() noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return *slotAt(0);
    }
    /// Read-only oldest element. @pre The ring is not empty.
    [[nodiscard]] LUDUS_INLINE const ElementType& GetFirst() const noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return *slotAt(0);
    }
    /// Newest element. @pre The ring is not empty.
    [[nodiscard]] LUDUS_INLINE ElementType& GetLast() noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return *slotAt(mSize - 1);
    }
    /// Read-only newest element. @pre The ring is not empty.
    [[nodiscard]] LUDUS_INLINE const ElementType& GetLast() const noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return *slotAt(mSize - 1);
    }

    /// @brief The oldest elements, up to the end of the storage, in FIFO order.
    ///
    /// GetLeadingView() followed by GetTrailingView() visits every live element in FIFO order. Both are valid and
    /// empty for an empty ring. Invalidated by any operation that removes the viewed elements.
    [[nodiscard]] BorrowedView<ElementType> GetLeadingView() noexcept
    {
        return BorrowedView<ElementType>(slotData() + headOrZero(), leadingSize());
    }
    /// Read-only form of GetLeadingView().
    [[nodiscard]] BorrowedView<const ElementType> GetLeadingView() const noexcept
    {
        return BorrowedView<const ElementType>(slotData() + headOrZero(), leadingSize());
    }
    /// @brief The newer elements that wrapped to the start of the storage; empty when nothing wrapped.
    [[nodiscard]] BorrowedView<ElementType> GetTrailingView() noexcept
    {
        return BorrowedView<ElementType>(slotData(), mSize - leadingSize());
    }
    /// Read-only form of GetTrailingView().
    [[nodiscard]] BorrowedView<const ElementType> GetTrailingView() const noexcept
    {
        return BorrowedView<const ElementType>(slotData(), mSize - leadingSize());
    }

    // --- Adding and removing ------------------------------------------------
    /// @brief Append a copy of `value` at the back unless the ring is full.
    ///
    /// No live element moves, so `value` may refer to an element of this ring.
    /// @return False when full; the ring and `value` are unchanged.
    [[nodiscard]] bool TryAdd(const ElementType& value) noexcept(std::is_nothrow_copy_constructible_v<ElementType>)
    {
        return TryAddInPlace(value) != nullptr;
    }
    /// @brief Append by moving `value` at the back unless the ring is full.
    ///
    /// @return False when full; the ring is unchanged and `value` is not moved from.
    [[nodiscard]] bool TryAdd(ElementType&& value) noexcept(std::is_nothrow_move_constructible_v<ElementType>)
    {
        return TryAddInPlace(static_cast<ElementType&&>(value)) != nullptr;
    }
    /// @brief Append an element constructed from `args` at the back unless the ring is full.
    ///
    /// No live element moves, so `args` may refer to elements of this ring.
    /// @return The new element, or nullptr when full with the ring and `args` untouched.
    template <typename... Args>
    [[nodiscard]] ElementType*
    TryAddInPlace(Args&&... args) noexcept(std::is_nothrow_constructible_v<ElementType, Args...>)
    {
        if (mSize == Count)
        {
            return nullptr;
        }
        ElementType* slot = internal::ConstructAt(slotAt(mSize), static_cast<Args&&>(args)...);
        ++mSize;
        return slot;
    }

    /// @brief Destroy the oldest element. O(1).
    ///
    /// Invalidates borrows of that element only. @pre The ring is not empty.
    void RemoveFirst() noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        internal::DestroyAt(slotAt(0));
        --mSize;
        // An empty ring restarts at the first slot so later runs stay contiguous.
        mHead = mSize == 0 ? 0 : wrap(mHead + 1);
    }

    /// Destroy every live element and restart at the first slot.
    void Clear() noexcept
    {
        ElementType* slots = slotData();
        const usize leading = leadingSize();
        internal::DestroyRange(slots + headOrZero(), slots + headOrZero() + leading);
        internal::DestroyRange(slots, slots + (mSize - leading));
        mHead = 0;
        mSize = 0;
    }

    // --- Comparison ---------------------------------------------------------
    /// Element-wise equality in FIFO order; the physical slot positions are ignored.
    [[nodiscard]] friend bool operator==(const RingBuffer& left, const RingBuffer& right)
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
    [[nodiscard]] ElementType* slotData() noexcept
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
    [[nodiscard]] const ElementType* slotData() const noexcept
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

    // Physical slot for a position in [0, 2 * Count).
    [[nodiscard]] static constexpr usize wrap(usize position) noexcept
    {
        return position >= Count ? position - Count : position;
    }
    // The head as an offset that is valid even for a zero-capacity ring.
    [[nodiscard]] usize headOrZero() const noexcept
    {
        return mSize == 0 ? 0 : mHead;
    }
    [[nodiscard]] usize leadingSize() const noexcept
    {
        const usize toEnd = Count - headOrZero();
        return mSize < toEnd ? mSize : toEnd;
    }
    [[nodiscard]] ElementType* slotAt(usize index) noexcept
    {
        return slotData() + wrap(mHead + index);
    }
    [[nodiscard]] const ElementType* slotAt(usize index) const noexcept
    {
        return slotData() + wrap(mHead + index);
    }

    // Construct copies of other's elements into this empty ring.
    void copyFrom(const RingBuffer& other)
    {
        LUDUS_ASSERT(mSize == 0);
        const BorrowedView<const ElementType> leading = other.GetLeadingView();
        const BorrowedView<const ElementType> trailing = other.GetTrailingView();
        internal::UninitializedCopy(slotData(), leading.GetDataOrNull(), leading.GetSize());
        internal::UninitializedCopy(slotData() + leading.GetSize(), trailing.GetDataOrNull(), trailing.GetSize());
        mHead = 0;
        mSize = other.mSize;
    }

    // Relocate other's elements into this empty ring, leaving other empty.
    void takeFrom(RingBuffer& other) noexcept
    {
        LUDUS_ASSERT(mSize == 0);
        const BorrowedView<ElementType> leading = other.GetLeadingView();
        const BorrowedView<ElementType> trailing = other.GetTrailingView();
        internal::UninitializedRelocate(slotData(), leading.GetDataOrNull(), leading.GetSize());
        internal::UninitializedRelocate(slotData() + leading.GetSize(), trailing.GetDataOrNull(), trailing.GetSize());
        mHead = 0;
        mSize = other.mSize;
        other.mHead = 0;
        other.mSize = 0;
    }

    detail::InlineSlots<ElementType, Count> mSlots;
    usize mHead = 0;
    usize mSize = 0;
};
} // namespace ludus::foundation::core

namespace ludus::foundation
{
/// Engine-namespace alias for core::RingBuffer.
template <typename ElementType, core::usize Count>
using RingBuffer = core::RingBuffer<ElementType, Count>;
} // namespace ludus::foundation
