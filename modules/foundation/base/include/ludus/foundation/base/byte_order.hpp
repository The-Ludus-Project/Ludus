#pragma once

// Bounded unsigned integer codecs. Only the first sizeof(T) bytes are consumed
// or written; trailing bytes are untouched. Truncation preserves output/buffer.
// Byte accesses support unaligned buffers without assuming native byte order.
// Thanks to Jason Hughes, "What to Look for When Evaluating Middleware for
// Integration", Game Engine Gems, section 1.13 "Platform Portability", p. 12,
// for the file/network endian audit lesson. These original codecs use bounded
// byte operations; no native object layout or chapter implementation is copied.
// Review: docs/architecture/primitive-types.md.
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/borrowed_view.hpp>
#include <ludus/foundation/base/checked_integer.hpp>

namespace ludus::foundation::core
{
namespace detail
{
inline constexpr usize BITS_PER_BYTE = 8;

template <bool IsLittleEndian, UnsignedInteger ValueType>
[[nodiscard]] constexpr bool TryReadEndian(BorrowedView<const uint8> bytes, ValueType& outValue) noexcept
{
    if (bytes.IsValid() == false || bytes.GetSize() < sizeof(ValueType))
    {
        return false;
    }
    const uint8* data = bytes.GetDataOrNull();
    ValueType result{};
    for (usize i = 0; i < sizeof(ValueType); ++i)
    {
        const usize index = IsLittleEndian == true ? i : sizeof(ValueType) - 1 - i;
        result = static_cast<ValueType>(result | (static_cast<ValueType>(data[index]) << (i * BITS_PER_BYTE)));
    }
    outValue = result;
    return true;
}

template <bool IsLittleEndian, UnsignedInteger ValueType>
[[nodiscard]] constexpr bool TryWriteEndian(ValueType value, BorrowedView<uint8> bytes) noexcept
{
    if (bytes.IsValid() == false || bytes.GetSize() < sizeof(ValueType))
    {
        return false;
    }
    uint8* data = bytes.GetDataOrNull();
    for (usize i = 0; i < sizeof(ValueType); ++i)
    {
        const usize index = IsLittleEndian == true ? i : sizeof(ValueType) - 1 - i;
        data[index] = static_cast<uint8>(value >> (i * BITS_PER_BYTE));
    }
    return true;
}
} // namespace detail

/// Read the first sizeof(ValueType) bytes as a little-endian unsigned integer.
/// @param bytes Readable byte view; unaligned input is supported.
/// @param outValue Preserved if the view is invalid or too short.
/// @return True if the value was read.
template <UnsignedInteger ValueType>
[[nodiscard]] constexpr bool TryReadLittleEndian(BorrowedView<const uint8> bytes, ValueType& outValue) noexcept
{
    return detail::TryReadEndian<true>(bytes, outValue);
}

/// Read the first sizeof(ValueType) bytes as a big-endian unsigned integer.
/// @param bytes Readable byte view; unaligned input is supported.
/// @param outValue Preserved if the view is invalid or too short.
/// @return True if the value was read.
template <UnsignedInteger ValueType>
[[nodiscard]] constexpr bool TryReadBigEndian(BorrowedView<const uint8> bytes, ValueType& outValue) noexcept
{
    return detail::TryReadEndian<false>(bytes, outValue);
}

/// Write a little-endian unsigned integer into the first sizeof(ValueType) bytes.
/// @param value Unsigned integer to encode.
/// @param bytes Writable byte view; unaligned output is supported.
/// @return False for an invalid or short view, without modifying any bytes. On success, trailing bytes stay untouched.
template <UnsignedInteger ValueType>
[[nodiscard]] constexpr bool TryWriteLittleEndian(ValueType value, BorrowedView<uint8> bytes) noexcept
{
    return detail::TryWriteEndian<true>(value, bytes);
}

/// Write a big-endian unsigned integer into the first sizeof(ValueType) bytes.
/// @param value Unsigned integer to encode.
/// @param bytes Writable byte view; unaligned output is supported.
/// @return False for an invalid or short view, without modifying any bytes. On success, trailing bytes stay untouched.
template <UnsignedInteger ValueType>
[[nodiscard]] constexpr bool TryWriteBigEndian(ValueType value, BorrowedView<uint8> bytes) noexcept
{
    return detail::TryWriteEndian<false>(value, bytes);
}
} // namespace ludus::foundation::core

namespace ludus::foundation
{
using core::TryReadBigEndian;
using core::TryReadLittleEndian;
using core::TryWriteBigEndian;
using core::TryWriteLittleEndian;
} // namespace ludus::foundation
