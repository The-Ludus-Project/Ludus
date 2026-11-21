#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/strings/status.hpp>

#include <span>
#include <string_view>

namespace ludus::foundation
{
class String;

/// @brief Borrowed validation proof, in bytes.
///
/// Only ValidateUtf8 sets a nonempty view; a default-constructed proof holds an empty view, which is
/// valid UTF-8. The proof borrows the validated bytes and does not keep them alive.
/// Mutating the source invalidates the proof.
/// No normalization, grapheme, display-width or Unicode case-folding claims.
class Utf8View final
{
public:
    /// @brief Borrowed counted view of the validated bytes; sizes count bytes, not scalars.
    ///
    /// Valid only while the source storage passed to ValidateUtf8 is alive and unchanged.
    [[nodiscard]] std::string_view GetView() const noexcept
    {
        return mView;
    }

private:
    std::string_view mView;
    friend StringStatus ValidateUtf8(std::string_view, Utf8View&, usize&) noexcept;
    friend StringStatus TrySliceUtf8(Utf8View, usize, usize, Utf8View&) noexcept;
};
/// @brief Checks that `bytes` is well-formed UTF-8 (Unicode Standard, section 3.9, table 3-7).
///
/// Rejects overlong encodings, surrogates, values above U+10FFFF and truncated sequences. Does not
/// allocate.
/// Failure preserves output and reports the first invalid sequence's byte offset.
///
/// U+0000 is valid Unicode. Success sets errorOffset to zero.
/// @param bytes Bytes to validate; borrowed by `output` on success.
/// @param output Receives the proof on success.
/// @param errorOffset On failure, byte offset at which the first ill-formed sequence starts.
/// @return Ok or InvalidUtf8.
[[nodiscard]] StringStatus ValidateUtf8(std::string_view bytes, Utf8View& output, usize& errorOffset) noexcept;
/// @brief Checks that `bytes` contains no zero byte, so it can cross a C-string boundary intact.
///
/// The check is about bytes only: it does not validate UTF-8 and does not require or look for a
/// terminator beyond `bytes`. Pass a separately terminated copy (such as String::GetData) to the C API.
/// @return Ok; EmbeddedZero when any byte in `bytes` is zero.
[[nodiscard]] StringStatus ValidateCString(std::string_view bytes) noexcept;

/// @brief Largest code point boundary of `text` at or before byte `offset`.
///
/// A boundary is zero, the size, or a byte offset whose byte starts a sequence. Because `text` is
/// validated, cutting there keeps both sides well-formed. Constant time: it steps back over at most
/// three continuation bytes. Does not allocate.
/// @param text Validated text.
/// @param offset Byte offset; values beyond the size select the size.
/// @return A byte offset no larger than `offset` or the size of `text`.
[[nodiscard]] usize FindPrecedingUtf8Boundary(Utf8View text, usize offset) noexcept;
/// @brief Checked text slice: borrows `size` bytes of `text` starting at byte `offset`.
///
/// Unlike a byte substring, the slice is checked to start and end on code point boundaries, so the
/// result is again a validated proof. Does not allocate or copy; the slice borrows the same storage
/// as `text`.
/// @param text Validated text to slice.
/// @param offset First byte of the slice.
/// @param size Number of bytes in the slice.
/// @param outSlice Receives the slice on success.
/// @return Ok; OutOfRange when the range extends beyond `text`; SplitsCodePoint when either end lies
/// inside a UTF-8 sequence (use FindPrecedingUtf8Boundary to choose a boundary). Failure leaves
/// `outSlice` unchanged.
[[nodiscard]] StringStatus TrySliceUtf8(Utf8View text, usize offset, usize size, Utf8View& outSlice) noexcept;
/// @brief Explicit display repair: copies `bytes` and replaces each ill-formed part with U+FFFD.
///
/// Strict validation never repairs; this is the separate, named operation for showing text that may
/// be malformed, such as diagnostics or names reported by a native API. Well-formed sequences are
/// copied unchanged. Each maximal subpart of an ill-formed subsequence becomes one U+FFFD (the
/// three bytes EF BF BD), following the Unicode Standard's recommended practice, so a truncated
/// sequence yields one replacement and each stray continuation byte yields its own. The result is
/// well-formed UTF-8 and therefore longer than `bytes` by two bytes per replaced byte at most.
///
/// The repaired text is built in new storage from `outText`'s domain and replaces `outText` only
/// on success, so `bytes` may borrow `outText`. Allocates at most once.
/// @param bytes Bytes to repair; they need not be valid UTF-8 and may contain zero bytes.
/// @param maximumBytes Consumer byte limit for the repaired text, excluding the terminator.
/// @param outText Receives the repaired text and keeps its domain; its old storage is released.
/// @param outReplacementCount Receives the number of U+FFFD replacements; zero means `bytes` was
/// already well-formed.
/// @return Ok; CapacityExceeded when the repaired text is longer than `maximumBytes`; TooLarge when
/// it exceeds String's maximum capacity; OutOfMemory when the domain cannot supply the storage.
/// Failure leaves `outText` and `outReplacementCount` unchanged.
[[nodiscard]] StringStatus
TryRepairUtf8(std::string_view bytes, usize maximumBytes, String& outText, usize& outReplacementCount) noexcept;

/// @brief Counted byte view of C++20 `char8_t` code units, such as a `u8"..."` literal.
///
/// Borrows the units without copying: C++ lets `char` glvalues examine any object's bytes. The view
/// is not validated (call ValidateUtf8 when the units come from outside the program) and lives only
/// as long as the units' storage.
/// @param units Counted code units; no terminator is required or read.
/// @return The same bytes, with the same size.
[[nodiscard]] std::string_view GetByteView(std::u8string_view units) noexcept;
/// @brief Copies counted bytes into `char8_t` code units, for APIs such as `std::filesystem::path`.
///
/// The reverse of GetByteView needs a copy: reading `char` storage through a `char8_t` pointer is not
/// permitted by C++'s aliasing rules. Does not validate or allocate, and writes no terminator.
/// @param bytes Bytes to copy.
/// @param outUnits Destination units; only the first `bytes.size()` are written.
/// @param outUnitCount Receives `bytes.size()` on success.
/// @return Ok; CapacityExceeded when `outUnits` is shorter than `bytes`. Failure writes nothing.
[[nodiscard]] StringStatus
TryCopyToUtf8Units(std::string_view bytes, std::span<char8_t> outUnits, usize& outUnitCount) noexcept;
} // namespace ludus::foundation
