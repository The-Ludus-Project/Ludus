#include <ludus/foundation/strings/utf16.hpp>

#include <ludus/foundation/base/checked_integer.hpp>

#include "internal/utf8_sequence.hpp"

// Thanks to the Unicode Consortium, The Unicode Standard 17.0, section 3.9 "Unicode Encoding
// Forms", definitions D91 (UTF-16) and D92 (UTF-8), for surrogate-pair composition and the
// well-formed sequences converted here (https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/).
// Every conversion reports a count and an error offset, two distinctly named usize outputs.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
namespace ludus::foundation
{
namespace
{
/// First and last high (leading) surrogate.
constexpr uint32 HIGH_SURROGATE_MINIMUM = 0xD800;
constexpr uint32 HIGH_SURROGATE_MAXIMUM = 0xDBFF;
/// First and last low (trailing) surrogate.
constexpr uint32 LOW_SURROGATE_MINIMUM = 0xDC00;
constexpr uint32 LOW_SURROGATE_MAXIMUM = 0xDFFF;
/// First supplementary-plane scalar, which UTF-16 encodes as a surrogate pair.
constexpr uint32 SUPPLEMENTARY_MINIMUM = 0x10000;
/// Scalar bits carried by each surrogate of a pair, and their mask.
constexpr uint32 SURROGATE_PAYLOAD_BITS = 10;
constexpr uint32 SURROGATE_PAYLOAD_MASK = 0x3FF;

[[nodiscard]] constexpr bool IsSurrogate(uint32 unit) noexcept
{
    return unit >= HIGH_SURROGATE_MINIMUM && unit <= LOW_SURROGATE_MAXIMUM;
}
[[nodiscard]] constexpr bool IsHighSurrogate(uint32 unit) noexcept
{
    return unit >= HIGH_SURROGATE_MINIMUM && unit <= HIGH_SURROGATE_MAXIMUM;
}
[[nodiscard]] constexpr bool IsLowSurrogate(uint32 unit) noexcept
{
    return unit >= LOW_SURROGATE_MINIMUM && unit <= LOW_SURROGATE_MAXIMUM;
}

/// Validates `bytes` and counts its UTF-16 units; outputs follow MeasureUtf8AsUtf16.
[[nodiscard]] StringStatus CountUtf16Units(std::string_view bytes, usize& outUnitCount, usize& outErrorOffset) noexcept
{
    usize units = 0;
    for (usize i = 0; i < bytes.size();)
    {
        const detail::Utf8Sequence sequence = detail::ScanUtf8Sequence(bytes, i);
        if (sequence.IsWellFormed == false)
        {
            outErrorOffset = i;
            return StringStatus::InvalidUtf8;
        }
        // A byte count bounds the unit count, so this sum cannot overflow.
        units += sequence.Length == detail::UTF8_FOUR_BYTE_LENGTH ? 2 : 1;
        i += sequence.Length;
    }
    outUnitCount = units;
    outErrorOffset = 0;
    return StringStatus::Ok;
}

template <typename Unit>
[[nodiscard]] StringStatus
ConvertFromUtf8(std::string_view bytes, std::span<Unit> outUnits, usize& outUnitCount, usize& outErrorOffset) noexcept
{
    usize required = 0;
    const StringStatus measured = CountUtf16Units(bytes, required, outErrorOffset);
    if (measured != StringStatus::Ok)
    {
        return measured;
    }
    if (required > outUnits.size())
    {
        return StringStatus::CapacityExceeded;
    }
    usize written = 0;
    for (usize i = 0; i < bytes.size();)
    {
        const usize length = detail::ScanUtf8Sequence(bytes, i).Length;
        const uint32 scalar = detail::DecodeUtf8Sequence(bytes, i, length);
        if (scalar < SUPPLEMENTARY_MINIMUM)
        {
            outUnits[written++] = static_cast<Unit>(scalar);
        }
        else
        {
            const uint32 offset = scalar - SUPPLEMENTARY_MINIMUM;
            outUnits[written++] = static_cast<Unit>(HIGH_SURROGATE_MINIMUM + (offset >> SURROGATE_PAYLOAD_BITS));
            outUnits[written++] = static_cast<Unit>(LOW_SURROGATE_MINIMUM + (offset & SURROGATE_PAYLOAD_MASK));
        }
        i += length;
    }
    outUnitCount = written;
    return StringStatus::Ok;
}

/// Scalar at `units[i]` and the number of units it occupies; zero units for an unpaired surrogate.
template <typename Unit>
[[nodiscard]] constexpr usize DecodeUtf16(std::basic_string_view<Unit> units, usize i, uint32& outScalar) noexcept
{
    const uint32 unit = static_cast<uint32>(units[i]);
    if (IsSurrogate(unit) == false)
    {
        outScalar = unit;
        return 1;
    }
    if (IsHighSurrogate(unit) == false || i + 1 >= units.size() ||
        IsLowSurrogate(static_cast<uint32>(units[i + 1])) == false)
    {
        return 0;
    }
    const uint32 low = static_cast<uint32>(units[i + 1]);
    outScalar = SUPPLEMENTARY_MINIMUM + ((unit - HIGH_SURROGATE_MINIMUM) << SURROGATE_PAYLOAD_BITS) +
                (low - LOW_SURROGATE_MINIMUM);
    return 2;
}

template <typename Unit>
[[nodiscard]] StringStatus
CountUtf8Bytes(std::basic_string_view<Unit> units, usize& outByteCount, usize& outErrorOffset) noexcept
{
    usize bytes = 0;
    for (usize i = 0; i < units.size();)
    {
        uint32 scalar = 0;
        const usize consumed = DecodeUtf16(units, i, scalar);
        if (consumed == 0)
        {
            outErrorOffset = i;
            return StringStatus::InvalidUtf16;
        }
        // Up to three bytes per unit can exceed usize on 32-bit targets.
        if (TryAdd(bytes, detail::GetUtf8EncodedLength(scalar), bytes) == false)
        {
            outErrorOffset = 0;
            return StringStatus::TooLarge;
        }
        i += consumed;
    }
    outByteCount = bytes;
    outErrorOffset = 0;
    return StringStatus::Ok;
}

template <typename Unit>
[[nodiscard]] StringStatus ConvertFromUtf16(std::basic_string_view<Unit> units,
                                            std::span<char> outBytes,
                                            usize& outByteCount,
                                            usize& outErrorOffset) noexcept
{
    usize required = 0;
    const StringStatus measured = CountUtf8Bytes(units, required, outErrorOffset);
    if (measured != StringStatus::Ok)
    {
        return measured;
    }
    if (required > outBytes.size())
    {
        return StringStatus::CapacityExceeded;
    }
    usize written = 0;
    for (usize i = 0; i < units.size();)
    {
        uint32 scalar = 0;
        i += DecodeUtf16(units, i, scalar);
        detail::EncodeUtf8Scalar(scalar, outBytes.data() + written);
        written += detail::GetUtf8EncodedLength(scalar);
    }
    outByteCount = written;
    return StringStatus::Ok;
}
} // namespace

StringStatus MeasureUtf8AsUtf16(std::string_view bytes, usize& outUnitCount, usize& outErrorOffset) noexcept
{
    return CountUtf16Units(bytes, outUnitCount, outErrorOffset);
}
StringStatus ConvertUtf8ToUtf16(std::string_view bytes,
                                std::span<char16_t> outUnits,
                                usize& outUnitCount,
                                usize& outErrorOffset) noexcept
{
    return ConvertFromUtf8(bytes, outUnits, outUnitCount, outErrorOffset);
}
StringStatus MeasureUtf16AsUtf8(std::u16string_view units, usize& outByteCount, usize& outErrorOffset) noexcept
{
    return CountUtf8Bytes(units, outByteCount, outErrorOffset);
}
StringStatus ConvertUtf16ToUtf8(std::u16string_view units,
                                std::span<char> outBytes,
                                usize& outByteCount,
                                usize& outErrorOffset) noexcept
{
    return ConvertFromUtf16(units, outBytes, outByteCount, outErrorOffset);
}
#if defined(LUDUS_PLATFORM_WINDOWS)
static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows wchar_t holds UTF-16 units");
StringStatus ConvertUtf8ToUtf16(std::string_view bytes,
                                std::span<wchar_t> outUnits,
                                usize& outUnitCount,
                                usize& outErrorOffset) noexcept
{
    return ConvertFromUtf8(bytes, outUnits, outUnitCount, outErrorOffset);
}
StringStatus MeasureUtf16AsUtf8(std::wstring_view units, usize& outByteCount, usize& outErrorOffset) noexcept
{
    return CountUtf8Bytes(units, outByteCount, outErrorOffset);
}
StringStatus ConvertUtf16ToUtf8(std::wstring_view units,
                                std::span<char> outBytes,
                                usize& outByteCount,
                                usize& outErrorOffset) noexcept
{
    return ConvertFromUtf16(units, outBytes, outByteCount, outErrorOffset);
}
#endif
} // namespace ludus::foundation
// NOLINTEND(bugprone-easily-swappable-parameters)
