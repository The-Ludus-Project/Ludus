#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

#include "internal/code_editor.h"

namespace ludus::editor
{
/// The maximum file-name suffix patterns one language record declares. A small
/// fixed bound keeps the compiled-in table flat and lets the registry stay
/// Qt-free; raise it here, in the single owner, when a language needs more.
inline constexpr foundation::usize CODE_LANGUAGE_MAX_PATTERNS = 4;

/// One compiled-in bracket or auto-closing pair, stored as single code units.
/// Both members are ASCII for the first languages; the syntax service owns any
/// future multi-byte pairs.
struct CodeLanguagePair final
{
    char Open = '\0';
    char Close = '\0';
    [[nodiscard]] bool operator==(const CodeLanguagePair& other) const = default;
};

/// A compiled-in description of one editor language. It carries only the lexical
/// properties the current view and highlighter already branch on; grammar,
/// query, server, formatter and debugger profiles are added by the milestones
/// that own them (M3 grammars, M4 servers, M5/M6 providers, M8 debuggers). The
/// record is Qt-free so the syntax service and the browser editor can reuse it.
struct CodeLanguageDescriptor final
{
    /// The stable enum value this record describes. Persisted settings and the
    /// render-graph Source pane depend on the numeric identity, so it never
    /// changes once assigned.
    CodeLanguage Language = CodeLanguage::PlainText;
    /// A short lower-case identifier ("plaintext", "slang"); never localized.
    foundation::BorrowedView<const char> Identifier;
    /// A human-readable name for the status bar and language picker.
    foundation::BorrowedView<const char> DisplayName;
    /// Lower-case file-name suffixes that select this language (".slang"). The
    /// first entries fill the view; the array owner is the registry table.
    foundation::BorrowedView<const foundation::BorrowedView<const char>> SuffixPatterns;
    /// The line-comment token, or empty when the language has none. Plain text
    /// has none, so comment toggling is disabled, exactly as today.
    foundation::BorrowedView<const char> LineComment;
    /// Bracket pairs understood for matching and automatic pairing.
    foundation::BorrowedView<const CodeLanguagePair> BracketPairs;
    /// True when the single-line lexer colors this language. Plain text is
    /// false, which is the current PlainText fast path in the highlighter.
    bool HasLexicalColoring = false;
    /// True when the language participates in auto-closing and smart indent,
    /// matching the current Slang-only behavior in the view.
    bool HasStructuralEditing = false;
};

/// The compiled-in language table. It replaces the hard-coded CodeLanguage
/// switch selectors with one lookup table, without changing behavior: the only
/// records in M0 are PlainText and Slang, with the properties the current
/// editor already uses. Adding a language means adding a record and its data
/// files, not forking the view, the highlighter or the controller.
///
/// The registry is a value type over borrowed static storage. It owns no Qt
/// objects and is safe to read from the syntax worker or the browser editor.
class CodeLanguageRegistry final
{
public:
    /// The process-wide registry over static storage. GUI thread and workers may
    /// read it concurrently; it is immutable after construction.
    [[nodiscard]] static const CodeLanguageRegistry& Get() noexcept;

    /// The descriptor for a language value. Every CodeLanguage has a record, so
    /// this never fails; the reference is valid for the process lifetime.
    [[nodiscard]] const CodeLanguageDescriptor& Describe(CodeLanguage language) const noexcept;

    /// The language selected by a UTF-8 file name or path, matching the longest
    /// declared suffix. Returns PlainText when nothing matches, which preserves
    /// the current ".slang"-or-plain-text behavior. The comparison is
    /// case-insensitive over ASCII only.
    [[nodiscard]] CodeLanguage ClassifyFileName(foundation::BorrowedView<const char> fileName) const noexcept;

    /// Every record, in enum order, for the language picker and tests.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const CodeLanguageDescriptor> GetDescriptors() const noexcept
    {
        return mDescriptors;
    }

private:
    CodeLanguageRegistry() noexcept;
    foundation::BorrowedView<const CodeLanguageDescriptor> mDescriptors;
};
} // namespace ludus::editor
