#pragma once

// Ludus::StaticArray<ElementType, Count> — a fixed-size, owning, contiguous
// aggregate whose element count is a compile-time constant.
//
// Broad analogue of std::array: no indirection, no allocation. It is an
// aggregate (no user-declared constructors, no private data, no bases) so it
// supports aggregate initialization and constexpr, and is trivially copyable /
// trivially destructible exactly when ElementType is.
//
// Naming (see docs/architecture/containers.md): the type is StaticArray (not
// "Array") so it is immediately obvious at a call site that the size is fixed at
// compile time; the dynamically-sized owning array is Ludus::Array<T>. Member
// functions follow the Ludus verb-oriented convention (GetSize/IsEmpty/GetData/
// GetFirst/GetLast/Fill/Swap), not STL spellings. Lowercase begin/end/data/size
// aliases exist only for range-for / ranges / std-algorithm interop.
//
// Bounds checks in operator[] use LUDUS_ASSERT: rich in Debug/Development,
// compiled out (zero cost) in Profile/Release. There is no checked accessor that
// throws (Ludus is -fno-exceptions). StaticArray converts to BorrowedView for
// pointer+count interop; it does not include <span>.
//
// The storage member is public (mData) because an aggregate cannot hide it and
// remain brace-initializable; treat it as an implementation detail and use the
// accessors.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

#include <compare>
#include <type_traits>
#include <utility>

namespace ludus::foundation::core
{
/// Fixed-size owning aggregate; no allocation. Element count is a compile-time constant.
/// Bounds and nonempty element access are caller preconditions.
template <typename ElementType, usize Count>
struct StaticArray
{
    static_assert(Count > 0);

    /// Stored element type.
    using ValueType = ElementType;
    /// Size and index type.
    using SizeType = usize;
    /// Contiguous mutable iterator into the inline storage.
    using Iterator = ElementType*;
    /// Contiguous read-only iterator into the inline storage.
    using ConstIterator = const ElementType*;

    /// Aggregate storage for Count live elements; prefer the indexed accessors.
    ElementType mData[Count];

    // --- Size / capacity (all compile-time constants) -----------------------
    /// Element count; always Count.
    [[nodiscard]] static constexpr usize GetSize() noexcept
    {
        return Count;
    }
    /// Capacity; always Count, because every element is live.
    [[nodiscard]] static constexpr usize GetCapacity() noexcept
    {
        return Count;
    }
    /// True only when Count is 0 (handled by the zero-size specialization).
    [[nodiscard]] static constexpr bool IsEmpty() noexcept
    {
        return Count == 0;
    }

    // --- Data ---------------------------------------------------------------
    /// First element of the inline storage; valid for the array's lifetime.
    [[nodiscard]] constexpr ElementType* GetData() noexcept
    {
        return mData;
    }
    /// Read-only first element of the inline storage.
    [[nodiscard]] constexpr const ElementType* GetData() const noexcept
    {
        return mData;
    }

    // --- Element access -----------------------------------------------------
    /// Element at `index`. @pre index < Count (asserted in checked builds).
    [[nodiscard]] constexpr ElementType& operator[](usize index) noexcept
    {
        LUDUS_ASSERT(index < Count);
        return mData[index];
    }
    /// Read-only element at `index`. @pre index < Count (asserted in checked builds).
    [[nodiscard]] constexpr const ElementType& operator[](usize index) const noexcept
    {
        LUDUS_ASSERT(index < Count);
        return mData[index];
    }
    /// First element. @pre Count > 0.
    [[nodiscard]] constexpr ElementType& GetFirst() noexcept
    {
        return mData[0];
    }
    /// Read-only first element. @pre Count > 0.
    [[nodiscard]] constexpr const ElementType& GetFirst() const noexcept
    {
        return mData[0];
    }
    /// Last element. @pre Count > 0.
    [[nodiscard]] constexpr ElementType& GetLast() noexcept
    {
        return mData[Count - 1];
    }
    /// Read-only last element. @pre Count > 0.
    [[nodiscard]] constexpr const ElementType& GetLast() const noexcept
    {
        return mData[Count - 1];
    }

