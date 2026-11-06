#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/tooling/shader_language/diagnostic.hpp>

/// @file
/// LUSL tokens as produced by the preprocessor.
namespace ludus::tooling::shader_language
{
/// Lexical category of a token.
enum class TokenKind : foundation::uint8
{
    /// `[A-Za-z_][A-Za-z0-9_]*`. Keywords are identifiers until the parser classifies them.
    Identifier,
    /// A C preprocessing number: a digit, or `.` and a digit, followed by digits, letters, `_`,
    /// `.` and exponent signs (`e+`, `E-`, `p+`, `P-`). Its value is checked by later stages.
    Number,
    /// A double-quoted string literal, including its quotes and escapes.
    String,
    /// An operator or separator; Token::Symbol says which.
    Punctuator,
    /// A `//` or `/* */` comment, including its delimiters. Only FormatSource lexes comments;
    /// preprocessed and parsed tokens never contain them.
    Comment,
};

/// Operator or separator spelled by a Punctuator token. Lexing takes the longest match.
enum class Punctuator : foundation::uint8
{
    None,                ///< The token is not a punctuator.
    LeftBrace,           ///< `{`
    RightBrace,          ///< `}`
    LeftBracket,         ///< `[`
    RightBracket,        ///< `]`
    LeftParenthesis,     ///< `(`
    RightParenthesis,    ///< `)`
    Semicolon,           ///< `;`
    Comma,               ///< `,`
    Period,              ///< `.`
    Ellipsis,            ///< `...`
    Colon,               ///< `:`
    ColonColon,          ///< `::`
    Question,            ///< `?`
    Equal,               ///< `=`
    EqualEqual,          ///< `==`
    Exclamation,         ///< `!`, which only exists to report DiagnosticCode::LogicalNot
    ExclamationEqual,    ///< `!=`
    Less,                ///< `<`
    LessEqual,           ///< `<=`
    Greater,             ///< `>`
    GreaterEqual,        ///< `>=`
    Plus,                ///< `+`
    Minus,               ///< `-`
    Star,                ///< `*`
    Slash,               ///< `/`
    Percent,             ///< `%`
    Ampersand,           ///< `&`
    Pipe,                ///< `|`
    Caret,               ///< `^`
    Tilde,               ///< `~`
    LessLess,            ///< `<<`
    GreaterGreater,      ///< `>>`
    AmpersandAmpersand,  ///< `&&`
    PipePipe,            ///< `||`
    PlusEqual,           ///< `+=`
    MinusEqual,          ///< `-=`
    StarEqual,           ///< `*=`
    SlashEqual,          ///< `/=`
    PercentEqual,        ///< `%=`
    AmpersandEqual,      ///< `&=`
    PipeEqual,           ///< `|=`
    CaretEqual,          ///< `^=`
    LessLessEqual,       ///< `<<=`
    GreaterGreaterEqual, ///< `>>=`
    PlusPlus,            ///< `++`
    MinusMinus,          ///< `--`
    Arrow,               ///< `->`
    Hash,                ///< `#`
    HashHash,            ///< `##`
};

/// Token::ExpansionIndex of a token that does not come from a macro expansion.
inline constexpr foundation::uint32 NO_EXPANSION = 0xffffffffU;

/// @brief One token; its spelling is borrowed from the request or the result.
///
/// When `IsSynthesized` is false, the spelling is `SpellingSize` bytes at `SpellingOffset` in the
/// text of request file `Location.FileIndex`, which the caller keeps alive. When it is true, the
/// spelling is in the result's synthesized text: configuration values, `#` and `##` create such
/// spellings. Use GetTokenSpelling rather than indexing either buffer directly.
struct Token final
{
    /// Lexical category.
    TokenKind Kind = TokenKind::Identifier;
    /// Which punctuator, for TokenKind::Punctuator; Punctuator::None otherwise.
    Punctuator Symbol = Punctuator::None;
    /// True for the first token on its source line.
    bool IsAtLineStart = false;
    /// True when whitespace or a comment precedes the token on its line.
    bool HasLeadingSpace = false;
    /// True when the spelling is stored in the result's synthesized text.
    bool IsSynthesized = false;
    /// Byte offset of the spelling in its storage.
    foundation::uint32 SpellingOffset = 0;
    /// Byte size of the spelling.
    foundation::uint32 SpellingSize = 0;
    /// Where the token is spelled. For a token from a macro body this is inside the definition.
    SourceLocation Location;
    /// Index of the outermost macro use that produced the token, or NO_EXPANSION.
    foundation::uint32 ExpansionIndex = NO_EXPANSION;
};
} // namespace ludus::tooling::shader_language
