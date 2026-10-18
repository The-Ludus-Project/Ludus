#pragma once

// Ludus::Array<ElementType> — a dynamically sized, owning, heap-backed,
// contiguous array. This is the engine's default growable sequence container.
//
// Naming (see docs/architecture/containers.md): the dynamic owning array is
// "Array" (not "Vector") because "Vector" is heavily overloaded in a
// graphics/math engine (Vec2/Vec3/Vec4, direction, velocity, ...). Reserving
// "Vector" for mathematics and calling the growable sequence "Array" removes
// that collision; the fixed-size sibling is Ludus::StaticArray<T, N>. Member
// functions follow the Ludus verb-oriented convention (GetSize/IsEmpty/GetData/
// EnsureCapacity/Add/RemoveLast/...), not STL spellings.
//
// Representation: { ElementType* mData; usize mSize; usize mCapacity; }.
// Invariants (checked by LUDUS_ASSERT in Debug/Development, free in Release):
//   * mSize <= mCapacity
//   * (mData == nullptr) == (mCapacity == 0)
//   * elements [0, mSize) are live objects; [mSize, mCapacity) is raw storage.
// A default-constructed / moved-from Array is exactly {nullptr, 0, 0}.
//
// Error model (Ludus is -fno-exceptions, see AGENTS.md):
//   * Preconditions (index bounds, non-empty GetLast/RemoveLast) -> LUDUS_ASSERT.
//   * Allocation failure / capacity overflow:
//       - infallible API (Add, EnsureCapacity, Resize, ...) -> LUDUS_FATAL.
//       - fallible API (TryEnsureCapacity/TryAdd/TryResize/TryAddRange) -> returns
//         false and leaves the container UNCHANGED (strong guarantee).
//
// Growth: geometric 2x (newCap = oldCap * 2) with a small minimum first
// capacity, overflow-checked. See docs/architecture/containers.md sections 15/32
// (the factor was settled at 2x by benchmark; the constant lives in
// array_support.cpp ComputeGrowthCapacity).
//
// The heavy/cold paths (allocation, growth math, OOM/overflow handling) live
// out-of-line in array_support.cpp so this header stays cheap to parse and the
// common inline paths stay small. Element lifetime lives in the
// detail/contiguous_storage.hpp helpers.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/detail/contiguous_storage.hpp>
#include <ludus/foundation/containers/relocation.hpp>

#include <initializer_list>
#include <span>
#include <type_traits>
#include <utility>

