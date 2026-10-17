#include <ludus/localization/catalog.hpp>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include <atomic>
#include <utility>

// Thanks to James Boer, "A Flexible Text Parsing System", Game Programming
// Gems 2, section 1.17, pp.112-117: validate readable authoring data offline and
// keep the shipping reader small. Thanks to Jason Hughes, "Pointer Patching
// Assets", Game Engine Gems 2, chapter 20, pp.345-357: prepare bounded
// immutable data before publication. We use checked little-endian offsets, not
// the chapter's native pointer patching. Original implementation; review:
// docs/architecture/localization-gems-review.md.

namespace ludus::localization
{
namespace
{
using foundation::int32;

constexpr uint32 MAGIC = 0x434f4c4c; // "LLOC" in little endian.
constexpr usize VERSION_1_HEADER_BYTES = 48;
constexpr uint32 VERSION_1_RECORD_BYTES = 20;
constexpr usize VERSION_2_HEADER_BYTES = 64;
constexpr uint32 VERSION_2_RECORD_BYTES = 40;
constexpr uint32 ARGUMENT_RECORD_BYTES = 36;
constexpr uint32 SLICE_BYTES = 8;
constexpr uint32 STATIC_UTF8_PROFILE = 1;
constexpr uint32 ICU_MF1_PROFILE = 2;
constexpr uint32 KIND_STATIC = 0;
constexpr uint32 KIND_DYNAMIC = 1;
constexpr uint32 FLAG_REQUIRED = 1;
constexpr usize MAX_ARGUMENT_NAME_BYTES = 64;
constexpr uint32 MAX_TEXT_ARGUMENT_BYTES = 4096;
constexpr uint32 MAX_ENUM_VALUES = 64;
constexpr int64 COUNT_LIMIT = 1000000000;
// Version 2 record fields, as byte offsets inside one record.
constexpr usize RECORD_KIND = 16;
constexpr usize RECORD_PROVIDER = 20;
constexpr usize RECORD_FIRST_ARGUMENT = 24;
constexpr usize RECORD_ARGUMENT_COUNT = 28;
std::atomic<uint64> gNextToken{1};

uint32 ReadWord(std::span<const uint8> bytes, usize offset) noexcept
{
    uint32 value{};
    static_cast<void>(foundation::TryReadLittleEndian(bytes.subspan(offset, 4), value));
    return value;
}

bool IsValidId(std::string_view value) noexcept
{
    // Same grammar as Content::ValidId, without linking content acquisition.
    if (value.empty() == true || value.size() > kMaxIdentifierBytes || value.front() == '/' || value.back() == '/')
    {
        return false;
    }
    char previous{};
    for (char character : value)
    {
        if (((character < 'a' || character > 'z') && (character < '0' || character > '9') && character != '-' &&
             character != '/') ||
            (character == '/' && previous == '/'))
        {
            return false;
        }
        previous = character;
    }
    return true;
}

bool IsAsciiLetter(char character) noexcept
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

bool IsValidLocale(std::string_view value) noexcept
{
    // Project-selected labels with ASCII BCP47 subtag shape. No canonicalization,
    // registry lookup or language negotiation is implied by this narrow codec.
    if (value.empty() == true || value.size() > kMaxIdentifierBytes)
    {
        return false;
    }
    usize segment{};
    bool isPrimary = true;
    for (char character : value)
    {
        if (character == '-')
        {
            if (segment == 0 || (isPrimary == true && segment < 2))
            {
                return false;
            }
            isPrimary = false;
            segment = 0;
        }
        else if ((IsAsciiLetter(character) == false && (isPrimary == true || character < '0' || character > '9')) ||
                 ++segment > 8)
        {
            return false;
        }
    }
    return segment != 0 && (isPrimary == false || segment >= 2);
}

/// Linguistic data labels are printable ASCII without spaces, such as icu4c-78.3/cldr-48/unicode-17.0.
bool IsValidLinguisticLabel(std::string_view value) noexcept
{
    if (value.empty() == true || value.size() > kMaxIdentifierBytes)
    {
        return false;
    }
    for (char character : value)
    {
        if (character < '!' || character > '~')
        {
            return false;
        }
    }
    return true;
}

/// Argument names and enum tokens share `[a-z][a-z0-9_]*` within 64 bytes.
bool IsValidArgumentName(std::string_view value) noexcept
{
    if (value.empty() == true || value.size() > MAX_ARGUMENT_NAME_BYTES || value.front() < 'a' || value.front() > 'z')
    {
        return false;
    }
    for (char character : value)
    {
        const bool isAllowed =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
        if (isAllowed == false)
        {
            return false;
        }
    }
    return true;
}

bool IsValidText(std::string_view text, usize& outInvalidByte) noexcept
{
    foundation::Utf8View validated;
    usize invalidByte{};
    if (foundation::ValidateUtf8(text, validated, invalidByte) != foundation::StringStatus::Ok)
    {
        outInvalidByte = invalidByte;
        return false;
    }
    const usize zero = text.find('\0');
    if (zero != std::string_view::npos)
    {
        outInvalidByte = zero;
        return false;
    }
    return true;
}

Status MakeToken(uint64& outToken) noexcept
{
    uint64 token = gNextToken.load(std::memory_order_relaxed);
    while (token != ~uint64{0})
    {
        if (gNextToken.compare_exchange_weak(token, token + 1, std::memory_order_relaxed) == true)
        {
            outToken = token;
            return Status::Ok;
        }
    }
    return Status::TokenExhausted;
}

Status Fail(Diagnostic& outDiagnostic, Status status, usize offset) noexcept
{
    outDiagnostic = {status, offset};
    return status;
}

/// Sequential reader of packed payload slices; every slice must start where the previous one ended.
class PackedCursor final
{
public:
    PackedCursor(std::span<const uint8> bytes, usize start) noexcept : mBytes(bytes), mCursor(start) {}

