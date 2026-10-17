#pragma once

// The ludus-icu-mf1-v1 profile walk shared by the private L0 probe and the L1
// host profile checker, so the cooker and the probe validate patterns with one
// implementation over ICU's own MessagePattern parts. Private; never installed.
//
// Thanks to Unicode, Inc. and the ICU project for ICU4C 78.3, particularly the
// MessagePattern and MessageFormat API references and the ICU User Guide,
// "Formatting Messages":
// https://unicode-org.github.io/icu/userguide/format_parse/messages/
// The literal decoder follows the part model MessageFormat::format documents:
// text between parts is literal and SKIP_SYNTAX/INSERT_CHAR parts are not
// output. The profile walk and decoder are original Ludus code; see
// docs/architecture/localization-l0.md.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/strings/utf8.hpp>

#include <string_view>

#include <unicode/locid.h>
#include <unicode/messagepattern.h>
#include <unicode/plurrule.h>
#include <unicode/unistr.h>

#include "corpus_types.hpp"

namespace ludus::icu_probe
{
using namespace foundation;
using corpus::ArgumentSchema;
using corpus::ArgumentType;
using corpus::Bytes;
using corpus::MessageSchema;
using corpus::Rejection;

inline constexpr usize MAX_PATTERN_BYTES = usize{16} * 1024;
inline constexpr usize MAX_LOCALE_TAG_BYTES = 128;
inline constexpr int32 MAX_SELECTOR_NESTING = 8;
inline constexpr int32 MAX_SELECTOR_BRANCHES = 256;

inline icu::UnicodeString ToUnicode(Bytes bytes) noexcept
{
    return icu::UnicodeString::fromUTF8(icu::StringPiece(bytes.Data, static_cast<int32>(bytes.Size)));
}

inline bool IsUtf8(Bytes bytes) noexcept
{
    Utf8View view;
    usize errorOffset = 0;
    return ValidateUtf8(bytes.GetView(), view, errorOffset) == StringStatus::Ok;
}

/// Builds a locale from an explicit tag; failure preserves outLocale.
inline UErrorCode MakeLocale(Bytes tag, icu::Locale& outLocale) noexcept
{
    if (tag.Size > MAX_LOCALE_TAG_BYTES)
    {
        return U_ILLEGAL_ARGUMENT_ERROR;
    }
    UErrorCode status = U_ZERO_ERROR;
    icu::Locale locale = icu::Locale::forLanguageTag(icu::StringPiece(tag.Data, static_cast<int32>(tag.Size)), status);
    if (U_FAILURE(status))
    {
        return status;
    }
    if (locale.isBogus() == true)
    {
        return U_MEMORY_ALLOCATION_ERROR;
    }
    outLocale = locale;
    return outLocale.isBogus() == true ? U_MEMORY_ALLOCATION_ERROR : U_ZERO_ERROR;
}

// ---------------------------------------------------------------------------
// ludus-icu-mf1-v1 profile validation over ICU's own MessagePattern parts.
// ---------------------------------------------------------------------------

inline const ArgumentSchema* FindArgument(const MessageSchema& schema, const icu::UnicodeString& name) noexcept
{
    for (usize i = 0; i < schema.ArgumentCount; ++i)
    {
        if (ToUnicode(schema.ArgumentsOrNull[i].Name) == name)
        {
            return &schema.ArgumentsOrNull[i];
        }
    }
    return nullptr;
}

inline bool IsApprovedNumberStyle(const icu::UnicodeString& style) noexcept
{
    static constexpr std::string_view APPROVED[] = {"integer", "::.0", "::.00"};
    for (std::string_view approved : APPROVED)
    {
        if (style == ToUnicode({approved.data(), approved.size()}))
        {
            return true;
        }
    }
    return false;
}

inline bool IsDeclaredEnumValue(const ArgumentSchema& argument, const icu::UnicodeString& keyword) noexcept
{
    for (usize i = 0; i < argument.ValueCount; ++i)
    {
        if (ToUnicode(argument.ValuesOrNull[i]) == keyword)
        {
            return true;
        }
    }
    return false;
}

inline bool IsIntegral(float64 value) noexcept
{
    constexpr float64 EXACT_INTEGER_LIMIT = 9007199254740992.0; // 2^53
    return value > -EXACT_INTEGER_LIMIT && value < EXACT_INTEGER_LIMIT &&
           static_cast<float64>(static_cast<int64>(value)) == value;
}

struct OpenArgument
{
    const ArgumentSchema* SchemaOrNull = nullptr;
    UMessagePatternArgType Type = UMSGPAT_ARG_TYPE_NONE;
};

inline Rejection CheckSelector(const icu::MessagePattern& pattern,
                               int32 index,
                               const OpenArgument& owner,
                               const icu::PluralRules& cardinal,
                               const icu::PluralRules& ordinal) noexcept
{
    const icu::UnicodeString keyword = pattern.getSubstring(pattern.getPart(index));
    if (owner.Type == UMSGPAT_ARG_TYPE_SELECT)
    {
        if (keyword == icu::UnicodeString(u"other") || IsDeclaredEnumValue(*owner.SchemaOrNull, keyword) == true)
        {
            return Rejection::Accepted;
        }
        return Rejection::UndeclaredEnumValue;
    }
    if (keyword.startsWith(icu::UnicodeString(u"=")) == true)
    {
        const icu::MessagePattern::Part& value = pattern.getPart(index + 1);
        const float64 exact = pattern.getNumericValue(value);
        if (IsIntegral(exact) == false)
        {
            return value.getType() == UMSGPAT_PART_TYPE_ARG_DOUBLE &&
                           (exact > static_cast<float64>(owner.SchemaOrNull->Maximum) ||
                            exact < static_cast<float64>(owner.SchemaOrNull->Minimum))
                       ? Rejection::ExactValueOutOfRange
                       : Rejection::NonIntegerExactValue;
        }
        const int64 integer = static_cast<int64>(exact);
        return integer < owner.SchemaOrNull->Minimum || integer > owner.SchemaOrNull->Maximum
                   ? Rejection::ExactValueOutOfRange
                   : Rejection::Accepted;
    }
    const icu::PluralRules& rules = owner.Type == UMSGPAT_ARG_TYPE_SELECTORDINAL ? ordinal : cardinal;
    // A keyword outside this locale's categories can never be selected.
    return rules.isKeyword(keyword) == true ? Rejection::Accepted : Rejection::DeadPluralKeyword;
}

inline Rejection CheckArgumentStart(const icu::MessagePattern& pattern,
                                    int32 index,
                                    const MessageSchema& schema,
                                    OpenArgument& outArgument) noexcept
{
    const UMessagePatternArgType type = pattern.getPart(index).getArgType();
    const icu::MessagePattern::Part& identity = pattern.getPart(index + 1);
    if (identity.getType() == UMSGPAT_PART_TYPE_ARG_NUMBER)
    {
        return Rejection::NumberedArgument;
    }
    const ArgumentSchema* argumentOrNull = FindArgument(schema, pattern.getSubstring(identity));
    if (argumentOrNull == nullptr)
    {
        return Rejection::UndeclaredArgument;
    }
    const ArgumentSchema& argument = *argumentOrNull;
    outArgument = {argumentOrNull, type};
    switch (type)
    {
        case UMSGPAT_ARG_TYPE_NONE:
            return argument.Type == ArgumentType::Text ? Rejection::Accepted : Rejection::ArgumentTypeMismatch;
        case UMSGPAT_ARG_TYPE_SIMPLE: {
            if (pattern.getSubstring(pattern.getPart(index + 2)) != icu::UnicodeString(u"number"))
            {
                return Rejection::UnsupportedArgumentType;
            }
            if (argument.Type != ArgumentType::Count && argument.Type != ArgumentType::Number)
            {
                return Rejection::ArgumentTypeMismatch;
            }
            const icu::MessagePattern::Part& style = pattern.getPart(index + 3);
            if (style.getType() == UMSGPAT_PART_TYPE_ARG_STYLE &&
                IsApprovedNumberStyle(pattern.getSubstring(style).trim()) == false)
            {
                return Rejection::UnapprovedNumberStyle;
            }
            return Rejection::Accepted;
        }
        case UMSGPAT_ARG_TYPE_PLURAL:
        case UMSGPAT_ARG_TYPE_SELECTORDINAL: {
            if (argument.Type != ArgumentType::Count)
            {
                return Rejection::ArgumentTypeMismatch;
            }
            // The offset belongs to the first style part, after ARG_START and ARG_NAME.
            const float64 offset = pattern.getPluralOffset(index + 2);
            return IsIntegral(offset) == true && offset >= 0.0 ? Rejection::Accepted : Rejection::InvalidPluralOffset;
        }
        case UMSGPAT_ARG_TYPE_SELECT:
            return argument.Type == ArgumentType::Enum ? Rejection::Accepted : Rejection::ArgumentTypeMismatch;
        case UMSGPAT_ARG_TYPE_CHOICE:
        default:
            return Rejection::UnsupportedArgumentType;
    }
}

inline Rejection ValidatePattern(const MessageSchema& schema,
                                 Bytes text,
                                 const icu::PluralRules& cardinal,
                                 const icu::PluralRules& ordinal) noexcept
{
    if (text.Size > MAX_PATTERN_BYTES)
    {
        return Rejection::PatternTooLarge;
    }
    if (IsUtf8(text) == false)
    {
        return Rejection::SyntaxError;
    }
    UErrorCode status = U_ZERO_ERROR;
    icu::MessagePattern pattern(UMSGPAT_APOS_DOUBLE_OPTIONAL, status);
    UParseError parseError{};
    pattern.parse(ToUnicode(text), &parseError, status);
    if (U_FAILURE(status))
    {
        return Rejection::SyntaxError;
    }
    OpenArgument open[MAX_SELECTOR_NESTING * 2 + 2] = {};
    int32 depth = 0;
    int32 branches = 0;
    const int32 parts = pattern.countParts();
    for (int32 i = 0; i < parts; ++i)
    {
        const icu::MessagePattern::Part& part = pattern.getPart(i);
        switch (part.getType())
        {
            case UMSGPAT_PART_TYPE_MSG_START:
                if (part.getValue() > MAX_SELECTOR_NESTING)
                {
                    return Rejection::NestingTooDeep;
                }
                break;
            case UMSGPAT_PART_TYPE_ARG_START: {
                OpenArgument argument;
                const Rejection rejection = CheckArgumentStart(pattern, i, schema, argument);
                if (rejection != Rejection::Accepted)
                {
                    return rejection;
                }
                if (depth >= static_cast<int32>(sizeof(open) / sizeof(open[0])))
                {
                    return Rejection::NestingTooDeep;
                }
                open[depth] = argument;
                ++depth;
                break;
            }
            case UMSGPAT_PART_TYPE_ARG_LIMIT:
                --depth;
                break;
            case UMSGPAT_PART_TYPE_ARG_SELECTOR: {
                if (++branches > MAX_SELECTOR_BRANCHES)
                {
                    return Rejection::TooManyBranches;
                }
                const Rejection rejection = CheckSelector(pattern, i, open[depth - 1], cardinal, ordinal);
                if (rejection != Rejection::Accepted)
                {
                    return rejection;
                }
                break;
            }
            default:
                break;
        }
    }
    return Rejection::Accepted;
}

/// Decodes an accepted pattern of an argument-free message to its literal text.
///
/// Applies the DOUBLE_OPTIONAL apostrophe rules exactly as ICU parsed them, so
/// `'{'name'}'` yields `{name}` and `''` yields `'`. Fails, leaving outText
/// unchanged, for a pattern that is not literal at the top level or for an
/// ICU failure. The caller validated the pattern with ValidatePattern first.
inline bool DecodeLiteralPattern(Bytes text, icu::UnicodeString& outText) noexcept
{
    UErrorCode status = U_ZERO_ERROR;
    icu::MessagePattern pattern(UMSGPAT_APOS_DOUBLE_OPTIONAL, status);
    UParseError parseError{};
    const icu::UnicodeString source = ToUnicode(text);
    if (source.isBogus() == true)
    {
        return false;
    }
    pattern.parse(source, &parseError, status);
    if (U_FAILURE(status))
    {
        return false;
    }
    icu::UnicodeString decoded;
    const int32 parts = pattern.countParts();
    int32 previous = 0;
    for (int32 i = 0; i < parts; ++i)
    {
        const icu::MessagePattern::Part& part = pattern.getPart(i);
        const UMessagePatternPartType type = part.getType();
        const bool isLiteralPart = type == UMSGPAT_PART_TYPE_MSG_START || type == UMSGPAT_PART_TYPE_MSG_LIMIT ||
                                   type == UMSGPAT_PART_TYPE_SKIP_SYNTAX || type == UMSGPAT_PART_TYPE_INSERT_CHAR;
        if (isLiteralPart == false || (type == UMSGPAT_PART_TYPE_MSG_START && i != 0))
        {
            return false;
        }
        // Literal text lies between parts; quoting apostrophes are skipped
        // and an INSERT_CHAR part is only an auto-quoting hint, never output.
        decoded.append(source, previous, part.getIndex() - previous);
        previous = part.getLimit();
        if (type == UMSGPAT_PART_TYPE_MSG_LIMIT)
        {
            break;
        }
    }
    if (decoded.isBogus() == true)
    {
        return false;
    }
    outText = decoded;
    return outText.isBogus() == false;
}
} // namespace ludus::icu_probe
