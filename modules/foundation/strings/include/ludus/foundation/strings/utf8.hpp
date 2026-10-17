#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/strings/status.hpp>

#include <string_view>

namespace ludus::foundation
{
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
} // namespace ludus::foundation
