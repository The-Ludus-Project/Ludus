#pragma once

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/strings/status.hpp>

#include <span>
#include <string_view>

// Strict UTF-8 and UTF-16 conversion for native OS boundaries, such as Win32 wide-character APIs.
//
// Engine text is UTF-8; convert only where a native API requires UTF-16. Conversions never repair:
// ill-formed input fails with the offset of the first ill-formed sequence, so a caller that wants
// to show malformed text repairs it explicitly first (TryRepairUtf8). Every operation is bounded by
// its output span, writes no terminator (reserve one unit or byte and write it yourself), does not
// allocate, and leaves its outputs unchanged on failure. U+0000 is converted like any other scalar;
// check with ValidateCString first when the native API reads a terminated string.
namespace ludus::foundation
{
/// @brief Counts the UTF-16 units that strict conversion of `bytes` produces.
/// @param bytes UTF-8 bytes to measure.
/// @param outUnitCount Receives the unit count on success: one per scalar below U+10000, two above.
/// @param outErrorOffset Byte offset of the first ill-formed sequence on InvalidUtf8, otherwise zero.
/// @return Ok or InvalidUtf8. Failure leaves `outUnitCount` unchanged.
[[nodiscard]] StringStatus
MeasureUtf8AsUtf16(std::string_view bytes, usize& outUnitCount, usize& outErrorOffset) noexcept;
/// @brief Strict UTF-8 to UTF-16 conversion into caller storage.
///
/// Validates and measures all of `bytes` before writing, so a failure writes no unit.
/// @param bytes UTF-8 bytes to convert.
/// @param outUnits Destination; only the first `outUnitCount` units are written.
/// @param outUnitCount Receives the number of units written on success.
/// @param outErrorOffset Byte offset of the first ill-formed sequence on InvalidUtf8, otherwise zero.
/// @return Ok; InvalidUtf8; CapacityExceeded when the converted text needs more than
/// `outUnits.size()` units (MeasureUtf8AsUtf16 reports how many). Failure leaves `outUnits` and
/// `outUnitCount` unchanged.
[[nodiscard]] StringStatus ConvertUtf8ToUtf16(std::string_view bytes,
                                              std::span<char16_t> outUnits,
                                              usize& outUnitCount,
                                              usize& outErrorOffset) noexcept;
/// @brief Counts the UTF-8 bytes that strict conversion of `units` produces.
/// @param units UTF-16 units to measure.
/// @param outByteCount Receives the byte count on success.
/// @param outErrorOffset Unit offset of the first unpaired surrogate on InvalidUtf16, otherwise zero.
/// @return Ok; InvalidUtf16; TooLarge when the byte count is not representable (possible only on
/// 32-bit targets). Failure leaves `outByteCount` unchanged.
[[nodiscard]] StringStatus
MeasureUtf16AsUtf8(std::u16string_view units, usize& outByteCount, usize& outErrorOffset) noexcept;
/// @brief Strict UTF-16 to UTF-8 conversion into caller storage.
///
/// Validates and measures all of `units` before writing, so a failure writes no byte. An unpaired
/// surrogate fails rather than being encoded or replaced.
/// @param units UTF-16 units to convert.
/// @param outBytes Destination; only the first `outByteCount` bytes are written.
/// @param outByteCount Receives the number of bytes written on success.
/// @param outErrorOffset Unit offset of the first unpaired surrogate on InvalidUtf16, otherwise zero.
/// @return Ok; InvalidUtf16; TooLarge as for MeasureUtf16AsUtf8; CapacityExceeded when the converted
/// text needs more than
/// `outBytes.size()` bytes (MeasureUtf16AsUtf8 reports how many). Failure leaves `outBytes` and
/// `outByteCount` unchanged.
[[nodiscard]] StringStatus ConvertUtf16ToUtf8(std::u16string_view units,
                                              std::span<char> outBytes,
                                              usize& outByteCount,
                                              usize& outErrorOffset) noexcept;
#if defined(LUDUS_PLATFORM_WINDOWS)
/// @brief ConvertUtf8ToUtf16 into Windows `wchar_t` storage, which holds UTF-16 units.
///
/// Windows only, where `wchar_t` is 16 bits wide. Same statuses and failure behavior.
[[nodiscard]] StringStatus ConvertUtf8ToUtf16(std::string_view bytes,
                                              std::span<wchar_t> outUnits,
                                              usize& outUnitCount,
                                              usize& outErrorOffset) noexcept;
/// @brief MeasureUtf16AsUtf8 for Windows `wchar_t` UTF-16 units. Same statuses and failure behavior.
[[nodiscard]] StringStatus
MeasureUtf16AsUtf8(std::wstring_view units, usize& outByteCount, usize& outErrorOffset) noexcept;
/// @brief ConvertUtf16ToUtf8 from Windows `wchar_t` UTF-16 units. Same statuses and failure behavior.
[[nodiscard]] StringStatus ConvertUtf16ToUtf8(std::wstring_view units,
                                              std::span<char> outBytes,
                                              usize& outByteCount,
                                              usize& outErrorOffset) noexcept;
#endif
} // namespace ludus::foundation
