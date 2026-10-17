#include "internal/record_json.h"

namespace ludus::content::identity_records::internal
{
namespace
{
constexpr usize INDENT_BYTES = 2;
constexpr std::string_view INDENT = "                                                                ";
// Containers deeper than the parser's ceiling cannot reach the writer.
static_assert(INDENT.size() >= INDENT_BYTES * foundation::parsing::MAX_JSON_DEPTH / 2);
} // namespace

IdentityRecordStatus Fail(IdentityRecordError& outError,
                          IdentityRecordStatus status,
                          std::string_view what,
                          std::string_view subject) noexcept
{
    outError.Status = status;
    char text[MAX_MESSAGE_BYTES];
    usize size = 0;
    for (const std::string_view part :
         {what, subject.empty() == true ? std::string_view{} : std::string_view(": "), subject})
    {
        for (const char item : part)
        {
            if (size == MAX_MESSAGE_BYTES)
            {
                break;
            }
            text[size] = item;
            ++size;
        }
    }
    // The bounded copy above never exceeds MessageText's capacity.
    [[maybe_unused]] const bool isSet = outError.Message.Set(std::string_view(text, size));
    LUDUS_ASSERT(isSet == true);
    return status;
}

bool TryReadIdentity(foundation::parsing::JsonValue value, foundation::identity::Uuid& outValue) noexcept
{
    std::string_view text;
    return value.String(text) == true &&
           foundation::identity::TryParseUuid(text.data(),
                                              text.size(),
                                              foundation::identity::UuidPolicy::RequireVersion4,
                                              outValue) == foundation::identity::UuidStatus::Success;
}

foundation::parsing::JsonLimits MakeLimits(usize maxInputBytes) noexcept
{
    foundation::parsing::JsonLimits limits;
    limits.MaxInputBytes = maxInputBytes;
    limits.MaxDepth = foundation::parsing::MAX_JSON_DEPTH / 2;
    limits.MaxObjectMembers = foundation::parsing::MAX_JSON_OBJECT_MEMBERS;
    return limits;
}

IndentedWriter::IndentedWriter(usize capacity) noexcept
    : mWriter(mScratch.Resize(capacity) == true ? mScratch.Data() : std::span<uint8>{}),
      mIsReady(mScratch.Data().size() == capacity)
{
}

void IndentedWriter::beginItem() noexcept
{
    if (mIsFirstItem == false)
    {
        mWriter.Raw(",");
    }
    mIsFirstItem = false;
    mWriter.Raw("\n");
    mWriter.Raw(INDENT.substr(0, mDepth * INDENT_BYTES));
}

void IndentedWriter::Open(char bracket) noexcept
{
    mWriter.Raw(std::string_view(&bracket, 1));
    ++mDepth;
    mIsFirstItem = true;
}

void IndentedWriter::Close(char bracket) noexcept
{
    LUDUS_ASSERT(mDepth > 0);
    --mDepth;
    if (mIsFirstItem == false)
    {
        mWriter.Raw("\n");
        mWriter.Raw(INDENT.substr(0, mDepth * INDENT_BYTES));
    }
    mWriter.Raw(std::string_view(&bracket, 1));
    mIsFirstItem = false;
}

void IndentedWriter::BeginMember(std::string_view key) noexcept
{
    beginItem();
    mWriter.String(key);
    mWriter.Raw(": ");
}

void IndentedWriter::BeginElement() noexcept
{
    beginItem();
}

void IndentedWriter::WriteString(std::string_view value) noexcept
{
    mWriter.String(value);
}

void IndentedWriter::WriteInteger(uint64 value) noexcept
{
    mWriter.Integer(value);
}

void IndentedWriter::WriteUuid(const foundation::identity::Uuid& value) noexcept
{
    char text[foundation::identity::UUID_TEXT_BUFFER_BYTES];
    if (foundation::identity::FormatUuid(value, text, sizeof(text)) != foundation::identity::UuidStatus::Success)
    {
        mHasFailed = true;
        return;
    }
    mWriter.String(std::string_view(text, foundation::identity::UUID_TEXT_LENGTH));
}

void IndentedWriter::WriteValue(foundation::parsing::JsonValue value) noexcept
{
    std::string_view text;
    uint64 unsignedValue = 0;
    int64 signedValue = 0;
    float64 number = 0;
    bool isTrue = false;
    if (value.Object() == true)
    {
        Open('{');
        for (usize i = 0; i < value.MemberCount(); ++i)
        {
            std::string_view key;
            foundation::parsing::JsonValue member;
            if (value.MemberAt(i, key, member) == false)
            {
                mHasFailed = true;
                return;
            }
            BeginMember(key);
            WriteValue(member);
        }
        Close('}');
    }
    else if (value.Array() == true)
    {
        Open('[');
        for (usize i = 0; i < value.Count(); ++i)
        {
            BeginElement();
            WriteValue(value.At(i));
        }
        Close(']');
    }
    else if (value.String(text) == true)
    {
        mWriter.String(text);
    }
    else if (value.Integer(unsignedValue) == true)
    {
        mWriter.Integer(unsignedValue);
    }
    else if (value.SignedInteger(signedValue) == true)
    {
        // Negative here: the unsigned branch took every nonnegative integer.
        mWriter.Raw("-");
        mWriter.Integer(static_cast<uint64>(-(signedValue + 1)) + 1);
    }
    else if (value.Number(number) == true)
    {
        mWriter.Number(number);
    }
    else if (value.Boolean(isTrue) == true)
    {
        mWriter.Boolean(isTrue);
    }
    else if (value.Null() == true)
    {
        mWriter.Raw("null");
    }
    else
    {
        mHasFailed = true;
    }
}

IdentityRecordStatus IndentedWriter::Finish(Bytes& outBytes) noexcept
{
    if (mIsReady == false)
    {
        return IdentityRecordStatus::OutOfMemory;
    }
    std::span<const uint8> written;
    const foundation::parsing::ParseStatus status = mWriter.Finish(written);
    if (mHasFailed == true || status != foundation::parsing::ParseStatus::Ok)
    {
        return status == foundation::parsing::ParseStatus::OutOfMemory ? IdentityRecordStatus::OutOfMemory
                                                                       : IdentityRecordStatus::Limit;
    }
    Bytes result;
    if (result.Resize(written.size()) == false)
    {
        return IdentityRecordStatus::OutOfMemory;
    }
    for (usize i = 0; i < written.size(); ++i)
    {
        result.Data()[i] = written[i];
    }
    outBytes = Move(result);
    return IdentityRecordStatus::Success;
}
} // namespace ludus::content::identity_records::internal
