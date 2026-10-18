#pragma once

#include <ludus/foundation/base/core.h>

#include <type_traits>

namespace ludus::foundation
{
/// @cond INTERNAL
namespace detail
{
template <typename T>
void SortSwap(T& left, T& right) noexcept
{
    T saved = Move(left);
    left = Move(right);
    right = Move(saved);
}

template <typename T, typename Compare>
void SortSift(T* values, usize root, usize count, const Compare& less) noexcept
{
    // Testing root against count/2 avoids overflow when computing the child.
    while (root < count / 2)
    {
        usize child = root * 2 + 1;
        if (child + 1 < count && less(values[child], values[child + 1]))
        {
            ++child;
        }
        if (less(values[root], values[child]) == false)
        {
            break;
        }
        SortSwap(values[root], values[child]);
        root = child;
    }
}
} // namespace detail
/// @endcond

/// Sorts initialized contiguous elements in place using O(count log count) comparisons and O(1) extra storage.
/// Equivalent elements may be reordered. No allocation occurs; existing element addresses remain valid.
/// @param values Array of at least count initialized elements; may be null when count is zero.
/// @param count Number of elements to sort.
/// @param less Nonthrowing strict weak ordering over elements.
/// @pre Element move construction/assignment and the comparator cannot throw. Values is nonnull for nonzero count.
template <typename T, typename Compare>
void Sort(T* values, usize count, const Compare& less) noexcept
{
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>);
    LUDUS_ASSERT(values != nullptr || count == 0);
    if (count < 2)
    {
        return;
    }
    for (usize root = count / 2; root > 0;)
    {
        detail::SortSift(values, --root, count, less);
    }
    for (usize remaining = count; remaining > 1;)
    {
        detail::SortSwap(values[0], values[--remaining]);
        detail::SortSift(values, 0, remaining, less);
    }
}
} // namespace ludus::foundation
