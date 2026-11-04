#include "internal/code_language_registry.h"

#include <ludus/foundation/base/assert.hpp>

namespace ludus::editor
{
namespace
{
using foundation::BorrowedView;
using foundation::usize;

/// The length of a null-terminated ASCII string literal, excluding the
/// terminator. Used so the compiled-in table borrows exact spans rather than
/// the array constructor's count that would include the trailing byte.
[[nodiscard]] constexpr usize LiteralLength(const char* text) noexcept
{
    usize length = 0;
    while (text[length] != '\0')
    {
        length = length + 1;
    }
    return length;
}

/// Borrow a string literal without its terminator, for the Qt-free table.
[[nodiscard]] constexpr BorrowedView<const char> Literal(const char* text) noexcept
{
    return BorrowedView<const char>(text, LiteralLength(text));
}

/// Fold one ASCII code unit to lower case; non-ASCII bytes pass through.
[[nodiscard]] constexpr char ToLowerAscii(char value) noexcept
{
    constexpr char UPPER_A = 'A';
    constexpr char UPPER_Z = 'Z';
    constexpr char CASE_GAP = 'a' - 'A';
    if (value >= UPPER_A && value <= UPPER_Z)
    {
        return static_cast<char>(value + CASE_GAP);
    }
    return value;
}

/// Whether a file name ends with a suffix, comparing ASCII case-insensitively.
[[nodiscard]] bool EndsWithAscii(BorrowedView<const char> fileName, BorrowedView<const char> suffix) noexcept
{
    if (fileName.GetSize() < suffix.GetSize())
    {
        return false;
    }
    const usize offset = fileName.GetSize() - suffix.GetSize();
    for (usize index = 0; index < suffix.GetSize(); index = index + 1)
    {
        if (ToLowerAscii(fileName[offset + index]) != ToLowerAscii(suffix[index]))
        {
            return false;
        }
    }
    return true;
}

// Static storage for the Slang record's patterns and pairs. Plain text has no
// patterns, comment or pairs, so its views stay empty.
constexpr BorrowedView<const char> SLANG_SUFFIXES[] = {
    Literal(".slang"),
    Literal(".slangh"),
};

constexpr CodeLanguagePair SLANG_BRACKETS[] = {
    { .Open = '(', .Close = ')' },
    { .Open = '[', .Close = ']' },
    { .Open = '{', .Close = '}' },
};

// The table itself, in CodeLanguage enum order. Every field matches the current
// behavior: plain text has no coloring and no structural editing; Slang colors
// lexically, toggles "//" comments and pairs brackets.
const CodeLanguageDescriptor TABLE[] = {
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::PlainText,
        .Identifier = Literal("plaintext"),
        .DisplayName = Literal("Plain Text"),
        .SuffixPatterns = {},
        .LineComment = {},
        .BracketPairs = {},
        .HasLexicalColoring = false,
        .HasStructuralEditing = false,
    },
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::Slang,
        .Identifier = Literal("slang"),
        .DisplayName = Literal("Slang"),
        .SuffixPatterns = BorrowedView<const BorrowedView<const char>>(SLANG_SUFFIXES),
        .LineComment = Literal("//"),
        .BracketPairs = BorrowedView<const CodeLanguagePair>(SLANG_BRACKETS),
        .HasLexicalColoring = true,
        .HasStructuralEditing = true,
    },
};
} // namespace

CodeLanguageRegistry::CodeLanguageRegistry() noexcept : mDescriptors(BorrowedView<const CodeLanguageDescriptor>(TABLE))
{
}

const CodeLanguageRegistry& CodeLanguageRegistry::Get() noexcept
{
    static const CodeLanguageRegistry registry;
    return registry;
}

const CodeLanguageDescriptor& CodeLanguageRegistry::Describe(CodeLanguage language) const noexcept
{
    const usize index = static_cast<usize>(language);
    LUDUS_ASSERT(index < mDescriptors.GetSize());
    const CodeLanguageDescriptor& descriptor = mDescriptors[index];
    // The table is in enum order, so the index must identify its own record.
    LUDUS_ASSERT(descriptor.Language == language);
    return descriptor;
}

CodeLanguage CodeLanguageRegistry::ClassifyFileName(BorrowedView<const char> fileName) const noexcept
{
    CodeLanguage best = CodeLanguage::PlainText;
    usize bestLength = 0;
    for (usize record = 0; record < mDescriptors.GetSize(); record = record + 1)
    {
        const CodeLanguageDescriptor& descriptor = mDescriptors[record];
        for (usize pattern = 0; pattern < descriptor.SuffixPatterns.GetSize(); pattern = pattern + 1)
        {
            const BorrowedView<const char> suffix = descriptor.SuffixPatterns[pattern];
            if (suffix.GetSize() > bestLength && EndsWithAscii(fileName, suffix) == true)
            {
                best = descriptor.Language;
                bestLength = suffix.GetSize();
            }
        }
    }
    return best;
}
} // namespace ludus::editor
