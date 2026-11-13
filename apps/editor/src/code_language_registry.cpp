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

// Auto-closing adds the string quote to the brackets; surrounding adds both the
// string and character quotes, so a selection can be wrapped in either.
constexpr CodeLanguagePair SLANG_AUTO_CLOSING[] = {
    { .Open = '(', .Close = ')' },
    { .Open = '[', .Close = ']' },
    { .Open = '{', .Close = '}' },
    { .Open = '"', .Close = '"' },
};

constexpr BorrowedView<const char> LUSL_SUFFIXES[] = {
    Literal(".lusl"),
    Literal(".luslh"),
};

constexpr CodeLanguagePair SLANG_SURROUNDING[] = {
    { .Open = '(', .Close = ')' },
    { .Open = '[', .Close = ']' },
    { .Open = '{', .Close = '}' },
    { .Open = '"', .Close = '"' },
    { .Open = '\'', .Close = '\'' },
};

constexpr BorrowedView<const char> LUAU_SUFFIXES[] = {
    Literal(".luau"),
    Literal(".lua"),
};

constexpr BorrowedView<const char> C_SUFFIXES[] = {
    Literal(".c"),
};

constexpr BorrowedView<const char> CPP_SUFFIXES[] = {
    Literal(".cpp"),
    Literal(".cc"),
    Literal(".cxx"),
    Literal(".h"),
    Literal(".hh"),
    Literal(".hpp"),
    Literal(".hxx"),
    Literal(".inl"),
    Literal(".ipp"),
};

constexpr BorrowedView<const char> JSON_SUFFIXES[] = {
    Literal(".json"),
    Literal(".jsonc"),
};

constexpr BorrowedView<const char> CMAKE_SUFFIXES[] = {
    Literal("cmakelists.txt"),
    Literal(".cmake"),
};

constexpr BorrowedView<const char> MARKDOWN_SUFFIXES[] = {
    Literal(".md"),
    Literal(".markdown"),
};

