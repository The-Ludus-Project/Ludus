#pragma once

// Opt-in std::span adapters for the existing span compatibility interfaces that
// ADR 0003 still permits (docs/decisions/0003-standard-library-usage-policy.md).
// The container headers themselves are span-free: they convert to BorrowedView.
// Include this header only in code that must call an existing std::span API, so
// <span> and its per-type instantiations stay out of every other translation
// unit. New view APIs take BorrowedView instead.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

#include <span>
#include <type_traits>
#include <utility>

namespace ludus::foundation
{
/// Borrow a contiguous container's live elements as a mutable std::span for an existing span API.
/// Accepts Array, StaticArray, FixedArray and any type with GetData() and GetSize().
/// @return A span over [GetData(), GetData() + GetSize()); invalidated with element borrows of the container.
template <typename ContainerType>
    requires requires(ContainerType& inoutContainer) {
        inoutContainer.GetData();
        inoutContainer.GetSize();
    }
[[nodiscard]] constexpr std::span<std::remove_pointer_t<decltype(std::declval<ContainerType&>().GetData())>>
MakeSpan(ContainerType& inoutContainer) noexcept
{
    return std::span<std::remove_pointer_t<decltype(inoutContainer.GetData())>>(inoutContainer.GetData(),
                                                                                inoutContainer.GetSize());
}

/// Borrow a contiguous container's live elements as a read-only std::span for an existing span API.
/// @return A span over the live elements; invalidated with element borrows of the container.
template <typename ContainerType>
    requires requires(const ContainerType& container) {
        container.GetData();
        container.GetSize();
    }
[[nodiscard]] constexpr std::span<std::remove_pointer_t<decltype(std::declval<const ContainerType&>().GetData())>>
MakeSpan(const ContainerType& container) noexcept
{
    return std::span<std::remove_pointer_t<decltype(container.GetData())>>(container.GetData(), container.GetSize());
}

/// Rejected: a span over a temporary container would dangle at the end of the full expression.
template <typename ContainerType>
    requires requires(const ContainerType& container) {
        container.GetData();
        container.GetSize();
    }
void MakeSpan(const ContainerType&& container) = delete;

/// Convert a borrowed view to a std::span over the same elements for an existing span API.
/// @pre view.IsValid(); std::span cannot represent a null pointer with a nonzero extent.
template <typename ElementType>
[[nodiscard]] constexpr std::span<ElementType> MakeSpan(BorrowedView<ElementType> view) noexcept
{
    LUDUS_ASSERT(view.IsValid() == true);
    return std::span<ElementType>(view.GetDataOrNull(), view.GetSize());
}

/// Borrow the elements of a std::span from an existing span API as a BorrowedView.
/// @return A valid view over the same pointer and extent; storage lifetime remains the caller's.
template <typename ElementType, usize Extent>
[[nodiscard]] constexpr BorrowedView<ElementType> MakeBorrowedView(std::span<ElementType, Extent> elements) noexcept
{
    return BorrowedView<ElementType>(elements.data(), elements.size());
}
} // namespace ludus::foundation
