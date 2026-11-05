#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>

namespace ludus::tooling::shader_language::internal
{
/// Smallest capacity reserved by TryReserveGrowing.
inline constexpr foundation::usize MINIMUM_GROWTH_CAPACITY = 16;

/// @brief Ensure capacity for `required` elements, at least doubling when growth is needed.
///
/// Array::TryEnsureCapacity reserves exactly; repeated small reservations in a loop would copy the
/// whole array each time. Doubling keeps repeated appends amortized linear.
/// @return False on allocation failure or overflow; the array is unchanged.
template <typename ElementType>
[[nodiscard]] bool TryReserveGrowing(foundation::Array<ElementType>& inoutArray, foundation::usize required) noexcept
{
    const foundation::usize capacity = inoutArray.GetCapacity();
    if (required <= capacity)
    {
        return true;
    }
    foundation::usize grown = capacity <= foundation::Array<ElementType>::GetMaxSize() / 2
                                  ? capacity * 2
                                  : foundation::Array<ElementType>::GetMaxSize();
    grown = grown < MINIMUM_GROWTH_CAPACITY ? MINIMUM_GROWTH_CAPACITY : grown;
    return inoutArray.TryEnsureCapacity(grown > required ? grown : required);
}
} // namespace ludus::tooling::shader_language::internal
