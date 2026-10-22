#include "codec.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/parsing/json.hpp>

#include <string_view>

namespace ludus::tooling::shader_compiler::internal
{
using namespace foundation;
namespace
{
constexpr uint8 HEX_RADIX = 16;
constexpr const char* HEX_DIGITS = "0123456789abcdef";
void WriteHex(parsing::JsonWriter& inoutWriter, const Array<uint8>& bytes) noexcept
{
    inoutWriter.Raw("\"");
    for (const uint8 byte : bytes)
    {
        inoutWriter.Raw({&HEX_DIGITS[byte / HEX_RADIX], 1});
        inoutWriter.Raw({&HEX_DIGITS[byte % HEX_RADIX], 1});
    }
    inoutWriter.Raw("\"");
}
} // namespace
bool Process(Compiler& inoutCompiler, BorrowedView<const uint8> bytes, Array<uint8>& outBytes) noexcept
{
    parsing::JsonDocument document;
    parsing::ParseError error;
    parsing::JsonLimits limits;
    limits.MaxInputBytes = MAX_MESSAGE_BYTES;
    limits.MaxWorkspaceBytes = MAX_MESSAGE_BYTES * 2;
    limits.MaxStringBytes = MAX_SOURCE_BYTES;
    limits.MaxObjectMembers = MAX_SOURCE_FILES;
    Result result;
    if (bytes.IsValid() == true &&
        document.Read({reinterpret_cast<const char*>(bytes.GetDataOrNull()), bytes.GetSize()}, error, limits) ==
            parsing::ParseStatus::Ok)
    {
        const parsing::JsonValue root = document.Root();
        uint64 version = 0;
        uint64 target = 0;
        uint64 stage = 0;
        Request request;
        const parsing::JsonValue files = root.Get("files");
        std::string_view source;
        std::string_view entry;
        StaticArray<SourceFile, MAX_SOURCE_FILES> sources{};
        bool isValid = root.Get("identity").Integer(request.Identity);
        result.Identity = request.Identity;
        isValid = isValid == true && root.Get("version").Integer(version) == true && version == PROTOCOL_VERSION &&
                  root.Get("target").Integer(target) == true && target <= static_cast<uint8>(Target::SpirvRayQuery) &&
                  root.Get("stage").Integer(stage) == true && stage <= static_cast<uint8>(Stage::Compute) &&
                  root.Get("source").String(source) == true && root.Get("entry").String(entry) == true &&
                  files.Object() == true && files.MemberCount() <= MAX_SOURCE_FILES;
        if (isValid == true)
        {
            for (usize i = 0; i < files.MemberCount(); ++i)
            {
                std::string_view path;
                std::string_view text;
                parsing::JsonValue value;
                if (files.MemberAt(i, path, value) == false || value.String(text) == false)
                {
                    isValid = false;
                    break;
                }
                sources[i] = {{path.data(), path.size()}, {text.data(), text.size()}};
            }
        }
        if (isValid == true)
        {
            request.Files = {sources.GetData(), files.MemberCount()};
            request.Source = {source.data(), source.size()};
            request.Entry = {entry.data(), entry.size()};
            request.OutputTarget = static_cast<Target>(target);
            request.EntryStage = static_cast<Stage>(stage);
            static_cast<void>(inoutCompiler.Compile(request, result));
        }
    }
    if (outBytes.TryResize(MAX_MESSAGE_BYTES) == false)
    {
        return false;
    }
    parsing::JsonWriter writer{{outBytes.GetData(), outBytes.GetSize()}};
    writer.Raw("{\"version\":");
    writer.Integer(PROTOCOL_VERSION);
    writer.Raw(",\"implementation\":\"" LUDUS_WORKER_IDENTITY "\",\"library_sha256\":\"" LUDUS_SLANG_LIBRARY_HASH "\"");
    writer.Raw(",\"library_inventory_sha256\":\"" LUDUS_SLANG_INVENTORY_HASH "\"");
    writer.Raw(",\"compiler\":\"" LUDUS_SLANG_VERSION "\",\"identity\":");
    writer.Integer(result.Identity);
    writer.Raw(",\"status\":");
    writer.Integer(static_cast<uint8>(result.Outcome));
    writer.Raw(",\"artifact\":");
    WriteHex(writer, result.Artifact);
    writer.Raw(",\"reflection\":");
    WriteHex(writer, result.Reflection);
    writer.Raw(",\"source_read_mask\":");
    WriteHex(writer, result.SourceReadMask);
    writer.Raw(",\"diagnostics\":");
    writer.String({reinterpret_cast<const char*>(result.Diagnostics.GetData()), result.Diagnostics.GetSize()});
    writer.Raw("}");
    std::span<const uint8> output;
    return writer.Finish(output) == parsing::ParseStatus::Ok && outBytes.TryResize(output.size()) == true;
}
} // namespace ludus::tooling::shader_compiler::internal