namespace ludus::foundation::core
{
namespace detail
{
// Raw, alignment-aware, nothrow allocation seam. Implemented once in
// array_support.cpp through FoundationMemory's Containers allocation domain,
// which accounts capacity bytes and uses the aligned nothrow operator
// new/delete backend. No call site changes when it is re-pointed. Returns
// nullptr on failure (never throws, never terminates here).
/// Internal allocation seam used by Array; not a general allocator API.
///
/// Returns uninitialized storage of `bytes` bytes aligned to `alignment`, or nullptr on failure or for zero bytes.
/// Never terminates and never throws; the calling container decides whether failure is fatal.
[[nodiscard]] void*
AllocateBytes(usize bytes, // NOLINT(bugprone-easily-swappable-parameters): allocation seam signature
              usize alignment) noexcept;
/// Internal: release a block returned by AllocateBytes with the same size and alignment. A null pointer is ignored.
void FreeBytes(void* ptr,
               usize bytes, // NOLINT(bugprone-easily-swappable-parameters): allocation seam signature
               usize alignment) noexcept;

// Cold, non-template helpers (defined in array_support.cpp):
//   * ComputeGrowthCapacity: 2x geometric growth with minimum + overflow check.
//   * OnAllocationFailure / OnCapacityOverflow: LUDUS_FATAL sinks for the
//     infallible API, kept out-of-line and cold so the hot path carries no
//     diagnostic string weight.
/// Internal growth policy: at least `requested` elements, 2x the current capacity and a cache-line minimum.
/// @return The new element capacity, or 0 when `requested` exceeds the addressable element count.
[[nodiscard]] usize
ComputeGrowthCapacity(usize currentCapacity, // NOLINT(bugprone-easily-swappable-parameters): growth-policy inputs
                      usize requested,
                      usize elementSize) noexcept;
/// Internal: fatal sink for allocation failure in the infallible container operations. Does not return.
[[noreturn]] LUDUS_COLD void OnAllocationFailure() noexcept;
/// Internal: fatal sink for a capacity request that overflows the element count. Does not return.
[[noreturn]] LUDUS_COLD void OnCapacityOverflow() noexcept;

// Largest element count representable without size overflow, for a given element
// size. The exception-free replacement for std::length_error.
/// Internal: largest element count whose byte size cannot overflow, for elements of `elementSize` bytes.
[[nodiscard]] LUDUS_INLINE constexpr usize MaxElementCount(usize elementSize) noexcept
{
    // Reserve headroom below SIZE_MAX; PTRDIFF_MAX-style ceiling on element count.
    constexpr usize kPtrdiffMax = static_cast<usize>(-1) >> 1;
    return elementSize == 0 ? static_cast<usize>(-1) : kPtrdiffMax / elementSize;
}
} // namespace detail

/// @brief Owning, contiguous, growable sequence with explicit fallible mutation APIs.
///
/// Growth invalidates all element borrows; ordered insertion/removal invalidates
/// borrows at and after the affected position. Failed Try operations preserve
/// the container. Infallible allocating operations terminate on exhaustion or
/// capacity overflow. A default or moved-from array owns no storage.
/// @note One owner; synchronize access externally. See docs/architecture/containers.md.
template <typename ElementType>
class Array
{
    static_assert(std::is_const_v<ElementType> == false, "Array<const T> is not supported");
    static_assert(std::is_reference_v<ElementType> == false, "Array<T&> is not supported");
    // Reallocation of a non-relocatable type uses move-construction; a throwing
    // move would break the strong guarantee. Under -fno-exceptions moves cannot
    // throw, but require noexcept-movability (or copyability) to document intent.
    static_assert(IsTriviallyRelocatable<ElementType> || std::is_nothrow_move_constructible_v<ElementType> ||
                      std::is_copy_constructible_v<ElementType>,
                  "Array<T> requires T to be trivially relocatable, nothrow-move-constructible, or copy-constructible");

public:
    /// Stored element type.
    using ValueType = ElementType;
    /// Size and index type.
    using SizeType = usize;
    /// Contiguous mutable iterator; invalidated with element borrows.
    using Iterator = ElementType*;
    /// Contiguous read-only iterator; invalidated with element borrows.
    using ConstIterator = const ElementType*;

    // --- Construction / destruction ----------------------------------------
    /// Empty array that owns no storage. Never allocates.
    constexpr Array() noexcept = default;

    /// @brief Braces supply elements: `Array<int32>{3}` holds one value, whereas (3) value-initializes three
    /// elements.
    ///
    /// The list's const elements are copied.
    Array(std::initializer_list<ElementType> items) noexcept(std::is_nothrow_copy_constructible_v<ElementType>)
        requires std::is_copy_constructible_v<ElementType>
    {
        const usize count = items.size();
        if (count > 0)
        {
            GrowExact(count);
            internal_CopyTail(items.begin(), count);
            mSize = count;
        }
    }

    /// @brief Construct `count` value-initialized elements in one exact allocation.
    ///
    /// Allocation failure or capacity overflow is fatal. Use TryResize on an empty array for a fallible equivalent.
    explicit Array(usize count)
    {
        if (count > 0)
        {
            GrowExact(count);
            internal_ValueConstructTail(count);
            mSize = count;
        }
    }

    /// @brief Construct `count` copies of `value` in one exact allocation.
    ///
    /// Allocation failure or capacity overflow is fatal.
    Array(usize count, const ElementType& value)
    {
        if (count > 0)
        {
            GrowExact(count);
            internal_FillTail(count, value);
            mSize = count;
        }
    }

    /// @brief Deep copy with capacity equal to other's size. Allocation failure is fatal.
    Array(const Array& other)
    {
        if (other.mSize > 0)
        {
            GrowExact(other.mSize);
            internal_CopyTail(other.mData, other.mSize);
            mSize = other.mSize;
        }
    }

    /// @brief Take other's storage without allocating. other becomes empty and owns no storage.
    ///
    /// Borrows into other's elements now refer to this array's elements.
    Array(Array&& other) noexcept : mData(other.mData), mSize(other.mSize), mCapacity(other.mCapacity)
    {
        other.mData = nullptr;
        other.mSize = 0;
        other.mCapacity = 0;
    }

