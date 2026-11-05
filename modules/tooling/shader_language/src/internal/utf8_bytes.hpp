#pragma once

#include <ludus/foundation/base/core.h>

namespace ludus::tooling::shader_language::internal
{
/// Mask selecting the two high bits of a UTF-8 byte.
inline constexpr foundation::uint8 UTF8_HIGH_BITS_MASK = 0xc0;
/// The two high bits of a UTF-8 continuation byte.
inline constexpr foundation::uint8 UTF8_CONTINUATION_BITS = 0x80;
/// The first byte value outside ASCII.
inline constexpr foundation::uint8 FIRST_NON_ASCII_BYTE = 0x80;

/// True for a UTF-8 continuation byte (`10xxxxxx`).
[[nodiscard]] LUDUS_INLINE constexpr bool IsUtf8Continuation(char value) noexcept
{
    return (static_cast<foundation::uint8>(value) & UTF8_HIGH_BITS_MASK) == UTF8_CONTINUATION_BITS;
}

/// True for a byte outside ASCII.
[[nodiscard]] LUDUS_INLINE constexpr bool IsNonAscii(char value) noexcept
{
    return static_cast<foundation::uint8>(value) >= FIRST_NON_ASCII_BYTE;
}
} // namespace ludus::tooling::shader_language::internal
