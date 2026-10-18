#include <ludus/foundation/base/core.h>
#include <ludus/foundation/parsing/json.hpp>
#include <ludus/foundation/parsing/parsing.hpp>

#include <span>
#include <string_view>

#include "packages.h"
#include "session.h"

namespace ludus::s2
{
namespace
{
using namespace parsing;
bool Hex(std::string_view text, uint64& output) noexcept
{
    if (text.size() != 16)
    {
        return false;
    }
    uint64 value = 0;
    for (const char c : text)
    {
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f'))
        {
            return false;
        }
        value = (value << 4U) | static_cast<uint64>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    output = value;
    return true;
}
void Id(JsonWriter& writer, uint64 value) noexcept
{
    constexpr char HEX[] = "0123456789abcdef";
    char bytes[16] = {};
    for (usize i = 0; i < 16; ++i)
    {
        bytes[15 - i] = HEX[(value >> (i * 4)) & 15U];
    }
    writer.String({bytes, sizeof(bytes)});
}
} // namespace

bool Session::Control(std::string_view input, std::span<uint8> output, usize& written) noexcept
{
    if (mActive == nullptr || input.empty() || input.size() > 4096 || output.size() < 4096)
    {
        return false;
    }
    JsonDocument document;
    ParseError error;
    JsonLimits limits
    {
        .MaxInputBytes = 4096,
        .MaxWorkspaceBytes = 65536,
        .MaxDepth = 1,
        .MaxObjectMembers = 12,
        .MaxArrayElements = 0,
        .MaxValues = 12,
        .MaxStringBytes = 128,
    };
    if (document.Read(input, error, limits) != ParseStatus::Ok)
    {
        return false;
    }
    const JsonValue request = document.Root();
    std::string_view action;
    std::string_view sessionText;
    std::string_view executionText;
    std::string_view stopText;
    uint64 version = 0;
    uint64 revision = 0;
    uint64 session = 0;
    uint64 execution = 0;
    uint64 stop = 0;
    if (request.Get("version").Integer(version) == false || version != 1 ||
        request.Get("action").String(action) == false || request.Get("session").String(sessionText) == false ||
        request.Get("execution").String(executionText) == false || request.Get("stop").String(stopText) == false ||
        request.Get("revision").Integer(revision) == false || Hex(sessionText, session) == false ||
        Hex(executionText, execution) == false || Hex(stopText, stop) == false || session != mWorld.Session ||
        execution != mWorld.Execution || revision != mActive->Revision ||
        stop != (Vm().IsPaused() ? Vm().Inspect().Stop : 0))
    {
        return false;
    }
    int32 resolved = -1;
    if (action == "breakpoint")
    {
        if (request.Fields(
                {"version", "action", "session", "execution", "revision", "stop", "asset", "line", "enabled"}) == false)
        {
            return false;
        }
        std::string_view assetText;
        uint64 asset = 0;
        uint64 line = 0;
        bool enabled = false;
        if (request.Get("asset").String(assetText) == false || Hex(assetText, asset) == false ||
            request.Get("line").Integer(line) == false || line == 0 || line > 65536 ||
            request.Get("enabled").Boolean(enabled) == false)
        {
            return false;
        }
        resolved = Breakpoint({ .Asset = asset, .Line = static_cast<int32>(line), .Enabled = enabled });
        if (resolved < 0)
        {
            return false;
        }
    }
    else if (action == "reload")
    {
        if (request.Fields({"version", "action", "session", "execution", "revision", "stop", "package", "expected"}) ==
            false)
        {
            return false;
        }
        std::string_view name;
        std::string_view expected;
        if (request.Get("package").String(name) == false || request.Get("expected").String(expected) == false)
        {
            return false;
        }
        const Package* package = name == "replacement" ? &REPLACEMENT
                                 : name == "failed"    ? &FAILED
                                 : name == "narrow"    ? &NARROW
                                                       : nullptr;
        if (package == nullptr || Prepare(*package, expected) != Replacement::Ready ||
            Commit(expected) != Replacement::Committed)
        {
            return false;
        }
    }
    else
    {
        if (request.Fields({"version", "action", "session", "execution", "revision", "stop"}) == false)
        {
            return false;
        }
        if (action != "inspect")
        {
            if (Vm().IsPaused() == false)
            {
                return false;
            }
            const ResumeMode mode = action == "continue" ? ResumeMode::Continue
                                    : action == "into"   ? ResumeMode::Into
                                    : action == "over"   ? ResumeMode::Over
                                                         : ResumeMode::Out;
            if (action != "continue" && action != "into" && action != "over" && action != "out")
            {
                return false;
            }
            const Status status = Resume(mode);
            if (status != Status::Completed && status != Status::Paused && mWorld.Faulted == false)
            {
                return false;
            }
        }
    }
    JsonWriter writer(output.first(4096));
    writer.Raw("{\"version\":1,\"provider\":\"luau\",\"session\":");
    Id(writer, mWorld.Session);
    writer.Raw(",\"execution\":");
    Id(writer, mWorld.Execution);
    writer.Raw(",\"revision\":");
    writer.Integer(mActive->Revision);
    writer.Raw(",\"package\":");
    writer.String(mActive->Key);
    writer.Raw(",\"stop\":");
    Id(writer, Vm().IsPaused() ? Vm().Inspect().Stop : 0);
    writer.Raw(",\"partial\":");
    writer.Boolean(mPending);
    writer.Raw(",\"faulted\":");
    writer.Boolean(mWorld.Faulted);
    writer.Raw(",\"tick\":");
    Id(writer, mWorld.Tick);
    writer.Raw(",\"operation\":");
    writer.Integer(mLast.Operation);
    writer.Raw(",\"native_status\":");
    writer.Integer(mLast.NativeStatus);
    writer.Raw(",\"asset\":");
    Id(writer, mLast.Source.Asset);
    writer.Raw(",\"instance\":");
    Id(writer, mLast.Source.Instance);
    writer.Raw(",\"world\":");
    Id(writer, mWorld.Entities.GetWorld());
    writer.Raw(",\"phase\":");
    writer.Integer(static_cast<uint8>(mWorld.At));
    writer.Raw(",\"resolved_line\":");
    writer.Number(resolved);
    writer.Raw(",\"states\":[");
    for (uint32 i = 0; i < 2; ++i)
    {
        if (i != 0)
        {
            writer.Raw(",");
        }
        writer.Raw("{\"interactions\":");
        writer.Integer(mWorld.States[i].Interactions);
        writer.Raw(",\"open_requested\":");
        writer.Boolean(mWorld.States[i].OpenRequested);
        writer.Raw("}");
    }
    writer.Raw("]");
    if (action == "inspect" && Vm().IsPaused())
    {
        const auto& snapshot = Vm().Inspect();
        writer.Raw(",\"truncated\":");
        writer.Boolean(snapshot.Truncated || snapshot.FrameCount > 4 || snapshot.LocalCount > 8);
        writer.Raw(",\"frames\":[");
        for (uint32 i = 0; i < snapshot.FrameCount && i < 4; ++i)
        {
            if (i != 0)
            {
                writer.Raw(",");
            }
            writer.Raw("{\"source\":");
            writer.String(snapshot.Frames[i].Source);
            writer.Raw(",\"function\":");
            writer.String(snapshot.Frames[i].Function);
            writer.Raw(",\"compiled_line\":");
            writer.Number(snapshot.Frames[i].Line);
            writer.Raw(",\"authored_line\":");
            writer.Number(snapshot.Frames[i].Line - mActive->FirstLine + 1);
            writer.Raw("}");
        }
        writer.Raw("],\"locals\":[");
        for (uint32 i = 0; i < snapshot.LocalCount && i < 8; ++i)
        {
            if (i != 0)
            {
                writer.Raw(",");
            }
            const auto& value = snapshot.Locals[i];
            writer.Raw("{\"name\":");
            writer.String(value.Name);
            writer.Raw(",\"kind\":");
            writer.Integer(static_cast<uint8>(value.Kind));
            writer.Raw(",\"number\":");
            writer.Number(value.Kind == runtime::scripting::DebugKind::Number ? value.Number : 0);
            writer.Raw(",\"boolean\":");
            writer.Boolean(value.Boolean);
            writer.Raw(",\"bytes_hex\":");
            writer.String(value.Bytes);
            writer.Raw(",\"truncated\":");
            writer.Boolean(value.Truncated);
            writer.Raw("}");
        }
        writer.Raw("]");
    }
    writer.Raw("}");
    std::span<const uint8> result;
    if (writer.Finish(result) != ParseStatus::Ok)
    {
        return false;
    }
    written = result.size();
    return true;
}
} // namespace ludus::s2
