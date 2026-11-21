#include <ludus/foundation/strings/utf8.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/strings/string.hpp>

#include "internal/utf8_sequence.hpp"

namespace ludus::foundation
{
// Thanks to the Unicode Consortium, The Unicode Standard 17.0, section 3.9,
// table 3-7 (https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/).
// Validate only well-formed UTF-8 scalar sequences; normalization and grapheme
// handling stay above FoundationStrings. See docs/architecture/strings.md.
namespace
{
/// U+FFFD REPLACEMENT CHARACTER encoded as UTF-8.
constexpr std::string_view REPLACEMENT_CHARACTER = "\xEF\xBF\xBD";
} // namespace

StringStatus ValidateUtf8(std::string_view bytes, Utf8View& output, usize& errorOffset) noexcept
{
    usize i = 0;
    while (i < bytes.size())
    {
        const detail::Utf8Sequence sequence = detail::ScanUtf8Sequence(bytes, i);
        if (sequence.IsWellFormed == false)
        {
            errorOffset = i;
            return StringStatus::InvalidUtf8;
        }
        i += sequence.Length;
    }
    output.mView = bytes;
    errorOffset = 0;
    return StringStatus::Ok;
}
StringStatus ValidateCString(std::string_view bytes) noexcept
{
    return bytes.find('\0') == std::string_view::npos ? StringStatus::Ok : StringStatus::EmbeddedZero;
}
usize FindPrecedingUtf8Boundary(Utf8View text, usize offset) noexcept
{
    const std::string_view bytes = text.GetView();
    usize boundary = offset < bytes.size() ? offset : bytes.size();
    while (boundary > 0 && boundary < bytes.size() && detail::IsUtf8Continuation(bytes[boundary]) == true)
    {
        --boundary;
    }
    return boundary;
}
// The two offsets are the conventional (position, count) pair of a substring.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
StringStatus TrySliceUtf8(Utf8View text, usize offset, usize size, Utf8View& outSlice) noexcept
{
    const std::string_view bytes = text.GetView();
    if (offset > bytes.size() || size > bytes.size() - offset)
    {
        return StringStatus::OutOfRange;
    }
    if (FindPrecedingUtf8Boundary(text, offset) != offset ||
        FindPrecedingUtf8Boundary(text, offset + size) != offset + size)
    {
        return StringStatus::SplitsCodePoint;
    }
    outSlice.mView = {bytes.data() + offset, size};
    return StringStatus::Ok;
}
StringStatus
TryRepairUtf8(std::string_view bytes, usize maximumBytes, String& outText, usize& outReplacementCount) noexcept
{
    usize repairedSize = 0;
    usize replacements = 0;
    for (usize i = 0; i < bytes.size();)
    {
        const detail::Utf8Sequence sequence = detail::ScanUtf8Sequence(bytes, i);
        const usize produced = sequence.IsWellFormed == true ? sequence.Length : REPLACEMENT_CHARACTER.size();
        if (TryAdd(repairedSize, produced, repairedSize) == false)
        {
            return StringStatus::TooLarge;
        }
        replacements += sequence.IsWellFormed == true ? 0 : 1;
        i += sequence.Length;
    }
    if (repairedSize > maximumBytes)
    {
        return StringStatus::CapacityExceeded;
    }
    // Stage in new storage so `bytes` may borrow `outText` and failure leaves it untouched.
    String repaired(outText.GetDomain());
    const StringStatus reserved = repaired.TryEnsureCapacity(repairedSize);
    if (reserved != StringStatus::Ok)
    {
        return reserved;
    }
    for (usize i = 0; i < bytes.size();)
    {
        const detail::Utf8Sequence sequence = detail::ScanUtf8Sequence(bytes, i);
        const std::string_view piece =
            sequence.IsWellFormed == true ? std::string_view{bytes.data() + i, sequence.Length} : REPLACEMENT_CHARACTER;
        // Capacity is reserved, so appending cannot fail.
        const StringStatus appended = repaired.TryAppend(piece);
        LUDUS_ASSERT(appended == StringStatus::Ok);
        static_cast<void>(appended);
        i += sequence.Length;
    }
    outText = Move(repaired);
    outReplacementCount = replacements;
    return StringStatus::Ok;
}
std::string_view GetByteView(std::u8string_view units) noexcept
{
    // char glvalues may examine the bytes of any object ([basic.lval]), including char8_t units.
    return {reinterpret_cast<const char*>(units.data()), units.size()};
}
StringStatus TryCopyToUtf8Units(std::string_view bytes, std::span<char8_t> outUnits, usize& outUnitCount) noexcept
{
    if (bytes.size() > outUnits.size())
    {
        return StringStatus::CapacityExceeded;
    }
    for (usize i = 0; i < bytes.size(); ++i)
    {
        outUnits[i] = static_cast<char8_t>(bytes[i]);
    }
    outUnitCount = bytes.size();
    return StringStatus::Ok;
}
} // namespace ludus::foundation
