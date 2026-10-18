#pragma once

// Streaming, locale-free pbrt-v4 tokenizer with line/column tracking.
//
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, "pbrt-v4 Input File
// Format" (https://pbrt.org/fileformat-v4), for the lexical grammar: quoted
// strings, '#' comments, '[' ']' lists, bare directive words and numbers.

#include "../byte_source.h"
#include "../decimal_number.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/strings/string.hpp>

#include <string_view>

namespace ludus::content::scene_cook::importers::pbrt
{
using foundation::String;
using foundation::core::float64;
using foundation::core::uint32;
using foundation::core::uint8;
using foundation::core::usize;

/// Longest admitted token in bytes (strings and numbers).
inline constexpr usize MAX_TOKEN_BYTES = 4096;

/// Lexical class of a token.
enum class TokenKind : uint8
{
    /// A bare word: a directive name, or `true`/`false` in older files.
    Word,
    /// A quoted string with escapes resolved.
    String,
    /// A numeric literal (text only; see importers::ParseNumber).
    Number,
    OpenBracket,
    CloseBracket,
    End,
};

/// One token; Text stays valid until the token is replaced.
struct Token final
{
    TokenKind Kind = TokenKind::End;
    String Text;
    uint32 Line = 0;
    uint32 Column = 0;
};

/// Outcome of Tokenizer::Advance.
enum class TokenizerStatus : uint8
{
    Ok,
    /// Unterminated string, unknown character, invalid escape or overlong token.
    SyntaxError,
    IoError,
    OutOfMemory,
};

/// Tokenizer with one token of lookahead over a ByteSource.
class Tokenizer final
{
public:
    explicit Tokenizer(ByteSource& inoutSource) noexcept : mSource(inoutSource) {}
    Tokenizer(const Tokenizer&) = delete;
    Tokenizer& operator=(const Tokenizer&) = delete;

    /// The lookahead token (End after the input is exhausted).
    [[nodiscard]] const Token& GetCurrent() const noexcept
    {
        return mCurrent;
    }
    /// Replace the lookahead with the next token.
    [[nodiscard]] TokenizerStatus Advance() noexcept;
    /// Description of the last syntax error; empty otherwise.
    [[nodiscard]] std::string_view GetErrorMessage() const noexcept
    {
        return mErrorMessage;
    }
    /// Line of the last syntax error.
    [[nodiscard]] uint32 GetErrorLine() const noexcept
    {
        return mErrorLine;
    }

private:
    [[nodiscard]] bool peekByte(uint8& outByte) noexcept;
    void consumeByte() noexcept;
    [[nodiscard]] TokenizerStatus failSyntax(std::string_view message) noexcept;
    [[nodiscard]] TokenizerStatus appendToToken(char value) noexcept;
    [[nodiscard]] TokenizerStatus readString() noexcept;
    [[nodiscard]] TokenizerStatus readBare(TokenKind kind) noexcept;

    static constexpr usize BUFFER_BYTES = 65536;
    ByteSource& mSource;
    uint8 mBuffer[BUFFER_BYTES]{};
    usize mBufferSize = 0;
    usize mBufferOffset = 0;
    bool mIsAtEnd = false;
    bool mHasReadFailed = false;
    uint32 mLine = 1;
    uint32 mColumn = 1;
    Token mCurrent;
    std::string_view mErrorMessage;
    uint32 mErrorLine = 0;
};
} // namespace ludus::content::scene_cook::importers::pbrt
