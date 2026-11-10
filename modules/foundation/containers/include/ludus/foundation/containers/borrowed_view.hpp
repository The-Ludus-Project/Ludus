#pragma once

#include <ludus/foundation/base/core.h>

namespace ludus::foundation
{
/// Non-owning contiguous elements with an explicit extent. No allocation or lifetime extension.
/// The caller keeps the storage alive and unmoved while using the view. Const ElementType
/// provides read-only access. A null pointer with a nonzero extent is invalid;
/// consumers must check IsValid before accessing elements. Empty views mean no elements.
template <typename ElementType>
class BorrowedView final
{
public:
    /// Construct an empty valid view without allocating.
    constexpr BorrowedView() noexcept = default;
    /// Borrow count elements; storage validity/lifetime remains the caller's responsibility.
    /// @param dataOrNull Null is allowed only when count is zero for a valid view.
    /// @param count Number of elements, not bytes. Construction preserves invalid input for validation.
    constexpr BorrowedView(ElementType* dataOrNull, usize count) noexcept : mDataOrNull(dataOrNull), mSize(count) {}
    /// Borrow a built-in array, preserving its extent without allocating or copying.
    template <usize Count>
    constexpr BorrowedView(ElementType (&inoutElements)[Count]) noexcept : mDataOrNull(inoutElements), mSize(Count)
    {
    }
    /// Whether the pointer/extent pair permits access; does not prove storage lifetime.
    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return mDataOrNull != nullptr || mSize == 0;
    }
    /// Whether no elements are present; an invalid nonzero extent is not empty.
    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return mSize == 0;
    }
    /// Number of borrowed elements, including an invalid extent until the caller validates it.
    [[nodiscard]] constexpr usize GetSize() const noexcept
    {
        return mSize;
    }
    /// Borrowed address, possibly null. No ownership transfer or lifetime extension.
    [[nodiscard]] constexpr ElementType* GetDataOrNull() const noexcept
    {
        return mDataOrNull;
    }
    /// Access an element without transferring ownership.
    /// @pre IsValid() and index < GetSize(); storage is alive and contains the declared extent.
    [[nodiscard]] constexpr ElementType& operator[](usize index) const noexcept
    {
        LUDUS_ASSERT(IsValid() == true && index < mSize);
        return mDataOrNull[index];
    }

    /// Range-for access to the first borrowed element. Does not extend storage lifetime.
    /// @pre IsValid() and the declared storage remains alive throughout iteration.
    [[nodiscard]] LUDUS_INLINE constexpr ElementType* begin() const noexcept
    {
        LUDUS_ASSERT(IsValid() == true);
        return mDataOrNull;
    }
    /// Range-for sentinel after the last element; null for a default empty view.
    /// @pre IsValid() and the declared storage remains alive throughout iteration.
    [[nodiscard]] LUDUS_INLINE constexpr ElementType* end() const noexcept
    {
        LUDUS_ASSERT(IsValid() == true);
        return mSize == 0 ? mDataOrNull : mDataOrNull + mSize;
    }

private:
    ElementType* mDataOrNull = nullptr;
    usize mSize = 0;
};
} // namespace ludus::foundation
