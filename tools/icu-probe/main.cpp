// Private L0 backend probe for docs/architecture/localization-l0.md.
//
// Thanks to Unicode, Inc. and the ICU project for ICU4C 78.3, particularly the
// MessageFormat, MessagePattern and PluralRules API references and the ICU User
// Guide, "Formatting Messages":
// https://unicode-org.github.io/icu/userguide/format_parse/messages/
// The probe adopts ICU's named-argument MessageFormat 1 API and MessagePattern
// part model unchanged; the profile walk, typed argument adapter, isolate
// insertion, fail-point sweep and metrics are original Ludus code. ICU4C is used
// under the Unicode License v3; see ICU-LICENSE.txt beside the probe binary.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/strings/utf8.hpp>
#include <ludus/foundation/time/time.hpp>

#include <cstdlib>
#include <limits>
#include <string_view>

#include <unicode/bytestream.h>
#include <unicode/dcfmtsym.h>
#include <unicode/fmtable.h>
#include <unicode/localpointer.h>
#include <unicode/locid.h>
#include <unicode/messagepattern.h>
#include <unicode/msgfmt.h>
#include <unicode/numberformatter.h>
#include <unicode/plurrule.h>
#include <unicode/uchar.h>
#include <unicode/uclean.h>
#include <unicode/udata.h>
#include <unicode/unistr.h>
#include <unicode/ures.h>
#include <unicode/ustring.h>
#include <unicode/utf8.h>
#include <unicode/uversion.h>

#include "corpus.h"
#include "profile.hpp"

#if defined(__cpp_exceptions)
#    error The L0 backend probe and every ICU translation unit must compile without C++ exceptions
#endif
#if !UCONFIG_NO_FILE_IO || !UCONFIG_NO_MF2 || !UCONFIG_NO_SERVICE
#    error The L0 profile registers embedded data only and excludes MF2 and ICU services
#endif