    // --- Iterators (lowercase for range-for / ranges interop) ---------------
    /// Range-for/standard interop: first element.
    [[nodiscard]] constexpr Iterator begin() noexcept
    {
        return mData;
    }
    /// Range-for/standard interop: read-only first element.
    [[nodiscard]] constexpr ConstIterator begin() const noexcept
    {
        return mData;
    }
    /// Standard interop: read-only first element.
    [[nodiscard]] constexpr ConstIterator cbegin() const noexcept
    {
        return mData;
    }
    /// Range-for/standard interop: one past the last element.
    [[nodiscard]] constexpr Iterator end() noexcept
    {
        return mData + Count;
    }
    /// Range-for/standard interop: read-only one past the last element.
    [[nodiscard]] constexpr ConstIterator end() const noexcept
    {
        return mData + Count;
    }
    /// Standard interop: read-only one past the last element.
    [[nodiscard]] constexpr ConstIterator cend() const noexcept
    {
        return mData + Count;
    }

    // Lowercase data()/size() so free std:: utilities and generic code work.
    /// Standard interop spelling of GetData().
    [[nodiscard]] constexpr ElementType* data() noexcept
    {
        return mData;
    }
    /// Standard interop spelling of the read-only GetData().
    [[nodiscard]] constexpr const ElementType* data() const noexcept
    {
        return mData;
    }
    /// Standard interop spelling of GetSize().
    [[nodiscard]] static constexpr usize size() noexcept
    {
        return Count;
    }

    // --- Bulk ops -----------------------------------------------------------
    /// Copy-assign `value` to every element. `value` may refer to an element of this array.
    constexpr void Fill(const ElementType& value)
    {
        for (usize i = 0; i < Count; ++i)
        {
            mData[i] = value;
        }
    }

    /// @brief Swap element-by-element with `other` using ADL swap.
    ///
    /// O(Count); element addresses do not move, their values do.
    constexpr void Swap(StaticArray& other) noexcept(std::is_nothrow_swappable_v<ElementType>)
    {
        using std::swap;
        for (usize i = 0; i < Count; ++i)
        {
            swap(mData[i], other.mData[i]);
        }
    }

    // --- Borrowed views -----------------------------------------------------
    /// Borrow all Count elements as a mutable view; valid while this array is alive and unmoved.
    [[nodiscard]] constexpr operator BorrowedView<ElementType>() noexcept
    {
        return BorrowedView<ElementType>(mData, Count);
    }
    /// Borrow all Count elements as a read-only view; valid while this array is alive and unmoved.
    [[nodiscard]] constexpr operator BorrowedView<const ElementType>() const noexcept
    {
        return BorrowedView<const ElementType>(mData, Count);
    }
    /// Mutable view over all Count elements; valid while this array is alive and unmoved.
    [[nodiscard]] constexpr BorrowedView<ElementType> AsView() noexcept
    {
        return BorrowedView<ElementType>(mData, Count);
    }
    /// Read-only view over all Count elements; valid while this array is alive and unmoved.
    [[nodiscard]] constexpr BorrowedView<const ElementType> AsView() const noexcept
    {
        return BorrowedView<const ElementType>(mData, Count);
    }

    // --- Comparison (defaulted; value/key usable) ---------------------------
    /// Element-wise equality.
    [[nodiscard]] friend constexpr bool operator==(const StaticArray&, const StaticArray&) = default;
    /// Lexicographic element-wise ordering; requires ordered elements.
    [[nodiscard]] friend constexpr auto operator<=>(const StaticArray&, const StaticArray&) = default;
};

/// Empty fixed-size aggregate. Data/iterators are nondereferenceable; element access is invalid.
template <typename ElementType>
struct StaticArray<ElementType, 0>
{
    /// Element type of the empty array.
    using ValueType = ElementType;
    /// Size and index type.
    using SizeType = usize;
    /// Iterator type; every iterator of the empty array is null.
    using Iterator = ElementType*;
    /// Read-only iterator type; every iterator of the empty array is null.
    using ConstIterator = const ElementType*;

