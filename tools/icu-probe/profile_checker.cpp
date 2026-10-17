// L1 host profile checker for the ludus-icu-mf1-v1 localization cooker.
//
// The cooker (scripts/python/localization_release.py) writes a bounded binary
// request naming (locale, argument schema, pattern) items; this tool validates
// each pattern with pinned ICU4C through the profile walk it shares with the
// L0 probe (profile.hpp) and decodes accepted argument-free patterns to their
// literal UTF-8 text. ICU stays private to this host tool: it never links into
// the engine, an installed SDK or a cooked package. Following the L0 finding
// that ICU4C 78.3 is unsafe under allocation failure, any ICU allocation
// failure here is fatal. Protocol: docs/modules/localization.md.
//
// Thanks to Unicode, Inc. and the ICU project for ICU4C 78.3, used under the
// Unicode License v3 (ICU-LICENSE.txt beside the binary). Original Ludus code.

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <cstdlib>
#include <span>
#include <string_view>

#include <unicode/bytestream.h>
#include <unicode/dcfmtsym.h>
#include <unicode/fmtable.h>
#include <unicode/localpointer.h>
#include <unicode/msgfmt.h>
#include <unicode/numberformatter.h>
#include <unicode/uclean.h>
#include <unicode/udata.h>
#include <unicode/ures.h>
#include <unicode/uversion.h>

#include "profile.hpp"

#if defined(__cpp_exceptions)
#    error The localization profile checker and every ICU translation unit must compile without C++ exceptions
#endif

namespace ludus::icu_probe
{
alignas(16) extern const foundation::uint8 ICU_DATA[];
extern const foundation::usize ICU_DATA_BYTES;

namespace
{
using foundation::Array;
using foundation::logging::LogCategory;

inline constexpr LogCategory LOG_PROFILE{"LocalizationProfile"};
inline constexpr uint32 REQUEST_MAGIC = 0x5152504c;  // "LPRQ" in little endian.
inline constexpr uint32 RESPONSE_MAGIC = 0x5352504c; // "LPRS" in little endian.
inline constexpr uint32 PROTOCOL_VERSION = 1;
inline constexpr usize MAX_REQUEST_BYTES = usize{256} * 1024 * 1024;
inline constexpr uint32 MAX_LOCALES = 128;
inline constexpr uint32 MAX_ITEMS = uint32{1} << 24;
inline constexpr uint32 MAX_ARGUMENTS = 32;
inline constexpr uint32 MAX_ENUM_VALUES = 64;
inline constexpr uint32 MAX_ARGUMENT_NAME_BYTES = 64;
inline constexpr uint32 MAX_TEXT_ARGUMENT_BYTES = 4096;
inline constexpr int64 COUNT_LIMIT = 1000000000;
inline constexpr usize VERSION_TEXT_BYTES = U_MAX_VERSION_STRING_LENGTH + 1;

/// Per-locale outcome reported to the cooker.
enum class LocaleStatus : uint8
{
    Ok = 0,
    InvalidTag = 1,
    MissingData = 2,
};

// ---------------------------------------------------------------------------
// ICU memory hooks: allocation failure is fatal (L0 allocation-failure findings).
// ---------------------------------------------------------------------------

void* U_CALLCONV AllocateOrDie(const void* /*context*/, usize bytes) noexcept
{
    void* blockOrNull = std::malloc(bytes == 0 ? 1 : bytes);
    if (blockOrNull == nullptr)
    {
        LUDUS_FATAL("ICU allocation failed; ICU4C cannot recover from allocation failure");
    }
    return blockOrNull;
}

// ICU fixes the parameter order in UMemReallocFn; it cannot use distinct wrappers.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void* U_CALLCONV ReallocateOrDie(const void* /*context*/, void* blockOrNull, usize bytes) noexcept
{
    void* resizedOrNull = std::realloc(blockOrNull, bytes == 0 ? 1 : bytes);
    if (resizedOrNull == nullptr)
    {
        LUDUS_FATAL("ICU reallocation failed; ICU4C cannot recover from allocation failure");
    }
    return resizedOrNull;
}

void U_CALLCONV FreeBlock(const void* /*context*/, void* blockOrNull) noexcept
{
    std::free(blockOrNull);
}

/// Host-owned ICU lifetime, in the order L0 established: hooks, data, warm-up.
bool InitializeIcu() noexcept
{
    UErrorCode status = U_ZERO_ERROR;
    u_setMemoryFunctions(nullptr, AllocateOrDie, ReallocateOrDie, FreeBlock, &status);
    udata_setCommonData(ICU_DATA, &status);
    static_cast<void>(icu::Locale::getRoot());
    const icu::LocalPointer<icu::PluralRules> rules(icu::PluralRules::forLocale(icu::Locale("und"), status));
    static_cast<void>(icu::number::NumberFormatter::forSkeleton(icu::UnicodeString(u"precision-integer"), status));
    return U_SUCCESS(status);
}

// ---------------------------------------------------------------------------
// Bounded little-endian request decoding; any malformed field fails the request.
// ---------------------------------------------------------------------------

class RequestReader final
{
public:
    explicit RequestReader(std::span<const uint8> bytes) noexcept : mBytes(bytes) {}