namespace ludus::icu_probe
{
alignas(16) extern const foundation::uint8 ICU_DATA[];
extern const foundation::usize ICU_DATA_BYTES;

using namespace foundation;
using namespace foundation::logging;
using corpus::ArgumentSchema;
using corpus::ArgumentType;
using corpus::ArgumentValue;
using corpus::Bytes;
using corpus::MessageSchema;
using corpus::Rejection;
using corpus::ValueKind;

inline constexpr LogCategory LOG_L0{"LocalizationL0"};
inline constexpr usize MAX_OUTPUT_BYTES = usize{64} * 1024;
inline constexpr usize MAX_ARGUMENTS = 32;
inline constexpr usize MAX_ALLOCATION_BYTES = usize{64} * 1024 * 1024;
inline constexpr usize SWEEP_POINT_LIMIT = 4096;
inline constexpr usize WARM_CONSTRUCTIONS = 200;
inline constexpr usize WARM_FORMATS = 2000;
inline constexpr UChar FIRST_STRONG_ISOLATE = 0x2068;
inline constexpr UChar POP_DIRECTIONAL_ISOLATE = 0x2069;
inline constexpr usize LOCALE_COUNT = sizeof(corpus::LOCALES) / sizeof(corpus::LOCALES[0]);
inline constexpr usize MESSAGE_COUNT = sizeof(corpus::MESSAGES) / sizeof(corpus::MESSAGES[0]);
inline constexpr usize CASE_COUNT = sizeof(corpus::CASES) / sizeof(corpus::CASES[0]);

// ---------------------------------------------------------------------------
// Counting, fail-point allocator installed through u_setMemoryFunctions.
// ---------------------------------------------------------------------------

struct Allocation
{
    usize Live = 0;
    usize LiveBytes = 0;
    usize PeakBytes = 0;
    usize Attempts = 0;
    usize FailAt = 0;
    usize Denied = 0;
};

struct alignas(16) BlockHeader
{
    usize Bytes = 0;
};

Allocation& GetAllocation() noexcept
{
    static Allocation allocation;
    return allocation;
}

bool ShouldDeny(usize bytes) noexcept
{
    Allocation& allocation = GetAllocation();
    ++allocation.Attempts;
    // Deny exactly one attempt: later allocations succeed, so error propagation
    // is observed even when a subsequent step could mask the first failure.
    if ((allocation.FailAt != 0 && allocation.Attempts == allocation.FailAt) || bytes > MAX_ALLOCATION_BYTES)
    {
        ++allocation.Denied;
        return true;
    }
    return false;
}

void Track(usize oldBytes, usize newBytes, bool isNew) noexcept
{
    Allocation& allocation = GetAllocation();
    if (isNew == true)
    {
        ++allocation.Live;
    }
    allocation.LiveBytes = allocation.LiveBytes - oldBytes + newBytes;
    if (allocation.LiveBytes > allocation.PeakBytes)
    {
        allocation.PeakBytes = allocation.LiveBytes;
    }
}

void* U_CALLCONV Allocate(const void* /*context*/, usize bytes) noexcept
{
    if (ShouldDeny(bytes) == true)
    {
        return nullptr;
    }
    BlockHeader* header = static_cast<BlockHeader*>(std::malloc(sizeof(BlockHeader) + bytes));
    if (header == nullptr)
    {
        return nullptr;
    }
    header->Bytes = bytes;
    Track(0, bytes, true);
    return header + 1;
}

void U_CALLCONV Free(const void* /*context*/, void* blockOrNull) noexcept
{
    if (blockOrNull == nullptr)
    {
        return;
    }
    BlockHeader* header = static_cast<BlockHeader*>(blockOrNull) - 1;
    Allocation& allocation = GetAllocation();
    --allocation.Live;
    allocation.LiveBytes -= header->Bytes;
    std::free(header);
}

// ICU fixes the parameter order in UMemReallocFn; it cannot use distinct wrappers.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void* U_CALLCONV Reallocate(const void* context, void* blockOrNull, usize bytes) noexcept
{
    if (blockOrNull == nullptr)
    {
        return Allocate(context, bytes);
    }
    if (ShouldDeny(bytes) == true)
    {
        return nullptr; // A rejected reallocation leaves the original block intact.
    }
    BlockHeader* header = static_cast<BlockHeader*>(blockOrNull) - 1;
    const usize oldBytes = header->Bytes;
    BlockHeader* resized = static_cast<BlockHeader*>(std::realloc(header, sizeof(BlockHeader) + bytes));
    if (resized == nullptr)
    {
        return nullptr;
    }
    resized->Bytes = bytes;
    Track(oldBytes, bytes, false);
    return resized + 1;
}

// ---------------------------------------------------------------------------
// Host-owned ICU lifetime: memory hooks and embedded data, then u_cleanup.
// ---------------------------------------------------------------------------

bool InitializeIcu() noexcept
{
    UErrorCode status = U_ZERO_ERROR;
    u_setMemoryFunctions(nullptr, Allocate, Reallocate, Free, &status);
    udata_setCommonData(ICU_DATA, &status);
    // Warm ICU's static locale cache. Format() reads Locale::getRoot(), which
    // returns a reference through a null cache if this first allocation fails
    // (locid.cpp getLocale), and the failed init-once persists until u_cleanup.
    // The host must succeed here before any formatter exists; ICU offers no
    // defined way to detect that failure afterwards.
    static_cast<void>(icu::Locale::getRoot());
    // UnifiedCache has the same hazard: a failed construction is destroyed
    // through a null hashtable (unifiedcache.cpp cacheInit). Plural rules are
    // the cheapest public path that creates it.
    const icu::LocalPointer<icu::PluralRules> rules(icu::PluralRules::forLocale(icu::Locale("und"), status));
    // Number-skeleton tables are another lazily built process singleton.
    static_cast<void>(icu::number::NumberFormatter::forSkeleton(icu::UnicodeString(u"precision-integer"), status));
    return U_SUCCESS(status);
}

/// Releases every ICU allocation; reports the blocks still live afterwards.
usize CleanupIcu() noexcept
{
    u_cleanup();
    return GetAllocation().Live;
}

bool Check(bool isPassed, std::string_view name) noexcept
{
    if (isPassed == true)
    {
        LUDUS_LOG_INFO(LOG_L0, "L0 PASS {}", name);
    }
    else
    {
        LUDUS_LOG_ERROR(LOG_L0, "L0 FAIL {}", name);
    }
    return isPassed;
}

// ---------------------------------------------------------------------------
// Typed argument adapter: validate the full schema before ICU sees a value.
// ---------------------------------------------------------------------------

enum class FormatStatus : uint8
{
    Ok,
    InvalidArguments,
    InvalidUtf8,
    BufferTooSmall,
    OutOfMemory,
    BackendFailure,
};

std::string_view GetStatusName(FormatStatus status) noexcept
{
    switch (status)
    {
        case FormatStatus::Ok:
            return "Ok";
        case FormatStatus::InvalidArguments:
            return "InvalidArguments";
        case FormatStatus::InvalidUtf8:
            return "InvalidUtf8";
        case FormatStatus::BufferTooSmall:
            return "BufferTooSmall";
        case FormatStatus::OutOfMemory:
            return "OutOfMemory";
        case FormatStatus::BackendFailure:
            return "BackendFailure";
    }
    return "Unknown";
}

bool IsForbiddenUserCodePoint(UChar32 codePoint) noexcept
{
    const bool isControl = codePoint < 0x20 || (codePoint >= 0x7f && codePoint <= 0x9f);
    const bool isEmbeddingOrOverride = codePoint >= 0x202a && codePoint <= 0x202e;
    const bool isIsolate = codePoint >= 0x2066 && codePoint <= 0x2069;
    return isControl || isEmbeddingOrOverride || isIsolate;
}

bool IsValidText(const ArgumentSchema& schema, Bytes text) noexcept
{
    if (text.Size > schema.MaxBytes || IsUtf8(text) == false)
    {
        return false;
    }
    for (usize i = 0; i < text.Size;)
    {
        UChar32 codePoint = 0;
        U8_NEXT_UNSAFE(text.Data, i, codePoint);
        if (codePoint == 0 || (schema.Origin == corpus::TextOrigin::User && IsForbiddenUserCodePoint(codePoint)))
        {
            return false;
        }
    }
    return true;
}

bool IsValidArgument(const ArgumentSchema& schema, const ArgumentValue& value) noexcept
{
    switch (schema.Type)
    {
        case ArgumentType::Text:
            return value.Kind == ValueKind::Text && IsValidText(schema, value.Text);
        case ArgumentType::Count:
            return value.Kind == ValueKind::Integer && value.Integer >= schema.Minimum &&
                   value.Integer <= schema.Maximum;
        case ArgumentType::Number: {
            if (value.Kind == ValueKind::Integer)
            {
                return value.Integer >= schema.Minimum && value.Integer <= schema.Maximum;
            }
            const float64 number = value.Number;
            // Comparisons with NaN are false, so a nonfinite value fails the range.
            return value.Kind == ValueKind::Number && number >= static_cast<float64>(schema.Minimum) &&
                   number <= static_cast<float64>(schema.Maximum);
        }
        case ArgumentType::Enum: {
            if (value.Kind != ValueKind::Text)
            {
                return false;
            }
            for (usize i = 0; i < schema.ValueCount; ++i)
            {
                if (schema.ValuesOrNull[i].GetView() == value.Text.GetView())
                {
                    return true;
                }
            }
            return schema.Unknown == corpus::UnknownPolicy::Other && IsUtf8(value.Text) == true;
        }
    }
    return false;
}

bool IsDeclaredValue(const ArgumentSchema& schema, Bytes text) noexcept
{
    for (usize i = 0; i < schema.ValueCount; ++i)
    {
        if (schema.ValuesOrNull[i].GetView() == text.GetView())
        {
            return true;
        }
    }
    return false;
}

/// Copies a string into a Formattable; ICU's own setString cannot report OOM.
bool AdoptString(const icu::UnicodeString& text, icu::Formattable& outValue) noexcept
{
    // UnicodeString derives from UMemory: this new expression uses ICU's
    // non-throwing allocator and yields nullptr on failure.
    icu::UnicodeString* copyOrNull = new icu::UnicodeString(text); // NOLINT(cppcoreguidelines-owning-memory)
    if (copyOrNull == nullptr || copyOrNull->isBogus() == true || text.isBogus() == true)
    {
        delete copyOrNull; // NOLINT(cppcoreguidelines-owning-memory)
        return false;
    }
    outValue.adoptString(copyOrNull);
    return true;
}

struct FormatRequest
{
    const MessageSchema* Schema;
    Bytes LocaleTag;
    Bytes Pattern;
    const ArgumentValue* ValuesOrNull = nullptr;
    usize ValueCount = 0;
};

struct FormatResult
{
    FormatStatus Result = FormatStatus::Ok;
    usize Written = 0;
    usize RequiredBytes = 0;
    UErrorCode Backend = U_ZERO_ERROR;
};

FormatStatus ConvertStatus(UErrorCode status) noexcept
{
    return status == U_MEMORY_ALLOCATION_ERROR ? FormatStatus::OutOfMemory : FormatStatus::BackendFailure;
}

/// Formats one complete message; the caller's bytes change only on success.
FormatResult FormatMessage(const FormatRequest& request, char* output, usize capacity) noexcept
{
    FormatResult result;
    const MessageSchema& schema = *request.Schema;
    if (request.ValueCount != schema.ArgumentCount || schema.ArgumentCount > MAX_ARGUMENTS)
    {
        result.Result = FormatStatus::InvalidArguments;
        return result;
    }
    const ArgumentValue* ordered[MAX_ARGUMENTS] = {};
    for (usize i = 0; i < request.ValueCount; ++i)
    {
        const ArgumentValue& value = request.ValuesOrNull[i];
        bool isDeclared = false;
        for (usize j = 0; j < schema.ArgumentCount; ++j)
        {
            if (schema.ArgumentsOrNull[j].Name.GetView() == value.Name.GetView())
            {
                if (ordered[j] != nullptr || IsValidArgument(schema.ArgumentsOrNull[j], value) == false)
                {
                    result.Result = FormatStatus::InvalidArguments;
                    return result;
                }
                ordered[j] = &value;
                isDeclared = true;
            }
        }
        if (isDeclared == false)
        {
            result.Result = FormatStatus::InvalidArguments;
            return result;
        }
    }
    if (IsUtf8(request.Pattern) == false)
    {
        result.Result = FormatStatus::InvalidUtf8;
        return result;
    }
    icu::UnicodeString names[MAX_ARGUMENTS];
    icu::Formattable arguments[MAX_ARGUMENTS];
    for (usize i = 0; i < schema.ArgumentCount; ++i)
    {
        const ArgumentSchema& argument = schema.ArgumentsOrNull[i];
        const ArgumentValue& value = *ordered[i];
        bool isAdopted = true;
        names[i] = ToUnicode(argument.Name);
        switch (argument.Type)
        {
            case ArgumentType::Text: {
                // Isolate inserted text so its direction cannot reorder the sentence.
                icu::UnicodeString text(FIRST_STRONG_ISOLATE);
                text.append(ToUnicode(value.Text)).append(POP_DIRECTIONAL_ISOLATE);
                isAdopted = AdoptString(text, arguments[i]);
                break;
            }
            case ArgumentType::Count:
                arguments[i].setInt64(value.Integer);
                break;
            case ArgumentType::Number:
                arguments[i].setDouble(value.Kind == ValueKind::Integer ? static_cast<float64>(value.Integer)
                                                                        : value.Number);
                break;
            case ArgumentType::Enum:
                // An accepted unknown token selects the mandatory other branch.
                isAdopted = AdoptString(IsDeclaredValue(argument, value.Text) == true ? ToUnicode(value.Text)
                                                                                      : icu::UnicodeString(u"other"),
                                        arguments[i]);
                break;
        }
        if (isAdopted == false || names[i].isBogus() == true)
        {
            result.Result = FormatStatus::OutOfMemory;
            return result;
        }
    }
    UErrorCode status = U_ZERO_ERROR;
    icu::Locale locale("und");
    status = MakeLocale(request.LocaleTag, locale);
    if (U_FAILURE(status))
    {
        result.Backend = status;
        result.Result = ConvertStatus(status);
        return result;
    }
    UParseError parseError{};
    icu::MessageFormat format(ToUnicode(request.Pattern), locale, parseError, status);
    icu::UnicodeString formatted;
    format.format(names, arguments, static_cast<int32>(schema.ArgumentCount), formatted, status);
    if (U_FAILURE(status) || formatted.isBogus() == true)
    {
        result.Backend = status;
        result.Result = U_FAILURE(status) ? ConvertStatus(status) : FormatStatus::OutOfMemory;
        return result;
    }
    // Count first, then copy only a complete result; never truncate.
    icu::CheckedArrayByteSink counter(nullptr, 0);
    formatted.toUTF8(counter);
    result.RequiredBytes = static_cast<usize>(counter.NumberOfBytesAppended());
    if (result.RequiredBytes > MAX_OUTPUT_BYTES || result.RequiredBytes > capacity)
    {
        result.Result = FormatStatus::BufferTooSmall;
        return result;
    }
    icu::CheckedArrayByteSink sink(output, static_cast<int32>(capacity));
    formatted.toUTF8(sink);
    result.Written = result.RequiredBytes;
    return result;
}

FormatRequest MakeRequest(const corpus::Case& testCase) noexcept
{
    const corpus::Pattern& pattern = corpus::PATTERNS[testCase.Locale * MESSAGE_COUNT + testCase.Message];
    return
    {
        .Schema = &corpus::MESSAGES[testCase.Message],
        .LocaleTag = corpus::LOCALES[testCase.Locale],
        .Pattern = pattern.Text,
        .ValuesOrNull = testCase.ValuesOrNull,
        .ValueCount = testCase.ValueCount,
    };
}

char* GetScratch() noexcept
{
    static char scratch[MAX_OUTPUT_BYTES];
    return scratch;
}

bool IsExpected(const corpus::Case& testCase, const FormatResult& result) noexcept
{
    if (testCase.Expected == corpus::Outcome::InvalidArguments)
    {
        return result.Result == FormatStatus::InvalidArguments;
    }
    return result.Result == FormatStatus::Ok &&
           std::string_view(GetScratch(), result.Written) == testCase.Output.GetView();
}

// ---------------------------------------------------------------------------
// Checks.
// ---------------------------------------------------------------------------

usize FindCase(std::string_view locale, std::string_view key, usize occurrence) noexcept
{
    for (usize i = 0; i < CASE_COUNT; ++i)
    {
        const corpus::Case& testCase = corpus::CASES[i];
        if (corpus::LOCALES[testCase.Locale].GetView() == locale &&
            corpus::MESSAGES[testCase.Message].Key.GetView() == key &&
            testCase.Expected == corpus::Outcome::Formatted && occurrence-- == 0)
        {
            return i;
        }
    }
    return 0;
}

bool AreVersionsEqual(const UVersionInfo left, const UVersionInfo right) noexcept
{
    for (usize i = 0; i < U_MAX_VERSION_LENGTH; ++i)
    {
        if (left[i] != right[i])
        {
            return false;
        }
    }
    return true;
}

bool CheckPinnedData() noexcept
{
    UVersionInfo actual{};
    UVersionInfo pinned{};
    u_getVersion(actual);
    u_versionFromString(pinned, corpus::PINNED.Icu.Data);
    bool isPassed = AreVersionsEqual(actual, pinned);
    u_getUnicodeVersion(actual);
    u_versionFromString(pinned, corpus::PINNED.Unicode.Data);
    isPassed = isPassed && AreVersionsEqual(actual, pinned);
    UErrorCode status = U_ZERO_ERROR;
    icu::LocalUResourceBundlePointer version(ures_openDirect(nullptr, "icuver", &status));
    int32 length = 0;
    const UChar* cldr = ures_getStringByKey(version.getAlias(), "CLDRVersion", &length, &status);
    isPassed = isPassed && U_SUCCESS(status) && icu::UnicodeString(cldr, length) == ToUnicode(corpus::PINNED.Cldr);
    LUDUS_LOG_INFO(LOG_L0,
                   "L0 METRIC pinned-data icu={} cldr={} unicode={} data-bytes={}",
                   corpus::PINNED.Icu.GetView(),
                   corpus::PINNED.Cldr.GetView(),
                   corpus::PINNED.Unicode.GetView(),
                   ICU_DATA_BYTES);
    return Check(isPassed, "pinned-icu-cldr-unicode-versions");
}

struct LocaleRules
{
    icu::LocalPointer<icu::PluralRules> Cardinal;
    icu::LocalPointer<icu::PluralRules> Ordinal;
    // Explicit IDs only: the default and static locale getters are not OOM-safe.
    icu::Locale Locale{"und"};
};

bool PrepareLocales(LocaleRules (&outRules)[LOCALE_COUNT]) noexcept
{
    bool isPassed = true;
    for (usize i = 0; i < LOCALE_COUNT; ++i)
    {
        const Bytes tag = corpus::LOCALES[i];
        UErrorCode status = U_ZERO_ERROR;
        bool isLocalePassed = U_SUCCESS(MakeLocale(tag, outRules[i].Locale));
        char canonical[MAX_LOCALE_TAG_BYTES] = {};
        icu::CheckedArrayByteSink sink(canonical, static_cast<int32>(sizeof(canonical)));
        outRules[i].Locale.toLanguageTag(sink, status);
        // Labels are already canonical BCP 47: the pinned service must round-trip them.
        isLocalePassed = isLocalePassed && U_SUCCESS(status) && sink.Overflowed() == false &&
                         std::string_view(canonical, static_cast<usize>(sink.NumberOfBytesAppended())) == tag.GetView();
        // Formatting symbols must come from the requested language, not root fallback.
        icu::DecimalFormatSymbols symbols(outRules[i].Locale, status);
        const icu::Locale actual = symbols.getLocale(ULOC_ACTUAL_LOCALE, status);
        isLocalePassed = isLocalePassed && U_SUCCESS(status) &&
                         std::string_view(actual.getLanguage()) == std::string_view(outRules[i].Locale.getLanguage());
        outRules[i].Cardinal.adoptInstead(
            icu::PluralRules::forLocale(outRules[i].Locale, UPLURAL_TYPE_CARDINAL, status));
        outRules[i].Ordinal.adoptInstead(icu::PluralRules::forLocale(outRules[i].Locale, UPLURAL_TYPE_ORDINAL, status));
        isLocalePassed = isLocalePassed && U_SUCCESS(status) && outRules[i].Cardinal.isValid() == true &&
                         outRules[i].Ordinal.isValid() == true;
        if (isLocalePassed == false)
        {
            LUDUS_LOG_ERROR(LOG_L0, "L0 locale {} unavailable: {}", tag.GetView(), u_errorName(status));
        }
        isPassed = isPassed && isLocalePassed;
    }
    return Check(isPassed, "canonical-locales-with-sliced-data");
}

bool CheckProfile(const LocaleRules (&rules)[LOCALE_COUNT]) noexcept
{
    bool isPassed = true;
    for (const corpus::Pattern& pattern : corpus::PATTERNS)
    {
        const LocaleRules& locale = rules[pattern.Locale];
        const Rejection rejection =
            ValidatePattern(corpus::MESSAGES[pattern.Message], pattern.Text, *locale.Cardinal, *locale.Ordinal);
        if (rejection != Rejection::Accepted)
        {
            LUDUS_LOG_ERROR(LOG_L0,
                            "L0 corpus pattern rejected: {} {} reason={}",
                            corpus::LOCALES[pattern.Locale].GetView(),
                            corpus::MESSAGES[pattern.Message].Key.GetView(),
                            static_cast<uint32>(rejection));
            isPassed = false;
        }
    }
    isPassed = Check(isPassed, "profile-accepts-every-corpus-pattern");
    bool isRejected = true;
    for (const corpus::RejectionCase& rejection : corpus::REJECTIONS)
    {
        const LocaleRules& locale = rules[rejection.Locale];
        const Rejection actual =
            ValidatePattern(corpus::MESSAGES[rejection.Message], rejection.Pattern, *locale.Cardinal, *locale.Ordinal);
        if (actual != rejection.Reason)
        {
            LUDUS_LOG_ERROR(LOG_L0,
                            "L0 rejection mismatch: {} expected={} actual={}",
                            rejection.Pattern.GetView(),
                            static_cast<uint32>(rejection.Reason),
                            static_cast<uint32>(actual));
            isRejected = false;
        }
    }
    // Limits that are impractical to spell in a JSON fixture.
    static char large[MAX_PATTERN_BYTES + 1];
    for (char& byte : large)
    {
        byte = 'a';
    }
    const LocaleRules& english = rules[0];
    isRejected = isRejected &&
                 ValidatePattern(corpus::MESSAGES[0], {large, sizeof(large)}, *english.Cardinal, *english.Ordinal) ==
                     Rejection::PatternTooLarge;
    static char branches[8192];
    usize length = 0;
    const std::string_view head = "{count, plural, ";
    for (char byte : head)
    {
        branches[length++] = byte;
    }
    for (int32 i = 0; i <= MAX_SELECTOR_BRANCHES; ++i)
    {
        branches[length++] = '=';
        const int32 hundreds = i / 100;
        const int32 tens = (i / 10) % 10;
        if (hundreds != 0)
        {
            branches[length++] = static_cast<char>('0' + hundreds);
        }
        if (hundreds != 0 || tens != 0)
        {
            branches[length++] = static_cast<char>('0' + tens);
        }
        branches[length++] = static_cast<char>('0' + i % 10);
        for (char byte : std::string_view(" {x} "))
        {
            branches[length++] = byte;
        }
    }
    for (char byte : std::string_view("other {y}}"))
    {
        branches[length++] = byte;
    }
    usize inventory = 0;
    while (corpus::MESSAGES[inventory].Key.GetView() != "inventory/item-count")
    {
        ++inventory;
    }
    isRejected =
        isRejected &&
        ValidatePattern(corpus::MESSAGES[inventory], {branches, length}, *english.Cardinal, *english.Ordinal) ==
            Rejection::TooManyBranches;
    static constexpr char INVALID_UTF8[] = "Play \xff";
    isRejected = isRejected && ValidatePattern(corpus::MESSAGES[0],
                                               {INVALID_UTF8, sizeof(INVALID_UTF8) - 1},
                                               *english.Cardinal,
                                               *english.Ordinal) == Rejection::SyntaxError;
    return Check(isRejected, "profile-rejects-every-negative-fixture") && isPassed;
}

bool CheckCorpus() noexcept
{
    usize formatted = 0;
    usize invalid = 0;
    bool isPassed = true;
    for (const corpus::Case& testCase : corpus::CASES)
    {
        const FormatResult result = FormatMessage(MakeRequest(testCase), GetScratch(), MAX_OUTPUT_BYTES);
        if (IsExpected(testCase, result) == false)
        {
            LUDUS_LOG_ERROR(LOG_L0,
                            "L0 case mismatch: {} {} status={} backend={} actual=[{}] expected=[{}]",
                            corpus::LOCALES[testCase.Locale].GetView(),
                            corpus::MESSAGES[testCase.Message].Key.GetView(),
                            GetStatusName(result.Result),
                            u_errorName(result.Backend),
                            std::string_view(GetScratch(), result.Result == FormatStatus::Ok ? result.Written : 0),
                            testCase.Output.GetView());
            isPassed = false;
        }
        if (testCase.Expected == corpus::Outcome::Formatted)
        {
            ++formatted;
        }
        else
        {
            ++invalid;
        }
    }
    LUDUS_LOG_INFO(LOG_L0, "L0 METRIC corpus formatted={} rejected-arguments={}", formatted, invalid);
    return Check(isPassed, "corpus-format-and-argument-validation");
}

bool CheckOutputContract() noexcept
{
    const corpus::Case& testCase = corpus::CASES[FindCase("en-US", "inventory/item-count", 0)];
    char small[8] = {'s', 'e', 'n', 't', 'i', 'n', 'e', 'l'};
    const FormatResult tooSmall = FormatMessage(MakeRequest(testCase), small, sizeof(small));
    const bool isPreserved = std::string_view(small, sizeof(small)) == "sentinel";
    const bool isPassed = tooSmall.Result == FormatStatus::BufferTooSmall &&
                          tooSmall.RequiredBytes == testCase.Output.Size && isPreserved;
    static constexpr char INVALID_UTF8[] = "a\xff";
    const corpus::ArgumentValue invalid[] = {
        {{"player", 6}, ValueKind::Text, 0, 0.0, {INVALID_UTF8, 2}},
        {{"message", 7}, ValueKind::Text, 0, 0.0, {"hi", 2}},
    };
    usize chat = 0;
    while (corpus::MESSAGES[chat].Key.GetView() != "chat/player-said")
    {
        ++chat;
    }
    const FormatRequest request
    {
        .Schema = &corpus::MESSAGES[chat],
        .LocaleTag = corpus::LOCALES[0],
        .Pattern = corpus::PATTERNS[chat].Text,
        .ValuesOrNull = invalid,
        .ValueCount = 2,
    };
    const FormatResult rejected = FormatMessage(request, small, sizeof(small));
    const corpus::ArgumentValue duplicate[] = {
        {{"player", 6}, ValueKind::Text, 0, 0.0, {"a", 1}},
        {{"player", 6}, ValueKind::Text, 0, 0.0, {"b", 1}},
    };
    FormatRequest duplicated = request;
    duplicated.ValuesOrNull = duplicate;
    const FormatResult duplicateResult = FormatMessage(duplicated, small, sizeof(small));
    usize distance = 0;
    while (corpus::MESSAGES[distance].Key.GetView() != "hud/distance")
    {
        ++distance;
    }
    const corpus::ArgumentValue notANumber[] = {
        {{"distance", 8}, ValueKind::Number, 0, std::numeric_limits<float64>::quiet_NaN(), {}},
    };
    const corpus::ArgumentValue infinity[] = {
        {{"distance", 8}, ValueKind::Number, 0, std::numeric_limits<float64>::infinity(), {}},
    };
    FormatRequest nonfinite
    {
        .Schema = &corpus::MESSAGES[distance],
        .LocaleTag = corpus::LOCALES[0],
        .Pattern = corpus::PATTERNS[distance].Text,
        .ValuesOrNull = notANumber,
        .ValueCount = 1,
    };
    const FormatResult nanResult = FormatMessage(nonfinite, small, sizeof(small));
    nonfinite.ValuesOrNull = infinity;
    const FormatResult infinityResult = FormatMessage(nonfinite, small, sizeof(small));
    return Check(isPassed && rejected.Result == FormatStatus::InvalidArguments &&
                     duplicateResult.Result == FormatStatus::InvalidArguments &&
                     nanResult.Result == FormatStatus::InvalidArguments &&
                     infinityResult.Result == FormatStatus::InvalidArguments &&
                     std::string_view(small, sizeof(small)) == "sentinel",
                 "complete-output-or-unchanged-caller-bytes");
}

/// Records raw backend behavior that the adapter must not rely on.
bool ObserveBackend() noexcept
{
    UErrorCode status = U_ZERO_ERROR;
    UParseError parseError{};
    const icu::Locale english("en_US");
    icu::MessageFormat missing(icu::UnicodeString(u"{a} and {b}"), english, parseError, status);
    const icu::UnicodeString names[] = {icu::UnicodeString(u"a")};
    const icu::Formattable values[] = {icu::Formattable(icu::UnicodeString(u"x"))};
    icu::UnicodeString output;
    missing.format(names, values, 1, output, status);
    const bool isMissingLiteral = U_SUCCESS(status) && output == icu::UnicodeString(u"x and {b}");
    LUDUS_LOG_INFO(LOG_L0,
                   "L0 OBSERVED icu-missing-named-argument status={} literal={}",
                   u_errorName(status),
                   isMissingLiteral);

    status = U_ZERO_ERROR;
    icu::MessageFormat plural(icu::UnicodeString(u"{n, plural, other {#}}"), english, parseError, status);
    const icu::UnicodeString pluralNames[] = {icu::UnicodeString(u"n")};
    const icu::Formattable text[] = {icu::Formattable(icu::UnicodeString(u"3"))};
    output.remove();
    plural.format(pluralNames, text, 1, output, status);
    const UErrorCode textAsCount = status;
    LUDUS_LOG_INFO(LOG_L0, "L0 OBSERVED icu-text-for-plural status={}", u_errorName(textAsCount));

    status = U_ZERO_ERROR;
    icu::MessageFormat number(icu::UnicodeString(u"{n, number}"), english, parseError, status);
    const icu::Formattable nan[] = {icu::Formattable(std::numeric_limits<float64>::quiet_NaN())};
    output.remove();
    number.format(pluralNames, nan, 1, output, status);
    const bool isNanFormatted = U_SUCCESS(status) && output == icu::UnicodeString(u"NaN");
    LUDUS_LOG_INFO(LOG_L0,
                   "L0 OBSERVED icu-nan-number status={} formatted-nan={}",
                   u_errorName(status),
                   isNanFormatted);

    static constexpr char INVALID_UTF8[] = "a\xff"
                                           "b";
    const icu::UnicodeString replaced = icu::UnicodeString::fromUTF8(icu::StringPiece(INVALID_UTF8, 3));
    const bool isReplaced = replaced.isBogus() == false && replaced == icu::UnicodeString(u"a�b");
    LUDUS_LOG_INFO(LOG_L0, "L0 OBSERVED icu-fromutf8-invalid replacement={}", isReplaced);

    const bool isDoubleOptional = missing.getApostropheMode() == UMSGPAT_APOS_DOUBLE_OPTIONAL;
    // Each observation is why the adapter validates first; a change in a future
    // backend must be reviewed rather than silently inherited.
    return Check(isMissingLiteral && U_FAILURE(textAsCount) && isNanFormatted && isReplaced && isDoubleOptional,
                 "backend-observations-require-adapter-validation");
}

struct SweepCase
{
    std::string_view Name;
    std::string_view Locale;
    std::string_view Key;
    usize Occurrence = 0;
};

// Representative formats for the fail-point sweep: nested select/plural, a
// number skeleton with grouping, and an offset plural with inserted text.
inline constexpr SweepCase SWEEP_CASES[] = {
    {"nested-select-plural", "ar", "loot/picked-up", 0},
    {"number-skeleton", "fr-FR", "hud/distance", 1},
    {"offset-plural-text", "ru-RU", "party/online", 0},
};

/// Counts allocation attempts of host initialization and each sweep case.
bool MeasureAllocationPoints() noexcept
{
    Allocation& allocation = GetAllocation();
    bool isPassed = CleanupIcu() == 0;
    allocation.Attempts = 0;
    isPassed = InitializeIcu() && isPassed;
    LUDUS_LOG_INFO(LOG_L0, "L0 METRIC sweep-points-host-initialization allocations={}", allocation.Attempts);
    for (const SweepCase& sweep : SWEEP_CASES)
    {
        const corpus::Case& testCase = corpus::CASES[FindCase(sweep.Locale, sweep.Key, sweep.Occurrence)];
        isPassed = CleanupIcu() == 0 && InitializeIcu() && isPassed;
        allocation.Attempts = 0;
        const FormatResult result = FormatMessage(MakeRequest(testCase), GetScratch(), MAX_OUTPUT_BYTES);
        isPassed = IsExpected(testCase, result) && isPassed;
        LUDUS_LOG_INFO(LOG_L0, "L0 METRIC sweep-points-{} allocations={}", sweep.Name, allocation.Attempts);
    }
    return Check(isPassed, "sweep-allocation-points");
}

/// Runs one isolated fail point. ICU may crash on allocation failure, so the
/// verifier starts a fresh process for every point and classifies the outcome.
/// Case index SWEEP_CASE_COUNT denies an allocation during host initialization.
int32 RunSweepPoint(usize caseIndex, usize failAt) noexcept
{
    constexpr usize SWEEP_CASE_COUNT = sizeof(SWEEP_CASES) / sizeof(SWEEP_CASES[0]);
    if (caseIndex > SWEEP_CASE_COUNT || failAt == 0)
    {
        return 2;
    }
    const bool isInitialization = caseIndex == SWEEP_CASE_COUNT;
    const SweepCase& sweep = SWEEP_CASES[isInitialization == true ? 0 : caseIndex];
    const corpus::Case& testCase = corpus::CASES[FindCase(sweep.Locale, sweep.Key, sweep.Occurrence)];
    Allocation& allocation = GetAllocation();
    allocation.FailAt = isInitialization == true ? failAt : 0;
    const bool isInitialized = InitializeIcu();
    allocation.FailAt = isInitialization == true ? 0 : failAt;
    allocation.Attempts = 0;
    FormatResult result;
    if (isInitialized == true)
    {
        result = FormatMessage(MakeRequest(testCase), GetScratch(), MAX_OUTPUT_BYTES);
    }
    allocation.FailAt = 0;
    const bool isCorrect = isInitialized == true && IsExpected(testCase, result);
    // Same process, no failure injected: does the earlier failure persist?
    const FormatResult recovered = FormatMessage(MakeRequest(testCase), GetScratch(), MAX_OUTPUT_BYTES);
    const bool isRecovered = IsExpected(testCase, recovered);
    const usize leaked = CleanupIcu();
    LUDUS_LOG_INFO(LOG_L0,
                   "L0 SWEEP case={} fail-at={} denied={} initialized={} status={} backend={} correct={} "
                   "recovery={} recovery-backend={} recovered={} leaked-blocks={}",
                   isInitialization == true ? std::string_view("host-initialization") : sweep.Name,
                   failAt,
                   allocation.Denied,
                   isInitialized,
                   GetStatusName(result.Result),
                   u_errorName(result.Backend),
                   isCorrect,
                   GetStatusName(recovered.Result),
                   u_errorName(recovered.Backend),
                   isRecovered,
                   leaked);
    return 0;
}

bool ParseCount(const char* text, usize& outValue) noexcept
{
    usize value = 0;
    usize digits = 0;
    for (; text[digits] != '\0'; ++digits)
    {
        const char digit = text[digits];
        if (digit < '0' || digit > '9' || digits >= 9)
        {
            return false;
        }
        value = value * 10 + static_cast<usize>(digit - '0');
    }
    outValue = value;
    return digits != 0;
}

uint64 GetElapsed(time::Timestamp start) noexcept
{
    time::Duration elapsed;
    return time::TryElapsed(start, time::Now(), elapsed) == time::TimeStatus::Ok ? elapsed.Nanoseconds : 0;
}

bool Measure() noexcept
{
    const corpus::Case& testCase = corpus::CASES[FindCase("ar", "loot/picked-up", 0)];
    const FormatRequest request = MakeRequest(testCase);
    Allocation& allocation = GetAllocation();

    // Cold: fresh ICU caches, first construction and format of a nested message.
    bool isPassed = CleanupIcu() == 0 && InitializeIcu() == true;
    allocation.Attempts = 0;
    time::Timestamp start = time::Now();
    const FormatResult cold = FormatMessage(request, GetScratch(), MAX_OUTPUT_BYTES);
    const uint64 coldNanoseconds = GetElapsed(start);
    const usize coldAllocations = allocation.Attempts;
    isPassed = isPassed && IsExpected(testCase, cold);

    // Warm construction: caches populated, pattern parsed each time.
    icu::Locale locale("und");
    isPassed = isPassed && U_SUCCESS(MakeLocale(request.LocaleTag, locale));
    const icu::UnicodeString pattern = ToUnicode(request.Pattern);
    allocation.Attempts = 0;
    start = time::Now();
    for (usize i = 0; i < WARM_CONSTRUCTIONS; ++i)
    {
        UErrorCode status = U_ZERO_ERROR;
        UParseError parseError{};
        const icu::MessageFormat format(pattern, locale, parseError, status);
        isPassed = isPassed && U_SUCCESS(status);
    }
    const uint64 constructNanoseconds = GetElapsed(start) / WARM_CONSTRUCTIONS;
    const usize constructAllocations = allocation.Attempts / WARM_CONSTRUCTIONS;

    // Warm format: one prepared formatter reused for a changing count.
    UErrorCode status = U_ZERO_ERROR;
    UParseError parseError{};
    const icu::MessageFormat format(pattern, locale, parseError, status);
    const icu::UnicodeString names[] = {icu::UnicodeString(u"picker_gender"), icu::UnicodeString(u"count")};
    icu::Formattable values[] = {icu::Formattable(icu::UnicodeString(u"masculine")), icu::Formattable(int64{0})};
    icu::UnicodeString output;
    usize totalBytes = 0;
    allocation.Attempts = 0;
    start = time::Now();
    for (usize i = 0; i < WARM_FORMATS; ++i)
    {
        values[1].setInt64(static_cast<int64>(i % 100 + 1));
        output.remove();
        format.format(names, values, 2, output, status);
        icu::CheckedArrayByteSink sink(GetScratch(), static_cast<int32>(MAX_OUTPUT_BYTES));
        output.toUTF8(sink);
        totalBytes += static_cast<usize>(sink.NumberOfBytesAppended());
    }
    const uint64 formatNanoseconds = GetElapsed(start) / WARM_FORMATS;
    const usize formatAllocations = allocation.Attempts / WARM_FORMATS;
    isPassed = isPassed && U_SUCCESS(status) && totalBytes > 0;
    LUDUS_LOG_INFO(LOG_L0,
                   "L0 METRIC dynamic-format cold-ns={} cold-allocations={} construct-ns={} construct-allocations={} "
                   "format-ns={} format-allocations={} peak-live-bytes={}",
                   coldNanoseconds,
                   coldAllocations,
                   constructNanoseconds,
                   constructAllocations,
                   formatNanoseconds,
                   formatAllocations,
                   allocation.PeakBytes);
    return Check(isPassed, "dynamic-format-measurements");
}

bool Run() noexcept
{
    bool isPassed = Check(InitializeIcu(), "embedded-data-registration");
    isPassed = CheckPinnedData() && isPassed;
    {
        LocaleRules rules[LOCALE_COUNT];
        isPassed = PrepareLocales(rules) && isPassed;
        isPassed = isPassed && CheckProfile(rules);
    }
    isPassed = CheckCorpus() && isPassed;
    isPassed = CheckOutputContract() && isPassed;
    isPassed = ObserveBackend() && isPassed;
    isPassed = MeasureAllocationPoints() && isPassed;
    isPassed = Measure() && isPassed;
    return Check(CleanupIcu() == 0, "host-cleanup-releases-every-allocation") && isPassed;
}
} // namespace ludus::icu_probe

int main(int argumentCount, char** arguments)
{
    using namespace ludus::foundation;
    using namespace ludus::foundation::logging;
    LogConfig config;
    config.EnableFile = false;
    config.EnableDebugger = false;
    const LogInitResult initialized = LogSystem::Initialize(config);
    if (initialized.Status != LogStatus::Ok)
    {
        return 2;
    }
    int32 result = 2;
    usize caseIndex = 0;
    usize failAt = 0;
    if (argumentCount == 1)
    {
        result = ludus::icu_probe::Run() == true ? 0 : 1;
    }
    else if (argumentCount == 4 && std::string_view(arguments[1]) == "sweep" &&
             ludus::icu_probe::ParseCount(arguments[2], caseIndex) == true &&
             ludus::icu_probe::ParseCount(arguments[3], failAt) == true)
    {
        result = ludus::icu_probe::RunSweepPoint(caseIndex, failAt);
    }
    LogSystem::Shutdown();
    return result;
}
