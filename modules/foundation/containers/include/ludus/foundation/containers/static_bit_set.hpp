#pragma once

// Ludus::StaticBitSet<BitCount> — a fixed-size set of bits packed into uint64
// words. Bit count is a compile-time constant; it never allocates and is a
// trivially copyable constexpr value type.
//
// Invariant: bits at or above BitCount in the last word are always zero, so
// counts, comparisons and complements never see stale tail bits. There is no
// proxy reference: individual bits are read with IsSet and written with
// Set/Reset/Assign/Flip. The in-memory word layout is not a serialization
// format; persist bits through an explicit schema.
//
// Thanks to Henry S. Warren, Jr., "Hacker's Delight", 2nd edition (Addison-
// Wesley, 2012), section 5-1 "Counting 1-Bits" (the parallel "sideways addition"
// population count) and section 5-4 "Counting Trailing 0's" (binary search on
// the isolated lowest bit). Both are adapted as portable constexpr C++ without
// compiler intrinsics or <bit>, which ADR 0003 does not list. A measured
// intrinsic path would need that policy change first.
// Design: docs/architecture/container-systems.md (StaticBitSet contract).

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::foundation::core
{
namespace detail
{
/// Internal: number of set bits in `word`.
[[nodiscard]] constexpr usize CountSetBits(uint64 word) noexcept
{
    word = word - ((word >> 1U) & 0x5555555555555555ULL);
    word = (word & 0x3333333333333333ULL) + ((word >> 2U) & 0x3333333333333333ULL);
    word = (word + (word >> 4U)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<usize>((word * 0x0101010101010101ULL) >> 56U);
}

/// Internal: index of the lowest set bit. @pre word != 0.
[[nodiscard]] constexpr usize CountTrailingZeroBits(uint64 word) noexcept
{
    usize count = 0;
    if ((word & 0xFFFFFFFFULL) == 0)
    {
        count += 32;
        word >>= 32U;
    }
    if ((word & 0xFFFFULL) == 0)
    {
        count += 16;
        word >>= 16U;
    }
    if ((word & 0xFFULL) == 0)
    {
        count += 8;
        word >>= 8U;
    }
    if ((word & 0xFULL) == 0)
    {
        count += 4;
        word >>= 4U;
    }
    if ((word & 0x3ULL) == 0)
    {
        count += 2;
        word >>= 2U;
    }
    if ((word & 0x1ULL) == 0)
    {
        count += 1;
    }
    return count;
}
} // namespace detail

/// @brief Fixed-size set of BitCount bits, indexed 0 to BitCount - 1, packed into uint64 words.
///
/// Default construction resets every bit. Index arguments are caller preconditions, asserted in checked builds.
/// Bit `i` lives in word `i / 64` at bit position `i % 64`; that layout is visible through GetWords() but is not
/// a wire format.
/// @note A value type with no allocation. Synchronize concurrent mutation externally.
template <usize BitCount>
class StaticBitSet
{
public:
    /// Bits stored per word.
    static constexpr usize BITS_PER_WORD = 64;
    /// Number of words holding the bits; 0 when BitCount is 0.
    static constexpr usize WORD_COUNT = (BitCount + BITS_PER_WORD - 1) / BITS_PER_WORD;

    /// Every bit reset.
    constexpr StaticBitSet() noexcept = default;

    // --- Size ---------------------------------------------------------------
    /// Number of bits; always BitCount.
    [[nodiscard]] LUDUS_INLINE static constexpr usize GetSize() noexcept
    {
        return BitCount;
    }

    // --- Single bits --------------------------------------------------------
    /// True when bit `index` is set. @pre index < GetSize().
    [[nodiscard]] LUDUS_INLINE constexpr bool IsSet(usize index) const noexcept
    {
        LUDUS_ASSERT(index < BitCount);
        return (mWords[index / BITS_PER_WORD] & bitMask(index)) != 0;
    }
    /// Set bit `index`. @pre index < GetSize().
    LUDUS_INLINE constexpr void Set(usize index) noexcept
    {
        LUDUS_ASSERT(index < BitCount);
        mWords[index / BITS_PER_WORD] |= bitMask(index);
    }
    /// Reset bit `index`. @pre index < GetSize().
    LUDUS_INLINE constexpr void Reset(usize index) noexcept
    {
        LUDUS_ASSERT(index < BitCount);
        mWords[index / BITS_PER_WORD] &= ~bitMask(index);
    }
    /// Set bit `index` when `isSet` is true, otherwise reset it. @pre index < GetSize().
    LUDUS_INLINE constexpr void Assign(usize index, bool isSet) noexcept
    {
        if (isSet == true)
        {
            Set(index);
        }
        else
        {
            Reset(index);
        }
    }
    /// Invert bit `index`. @pre index < GetSize().
    LUDUS_INLINE constexpr void Flip(usize index) noexcept
    {
        LUDUS_ASSERT(index < BitCount);
        mWords[index / BITS_PER_WORD] ^= bitMask(index);
    }

    // --- Whole set ----------------------------------------------------------
    /// Set every bit; tail bits past BitCount stay zero.
    constexpr void SetAll() noexcept
    {
        for (usize i = 0; i < WORD_COUNT; ++i)
        {
            mWords[i] = ~uint64{0};
        }
        maskTail();
    }
    /// Reset every bit.
    constexpr void ResetAll() noexcept
    {
        for (usize i = 0; i < WORD_COUNT; ++i)
        {
            mWords[i] = 0;
        }
    }
    /// Invert every bit; tail bits past BitCount stay zero.
    constexpr void FlipAll() noexcept
    {
        for (usize i = 0; i < WORD_COUNT; ++i)
        {
            mWords[i] = ~mWords[i];
        }
        maskTail();
    }

    // --- Queries ------------------------------------------------------------
    /// Number of set bits.
    [[nodiscard]] constexpr usize CountSet() const noexcept
    {
        usize count = 0;
        for (usize i = 0; i < WORD_COUNT; ++i)
        {
            count += detail::CountSetBits(mWords[i]);
        }
        return count;
    }
    /// True when at least one bit is set.
    [[nodiscard]] constexpr bool IsAnySet() const noexcept
    {
        for (usize i = 0; i < WORD_COUNT; ++i)
        {
            if (mWords[i] != 0)
            {
                return true;
            }
        }
        return false;
    }
    /// True when no bit is set. Always true when BitCount is 0.
    [[nodiscard]] constexpr bool IsNoneSet() const noexcept
    {
        return IsAnySet() == false;
    }
    /// True when every bit is set. Always true when BitCount is 0.
    [[nodiscard]] constexpr bool AreAllSet() const noexcept
    {
        return CountSet() == BitCount;
    }

    /// @brief Index of the first set bit at or after `first`, or GetSize() when there is none.
    ///
    /// Visits whole words, so iterating every set bit with repeated calls costs O(GetSize() / 64 + set bits).
    /// `first` may equal or exceed GetSize(), which returns GetSize().
    [[nodiscard]] constexpr usize FindNextSet(usize first) const noexcept
    {
        if (first >= BitCount)
        {
            return BitCount;
        }
        usize wordIndex = first / BITS_PER_WORD;
        uint64 word = mWords[wordIndex] & (~uint64{0} << (first % BITS_PER_WORD));
        for (;;)
        {
            if (word != 0)
            {
                return wordIndex * BITS_PER_WORD + detail::CountTrailingZeroBits(word);
            }
            ++wordIndex;
            if (wordIndex == WORD_COUNT)
            {
                return BitCount;
            }
            word = mWords[wordIndex];
        }
    }

    /// Read-only words, least significant bit of word 0 first. Tail bits past BitCount are zero.
    [[nodiscard]] constexpr BorrowedView<const uint64> GetWords() const noexcept
    {
        return BorrowedView<const uint64>(mWords.GetData(), WORD_COUNT);
    }

    // --- Set operations -----------------------------------------------------
    /// Keep only bits also set in `other`.
    constexpr StaticBitSet& operator&=(const StaticBitSet& other) noexcept
    {
        for (usize i = 0; i < WORD_COUNT; ++i)
        {
            mWords[i] &= other.mWords[i];
        }
        return *this;
    }
    /// Add every bit set in `other`.
    constexpr StaticBitSet& operator|=(const StaticBitSet& other) noexcept
    {
        for (usize i = 0; i < WORD_COUNT; ++i)
        {
            mWords[i] |= other.mWords[i];
        }
        return *this;
    }
    /// Invert every bit that is set in `other`.
    constexpr StaticBitSet& operator^=(const StaticBitSet& other) noexcept
    {
        for (usize i = 0; i < WORD_COUNT; ++i)
        {
            mWords[i] ^= other.mWords[i];
        }
        return *this;
    }

    /// @brief Move every bit `count` positions toward higher indices; bits shifted past the end are discarded.
    ///
    /// Vacated low bits are reset. A count of GetSize() or more resets every bit.
    constexpr StaticBitSet& operator<<=(usize count) noexcept
    {
        if (count >= BitCount)
        {
            ResetAll();
            return *this;
        }
        const usize wordShift = count / BITS_PER_WORD;
        const usize bitShift = count % BITS_PER_WORD;
        for (usize i = WORD_COUNT; i-- > 0;)
        {
            uint64 word = 0;
            if (i >= wordShift)
            {
                const usize source = i - wordShift;
                word = mWords[source] << bitShift;
                if (bitShift != 0 && source > 0)
                {
                    word |= mWords[source - 1] >> (BITS_PER_WORD - bitShift);
                }
            }
            mWords[i] = word;
        }
        maskTail();
        return *this;
    }
    /// @brief Move every bit `count` positions toward lower indices; bits shifted below 0 are discarded.
    ///
    /// Vacated high bits are reset. A count of GetSize() or more resets every bit.
    constexpr StaticBitSet& operator>>=(usize count) noexcept
    {
        if (count >= BitCount)
        {
            ResetAll();
            return *this;
        }
        const usize wordShift = count / BITS_PER_WORD;
        const usize bitShift = count % BITS_PER_WORD;
        for (usize i = 0; i < WORD_COUNT; ++i)
        {
            uint64 word = 0;
            const usize source = i + wordShift;
            if (source < WORD_COUNT)
            {
                word = mWords[source] >> bitShift;
                if (bitShift != 0 && source + 1 < WORD_COUNT)
                {
                    word |= mWords[source + 1] << (BITS_PER_WORD - bitShift);
                }
            }
            mWords[i] = word;
        }
        return *this;
    }

    /// Bits set in both operands.
    [[nodiscard]] friend constexpr StaticBitSet operator&(StaticBitSet left, const StaticBitSet& right) noexcept
    {
        left &= right;
        return left;
    }
    /// Bits set in either operand.
    [[nodiscard]] friend constexpr StaticBitSet operator|(StaticBitSet left, const StaticBitSet& right) noexcept
    {
        left |= right;
        return left;
    }
    /// Bits set in exactly one operand.
    [[nodiscard]] friend constexpr StaticBitSet operator^(StaticBitSet left, const StaticBitSet& right) noexcept
    {
        left ^= right;
        return left;
    }
    /// Complement within GetSize() bits.
    [[nodiscard]] friend constexpr StaticBitSet operator~(StaticBitSet value) noexcept
    {
        value.FlipAll();
        return value;
    }
    /// Copy shifted toward higher indices; see operator<<=.
    [[nodiscard]] friend constexpr StaticBitSet operator<<(StaticBitSet value, usize count) noexcept
    {
        value <<= count;
        return value;
    }
    /// Copy shifted toward lower indices; see operator>>=.
    [[nodiscard]] friend constexpr StaticBitSet operator>>(StaticBitSet value, usize count) noexcept
    {
        value >>= count;
        return value;
    }
    /// True when the same bits are set.
    [[nodiscard]] friend constexpr bool operator==(const StaticBitSet& left, const StaticBitSet& right) noexcept
    {
        for (usize i = 0; i < WORD_COUNT; ++i)
        {
            if (left.mWords[i] != right.mWords[i])
            {
                return false;
            }
        }
        return true;
    }

private:
    static constexpr usize TAIL_BITS = BitCount % BITS_PER_WORD;
    // A zero-bit set stores one always-zero word so every operation compiles
    // without a separate specialization; WORD_COUNT stays 0 for all logic.
    static constexpr usize STORAGE_WORD_COUNT = WORD_COUNT == 0 ? 1 : WORD_COUNT;

    [[nodiscard]] static constexpr uint64 bitMask(usize index) noexcept
    {
        return uint64{1} << (index % BITS_PER_WORD);
    }
    // Restore the invariant that bits at or above BitCount are zero.
    constexpr void maskTail() noexcept
    {
        if constexpr (TAIL_BITS != 0)
        {
            mWords[WORD_COUNT - 1] &= (uint64{1} << TAIL_BITS) - 1;
        }
    }

    StaticArray<uint64, STORAGE_WORD_COUNT> mWords{};
};
} // namespace ludus::foundation::core

namespace ludus::foundation
{
/// Engine-namespace alias for core::StaticBitSet.
template <core::usize BitCount>
using StaticBitSet = core::StaticBitSet<BitCount>;
} // namespace ludus::foundation