    /// @brief Replace the contents with a copy of other.
    ///
    /// Reuses capacity when it suffices; otherwise frees and reallocates exactly. Existing elements are destroyed
    /// first, and allocation failure is fatal. Self-assignment does nothing.
    Array& operator=(const Array& other)
    {
        if (this == &other)
        {
            return *this;
        }
        // Reuse existing capacity when possible; otherwise reallocate.
        ClearElements();
        if (other.mSize > mCapacity)
        {
            FreeStorage();
            GrowExact(other.mSize);
        }
        internal_CopyTail(other.mData, other.mSize);
        mSize = other.mSize;
        return *this;
    }

    /// @brief Destroy the current elements, free the storage, then take other's storage.
    ///
    /// other becomes empty. Self-assignment does nothing. Never allocates.
    Array& operator=(Array&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        ClearElements();
        FreeStorage();
        mData = other.mData;
        mSize = other.mSize;
        mCapacity = other.mCapacity;
        other.mData = nullptr;
        other.mSize = 0;
        other.mCapacity = 0;
        return *this;
    }

    /// Destroy the live elements and free the storage.
    ~Array()
    {
        ClearElements();
        FreeStorage();
    }

    // --- Size / capacity ----------------------------------------------------
    /// Number of live elements.
    [[nodiscard]] usize GetSize() const noexcept
    {
        return mSize;
    }
    /// Number of elements the current storage holds without reallocating.
    [[nodiscard]] usize GetCapacity() const noexcept
    {
        return mCapacity;
    }
    /// True when the array holds no live elements; capacity may still be retained.
    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return mSize == 0;
    }
    /// First element, or nullptr when no storage is owned. Valid until the next reallocation or destruction.
    [[nodiscard]] ElementType* GetData() noexcept
    {
        return mData;
    }
    /// Read-only first element, or nullptr when no storage is owned.
    [[nodiscard]] const ElementType* GetData() const noexcept
    {
        return mData;
    }
    /// Largest element count the array can request without byte-size overflow.
    [[nodiscard]] static constexpr usize GetMaxSize() noexcept
    {
        return detail::MaxElementCount(kElementSize);
    }

    /// @brief Guarantee GetCapacity() >= newCapacity without changing GetSize().
    ///
    /// If the current capacity already satisfies the request, does nothing (never shrinks). This is the Ludus-
    /// native spelling of "reserve": the name states the postcondition. Infallible (fatal on OOM/overflow).
    void EnsureCapacity(usize newCapacity)
    {
        if (TryEnsureCapacity(newCapacity) == false)
        {
            detail::OnAllocationFailure();
        }
    }

    /// Fallible EnsureCapacity: returns false (container unchanged) on OOM/overflow.
    [[nodiscard]] bool TryEnsureCapacity(usize newCapacity)
    {
        if (newCapacity <= mCapacity)
        {
            return true;
        }
        if (newCapacity > GetMaxSize())
        {
            return false;
        }
        return ReallocateTo(newCapacity);
    }

    /// @brief Set the size to `newSize`, destroying the tail or value-initializing new elements.
    ///
    /// Growth reallocates to exactly `newSize` and invalidates all borrows. Allocation failure is fatal. Supports
    /// move-only element types.
    void Resize(usize newSize)
    {
        if (TryResizeImpl(newSize, nullptr) == false)
        {
            detail::OnAllocationFailure();
        }
    }

    /// Copy-fill resize requires copy-constructible elements; value-initialized resize supports move-only types.
    void Resize(usize newSize, const ElementType& value)
        requires std::is_copy_constructible_v<ElementType>
    {
        if (TryResizeImpl(newSize, &value) == false)
        {
            detail::OnAllocationFailure();
        }
    }

    /// @brief Fallible Resize with value-initialized new elements.
    ///
    /// @return False on allocation failure or overflow, leaving the size, capacity and elements unchanged. Shrinking
    /// always succeeds without allocating.
    [[nodiscard]] bool TryResize(usize newSize)
    {
        return TryResizeImpl(newSize, nullptr);
    }

    /// @brief Fallible Resize that copies `value` into new elements.
    ///
    /// `value` may refer to an element of this array; it is copied before any relocation.
    /// @return False on allocation failure or overflow, leaving the array and `value` unchanged.
    [[nodiscard]] bool TryResize(usize newSize, const ElementType& value)
        requires std::is_copy_constructible_v<ElementType>
    {
        return TryResizeImpl(newSize, &value);
    }

    /// @brief Release capacity down to the current size (best effort; never grows, never
    /// fails observably — on OOM it keeps the larger allocation).
    void TrimCapacity()
    {
        if (mSize == mCapacity)
        {
            return;
        }
        if (mSize == 0)
        {
            FreeStorage();
            return;
        }
        (void)ReallocateTo(mSize);
    }