    /// Takes the slice named by a directory entry; Limit or Invalid leaves the cursor unchanged.
    template <usize LIMIT>
    [[nodiscard]] Status Take(usize entry, std::string_view& outSlice) noexcept
    {
        const uint32 offset = ReadWord(mBytes, entry);
        const uint32 length = ReadWord(mBytes, entry + 4);
        if (length > LIMIT)
        {
            return Status::Limit;
        }
        if (offset != mCursor || length > mBytes.size() - mCursor)
        {
            return Status::Invalid;
        }
        outSlice = {reinterpret_cast<const char*>(mBytes.data()) + mCursor, length};
        mCursor += length;
        return Status::Ok;
    }

    [[nodiscard]] usize GetCursor() const noexcept
    {
        return mCursor;
    }

private:
    std::span<const uint8> mBytes;
    usize mCursor;
};

Status
ValidateVersion1(std::span<const uint8> bytes, detail::CatalogLayout& outLayout, Diagnostic& outDiagnostic) noexcept;
Status
ValidateVersion2(std::span<const uint8> bytes, detail::CatalogLayout& outLayout, Diagnostic& outDiagnostic) noexcept;
} // namespace

Catalog::Catalog(Catalog&& other) noexcept
    : mBytes(std::move(other.mBytes)), mLayout(std::exchange(other.mLayout, detail::CatalogLayout{}))
{
}

Catalog& Catalog::operator=(Catalog&& other) noexcept
{
    if (this != &other)
    {
        mBytes = std::move(other.mBytes);
        mLayout = std::exchange(other.mLayout, detail::CatalogLayout{});
    }
    return *this;
}

bool Catalog::IsValid() const noexcept
{
    return mLayout.Token != 0;
}

uint32 Catalog::GetMessageCount() const noexcept
{
    return mLayout.Count;
}

uint32 Catalog::readWord(usize offset) const noexcept
{
    const std::string_view bytes = mBytes.GetView();
    return ReadWord({reinterpret_cast<const uint8*>(bytes.data()), bytes.size()}, offset);
}

std::string_view Catalog::getSlice(usize offset, usize length) const noexcept
{
    return mBytes.GetView().substr(offset, length);
}

std::string_view Catalog::getSliceAt(usize directoryEntry) const noexcept
{
    return getSlice(readWord(directoryEntry), readWord(directoryEntry + 4));
}

std::string_view Catalog::GetDomain() const& noexcept
{
    const usize header = mLayout.Version == 2 ? VERSION_2_HEADER_BYTES : VERSION_1_HEADER_BYTES;
    return IsValid() == true ? getSlice(header, readWord(16)) : std::string_view{};
}

std::string_view Catalog::GetLocale() const& noexcept
{
    const usize header = mLayout.Version == 2 ? VERSION_2_HEADER_BYTES : VERSION_1_HEADER_BYTES;
    return IsValid() == true ? getSlice(header + readWord(16), readWord(20)) : std::string_view{};
}

std::string_view Catalog::GetSourceLocale() const& noexcept
{
    const usize header = mLayout.Version == 2 ? VERSION_2_HEADER_BYTES : VERSION_1_HEADER_BYTES;
    return IsValid() == true ? getSlice(header + readWord(16) + readWord(20), readWord(24)) : std::string_view{};
}

std::string_view Catalog::GetLinguisticData() const& noexcept
{
    if (IsValid() == false || mLayout.Version != 2)
    {
        return {};
    }
    return getSlice(VERSION_2_HEADER_BYTES + readWord(16) + readWord(20) + readWord(24), readWord(52));
}

Status Catalog::findKey(std::string_view domain, std::string_view key, uint32& outIndex) const noexcept
{
    if (IsValid() == false || IsValidId(domain) == false || IsValidId(key) == false)
    {
        return Status::Invalid;
    }
    if (domain != GetDomain())
    {
        return Status::NotFound;
    }
    uint32 begin{};
    uint32 end = mLayout.Count;
    while (begin < end)
    {
        const uint32 index = begin + (end - begin) / 2;
        const usize record = mLayout.Records + static_cast<usize>(index) * mLayout.RecordBytes;
        const int order = getSliceAt(record).compare(key);
        if (order < 0)
        {
            begin = index + 1;
        }
        else if (order > 0)
        {
            end = index;
        }
        else
        {
            outIndex = index;
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

bool Catalog::isSchemaEqual(uint32 index, std::span<const ArgumentDescriptor> arguments) const noexcept
{
    if (mLayout.Version != 2)
    {
        return arguments.empty() == true; // Version 1 messages never declare arguments.
    }
    const usize record = mLayout.Records + static_cast<usize>(index) * mLayout.RecordBytes;
    const uint32 first = readWord(record + RECORD_FIRST_ARGUMENT);
    if (readWord(record + RECORD_ARGUMENT_COUNT) != arguments.size())
    {
        return false;
    }
    for (usize i = 0; i < arguments.size(); ++i)
    {
        const ArgumentDescriptor& expected = arguments[i];
        const usize argument = mLayout.Arguments + (static_cast<usize>(first) + i) * ARGUMENT_RECORD_BYTES;
        const uint32 type = readWord(argument + 8);
        const uint32 policy = readWord(argument + 12);
        if (getSliceAt(argument) != expected.Name || type != static_cast<uint32>(expected.Type))
        {
            return false;
        }
        bool isEqual = true;
        switch (expected.Type)
        {
            case ArgumentType::Text:
                isEqual =
                    policy == static_cast<uint32>(expected.Origin) && readWord(argument + 16) == expected.MaxBytes;
                break;
            case ArgumentType::Count:
            case ArgumentType::Number:
                isEqual = static_cast<int64>(static_cast<int32>(readWord(argument + 20))) == expected.Minimum &&
                          static_cast<int64>(static_cast<int32>(readWord(argument + 24))) == expected.Maximum;
                break;
            case ArgumentType::Enum: {
                const uint32 firstValue = readWord(argument + 28);
                isEqual = policy == static_cast<uint32>(expected.Unknown) &&
                          readWord(argument + 32) == expected.Values.size();
                for (usize j = 0; isEqual == true && j < expected.Values.size(); ++j)
                {
                    isEqual = getSliceAt(mLayout.Values + (static_cast<usize>(firstValue) + j) * SLICE_BYTES) ==
                              expected.Values[j];
                }
                break;
            }
        }
        if (isEqual == false)
        {
            return false;
        }
    }
    return true;
}

Status Catalog::BindMessage(MessageKey key, MessageBinding& output) const noexcept
{
    return BindMessage(MessageDescriptor{key, {}}, output);
}

Status Catalog::BindMessage(const MessageDescriptor& descriptor, MessageBinding& outBinding) const noexcept
{
    uint32 index{};
    const Status found = findKey(descriptor.Key.Domain, descriptor.Key.Key, index);
    if (found != Status::Ok)
    {
        return found;
    }
    if (descriptor.Arguments.size() > MAX_ARGUMENTS)
    {
        return Status::Invalid;
    }
    if (isSchemaEqual(index, descriptor.Arguments) == false)
    {
        return Status::SchemaMismatch;
    }
    outBinding.mToken = mLayout.Token;
    outBinding.mIndex = index;
    return Status::Ok;
}

Status Catalog::ResolveStatic(MessageBinding binding, ResolvedText& output) const& noexcept
{
    if (IsValid() == false)
    {
        return Status::Invalid;
    }
    if (binding.mToken != mLayout.Token || binding.mIndex >= mLayout.Count)
    {
        return Status::WrongCatalog;
    }
    const usize record = mLayout.Records + static_cast<usize>(binding.mIndex) * mLayout.RecordBytes;
    const std::string_view text = getSlice(readWord(record + 8), readWord(record + 12));
    if (mLayout.Version != 2)
    {
        const Origin origin = static_cast<Origin>(readWord(record + 16));
        output = {text, origin, origin == Origin::Translation ? GetLocale() : GetSourceLocale()};
        return Status::Ok;
    }
    if (readWord(record + RECORD_KIND) != KIND_STATIC)
    {
        return Status::RequiresArguments;
    }
    const uint32 provider = readWord(record + RECORD_PROVIDER);
    Origin origin = Origin::Translation;
    if (GetLocale() == GetSourceLocale())
    {
        origin = Origin::Source;
    }
    else if (provider + 1 == mLayout.ProviderCount)
    {
        origin = Origin::SourceFallback;
    }
    else if (provider != 0)
    {
        origin = Origin::Fallback;
    }
    output = {text, origin, getSliceAt(mLayout.Providers + static_cast<usize>(provider) * SLICE_BYTES)};
    return Status::Ok;
}

namespace
{
Status
ValidateVersion1(std::span<const uint8> bytes, detail::CatalogLayout& outLayout, Diagnostic& outDiagnostic) noexcept
{
    if (ReadWord(bytes, 28) != VERSION_1_RECORD_BYTES || ReadWord(bytes, 32) != STATIC_UTF8_PROFILE)
    {
        return Fail(outDiagnostic, Status::Unsupported, 4);
    }
    if (ReadWord(bytes, 8) != bytes.size() || ReadWord(bytes, 36) != 0 || ReadWord(bytes, 40) != 0 ||
        ReadWord(bytes, 44) != 0)
    {
        return Fail(outDiagnostic, Status::Invalid, 8);
    }
    const uint32 count = ReadWord(bytes, 12);
    const uint32 domainLength = ReadWord(bytes, 16);
    const uint32 localeLength = ReadWord(bytes, 20);
    const uint32 sourceLength = ReadWord(bytes, 24);
    if (count > kMaxMessages || domainLength > kMaxIdentifierBytes || localeLength > kMaxIdentifierBytes ||
        sourceLength > kMaxIdentifierBytes)
    {
        return Fail(outDiagnostic, Status::Limit, 12);
    }
    // Prior bounds make every addition/product safe on native AND wasm32.
    const usize records = VERSION_1_HEADER_BYTES + domainLength + localeLength + sourceLength;
    usize cursor = records + static_cast<usize>(count) * VERSION_1_RECORD_BYTES;
    if (cursor > bytes.size())
    {
        return Fail(outDiagnostic, Status::Invalid, 12);
    }
    const std::string_view data{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    const std::string_view locale = data.substr(VERSION_1_HEADER_BYTES + domainLength, localeLength);
    const std::string_view source = data.substr(VERSION_1_HEADER_BYTES + domainLength + localeLength, sourceLength);
    if (IsValidId(data.substr(VERSION_1_HEADER_BYTES, domainLength)) == false || IsValidLocale(locale) == false ||
        IsValidLocale(source) == false)
    {
        return Fail(outDiagnostic, Status::Invalid, VERSION_1_HEADER_BYTES);
    }
    std::string_view previous;
    for (uint32 index = 0; index < count; ++index)
    {
        const usize record = records + static_cast<usize>(index) * VERSION_1_RECORD_BYTES;
        const uint32 keyOffset = ReadWord(bytes, record);
        const uint32 keyLength = ReadWord(bytes, record + 4);
        const uint32 textOffset = ReadWord(bytes, record + 8);
        const uint32 textLength = ReadWord(bytes, record + 12);
        const uint32 origin = ReadWord(bytes, record + 16);
        if (keyLength > kMaxIdentifierBytes || textLength > kMaxTextBytes)
        {
            return Fail(outDiagnostic, Status::Limit, record);
        }
        if (keyOffset != cursor || keyLength > bytes.size() - cursor)
        {
            return Fail(outDiagnostic, Status::Invalid, record);
        }
        const std::string_view key = data.substr(cursor, keyLength);
        if (IsValidId(key) == false || (index != 0 && previous >= key))
        {
            return Fail(outDiagnostic, Status::Invalid, keyOffset);
        }
        previous = key;
        cursor += keyLength;
        if (textOffset != cursor || textLength > bytes.size() - cursor ||
            (locale == source ? origin != 0 : (origin != 1 && origin != 2)))
        {
            return Fail(outDiagnostic, Status::Invalid, record + 8);
        }
        usize invalidByte{};
        if (IsValidText(data.substr(cursor, textLength), invalidByte) == false)
        {
            return Fail(outDiagnostic, Status::Invalid, cursor + invalidByte);
        }
        cursor += textLength;
    }
    if (cursor != bytes.size())
    {
        return Fail(outDiagnostic, Status::Invalid, cursor);
    }
    outLayout.Version = 1;
    outLayout.Count = count;
    outLayout.Records = static_cast<uint32>(records);
    outLayout.RecordBytes = VERSION_1_RECORD_BYTES;
    return Status::Ok;
}

/// Validates one version 2 argument declaration and its enum tokens in packed order.
/// The enum-value directory of a package: its byte offset and entry count.
struct ValueTable final
{
    usize Directory = 0;
    uint32 Count = 0;
};

Status ValidateArgument(std::span<const uint8> bytes,
                        usize argument,
                        ValueTable table,
                        PackedCursor& inoutPayload,
                        uint32& inoutNextValue,
                        Diagnostic& outDiagnostic) noexcept
{
    std::string_view name;
    const Status taken = inoutPayload.Take<MAX_ARGUMENT_NAME_BYTES>(argument, name);
    if (taken != Status::Ok)
    {
        return Fail(outDiagnostic, taken, argument);
    }
    const uint32 type = ReadWord(bytes, argument + 8);
    const uint32 policy = ReadWord(bytes, argument + 12);
    const uint32 maxBytes = ReadWord(bytes, argument + 16);
    const int64 minimum = static_cast<int32>(ReadWord(bytes, argument + 20));
    const int64 maximum = static_cast<int32>(ReadWord(bytes, argument + 24));
    const uint32 firstValue = ReadWord(bytes, argument + 28);
    const uint32 values = ReadWord(bytes, argument + 32);
    const bool isText = type == static_cast<uint32>(ArgumentType::Text);
    const bool isEnum = type == static_cast<uint32>(ArgumentType::Enum);
    const bool isBounded =
        type == static_cast<uint32>(ArgumentType::Count) || type == static_cast<uint32>(ArgumentType::Number);
    const bool hasValidText = isText == false || (maxBytes >= 1 && maxBytes <= MAX_TEXT_ARGUMENT_BYTES);
    const bool hasValidBounds =
        isBounded == true ? (minimum >= -COUNT_LIMIT && minimum <= maximum && maximum <= COUNT_LIMIT && policy == 0)
                          : (minimum == 0 && maximum == 0);
    const bool hasValidValues = isEnum == true ? (values >= 1 && values <= MAX_ENUM_VALUES) : values == 0;
    if (IsValidArgumentName(name) == false || type > static_cast<uint32>(ArgumentType::Enum) || policy > 1 ||
        hasValidText == false || (isText == false && maxBytes != 0) || hasValidBounds == false ||
        hasValidValues == false || firstValue != inoutNextValue || values > table.Count - inoutNextValue)
    {
        return Fail(outDiagnostic, Status::Invalid, argument);
    }
    for (uint32 i = 0; i < values; ++i)
    {
        const usize entry = table.Directory + (static_cast<usize>(firstValue) + i) * SLICE_BYTES;
        std::string_view token;
        const Status value = inoutPayload.Take<MAX_ARGUMENT_NAME_BYTES>(entry, token);
        if (value != Status::Ok)
        {
            return Fail(outDiagnostic, value, entry);
        }
        bool isDuplicate = false;
        for (uint32 j = 0; j < i; ++j)
        {
            const usize earlier = table.Directory + (static_cast<usize>(firstValue) + j) * SLICE_BYTES;
            const std::string_view other{reinterpret_cast<const char*>(bytes.data()) + ReadWord(bytes, earlier),
                                         ReadWord(bytes, earlier + 4)};
            isDuplicate = isDuplicate == true || other == token;
        }
        if (IsValidArgumentName(token) == false || token == "other" || isDuplicate == true)
        {
            return Fail(outDiagnostic, Status::Invalid, entry);
        }
    }
    inoutNextValue += values;
    return Status::Ok;
}

Status
ValidateVersion2(std::span<const uint8> bytes, detail::CatalogLayout& outLayout, Diagnostic& outDiagnostic) noexcept
{
    if (bytes.size() < VERSION_2_HEADER_BYTES)
    {
        return Fail(outDiagnostic, Status::Invalid, 0);
    }
    if (ReadWord(bytes, 28) != VERSION_2_RECORD_BYTES || ReadWord(bytes, 32) != ICU_MF1_PROFILE ||
        ReadWord(bytes, 44) != ARGUMENT_RECORD_BYTES)
    {
        return Fail(outDiagnostic, Status::Unsupported, 28);
    }
    if (ReadWord(bytes, 8) != bytes.size() || ReadWord(bytes, 56) != 0 || ReadWord(bytes, 60) != 0)
    {
        return Fail(outDiagnostic, Status::Invalid, 8);
    }
    const uint32 count = ReadWord(bytes, 12);
    const uint32 domainLength = ReadWord(bytes, 16);
    const uint32 localeLength = ReadWord(bytes, 20);
    const uint32 sourceLength = ReadWord(bytes, 24);
    const uint32 providerCount = ReadWord(bytes, 36);
    const uint32 argumentCount = ReadWord(bytes, 40);
    const uint32 valueCount = ReadWord(bytes, 48);
    const uint32 labelLength = ReadWord(bytes, 52);
    if (count > kMaxMessages || domainLength > kMaxIdentifierBytes || localeLength > kMaxIdentifierBytes ||
        sourceLength > kMaxIdentifierBytes || labelLength > kMaxIdentifierBytes || providerCount == 0 ||
        providerCount > MAX_FALLBACK_PROVIDERS || argumentCount > static_cast<uint64>(count) * MAX_ARGUMENTS ||
        valueCount > static_cast<uint64>(argumentCount) * MAX_ENUM_VALUES)
    {
        return Fail(outDiagnostic, Status::Limit, 12);
    }
    // These bounds keep every product below 2^31, so usize arithmetic is safe on wasm32.
    const usize providers = VERSION_2_HEADER_BYTES + domainLength + localeLength + sourceLength + labelLength;
    const usize records = providers + static_cast<usize>(providerCount) * SLICE_BYTES;
    const usize arguments = records + static_cast<usize>(count) * VERSION_2_RECORD_BYTES;
    const usize values = arguments + static_cast<usize>(argumentCount) * ARGUMENT_RECORD_BYTES;
    const usize payload = values + static_cast<usize>(valueCount) * SLICE_BYTES;
    if (payload > bytes.size())
    {
        return Fail(outDiagnostic, Status::Invalid, 12);
    }
    const std::string_view data{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    const std::string_view locale = data.substr(VERSION_2_HEADER_BYTES + domainLength, localeLength);
    const std::string_view source = data.substr(VERSION_2_HEADER_BYTES + domainLength + localeLength, sourceLength);
    const std::string_view label = data.substr(providers - labelLength, labelLength);
    if (IsValidId(data.substr(VERSION_2_HEADER_BYTES, domainLength)) == false || IsValidLocale(locale) == false ||
        IsValidLocale(source) == false || IsValidLinguisticLabel(label) == false)
    {
        return Fail(outDiagnostic, Status::Invalid, VERSION_2_HEADER_BYTES);
    }
    PackedCursor cursor(bytes, payload);
    std::string_view path[MAX_FALLBACK_PROVIDERS];
    for (uint32 i = 0; i < providerCount; ++i)
    {
        const usize entry = providers + static_cast<usize>(i) * SLICE_BYTES;
        const Status taken = cursor.Take<kMaxIdentifierBytes>(entry, path[i]);
        if (taken != Status::Ok)
        {
            return Fail(outDiagnostic, taken, entry);
        }
        bool isDuplicate = false;
        for (uint32 j = 0; j < i; ++j)
        {
            isDuplicate = isDuplicate == true || path[j] == path[i];
        }
        if (IsValidLocale(path[i]) == false || isDuplicate == true)
        {
            return Fail(outDiagnostic, Status::Invalid, entry);
        }
    }
    // The path starts at the package locale and ends at the source; a source package has no fallback.
    if (path[0] != locale || path[providerCount - 1] != source || (locale == source) != (providerCount == 1))
    {
        return Fail(outDiagnostic, Status::Invalid, providers);
    }
    std::string_view previous;
    uint32 nextArgument = 0;
    uint32 nextValue = 0;
    for (uint32 index = 0; index < count; ++index)
    {
        const usize record = records + static_cast<usize>(index) * VERSION_2_RECORD_BYTES;
        const uint32 kind = ReadWord(bytes, record + RECORD_KIND);
        const uint32 provider = ReadWord(bytes, record + RECORD_PROVIDER);
        const uint32 firstArgument = ReadWord(bytes, record + RECORD_FIRST_ARGUMENT);
        const uint32 recordArguments = ReadWord(bytes, record + RECORD_ARGUMENT_COUNT);
        const uint32 sourceRevision = ReadWord(bytes, record + 32);
        const uint32 flags = ReadWord(bytes, record + 36);
        std::string_view key;
        Status taken = cursor.Take<kMaxIdentifierBytes>(record, key);
        if (taken != Status::Ok)
        {
            return Fail(outDiagnostic, taken, record);
        }
        if (IsValidId(key) == false || (index != 0 && previous >= key))
        {
            return Fail(outDiagnostic, Status::Invalid, ReadWord(bytes, record));
        }
        previous = key;
        if (kind != KIND_STATIC && kind != KIND_DYNAMIC)
        {
            return Fail(outDiagnostic, Status::Invalid, record + RECORD_KIND);
        }
        std::string_view text;
        const usize textStart = cursor.GetCursor();
        taken = kind == KIND_STATIC ? cursor.Take<kMaxTextBytes>(record + 8, text)
                                    : cursor.Take<MAX_PATTERN_BYTES>(record + 8, text);
        if (taken != Status::Ok)
        {
            return Fail(outDiagnostic, taken, record + 8);
        }
        usize invalidByte{};
        if (IsValidText(text, invalidByte) == false)
        {
            return Fail(outDiagnostic, Status::Invalid, textStart + invalidByte);
        }
        // A static record has no arguments; a dynamic one is formatted with its full schema in L2.
        if (provider >= providerCount || firstArgument != nextArgument || recordArguments > MAX_ARGUMENTS ||
            recordArguments > argumentCount - nextArgument || (kind == KIND_STATIC) != (recordArguments == 0) ||
            sourceRevision == 0 || (flags & ~FLAG_REQUIRED) != 0)
        {
            return Fail(outDiagnostic, Status::Invalid, record + RECORD_KIND);
        }
        for (uint32 i = 0; i < recordArguments; ++i)
        {
            const usize argument = arguments + (static_cast<usize>(firstArgument) + i) * ARGUMENT_RECORD_BYTES;
            const Status checked =
                ValidateArgument(bytes, argument, {values, valueCount}, cursor, nextValue, outDiagnostic);
            if (checked != Status::Ok)
            {
                return checked;
            }
            const std::string_view name = data.substr(ReadWord(bytes, argument), ReadWord(bytes, argument + 4));
            for (uint32 j = 0; j < i; ++j)
            {
                const usize earlier = arguments + (static_cast<usize>(firstArgument) + j) * ARGUMENT_RECORD_BYTES;
                if (data.substr(ReadWord(bytes, earlier), ReadWord(bytes, earlier + 4)) == name)
                {
                    return Fail(outDiagnostic, Status::Invalid, argument);
                }
            }
        }
        nextArgument += recordArguments;
    }
    if (nextArgument != argumentCount || nextValue != valueCount)
    {
        return Fail(outDiagnostic, Status::Invalid, arguments);
    }
    if (cursor.GetCursor() != bytes.size())
    {
        return Fail(outDiagnostic, Status::Invalid, cursor.GetCursor());
    }
    outLayout.Version = 2;
    outLayout.Count = count;
    outLayout.Records = static_cast<uint32>(records);
    outLayout.RecordBytes = VERSION_2_RECORD_BYTES;
    outLayout.Providers = static_cast<uint32>(providers);
    outLayout.ProviderCount = providerCount;
    outLayout.Arguments = static_cast<uint32>(arguments);
    outLayout.Values = static_cast<uint32>(values);
    return Status::Ok;
}
} // namespace

Status PrepareCatalog(std::span<const uint8> bytes,
                      const foundation::AllocationDomain& domain,
                      Catalog& output,
                      Diagnostic& diagnostic) noexcept
{
    if (bytes.size() > kMaxCatalogBytes)
    {
        return Fail(diagnostic, Status::Limit, 0);
    }
    if (bytes.size() < VERSION_1_HEADER_BYTES || ReadWord(bytes, 0) != MAGIC)
    {
        return Fail(diagnostic, Status::Invalid, 0);
    }
    detail::CatalogLayout layout;
    const uint32 version = ReadWord(bytes, 4);
    Status validated = Status::Unsupported;
    if (version == 1)
    {
        validated = ValidateVersion1(bytes, layout, diagnostic);
    }
    else if (version == 2)
    {
        validated = ValidateVersion2(bytes, layout, diagnostic);
    }
    else
    {
        return Fail(diagnostic, Status::Unsupported, 4);
    }
    if (validated != Status::Ok)
    {
        return validated;
    }
    Catalog candidate;
    const std::string_view data{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    if (foundation::CreateShared(data, domain, candidate.mBytes) != foundation::StringStatus::Ok)
    {
        return Fail(diagnostic, Status::OutOfMemory, 0);
    }
    const Status token = MakeToken(layout.Token);
    if (token != Status::Ok)
    {
        return Fail(diagnostic, token, 0);
    }
    candidate.mLayout = layout;
    output = std::move(candidate);
    diagnostic = {};
    return Status::Ok;
}
} // namespace ludus::localization
