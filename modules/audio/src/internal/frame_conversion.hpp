#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/checked_integer.hpp>

namespace ludus::audio::internal
{
// The existing source/session loop policy rounds to nearest, with ties upward.
// Keep the quotient/remainder formula (avoids overflowing frame * targetRate),
// but check its final sum as well as the product. No floating-point conversion.
// Thanks to Eric Lengyel, "Bit Hacks for Games", Game Engine Gems 2, chapter 24,
// pp. 391-401, for the integer-limit audit lesson, not an implementation listing.
// Review: docs/architecture/primitive-types.md, "Boundary adoption audit".
[[nodiscard]] constexpr bool TryResampleFrame(ludus::foundation::uint64 frame,
                                              ludus::foundation::uint32 sourceRate,
                                              ludus::foundation::uint32 targetRate,
                                              ludus::foundation::uint64& out) noexcept
{
    using namespace ludus::foundation;
    if (sourceRate == 0 || targetRate == 0)
    {
        return false;
    }
    uint64 whole{};
    uint64 remainder{};
    uint64 result{};
    if (TryMultiply(frame / sourceRate, static_cast<uint64>(targetRate), whole) == false ||
        TryMultiply(frame % sourceRate, static_cast<uint64>(targetRate), remainder) == false ||
        TryAdd(remainder, static_cast<uint64>(sourceRate / 2), remainder) == false ||
        TryAdd(whole, remainder / sourceRate, result) == false)
    {
        return false;
    }
    out = result;
    return true;
}
} // namespace ludus::audio::internal