    [[nodiscard]] bool ReadWord(uint32& outValue) noexcept
    {
        if (mBytes.size() - mCursor < sizeof(uint32) ||
            foundation::TryReadLittleEndian(mBytes.subspan(mCursor, sizeof(uint32)), outValue) == false)
        {
            return false;
        }
        mCursor += sizeof(uint32);
        return true;
    }

    [[nodiscard]] bool ReadSigned(int64& outValue) noexcept
    {
        uint32 word = 0;
        if (ReadWord(word) == false)
        {
            return false;
        }
        // Bounds are serialized as two's-complement int32 within +/-COUNT_LIMIT.
        const int64 value =
            word >= 0x80000000U ? static_cast<int64>(word) - (int64{1} << 32) : static_cast<int64>(word);
        if (value < -COUNT_LIMIT || value > COUNT_LIMIT)
        {
            return false;
        }
        outValue = value;
        return true;
    }

    [[nodiscard]] bool ReadBytes(uint32 limit, Bytes& outValue) noexcept
    {
        uint32 length = 0;
        if (ReadWord(length) == false || length > limit || mBytes.size() - mCursor < length)
        {
            return false;
        }
        outValue = {reinterpret_cast<const char*>(mBytes.data() + mCursor), length};
        mCursor += length;
        return true;
    }

