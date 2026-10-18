#include <ludus/content/json.h>
#include <ludus/content/limits.h>

#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>

namespace ludus::content
{
namespace
{
foundation::parsing::JsonValue Parsed(JsonValue value) noexcept
{
    return {value.Value};
}
Status Convert(foundation::parsing::ParseStatus result, foundation::parsing::ParseLimit limit) noexcept
{
    using foundation::parsing::ParseLimit;
    using foundation::parsing::ParseStatus;
    switch (result)
    {
        case ParseStatus::Ok:
            return Status::Ok;
        case ParseStatus::OutOfMemory:
            return Status::OutOfMemory;
        case ParseStatus::LimitExceeded:
            // Existing Content treats structural limits as invalid documents.
            return limit == ParseLimit::InputBytes || limit == ParseLimit::WorkspaceBytes ? Status::Limit
                                                                                          : Status::Invalid;
        default:
            return Status::Invalid;
    }
}
} // namespace

JsonValue JsonValue::Get(const char* key) const noexcept
{
    return {Parsed(*this).Get(key).Value};
}
JsonValue JsonValue::At(usize index) const noexcept
{
    return {Parsed(*this).At(index).Value};
}
usize JsonValue::Count() const noexcept
{
    return Parsed(*this).Count();
}
bool JsonValue::Array() const noexcept
{
    return Parsed(*this).Array();
}
bool JsonValue::String(std::string_view& output) const noexcept
{
    return Parsed(*this).String(output);
}
bool JsonValue::Integer(uint64& output) const noexcept
{
    return Parsed(*this).Integer(output);
}
bool JsonValue::Number(float64& output) const noexcept
{
    return Parsed(*this).Number(output);
}
bool JsonValue::Boolean(bool& output) const noexcept
{
    return Parsed(*this).Boolean(output);
}
bool JsonValue::Fields(std::initializer_list<const char*> required) const noexcept
{
    return Parsed(*this).Fields(required);
}
JsonDocument::~JsonDocument() noexcept = default;
Status JsonDocument::Read(std::string_view input, Diagnostic& diagnostic) noexcept
{
    diagnostic = {};
    // Preserve the established single-successful-read Content interface.
    if (mDocument.Root().Valid() || input.empty() || input.size() > MAX_DOCUMENT_BYTES)
    {
        return diagnostic.Result = Status::Limit;
    }
    foundation::parsing::ParseError error;
    foundation::parsing::JsonLimits limits;
    limits.MaxInputBytes = MAX_DOCUMENT_BYTES;
    const foundation::parsing::ParseStatus result = mDocument.Read(input, error, limits);
    diagnostic.Offset = error.Offset == foundation::parsing::UNKNOWN_BYTE_OFFSET ? 0 : error.Offset;
    return diagnostic.Result = Convert(result, error.Limit);
}
JsonValue JsonDocument::Root() const noexcept
{
    return {mDocument.Root().Value};
}
void JsonWriter::Raw(std::string_view value) noexcept
{
    mWriter.Raw(value);
}
void JsonWriter::String(std::string_view value) noexcept
{
    mWriter.String(value);
}
void JsonWriter::Integer(uint64 value) noexcept
{
    mWriter.Integer(value);
}
void JsonWriter::Number(float64 value) noexcept
{
    mWriter.Number(value);
}
void JsonWriter::Boolean(bool value) noexcept
{
    mWriter.Boolean(value);
}
Status JsonWriter::Finish(Bytes& output) noexcept
{
    std::span<const uint8> bytes;
    const auto result = mWriter.Finish(bytes);
    if (result != foundation::parsing::ParseStatus::Ok)
    {
        return result == foundation::parsing::ParseStatus::OutOfMemory ? Status::OutOfMemory : Status::Limit;
    }
    if (output.Resize(bytes.size()) == false)
    {
        return Status::OutOfMemory;
    }
    std::memcpy(output.Data().data(), bytes.data(), bytes.size());
    return Status::Ok;
}
} // namespace ludus::content
