#include "codec.hpp"

#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/parsing/json.hpp>

#include <cstring>
#include <string_view>

namespace ludus::tooling::shader_compiler::internal
{
using namespace foundation;
namespace
{
constexpr uint8 HEX_RADIX = 16;
constexpr uint8 HEX_LETTERS_OFFSET = 10;
std::string_view GetText(BorrowedView<const char> value) noexcept
{
    return {value.GetDataOrNull(), value.GetSize()};
}
bool IsTextValid(BorrowedView<const char> value) noexcept
{
    return value.IsValid() == true && value.GetSize() != 0 && GetText(value).find('\0') == std::string_view::npos;
}
bool ReadHex(parsing::JsonValue value, Array<uint8>& outBytes) noexcept
{
    std::string_view text;
    if (value.String(text) == false || text.size() % 2 != 0 || text.size() / 2 > MAX_ARTIFACT_BYTES ||
        outBytes.TryResize(text.size() / 2) == false)
    {
        return false;
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        const char digit = text[i];
        const bool isDecimal = digit >= '0' && digit <= '9';
        if (isDecimal == false && (digit < 'a' || digit > 'f'))
        {
            return false;
        }
        const uint8 nibble =
            isDecimal == true ? static_cast<uint8>(digit - '0') : static_cast<uint8>(digit - 'a' + HEX_LETTERS_OFFSET);
        if (i % 2 == 0)
        {
            outBytes[i / 2] = static_cast<uint8>(nibble * HEX_RADIX);
        }
        else
        {
            outBytes[i / 2] = static_cast<uint8>(outBytes[i / 2] + nibble);
        }
    }
    return true;
}
bool Finish(parsing::JsonWriter& inoutWriter, Array<uint8>& inoutBytes) noexcept
{
    std::span<const uint8> bytes;
    return inoutWriter.Finish(bytes) == parsing::ParseStatus::Ok && inoutBytes.TryResize(bytes.size()) == true;
}
} // namespace
bool IsValid(const Request& request) noexcept
{
    if (request.Files.IsValid() == false || request.Files.GetSize() == 0 ||
        request.Files.GetSize() > MAX_SOURCE_FILES || IsTextValid(request.Source) == false ||
        IsTextValid(request.Entry) == false || request.Entry.GetSize() > MAX_ENTRY_BYTES ||
        filesystem::ValidPath(GetText(request.Source)) == false ||
        static_cast<uint8>(request.OutputTarget) > static_cast<uint8>(Target::Metal) ||
        static_cast<uint8>(request.EntryStage) > static_cast<uint8>(Stage::Compute))
    {
        return false;
    }
    for (usize i = 0; i < request.Entry.GetSize(); ++i)
    {
        const char character = request.Entry[i];
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_' ||
            (i != 0 && character >= '0' && character <= '9'))
        {
            continue;
        }
        return false;
    }
    usize bytes = 0;
    bool hasSource = false;
    for (usize i = 0; i < request.Files.GetSize(); ++i)
    {
        const SourceFile& file = request.Files[i];
        if (IsTextValid(file.Path) == false || filesystem::ValidPath(GetText(file.Path)) == false ||
            file.Text.IsValid() == false || file.Text.GetSize() > MAX_SOURCE_BYTES - bytes ||
            GetText(file.Text).find('\0') != std::string_view::npos)
        {
            return false;
        }
        bytes += file.Text.GetSize();
        hasSource = hasSource == true || GetText(file.Path) == GetText(request.Source);
        for (usize j = 0; j < i; ++j)
        {
            if (GetText(file.Path) == GetText(request.Files[j].Path))
            {
                return false;
            }
        }
    }
    return hasSource;
}
bool EncodeRequest(const Request& request, Array<uint8>& outBytes) noexcept
{
    if (IsValid(request) == false || outBytes.TryResize(MAX_MESSAGE_BYTES) == false)
    {
        return false;
    }
    parsing::JsonWriter writer{{outBytes.GetData(), outBytes.GetSize()}};
    writer.Raw("{\"version\":");
    writer.Integer(PROTOCOL_VERSION);
    writer.Raw(",\"identity\":");
    writer.Integer(request.Identity);
    writer.Raw(",\"source\":");
    writer.String(GetText(request.Source));
    writer.Raw(",\"entry\":");
    writer.String(GetText(request.Entry));
    writer.Raw(",\"target\":");
    writer.Integer(static_cast<uint8>(request.OutputTarget));
    writer.Raw(",\"stage\":");
    writer.Integer(static_cast<uint8>(request.EntryStage));
    writer.Raw(",\"files\":{");
    for (usize i = 0; i < request.Files.GetSize(); ++i)
    {
        if (i != 0)
        {
            writer.Raw(",");
        }
        writer.String(GetText(request.Files[i].Path));
        writer.Raw(":");
        writer.String(GetText(request.Files[i].Text));
    }
    writer.Raw("}}");
    return Finish(writer, outBytes);
}
bool DecodeResult(BorrowedView<const uint8> bytes, Result& outResult) noexcept
{
    parsing::JsonDocument document;
    parsing::ParseError error;
    parsing::JsonLimits limits;
    limits.MaxInputBytes = MAX_MESSAGE_BYTES;
    limits.MaxStringBytes = MAX_MESSAGE_BYTES;
    limits.MaxWorkspaceBytes = MAX_MESSAGE_BYTES * 2;
    if (bytes.IsValid() == false ||
        document.Read({reinterpret_cast<const char*>(bytes.GetDataOrNull()), bytes.GetSize()}, error, limits) !=
            parsing::ParseStatus::Ok)
    {
        return false;
    }
    const parsing::JsonValue root = document.Root();
    uint64 version = 0;
    uint64 outcome = 0;
    Result result;
    std::string_view diagnostics;
    std::string_view compilerVersion;
    if (root.Get("version").Integer(version) == false || version != PROTOCOL_VERSION ||
        root.Get("compiler").String(compilerVersion) == false || compilerVersion != LUDUS_SLANG_VERSION ||
        root.Get("identity").Integer(result.Identity) == false || root.Get("status").Integer(outcome) == false ||
        outcome > static_cast<uint8>(Status::InvalidState) || outcome == static_cast<uint8>(Status::Pending) ||
        ReadHex(root.Get("artifact"), result.Artifact) == false ||
        ReadHex(root.Get("reflection"), result.Reflection) == false ||
        ReadHex(root.Get("source_read_mask"), result.SourceReadMask) == false ||
        result.SourceReadMask.GetSize() > MAX_SOURCE_FILES || root.Get("diagnostics").String(diagnostics) == false ||
        diagnostics.size() > MAX_DIAGNOSTIC_BYTES || result.Diagnostics.TryResize(diagnostics.size()) == false)
    {
        return false;
    }
    for (const uint8 value : result.SourceReadMask)
    {
        if (value > 1)
        {
            return false;
        }
    }
    result.Outcome = static_cast<Status>(outcome);
    if ((result.Outcome == Status::Ready &&
         (result.Artifact.IsEmpty() == true || result.Reflection.IsEmpty() == true)) ||
        (result.Outcome != Status::Ready &&
         (result.Artifact.IsEmpty() == false || result.Reflection.IsEmpty() == false ||
          result.SourceReadMask.IsEmpty() == false)))
    {
        return false;
    }
    if (diagnostics.empty() == false)
    {
        std::memcpy(result.Diagnostics.GetData(), diagnostics.data(), diagnostics.size());
    }
    outResult = Move(result);
    return true;
}
} // namespace ludus::tooling::shader_compiler::internal
