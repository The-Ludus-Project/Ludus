#include "pbrt_tokenizer.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::importers::pbrt
{
namespace
{
[[nodiscard]] bool IsSpace(uint8 value) noexcept
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
}

[[nodiscard]] bool IsDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

[[nodiscard]] bool IsNumberStart(uint8 value) noexcept
{
    return IsDigit(static_cast<char>(value)) == true || value == '-' || value == '+' || value == '.';
}

[[nodiscard]] bool IsWordByte(uint8 value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || IsDigit(static_cast<char>(value)) ||
           value == '_';
}

[[nodiscard]] bool IsBareByte(uint8 value) noexcept
{
    return IsSpace(value) == false && value != '"' && value != '[' && value != ']' && value != '#';
}
} // namespace

bool Tokenizer::peekByte(uint8& outByte) noexcept
{
    if (mBufferOffset == mBufferSize)
    {
        if (mIsAtEnd == true)
        {
            return false;
        }
        usize count = 0;
        if (mSource.Read(std::span<uint8>(mBuffer, BUFFER_BYTES), count) != Status::Ok)
        {
            mHasReadFailed = true;
            mIsAtEnd = true;
            return false;
        }
        mBufferOffset = 0;
        mBufferSize = count;
        if (count == 0)
        {
            mIsAtEnd = true;
            return false;
        }
    }
    outByte = mBuffer[mBufferOffset];
    return true;
}

void Tokenizer::consumeByte() noexcept
{
    if (mBuffer[mBufferOffset] == '\n')
    {
        ++mLine;
        mColumn = 1;
    }
    else
    {
        ++mColumn;
    }
    ++mBufferOffset;
}

TokenizerStatus Tokenizer::failSyntax(std::string_view message) noexcept
{
    mErrorMessage = message;
    mErrorLine = mCurrent.Line;
    return TokenizerStatus::SyntaxError;
}

TokenizerStatus Tokenizer::appendToToken(char value) noexcept
{
    if (mCurrent.Text.GetSize() >= MAX_TOKEN_BYTES)
    {
        return failSyntax("token exceeds the length limit");
    }
    return mCurrent.Text.TryAppend(std::string_view(&value, 1)) == foundation::StringStatus::Ok
               ? TokenizerStatus::Ok
               : TokenizerStatus::OutOfMemory;
}

TokenizerStatus Tokenizer::readString() noexcept
{
    consumeByte(); // Opening quote.
    uint8 value = 0;
    while (true)
    {
        if (peekByte(value) == false)
        {
            return mHasReadFailed == true ? TokenizerStatus::IoError : failSyntax("unterminated string");
        }
        if (value == '\n')
        {
            return failSyntax("newline in string");
        }
        consumeByte();
        if (value == '"')
        {
            return TokenizerStatus::Ok;
        }
        char decoded = static_cast<char>(value);
        if (value == '\\')
        {
            if (peekByte(value) == false)
            {
                return mHasReadFailed == true ? TokenizerStatus::IoError : failSyntax("unterminated string");
            }
            consumeByte();
            switch (value)
            {
                case 'n':
                    decoded = '\n';
                    break;
                case 't':
                    decoded = '\t';
                    break;
                case 'r':
                    decoded = '\r';
                    break;
                case 'b':
                    decoded = '\b';
                    break;
                case 'f':
                    decoded = '\f';
                    break;
                case '\\':
                case '"':
                case '\'':
                    decoded = static_cast<char>(value);
                    break;
                default:
                    return failSyntax("invalid escape in string");
            }
        }
        const TokenizerStatus status = appendToToken(decoded);
        if (status != TokenizerStatus::Ok)
        {
            return status;
        }
    }
}

TokenizerStatus Tokenizer::readBare(TokenKind kind) noexcept
{
    uint8 value = 0;
    while (peekByte(value) == true && IsBareByte(value) == true)
    {
        if (kind == TokenKind::Word && IsWordByte(value) == false)
        {
            return failSyntax("unexpected character");
        }
        consumeByte();
        const TokenizerStatus status = appendToToken(static_cast<char>(value));
        if (status != TokenizerStatus::Ok)
        {
            return status;
        }
    }
    return mHasReadFailed == true ? TokenizerStatus::IoError : TokenizerStatus::Ok;
}

TokenizerStatus Tokenizer::Advance() noexcept
{
    mCurrent.Text.Clear();
    uint8 value = 0;
    while (true)
    {
        if (peekByte(value) == false)
        {
            mCurrent.Kind = TokenKind::End;
            mCurrent.Line = mLine;
            mCurrent.Column = mColumn;
            return mHasReadFailed == true ? TokenizerStatus::IoError : TokenizerStatus::Ok;
        }
        if (IsSpace(value) == true)
        {
            consumeByte();
            continue;
        }
        if (value == '#')
        {
            while (peekByte(value) == true && value != '\n')
            {
                consumeByte();
            }
            continue;
        }
        break;
    }
    mCurrent.Line = mLine;
    mCurrent.Column = mColumn;
    if (value == '[' || value == ']')
    {
        consumeByte();
        mCurrent.Kind = value == '[' ? TokenKind::OpenBracket : TokenKind::CloseBracket;
        return TokenizerStatus::Ok;
    }
    if (value == '"')
    {
        mCurrent.Kind = TokenKind::String;
        return readString();
    }
    if (IsNumberStart(value) == true)
    {
        mCurrent.Kind = TokenKind::Number;
        return readBare(TokenKind::Number);
    }
    if (IsWordByte(value) == true)
    {
        mCurrent.Kind = TokenKind::Word;
        return readBare(TokenKind::Word);
    }
    return failSyntax("unexpected character");
}
} // namespace ludus::content::scene_cook::importers::pbrt
