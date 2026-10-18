#pragma once

#include <ludus/foundation/base/types.h>

#include <type_traits>

namespace ludus::foundation
{
/// Stable O(count log count) merge sort with no allocation.
/// @param values Mutable array of count initialized elements.
/// @param scratch Separate array of at least count initialized elements; contents may be overwritten.
/// @param count Element count; zero permits null arrays.
/// @param less Nonthrowing strict weak order; equivalent values retain their input order.
/// @return False for a null nonempty array or identical arrays, preserving values; otherwise true.
/// @pre Arrays do not partially overlap and have sufficient capacity. Copy assignment is nonthrowing.
template <typename T, typename Compare>
[[nodiscard]] bool StableSort(T* values, T* scratch, usize count, const Compare& less) noexcept
{
    static_assert(std::is_nothrow_copy_assignable_v<T>);
    static_assert(std::is_nothrow_invocable_r_v<bool, const Compare&, const T&, const T&>);
    if (count == 0)
    {
        return true;
    }
    if (values == nullptr || scratch == nullptr || values == scratch)
    {
        return false;
    }
    for (usize width = 1; width < count;)
    {
        for (usize first = 0; first < count;)
        {
            const usize middle = first + (width < count - first ? width : count - first);
            const usize last = middle + (width < count - middle ? width : count - middle);
            usize left = first, right = middle;
            for (usize output = first; output < last; ++output)
            {
                if (left < middle && (right == last || less(values[right], values[left]) == false))
                {
                    scratch[output] = values[left++];
                }
                else
                {
                    scratch[output] = values[right++];
                }
            }
            first = last;
        }
        for (usize i = 0; i < count; ++i)
        {
            values[i] = scratch[i];
        }
        if (width >= count - width)
        {
            break;
        }
        width *= 2;
    }
    return true;
}
} // namespace ludus::foundation