    /// Destroy every live element and keep the capacity. Never allocates.
    void Clear() noexcept
    {
        ClearElements();
    }

    // --- Element access -----------------------------------------------------
    /// Element at `index`. @pre index < GetSize() (asserted in checked builds).
    [[nodiscard]] ElementType& operator[](usize index) noexcept
    {
        LUDUS_ASSERT(index < mSize);
        return mData[index];
    }
    /// Read-only element at `index`. @pre index < GetSize() (asserted in checked builds).
    [[nodiscard]] const ElementType& operator[](usize index) const noexcept
    {
        LUDUS_ASSERT(index < mSize);
        return mData[index];
    }
    /// First element. @pre The array is not empty.
    [[nodiscard]] ElementType& GetFirst() noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return mData[0];
    }
    /// Read-only first element. @pre The array is not empty.
    [[nodiscard]] const ElementType& GetFirst() const noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return mData[0];
    }
    /// Last element. @pre The array is not empty.
    [[nodiscard]] ElementType& GetLast() noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return mData[mSize - 1];
    }
    /// Read-only last element. @pre The array is not empty.
    [[nodiscard]] const ElementType& GetLast() const noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        return mData[mSize - 1];
    }

    // --- Iterators (lowercase for range-for / ranges / std interop) ---------
    /// Range-for/standard interop: pointer to the first element (null when no storage is owned).
    [[nodiscard]] Iterator begin() noexcept
    {
        return mData;
    }
    /// Range-for/standard interop: read-only pointer to the first element.
    [[nodiscard]] ConstIterator begin() const noexcept
    {
        return mData;
    }
    /// Standard interop: read-only pointer to the first element.
    [[nodiscard]] ConstIterator cbegin() const noexcept
    {
        return mData;
    }
    /// Range-for/standard interop: one past the last live element.
    [[nodiscard]] Iterator end() noexcept
    {
        return mData + mSize;
    }
    /// Range-for/standard interop: read-only one past the last live element.
    [[nodiscard]] ConstIterator end() const noexcept
    {
        return mData + mSize;
    }
    /// Standard interop: read-only one past the last live element.
    [[nodiscard]] ConstIterator cend() const noexcept
    {
        return mData + mSize;
    }
    /// Standard interop spelling of GetData().
    [[nodiscard]] ElementType* data() noexcept
    {
        return mData;
    }
    /// Standard interop spelling of the read-only GetData().
    [[nodiscard]] const ElementType* data() const noexcept
    {
        return mData;
    }
    /// Standard interop spelling of GetSize().
    [[nodiscard]] usize size() const noexcept
    {
        return mSize;
    }

    // --- Adding elements ----------------------------------------------------
    /// @brief Append a copy of `value`; allocation failure is fatal.
    ///
    /// `value` may refer to an element of this array. Growth invalidates all borrows.
    void Add(const ElementType& value)
    {
        AddInPlace(value);
    }
    /// @brief Append by moving `value`; allocation failure is fatal.
    ///
    /// Growth invalidates all borrows. Use TryAdd at a status-returning boundary.
    void Add(ElementType&& value)
    {
        AddInPlace(static_cast<ElementType&&>(value));
    }

    /// @brief Construct a new element in place at the end from `args`.
    ///
    /// Returns a reference to the new element.
    template <typename... Args>
    ElementType& AddInPlace(Args&&... args)
    {
        if (mSize == mCapacity) [[unlikely]]
        {
            // Slow path: growth is required. Materialize the element from `args`
            // into a local BEFORE growing, because `args` may alias an existing
            // element (e.g. array.Add(array.GetLast())) and growth frees the old
            // block. The local lives only in this cold branch, so the hot path
            // below is not penalized (no per-iteration argument spill).
            ElementType value(static_cast<Args&&>(args)...);
            GrowForOne();
            ElementType* slot = internal::ConstructAt(mData + mSize, static_cast<ElementType&&>(value));
            ++mSize;
            return *slot;
        }
        ElementType* slot = internal::ConstructAt(mData + mSize, static_cast<Args&&>(args)...);
        ++mSize;
        return *slot;
    }

    /// Fallible add: returns false (container unchanged) on OOM/overflow.
    [[nodiscard]] bool TryAdd(const ElementType& value)
    {
        return TryAddInPlace(value) != nullptr;
    }
    /// @brief Fallible move append.
    ///
    /// @return False on allocation failure or overflow; the array is unchanged and `value` is not moved from, even when
    /// it refers to an element of this array.
    [[nodiscard]] bool TryAdd(ElementType&& value)
    {
        return TryAddInPlace(static_cast<ElementType&&>(value)) != nullptr;
    }

    /// @brief Fallible append that constructs the element from `args`.
    ///
    /// On growth the new element is constructed in the new block before existing elements are relocated, so `args` may
    /// borrow from this array.
    /// @return The new element, or nullptr on allocation failure or overflow with the array and `args` untouched.
    template <typename... Args>
    [[nodiscard]] ElementType*
    TryAddInPlace(Args&&... args) noexcept(std::is_nothrow_constructible_v<ElementType, Args...> &&
                                           std::is_nothrow_move_constructible_v<ElementType>)
    {
        if (mSize == mCapacity)
        {
            return TryInsertWithGrowth(mSize, static_cast<Args&&>(args)...);
        }
        ElementType* slot = internal::ConstructAt(mData + mSize, static_cast<Args&&>(args)...);
        ++mSize;
        return slot;
    }

    /// Append a contiguous range to the end; allocation failure is fatal.
    /// @pre Input must not overlap this array's storage.
    void AddRange(std::span<const ElementType> items)
    {
        if (TryAddRange(items) == false)
        {
            detail::OnAllocationFailure();
        }
    }

    /// @brief Fallible append of copies of `items`, with at most one allocation.
    ///
    /// @pre `items` does not overlap this array's storage (asserted).
    /// @return False on allocation failure or overflow, leaving the array unchanged.
    [[nodiscard]] bool TryAddRange(std::span<const ElementType> items)
    {
        const usize count = items.size();
        if (count == 0)
        {
            return true;
        }
        // Guard aliasing: appending a view into our own storage would be
        // invalidated by a reallocation. Callers must not do that; assert it.
        LUDUS_ASSERT(items.data() < mData || items.data() >= mData + mCapacity);
        if (mSize + count < mSize) // usize overflow
        {
            return false;
        }
        if (mSize + count > mCapacity)
        {
            // Grow geometrically like TryAdd: exact-fit growth makes repeated small
            // appends (byte writers) quadratic. Reserve/TryEnsureCapacity stay exact.
            const usize newCapacity = detail::ComputeGrowthCapacity(mCapacity, mSize + count, kElementSize);
            if (newCapacity == 0 || newCapacity > GetMaxSize() || ReallocateTo(newCapacity) == false)
            {
                return false;
            }
        }
        internal::UninitializedCopy(mData + mSize, items.data(), count);
        mSize += count;
        return true;
    }

    // --- Removing elements --------------------------------------------------
    /// Destroy the last element. @pre The array is not empty. Keeps the capacity.
    void RemoveLast() noexcept
    {
        LUDUS_ASSERT(mSize > 0);
        --mSize;
        internal::DestroyRange(mData + mSize, mData + mSize + 1);
    }

    /// Order-preserving remove at `index` (shifts the tail left). O(n).
    void RemoveAt(usize index) noexcept
    {
        LUDUS_ASSERT(index < mSize);
        internal::ShiftLeftByOne(mData + index, mData + mSize);
        --mSize;
        internal::DestroyRange(mData + mSize, mData + mSize + 1);
    }

    /// Order-preserving remove of the index range [first, last). O(n).
    void RemoveRange(usize first, usize last) noexcept
    {
        LUDUS_ASSERT(first <= last);
        LUDUS_ASSERT(last <= mSize);
        const usize count = last - first;
        if (count == 0)
        {
            return;
        }
        // Move the tail [last, mSize) down into [first, ...).
        ElementType* dst = mData + first;
        ElementType* src = mData + last;
        ElementType* stop = mData + mSize;
        for (; src != stop; ++src, ++dst)
        {
            *dst = static_cast<ElementType&&>(*src);
        }
        internal::DestroyRange(mData + (mSize - count), stop);
        mSize -= count;
    }

    /// @brief O(1) removal when order does not matter: overwrite the slot with the last element, then drop the
    /// last.
    ///
    /// The engine-preferred removal; named so the O(1) swap behaviour (and the resulting reorder) is explicit
    /// at call sites.
    void RemoveAtSwap(usize index) noexcept
    {
        LUDUS_ASSERT(index < mSize);
        const usize lastIndex = mSize - 1;
        if (index != lastIndex)
        {
            mData[index] = static_cast<ElementType&&>(mData[lastIndex]);
        }
        --mSize;
        internal::DestroyRange(mData + mSize, mData + mSize + 1);
    }

    // --- Inserting elements -------------------------------------------------
    /// @brief Insert a copy of `value` at `index`, shifting later elements; allocation failure is fatal.
    ///
    /// @pre index <= GetSize(). `value` may refer to an element of this array. Invalidates borrows at and after
    /// `index`, or all borrows on growth.
    ElementType& InsertAt(usize index, const ElementType& value)
    {
        return InsertAtInPlace(index, value);
    }
    /// @brief Move `value` into `index`, shifting later elements; allocation failure is fatal.
    ///
    /// @pre index <= GetSize().
    ElementType& InsertAt(usize index, ElementType&& value)
    {
        return InsertAtInPlace(index, static_cast<ElementType&&>(value));
    }

    /// Construct a new element in place at `index`, shifting the tail right. O(n).
    template <typename... Args>
    ElementType& InsertAtInPlace(usize index, Args&&... args)
    {
        LUDUS_ASSERT(index <= mSize);
        if (index == mSize)
        {
            return AddInPlace(static_cast<Args&&>(args)...);
        }
        // Materialize the new value into a temporary FIRST, before any shift or
        // reallocation. Essential when `args` aliases an element of this array
        // (e.g. array.InsertAt(i, array[j])): the shift/realloc would otherwise
        // move or free the referenced element before it is read.
        ElementType value(static_cast<Args&&>(args)...);
        if (mSize == mCapacity) [[unlikely]]
        {
            GrowForOne();
        }
        // Open a gap at `index`; the slot becomes a live moved-from object.
        internal::ShiftRightByOne(mData + index, mData + mSize);
        ++mSize;
        mData[index] = static_cast<ElementType&&>(value);
        return mData[index];
    }

    /// @brief Fallible ordered insertion.
    ///
    /// Allocate before touching arguments so OOM also preserves aliased rvalues and caller-owned move-only
    /// values.
    [[nodiscard]] ElementType*
    TryInsertAt(usize index, const ElementType& value) noexcept(noexcept(TryInsertAtInPlace(index, value)))
    {
        return TryInsertAtInPlace(index, value);
    }
    /// @brief Fallible move insertion at `index`.
    ///
    /// @pre index <= GetSize().
    /// @return The inserted element, or nullptr on allocation failure with the array unchanged and `value` not moved
    /// from.
    [[nodiscard]] ElementType*
    TryInsertAt(usize index,
                ElementType&& value) noexcept(noexcept(TryInsertAtInPlace(index, static_cast<ElementType&&>(value))))
    {
        return TryInsertAtInPlace(index, static_cast<ElementType&&>(value));
    }

    /// @brief Fallible ordered insertion that constructs the element from `args`.
    ///
    /// Allocation happens before `args` are read; on growth the element is built in the new block while borrows into
    /// the old block remain valid. @pre index <= GetSize().
    /// @return The inserted element, or nullptr on allocation failure or overflow with the array and `args` untouched.
    template <typename... Args>
    [[nodiscard]] ElementType*
    TryInsertAtInPlace(usize index, Args&&... args) noexcept(std::is_nothrow_constructible_v<ElementType, Args...> &&
                                                             std::is_nothrow_move_constructible_v<ElementType> &&
                                                             std::is_nothrow_move_assignable_v<ElementType>)
    {
        LUDUS_ASSERT(index <= mSize);
        if (mSize == mCapacity)
        {
            return TryInsertWithGrowth(index, static_cast<Args&&>(args)...);
        }
        else if (index == mSize)
        {
            internal::ConstructAt(mData + index, static_cast<Args&&>(args)...);
        }
        else
        {
            ElementType value(static_cast<Args&&>(args)...);
            internal::ShiftRightByOne(mData + index, mData + mSize);
            mData[index] = static_cast<ElementType&&>(value);
        }
        ++mSize;
        return mData + index;
    }

    // --- Misc ---------------------------------------------------------------
    /// Exchange storage with `other` without allocating. Element borrows follow their storage.
    void Swap(Array& other) noexcept
    {
        ElementType* d = mData;
        usize s = mSize;
        usize c = mCapacity;
        mData = other.mData;
        mSize = other.mSize;
        mCapacity = other.mCapacity;
        other.mData = d;
        other.mSize = s;
        other.mCapacity = c;
    }

    // --- std::span interop --------------------------------------------------
    /// Borrow the live elements as a mutable span; invalidated with element borrows.
    [[nodiscard]] operator std::span<ElementType>() noexcept
    {
        return std::span<ElementType>(mData, mSize);
    }
    /// Borrow the live elements as a read-only span; invalidated with element borrows.
    [[nodiscard]] operator std::span<const ElementType>() const noexcept
    {
        return std::span<const ElementType>(mData, mSize);
    }
    /// Mutable span over the live elements; invalidated with element borrows.
    [[nodiscard]] std::span<ElementType> AsSpan() noexcept
    {
        return std::span<ElementType>(mData, mSize);
    }
    /// Read-only span over the live elements; invalidated with element borrows.
    [[nodiscard]] std::span<const ElementType> AsSpan() const noexcept
    {
        return std::span<const ElementType>(mData, mSize);
    }

    /// Build an Array by copying a contiguous view (explicit range factory).
    [[nodiscard]] static Array FromRange(std::span<const ElementType> items)
    {
        Array result;
        result.EnsureCapacity(items.size());
        internal::UninitializedCopy(result.mData, items.data(), items.size());
        result.mSize = items.size();
        return result;
    }

    // --- Comparison ---------------------------------------------------------
    /// Element-wise equality; arrays of different sizes are unequal. Capacity is ignored.
    [[nodiscard]] friend bool operator==(const Array& a, const Array& b)
    {
        if (a.mSize != b.mSize)
        {
            return false;
        }
        for (usize i = 0; i < a.mSize; ++i)
        {
            if ((a.mData[i] == b.mData[i]) == false)
            {
                return false;
            }
        }
        return true;
    }