// The table is in CodeLanguage enum order.
const CodeLanguageDescriptor TABLE[] = {
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::PlainText,
        .Identifier = Literal("plaintext"),
        .DisplayName = Literal("Plain Text"),
        .SuffixPatterns = {},
        .LineComment = {},
        .BracketPairs = {},
        .AutoClosingPairs = {},
        .SurroundingPairs = {},
        .HasLexicalColoring = false,
        .HasStructuralEditing = false,
        .HasLanguageService = false,
    },
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::Slang,
        .Identifier = Literal("slang"),
        .DisplayName = Literal("Slang"),
        .SuffixPatterns = BorrowedView<const BorrowedView<const char>>(SLANG_SUFFIXES),
        .LineComment = Literal("//"),
        .BracketPairs = BorrowedView<const CodeLanguagePair>(SLANG_BRACKETS),
        .AutoClosingPairs = BorrowedView<const CodeLanguagePair>(SLANG_AUTO_CLOSING),
        .SurroundingPairs = BorrowedView<const CodeLanguagePair>(SLANG_SURROUNDING),
        .HasLexicalColoring = true,
        .HasStructuralEditing = true,
        .HasLanguageService = true,
    },
    // LUSL is C-like: the Slang pairs, comments and structural editing apply unchanged.
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::Lusl,
        .Identifier = Literal("lusl"),
        .DisplayName = Literal("LUSL"),
        .SuffixPatterns = BorrowedView<const BorrowedView<const char>>(LUSL_SUFFIXES),
        .LineComment = Literal("//"),
        .BracketPairs = BorrowedView<const CodeLanguagePair>(SLANG_BRACKETS),
        .AutoClosingPairs = BorrowedView<const CodeLanguagePair>(SLANG_AUTO_CLOSING),
        .SurroundingPairs = BorrowedView<const CodeLanguagePair>(SLANG_SURROUNDING),
        .HasLexicalColoring = true,
        .HasStructuralEditing = true,
        .HasLanguageService = true,
    },
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::Luau,
        .Identifier = Literal("luau"),
        .DisplayName = Literal("Luau"),
        .SuffixPatterns = BorrowedView<const BorrowedView<const char>>(LUAU_SUFFIXES),
        .LineComment = Literal("--"),
        .BracketPairs = BorrowedView<const CodeLanguagePair>(SLANG_BRACKETS),
        .AutoClosingPairs = BorrowedView<const CodeLanguagePair>(SLANG_AUTO_CLOSING),
        .SurroundingPairs = BorrowedView<const CodeLanguagePair>(SLANG_SURROUNDING),
        .HasLexicalColoring = true,
        .HasStructuralEditing = true,
        .HasLanguageService = false,
    },
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::C,
        .Identifier = Literal("c"),
        .DisplayName = Literal("C"),
        .SuffixPatterns = BorrowedView<const BorrowedView<const char>>(C_SUFFIXES),
        .LineComment = Literal("//"),
        .BracketPairs = BorrowedView<const CodeLanguagePair>(SLANG_BRACKETS),
        .AutoClosingPairs = BorrowedView<const CodeLanguagePair>(SLANG_AUTO_CLOSING),
        .SurroundingPairs = BorrowedView<const CodeLanguagePair>(SLANG_SURROUNDING),
        .HasLexicalColoring = true,
        .HasStructuralEditing = true,
        .HasLanguageService = false,
    },
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::Cpp,
        .Identifier = Literal("cpp"),
        .DisplayName = Literal("C++"),
        .SuffixPatterns = BorrowedView<const BorrowedView<const char>>(CPP_SUFFIXES),
        .LineComment = Literal("//"),
        .BracketPairs = BorrowedView<const CodeLanguagePair>(SLANG_BRACKETS),
        .AutoClosingPairs = BorrowedView<const CodeLanguagePair>(SLANG_AUTO_CLOSING),
        .SurroundingPairs = BorrowedView<const CodeLanguagePair>(SLANG_SURROUNDING),
        .HasLexicalColoring = true,
        .HasStructuralEditing = true,
        .HasLanguageService = false,
    },
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::Json,
        .Identifier = Literal("json"),
        .DisplayName = Literal("JSON"),
        .SuffixPatterns = BorrowedView<const BorrowedView<const char>>(JSON_SUFFIXES),
        .LineComment = {},
        .BracketPairs = BorrowedView<const CodeLanguagePair>(SLANG_BRACKETS),
        .AutoClosingPairs = BorrowedView<const CodeLanguagePair>(SLANG_AUTO_CLOSING),
        .SurroundingPairs = BorrowedView<const CodeLanguagePair>(SLANG_SURROUNDING),
        .HasLexicalColoring = true,
        .HasStructuralEditing = true,
        .HasLanguageService = false,
    },
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::CMake,
        .Identifier = Literal("cmake"),
        .DisplayName = Literal("CMake"),
        .SuffixPatterns = BorrowedView<const BorrowedView<const char>>(CMAKE_SUFFIXES),
        .LineComment = Literal("#"),
        .BracketPairs = BorrowedView<const CodeLanguagePair>(SLANG_BRACKETS),
        .AutoClosingPairs = BorrowedView<const CodeLanguagePair>(SLANG_AUTO_CLOSING),
        .SurroundingPairs = BorrowedView<const CodeLanguagePair>(SLANG_SURROUNDING),
        .HasLexicalColoring = true,
        .HasStructuralEditing = true,
        .HasLanguageService = false,
    },
    CodeLanguageDescriptor
    {
        .Language = CodeLanguage::Markdown,
        .Identifier = Literal("markdown"),
        .DisplayName = Literal("Markdown"),
        .SuffixPatterns = BorrowedView<const BorrowedView<const char>>(MARKDOWN_SUFFIXES),
        .LineComment = {},
        .BracketPairs = {},
        .AutoClosingPairs = {},
        .SurroundingPairs = {},
        .HasLexicalColoring = true,
        .HasStructuralEditing = false,
        .HasLanguageService = false,
    },
};
} // namespace

bool HasCodeLanguageService(CodeLanguage language) noexcept
{
    return CodeLanguageRegistry::Get().Describe(language).HasLanguageService;
}

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
            if (EndsWithAscii(fileName, suffix) == false)
            {
                continue;
            }
            const bool hasSuffix = suffix[0] == '.' || fileName.GetSize() == suffix.GetSize() ||
                                   fileName[fileName.GetSize() - suffix.GetSize() - 1] == '/' ||
                                   fileName[fileName.GetSize() - suffix.GetSize() - 1] == '\\';
            if (hasSuffix == true && suffix.GetSize() > bestLength)
            {
                best = descriptor.Language;
                bestLength = suffix.GetSize();
            }
        }
    }
    return best;
}
} // namespace ludus::editor