    /// Element count; always 0.
    [[nodiscard]] static constexpr usize GetSize() noexcept
    {
        return 0;
    }
    /// Capacity; always 0.
    [[nodiscard]] static constexpr usize GetCapacity() noexcept
    {
        return 0;
    }
    /// Always true.
    [[nodiscard]] static constexpr bool IsEmpty() noexcept
    {
        return true;
    }

    /// Always nullptr.
    [[nodiscard]] constexpr ElementType* GetData() noexcept
    {
        return nullptr;
    }
    /// Always nullptr.
    [[nodiscard]] constexpr const ElementType* GetData() const noexcept
    {
        return nullptr;
    }
    /// Standard interop spelling of GetData(); always nullptr.
    [[nodiscard]] constexpr ElementType* data() noexcept
    {
        return nullptr;
    }
    /// Standard interop spelling of the read-only GetData(); always nullptr.
    [[nodiscard]] constexpr const ElementType* data() const noexcept
    {
        return nullptr;
    }
    /// Standard interop spelling of GetSize(); always 0.
    [[nodiscard]] static constexpr usize size() noexcept
    {
        return 0;
    }

    /// Range-for/standard interop: null, equal to end().
    [[nodiscard]] constexpr Iterator begin() noexcept
    {
        return nullptr;
    }
    /// Range-for/standard interop: null, equal to end().
    [[nodiscard]] constexpr ConstIterator begin() const noexcept
    {
        return nullptr;
    }
    /// Standard interop: null, equal to cend().
    [[nodiscard]] constexpr ConstIterator cbegin() const noexcept
    {
        return nullptr;
    }
    /// Range-for/standard interop: null.
    [[nodiscard]] constexpr Iterator end() noexcept
    {
        return nullptr;
    }
    /// Range-for/standard interop: null.
    [[nodiscard]] constexpr ConstIterator end() const noexcept
    {
        return nullptr;
    }
    /// Standard interop: null.
    [[nodiscard]] constexpr ConstIterator cend() const noexcept
    {
        return nullptr;
    }

    /// No-op: there are no elements.
    constexpr void Fill(const ElementType&) noexcept {}
    /// No-op: there are no elements.
    constexpr void Swap(StaticArray&) noexcept {}

    /// Empty valid view.
    [[nodiscard]] constexpr operator BorrowedView<ElementType>() noexcept
    {
        return BorrowedView<ElementType>();
    }
    /// Empty valid read-only view.
    [[nodiscard]] constexpr operator BorrowedView<const ElementType>() const noexcept
    {
        return BorrowedView<const ElementType>();
    }
    /// Empty valid view.
    [[nodiscard]] constexpr BorrowedView<ElementType> AsView() noexcept
    {
        return BorrowedView<ElementType>();
    }
    /// Empty valid read-only view.
    [[nodiscard]] constexpr BorrowedView<const ElementType> AsView() const noexcept
    {
        return BorrowedView<const ElementType>();
    }

    /// Empty arrays are always equal.
    [[nodiscard]] friend constexpr bool operator==(const StaticArray&, const StaticArray&) noexcept
    {
        return true;
    }
    /// Empty arrays are always equivalent.
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const StaticArray&, const StaticArray&) noexcept
    {
        return std::strong_ordering::equal;
    }
};

/// Non-member Swap for ADL.
template <typename ElementType, usize Count>
constexpr void Swap(StaticArray<ElementType, Count>& a,
                    StaticArray<ElementType, Count>& b) noexcept(noexcept(a.Swap(b)))
{
    a.Swap(b);
}

// Deduction guide: StaticArray{a, b, c} deduces StaticArray<CommonType, N>.
/// Deduce `StaticArray{a, b, c}` as `StaticArray<decltype(a), 3>`; every initializer must convert to the first's type.
template <typename First, typename... Rest>
StaticArray(First, Rest...) -> StaticArray<First, 1 + sizeof...(Rest)>;
} // namespace ludus::foundation::core

namespace ludus::foundation
{
/// Engine-namespace alias for core::StaticArray.
template <typename ElementType, core::usize Count>
using StaticArray = core::StaticArray<ElementType, Count>;
} // namespace ludus::foundation