private:
    // Size of one element. Centralizes the (legitimate) sizeof(ElementType),
    // which may be a pointer type for Array<T*>; expressed once here so the
    // pointer-sizeof suppression lives in a single place.
    // Clang-tidy 18 with the web SDK misclassifies dependent constexpr
    // initializers; constexpr forbids dynamic initialization here.
    // NOLINTNEXTLINE(bugprone-sizeof-expression,bugprone-dynamic-static-initializers)
    static constexpr usize kElementSize = sizeof(ElementType);

    // Byte size of `count` elements.
    [[nodiscard]] static constexpr usize ByteSize(usize count) noexcept
    {
        return count * kElementSize;
    }

    // Allocate a raw block for `count` elements through the seam (nullptr on
    // failure). Returns typed storage; the void* cast lives here.
    [[nodiscard]] static ElementType* AllocateStorage(usize count) noexcept
    {
        return static_cast<ElementType*>(detail::AllocateBytes(ByteSize(count), alignof(ElementType)));
    }

    // Free a raw block of `capacity` elements through the seam. ElementType* ->
    // void* is a deliberate erase to the byte block (ElementType may be a pointer
    // type for Array<T*>).
    static void FreeStorageBytes(ElementType* dataPtr, usize capacity) noexcept
    {
        // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
        detail::FreeBytes(static_cast<void*>(dataPtr), ByteSize(capacity), alignof(ElementType));
    }

    // Destroy live elements, keep capacity.
    void ClearElements() noexcept
    {
        internal::DestroyRange(mData, mData + mSize);
        mSize = 0;
    }

    // Free the storage block (elements must already be destroyed). Resets to
    // the canonical empty representation.
    void FreeStorage() noexcept
    {
        if (mData != nullptr)
        {
            FreeStorageBytes(mData, mCapacity);
            mData = nullptr;
            mCapacity = 0;
        }
    }

    // Allocate exactly `capacity` slots into an EMPTY array (mData==nullptr).
    // Infallible: fatal on failure. Used by constructors.
    void GrowExact(usize capacity)
    {
        LUDUS_ASSERT(mData == nullptr && mSize == 0 && mCapacity == 0);
        if (capacity > GetMaxSize())
        {
            detail::OnCapacityOverflow();
        }
        ElementType* block = AllocateStorage(capacity);
        if (block == nullptr)
        {
            detail::OnAllocationFailure();
        }
        mData = block;
        mCapacity = capacity;
    }

    // Reallocate to exactly `newCapacity` (>= mSize), relocating live elements.
    // Returns false on allocation failure, leaving the array UNCHANGED.
    [[nodiscard]] bool ReallocateTo(usize newCapacity)
    {
        LUDUS_ASSERT(newCapacity >= mSize);
        if (newCapacity == 0)
        {
            FreeStorage();
            return true;
        }
        ElementType* newData = AllocateStorage(newCapacity);
        if (newData == nullptr)
        {
            return false;
        }
        internal::UninitializedRelocate(newData, mData, mSize);
        // Relocate consumed the sources; free only the old raw block.
        if (mData != nullptr)
        {
            FreeStorageBytes(mData, mCapacity);
        }
        mData = newData;
        mCapacity = newCapacity;
        return true;
    }

    // Grow to hold at least one more element, infallible (fatal on OOM/overflow).
    LUDUS_NOINLINE void GrowForOne()
    {
        const usize newCap = detail::ComputeGrowthCapacity(mCapacity, mCapacity + 1, kElementSize);
        if (newCap == 0)
        {
            detail::OnCapacityOverflow();
        }
        if (ReallocateTo(newCap) == false)
        {
            detail::OnAllocationFailure();
        }
    }

    // Caller requires growth. Keep this separate from the in-buffer shift:
    // append supports move-constructible elements without move assignment.
    template <typename... Args>
    [[nodiscard]] ElementType*
    TryInsertWithGrowth(usize index, Args&&... args) noexcept(std::is_nothrow_constructible_v<ElementType, Args...> &&
                                                              std::is_nothrow_move_constructible_v<ElementType>)
    {
        const usize newCapacity = detail::ComputeGrowthCapacity(mCapacity, mSize + 1, kElementSize);
        if (newCapacity == 0)
        {
            return nullptr;
        }
        ElementType* newData = AllocateStorage(newCapacity);
        if (newData == nullptr)
        {
            return nullptr;
        }
        // Arguments can borrow from the old block. Construct before any
        // relocation consumes them, including insertion at either end.
        internal::ConstructAt(newData + index, static_cast<Args&&>(args)...);
        if (index != 0)
        {
            internal::UninitializedRelocate(newData, mData, index);
        }
        if (index != mSize)
        {
            internal::UninitializedRelocate(newData + index + 1, mData + index, mSize - index);
        }
        if (mData != nullptr)
        {
            FreeStorageBytes(mData, mCapacity);
        }
        mData = newData;
        mCapacity = newCapacity;
        ++mSize;
        return mData + index;
    }

    // Resize implementation shared by Resize/TryResize. `fill` is nullptr for
    // value-initialization, or points to the value to copy for new elements.
    [[nodiscard]] bool TryResizeImpl(usize newSize, const ElementType* fill)
    {
        if (newSize < mSize)
        {
            internal::DestroyRange(mData + newSize, mData + mSize);
            mSize = newSize;
            return true;
        }
        if (newSize == mSize)
        {
            return true;
        }
        ElementType* resizedData = mData;
        if (newSize > mCapacity)
        {
            if (newSize > GetMaxSize())
            {
                return false;
            }
            resizedData = AllocateStorage(newSize);
            if (resizedData == nullptr)
            {
                return false;
            }
        }
        // Fill may refer to a live element in the old block. Construct the tail
        // before relocating (and therefore consuming) any of those elements.
        // Allocation failure above leaves both the array and fill untouched.
        const usize added = newSize - mSize;
        if (fill == nullptr)
        {
            internal::UninitializedValueConstruct(resizedData + mSize, added);
        }
        else if constexpr (std::is_copy_constructible_v<ElementType>)
        {
            internal::UninitializedFill(resizedData + mSize, added, *fill);
        }
        if (resizedData != mData)
        {
            internal::UninitializedRelocate(resizedData, mData, mSize);
            if (mData != nullptr)
            {
                FreeStorageBytes(mData, mCapacity);
            }
            mData = resizedData;
            mCapacity = newSize;
        }
        mSize = newSize;
        return true;
    }

    // Tail constructors used only right after GrowExact on an empty array.
    void internal_ValueConstructTail(usize count)
    {
        internal::UninitializedValueConstruct(mData, count);
    }
    void internal_FillTail(usize count, const ElementType& value)
    {
        internal::UninitializedFill(mData, count, value);
    }
    void internal_CopyTail(const ElementType* src, usize count)
    {
        internal::UninitializedCopy(mData, src, count);
    }

    ElementType* mData = nullptr;
    usize mSize = 0;
    usize mCapacity = 0;
};

/// Non-member Swap for ADL.
template <typename ElementType>
void Swap(Array<ElementType>& a, Array<ElementType>& b) noexcept
{
    a.Swap(b);
}
} // namespace ludus::foundation::core

namespace ludus::foundation
{
/// Engine-namespace alias for core::Array.
template <typename ElementType>
using Array = core::Array<ElementType>;
} // namespace ludus::foundation