    [[nodiscard]] bool IsComplete() const noexcept
    {
        return mCursor == mBytes.size();
    }

private:
    std::span<const uint8> mBytes;
    usize mCursor = 0;
};

struct Item final
{
    uint32 Locale = 0;
    Bytes Pattern;
    MessageSchema Schema;
};

struct Request final
{
    Array<Bytes> Locales;
    Array<Item> Items;
    Array<ArgumentSchema> Arguments;
    Array<Bytes> Values;
};

bool IsArgumentName(Bytes name) noexcept
{
    if (name.Size == 0 || name.Size > MAX_ARGUMENT_NAME_BYTES || name.Data[0] < 'a' || name.Data[0] > 'z')
    {
        return false;
    }
    for (usize i = 0; i < name.Size; ++i)
    {
        const char character = name.Data[i];
        const bool isAllowed =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
        if (isAllowed == false)
        {
            return false;
        }
    }
    return true;
}

bool ReadArgument(RequestReader& inoutReader, Request& inoutRequest, ArgumentSchema& outArgument) noexcept
{
    uint32 type = 0;
    uint32 policy = 0;
    uint32 maxBytes = 0;
    uint32 valueCount = 0;
    ArgumentSchema argument;
    if (inoutReader.ReadBytes(MAX_ARGUMENT_NAME_BYTES, argument.Name) == false ||
        IsArgumentName(argument.Name) == false || inoutReader.ReadWord(type) == false ||
        inoutReader.ReadWord(policy) == false || inoutReader.ReadWord(maxBytes) == false ||
        inoutReader.ReadSigned(argument.Minimum) == false || inoutReader.ReadSigned(argument.Maximum) == false ||
        inoutReader.ReadWord(valueCount) == false)
    {
        return false;
    }
    if (type > static_cast<uint32>(ArgumentType::Enum) || policy > 1 || argument.Minimum > argument.Maximum ||
        valueCount > MAX_ENUM_VALUES)
    {
        return false;
    }
    argument.Type = static_cast<ArgumentType>(type);
    const bool isText = argument.Type == ArgumentType::Text;
    const bool isEnum = argument.Type == ArgumentType::Enum;
    if ((isText == true && (maxBytes == 0 || maxBytes > MAX_TEXT_ARGUMENT_BYTES)) ||
        (isText == false && maxBytes != 0) || (isEnum == true && valueCount == 0) ||
        (isEnum == false && valueCount != 0) || (isText == false && isEnum == false && policy != 0))
    {
        return false;
    }
    argument.MaxBytes = maxBytes;
    argument.Origin = policy == 1 ? corpus::TextOrigin::User : corpus::TextOrigin::Game;
    argument.Unknown = policy == 1 ? corpus::UnknownPolicy::Reject : corpus::UnknownPolicy::Other;
    // Values are appended contiguously; ValuesOrNull is bound after the request is complete.
    argument.ValueCount = valueCount;
    argument.ValuesOrNull = nullptr;
    for (uint32 i = 0; i < valueCount; ++i)
    {
        Bytes value;
        if (inoutReader.ReadBytes(MAX_ARGUMENT_NAME_BYTES, value) == false || IsArgumentName(value) == false ||
            inoutRequest.Values.TryAdd(value) == false)
        {
            return false;
        }
    }
    outArgument = argument;
    return true;
}

bool ReadRequest(std::span<const uint8> bytes, Request& outRequest) noexcept
{
    RequestReader reader(bytes);
    uint32 magic = 0;
    uint32 version = 0;
    uint32 localeCount = 0;
    if (reader.ReadWord(magic) == false || magic != REQUEST_MAGIC || reader.ReadWord(version) == false ||
        version != PROTOCOL_VERSION || reader.ReadWord(localeCount) == false || localeCount == 0 ||
        localeCount > MAX_LOCALES)
    {
        return false;
    }
    for (uint32 i = 0; i < localeCount; ++i)
    {
        Bytes tag;
        if (reader.ReadBytes(static_cast<uint32>(MAX_LOCALE_TAG_BYTES), tag) == false ||
            outRequest.Locales.TryAdd(tag) == false)
        {
            return false;
        }
    }
    uint32 itemCount = 0;
    if (reader.ReadWord(itemCount) == false || itemCount > MAX_ITEMS)
    {
        return false;
    }
    Array<usize> firstArguments;
    for (uint32 i = 0; i < itemCount; ++i)
    {
        Item item;
        uint32 argumentCount = 0;
        // A pattern one byte over the profile limit still reaches the walk, so the
        // checker reports PatternTooLarge instead of a malformed request.
        if (reader.ReadWord(item.Locale) == false || item.Locale >= localeCount ||
            reader.ReadBytes(static_cast<uint32>(MAX_PATTERN_BYTES) + 1, item.Pattern) == false ||
            reader.ReadWord(argumentCount) == false || argumentCount > MAX_ARGUMENTS ||
            firstArguments.TryAdd(outRequest.Arguments.GetSize()) == false)
        {
            return false;
        }
        for (uint32 j = 0; j < argumentCount; ++j)
        {
            ArgumentSchema argument;
            if (ReadArgument(reader, outRequest, argument) == false || outRequest.Arguments.TryAdd(argument) == false)
            {
                return false;
            }
        }
        item.Schema.ArgumentCount = argumentCount;
        if (outRequest.Items.TryAdd(item) == false)
        {
            return false;
        }
    }
    if (reader.IsComplete() == false)
    {
        return false;
    }
    // Arrays no longer grow: bind schema views into their final storage.
    usize nextValue = 0;
    for (ArgumentSchema& argument : outRequest.Arguments)
    {
        argument.ValuesOrNull = argument.ValueCount == 0 ? nullptr : outRequest.Values.GetData() + nextValue;
        nextValue += argument.ValueCount;
    }
    for (usize i = 0; i < outRequest.Items.GetSize(); ++i)
    {
        Item& item = outRequest.Items[i];
        item.Schema.ArgumentsOrNull =
            item.Schema.ArgumentCount == 0 ? nullptr : outRequest.Arguments.GetData() + firstArguments[i];
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pinned data identity and per-locale rules.
// ---------------------------------------------------------------------------

struct LocaleRules final
{
    icu::LocalPointer<icu::PluralRules> Cardinal;
    icu::LocalPointer<icu::PluralRules> Ordinal;
    icu::Locale Locale{"und"};
    LocaleStatus Status = LocaleStatus::InvalidTag;
};

/// Requires the language's own sliced data: a root fallback would silently
/// accept any keyword ICU's root rules allow and decode with root symbols.
void PrepareLocale(Bytes tag, LocaleRules& outRules) noexcept
{
    if (IsUtf8(tag) == false || U_FAILURE(MakeLocale(tag, outRules.Locale)))
    {
        outRules.Status = LocaleStatus::InvalidTag;
        return;
    }
    UErrorCode status = U_ZERO_ERROR;
    const icu::DecimalFormatSymbols symbols(outRules.Locale, status);
    const icu::Locale actual = symbols.getLocale(ULOC_ACTUAL_LOCALE, status);
    outRules.Cardinal.adoptInstead(icu::PluralRules::forLocale(outRules.Locale, UPLURAL_TYPE_CARDINAL, status));
    outRules.Ordinal.adoptInstead(icu::PluralRules::forLocale(outRules.Locale, UPLURAL_TYPE_ORDINAL, status));
    const bool hasData = U_SUCCESS(status) && outRules.Cardinal.isValid() == true &&
                         outRules.Ordinal.isValid() == true &&
                         std::string_view(actual.getLanguage()) == std::string_view(outRules.Locale.getLanguage());
    outRules.Status = hasData == true ? LocaleStatus::Ok : LocaleStatus::MissingData;
}

class ResponseWriter final
{
public:
    [[nodiscard]] bool WriteWord(uint32 value) noexcept
    {
        uint8 encoded[sizeof(uint32)] = {};
        return foundation::TryWriteLittleEndian(value, std::span<uint8>(encoded)) == true &&
               mBytes.TryAddRange(std::span<const uint8>(encoded)) == true;
    }

    [[nodiscard]] bool WriteBytes(std::string_view text) noexcept
    {
        return text.size() <= 0xffffffffU && WriteWord(static_cast<uint32>(text.size())) == true &&
               mBytes.TryAddRange(std::span<const uint8>(reinterpret_cast<const uint8*>(text.data()), text.size())) ==
                   true;
    }

    [[nodiscard]] std::span<const uint8> GetBytes() const noexcept
    {
        return mBytes.AsSpan();
    }

private:
    Array<uint8> mBytes;
};

bool WriteVersion(ResponseWriter& inoutWriter, const UVersionInfo version) noexcept
{
    char text[VERSION_TEXT_BYTES] = {};
    u_versionToString(version, text);
    return inoutWriter.WriteBytes(std::string_view(text));
}

/// Writes the pinned ICU, CLDR and Unicode versions and the embedded data identity.
bool WriteIdentity(ResponseWriter& inoutWriter) noexcept
{
    UVersionInfo version{};
    u_getVersion(version);
    if (WriteVersion(inoutWriter, version) == false)
    {
        return false;
    }
    UErrorCode status = U_ZERO_ERROR;
    const icu::LocalUResourceBundlePointer bundle(ures_openDirect(nullptr, "icuver", &status));
    int32 length = 0;
    const UChar* cldrOrNull = ures_getStringByKey(bundle.getAlias(), "CLDRVersion", &length, &status);
    if (U_FAILURE(status) || cldrOrNull == nullptr)
    {
        return false;
    }
    char cldr[VERSION_TEXT_BYTES] = {};
    icu::CheckedArrayByteSink sink(cldr, static_cast<int32>(sizeof(cldr)));
    icu::UnicodeString(cldrOrNull, length).toUTF8(sink);
    if (sink.Overflowed() == true ||
        inoutWriter.WriteBytes(std::string_view(cldr, static_cast<usize>(sink.NumberOfBytesAppended()))) == false)
    {
        return false;
    }
    u_getUnicodeVersion(version);
    if (WriteVersion(inoutWriter, version) == false)
    {
        return false;
    }
    const foundation::Fingerprint128 fingerprint =
        foundation::StableFingerprint128(std::span<const uint8>(ICU_DATA, ICU_DATA_BYTES));
    return ICU_DATA_BYTES <= 0xffffffffU && inoutWriter.WriteWord(static_cast<uint32>(ICU_DATA_BYTES)) == true &&
           inoutWriter.WriteWord(static_cast<uint32>(fingerprint.Low)) == true &&
           inoutWriter.WriteWord(static_cast<uint32>(fingerprint.Low >> 32U)) == true &&
           inoutWriter.WriteWord(static_cast<uint32>(fingerprint.High)) == true &&
           inoutWriter.WriteWord(static_cast<uint32>(fingerprint.High >> 32U)) == true;
}

/// Decodes through the part walk and cross-checks ICU's own zero-argument format.
bool DecodeStatic(Bytes pattern, const icu::Locale& locale, Array<char>& outText) noexcept
{
    icu::UnicodeString decoded;
    if (DecodeLiteralPattern(pattern, decoded) == false)
    {
        return false;
    }
    UErrorCode status = U_ZERO_ERROR;
    UParseError parseError{};
    const icu::MessageFormat format(ToUnicode(pattern), locale, parseError, status);
    icu::UnicodeString formatted;
    format.format(nullptr, nullptr, 0, formatted, status);
    if (U_FAILURE(status) || formatted.isBogus() == true || formatted != decoded)
    {
        return false;
    }
    // The decoder removes syntax only, so UTF-8 output never exceeds the input.
    if (outText.TryResize(pattern.Size) == false)
    {
        return false;
    }
    icu::CheckedArrayByteSink sink(outText.GetData(), static_cast<int32>(pattern.Size));
    decoded.toUTF8(sink);
    if (sink.Overflowed() == true)
    {
        return false;
    }
    return outText.TryResize(static_cast<usize>(sink.NumberOfBytesAppended()));
}

bool Check(const Request& request, ResponseWriter& outWriter) noexcept
{
    if (outWriter.WriteWord(RESPONSE_MAGIC) == false || outWriter.WriteWord(PROTOCOL_VERSION) == false ||
        WriteIdentity(outWriter) == false ||
        outWriter.WriteWord(static_cast<uint32>(request.Locales.GetSize())) == false)
    {
        return false;
    }
    Array<LocaleRules> rules;
    if (rules.TryResize(request.Locales.GetSize()) == false)
    {
        return false;
    }
    for (usize i = 0; i < request.Locales.GetSize(); ++i)
    {
        PrepareLocale(request.Locales[i], rules[i]);
        if (outWriter.WriteWord(static_cast<uint32>(rules[i].Status)) == false)
        {
            return false;
        }
    }
    if (outWriter.WriteWord(static_cast<uint32>(request.Items.GetSize())) == false)
    {
        return false;
    }
    Array<char> text;
    for (const Item& item : request.Items)
    {
        const LocaleRules& locale = rules[item.Locale];
        Rejection rejection = Rejection::SyntaxError;
        if (locale.Status == LocaleStatus::Ok)
        {
            rejection = ValidatePattern(item.Schema, item.Pattern, *locale.Cardinal, *locale.Ordinal);
        }
        const bool hasText =
            locale.Status == LocaleStatus::Ok && rejection == Rejection::Accepted && item.Schema.ArgumentCount == 0;
        if (hasText == true && DecodeStatic(item.Pattern, locale.Locale, text) == false)
        {
            LUDUS_LOG_ERROR(LOG_PROFILE, "ICU could not decode an accepted literal pattern");
            return false;
        }
        const std::string_view value = hasText == true ? std::string_view(text.GetData(), text.GetSize()) : "";
        if (outWriter.WriteWord(static_cast<uint32>(rejection)) == false ||
            outWriter.WriteWord(hasText == true ? 1U : 0U) == false || outWriter.WriteBytes(value) == false)
        {
            return false;
        }
    }
    return true;
}

bool ReadFile(std::string_view directory, std::string_view name, Array<uint8>& outBytes) noexcept
{
    foundation::filesystem::Directory root;
    foundation::filesystem::File file;
    if (root.Open(directory).Succeeded() == false || root.OpenRead(name, file).Succeeded() == false ||
        file.Size() > MAX_REQUEST_BYTES || outBytes.TryResize(static_cast<usize>(file.Size())) == false)
    {
        return false;
    }
    const foundation::filesystem::ReadResult read = file.ReadAt(0, outBytes.AsSpan());
    return read.Outcome.Succeeded() == true && read.BytesRead == outBytes.GetSize();
}

int32 Run(std::string_view directory, std::string_view requestName, std::string_view responseName) noexcept
{
    if (InitializeIcu() == false)
    {
        LUDUS_LOG_ERROR(LOG_PROFILE, "ICU host initialization failed");
        return 2;
    }
    int32 result = 0;
    {
        Array<uint8> bytes;
        Request request;
        ResponseWriter response;
        if (ReadFile(directory, requestName, bytes) == false || ReadRequest(bytes.AsSpan(), request) == false)
        {
            LUDUS_LOG_ERROR(LOG_PROFILE, "Malformed or unreadable profile request {}", requestName);
            result = 2;
        }
        else if (Check(request, response) == false)
        {
            result = 3;
        }
        else
        {
            foundation::filesystem::WriteDirectory output;
            foundation::filesystem::WriteOptions options;
            options.MaxBytes = MAX_REQUEST_BYTES * 2;
            options.Sync = foundation::filesystem::SyncPolicy::PublishOnly;
            if (output.Open(directory).Succeeded() == false ||
                output.Publish(responseName, response.GetBytes(), options).Outcome.Succeeded() == false)
            {
                LUDUS_LOG_ERROR(LOG_PROFILE, "Could not publish profile response {}", responseName);
                result = 2;
            }
        }
    }
    // Every ICU object above is destroyed before host teardown.
    u_cleanup();
    return result;
}
} // namespace
} // namespace ludus::icu_probe

int main(int argumentCount, char** arguments)
{
    using namespace ludus::foundation::logging;
    LogConfig config;
    config.EnableFile = false;
    config.EnableDebugger = false;
    if (LogSystem::Initialize(config).Status != LogStatus::Ok)
    {
        return 2;
    }
    int result = 2;
    if (argumentCount == 4)
    {
        result = ludus::icu_probe::Run(arguments[1], arguments[2], arguments[3]);
    }
    else
    {
        LUDUS_LOG_ERROR(ludus::icu_probe::LOG_PROFILE, "usage: ludus_localization_profile DIRECTORY REQUEST RESPONSE");
    }
    LogSystem::Shutdown();
    return result;
}
