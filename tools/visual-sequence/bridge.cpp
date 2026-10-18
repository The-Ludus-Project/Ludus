// S3's private trusted-cooker owner. Thanks to Roblox Corporation for the pinned
// "Luau API Reference", debugger/protected-call API (https://luau.org/api/);
// this reuses S2's whole-VM retirement and POD
// transaction contract. No graph interpreter or installed SDK surface is added.
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/parsing/json.hpp>
#include <ludus/foundation/parsing/parsing.hpp>

#include <cstring>
#include <span>
#include <string_view>

#include "packages.h"
#include "session.h"

#if defined(__EMSCRIPTEN__)
#    include <emscripten.h>
#else
#    include <cerrno>
#    include <csignal>
#    include <unistd.h>
#endif

namespace ludus::s3
{
using namespace foundation;
using namespace parsing;
namespace
{
constexpr usize MAX_INPUT = 655360;
struct Slot
{
    uint8 Code[262144]{};
    char Key[65]{};
    char Name[96]{};
    runtime::scripting::Program Programs[2];
    s2::Package Package;
};
// Large owned bytecode is BSS, never a browser/native stack requirement.
Slot slots[2];
const s2::Package* catalog[] = {&slots[0].Package, &slots[1].Package};
s2::Session session;
uint32 activeSlot = 0;
char reply[8192]{};
uint8 runtimeReply[4096]{};

void Id(JsonWriter& writer, uint64 value) noexcept
{
    constexpr char HEX[] = "0123456789abcdef";
    char text[16]{};
    for (usize i = 0; i < 16; ++i)
    {
        text[15 - i] = HEX[(value >> (i * 4U)) & 15U];
    }
    writer.String({text, 16});
}

bool Hex(std::string_view text, std::span<uint8> output) noexcept
{
    if (text.empty() || text.size() % 2 != 0 || text.size() / 2 > output.size())
    {
        return false;
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f'))
        {
            return false;
        }
    }
    for (usize i = 0; i < text.size() / 2; ++i)
    {
        auto digit = [](char c) noexcept { return static_cast<uint8>(c <= '9' ? c - '0' : c - 'a' + 10); };
        output[i] = static_cast<uint8>((digit(text[i * 2]) << 4U) | digit(text[i * 2 + 1]));
    }
    return true;
}

bool Load(JsonValue request) noexcept
{
    if (request.Fields({"version", "action", "artifact", "expected"}) == false)
    {
        return false;
    }
    std::string_view expected;
    if (request.Get("expected").String(expected) == false ||
        (session.Active() != nullptr && (session.SafePoint() == false || expected != session.Active()->Key)) ||
        (session.Active() == nullptr && expected.empty() == false))
    {
        return false;
    }
    const JsonValue artifact = request.Get("artifact");
    if (artifact.Fields({"version", "key", "pin", "contract", "revision", "name", "code", "first_line"}) == false)
    {
        return false;
    }
    std::string_view key;
    std::string_view pin;
    std::string_view contract;
    std::string_view name;
    std::string_view code;
    uint64 version = 0;
    uint64 revision = 0;
    uint64 first = 0;
    if (artifact.Get("version").Integer(version) == false || version != 1 || artifact.Get("key").String(key) == false ||
        key.size() != 64 || artifact.Get("pin").String(pin) == false || pin != s2::BASE.Pin ||
        artifact.Get("contract").String(contract) == false || contract != s2::BASE.Contract ||
        artifact.Get("name").String(name) == false || name.size() != 24 || name.starts_with("@visual/") == false ||
        artifact.Get("code").String(code) == false || artifact.Get("revision").Integer(revision) == false ||
        revision == 0 || revision > 2147483647 || artifact.Get("first_line").Integer(first) == false || first == 0 ||
        first > 1024 || (session.Active() != nullptr && revision <= session.Active()->Revision))
    {
        return false;
    }
    uint8 keyBytes[32]{};
    uint8 identityBytes[8]{};
    if (Hex(key, keyBytes) == false || Hex(name.substr(8), identityBytes) == false)
    {
        return false;
    }
    const uint32 candidate = session.Active() == nullptr ? 0 : 1 - activeSlot;
    Slot& slot = slots[candidate];
    if (Hex(code, slot.Code) == false || slot.Code[0] == 0)
    {
        return false;
    }
    std::memcpy(slot.Key, key.data(), key.size());
    slot.Key[key.size()] = 0;
    std::memcpy(slot.Name, name.data(), name.size());
    slot.Name[name.size()] = 0;
    for (uint32 i = 0; i < 2; ++i)
    {
        slot.Programs[i] =
        {
            .Asset = uint64{0x100} + i,
            .Revision = revision,
            .Code = slot.Code,
            .Bytes = code.size() / 2,
            .Source = slot.Name,
        };
    }
    slot.Package =
    {
        .Key = slot.Key,
        .Contract = s2::BASE.Contract,
        .Pin = s2::BASE.Pin,
        .Revision = revision,
        .Schema = 1,
        .StateMax = 10,
        .Programs = slot.Programs,
        .Count = 2,
        .FirstLine = static_cast<int32>(first),
    };
    const bool loaded = session.Active() == nullptr
                            ? session.Initialize(slot.Package, catalog)
                            : session.Prepare(slot.Package, expected) == s2::Replacement::Ready &&
                                  session.Commit(expected) == s2::Replacement::Committed;
    if (loaded)
    {
        activeSlot = candidate;
    }
    return loaded;
}

std::string_view InspectRequest() noexcept
{
    // Typed context for local owner inspection, never a caller-supplied pointer.
    static uint8 bytes[512]{};
    JsonWriter writer(bytes);
    writer.Raw("{\"version\":1,\"action\":\"inspect\",\"session\":");
    Id(writer, session.World().Session);
    writer.Raw(",\"execution\":");
    Id(writer, session.World().Execution);
    writer.Raw(",\"revision\":");
    writer.Integer(session.Active()->Revision);
    writer.Raw(",\"stop\":");
    Id(writer, session.Vm().IsPaused() ? session.Vm().Inspect().Stop : 0);
    writer.Raw("}");
    std::span<const uint8> result;
    if (writer.Finish(result) != ParseStatus::Ok)
    {
        return {};
    }
    return {reinterpret_cast<const char*>(result.data()), result.size()};
}

const char* Process(std::string_view input) noexcept
{
    bool ok = false;
    std::string_view reason = "Rejected command; active execution retained";
    JsonDocument document;
    ParseError error;
    JsonLimits limits
    {
        .MaxInputBytes = MAX_INPUT,
        .MaxWorkspaceBytes = MAX_INPUT * 4,
        .MaxDepth = 3,
        .MaxObjectMembers = 12,
        .MaxArrayElements = 0,
        .MaxValues = 32,
        .MaxStringBytes = 524288,
    };
    if (document.Read(input, error, limits) == ParseStatus::Ok)
    {
        const auto request = document.Root();
        std::string_view action;
        uint64 version = 0;
        if (request.Get("version").Integer(version) && version == 1 && request.Get("action").String(action))
        {
            if (action == "load")
            {
                ok = Load(request);
            }
            else if (action == "close" && request.Fields({"version", "action"}))
            {
                session.Close();
                LUDUS_ASSERT(session.Heap() == 0);
                ok = true;
            }
            else if (session.Active() != nullptr)
            {
                // Private conformance hook. The workbench's closed authoring
                // router does not expose it to browser requests.
                if (action == "fail_candidate" && request.Fields({"version", "action", "attempt"}))
                {
                    uint64 attempt = 0;
                    if (request.Get("attempt").Integer(attempt) && attempt <= 100000)
                    {
                        session.FailCandidateAllocation(static_cast<usize>(attempt));
                        ok = true;
                    }
                }
                else if (action == "interact" && request.Fields({"version", "action", "instance", "amount"}))
                {
                    uint64 instance = 0;
                    uint64 amount = 0;
                    if (request.Get("instance").Integer(instance) && instance < 2 &&
                        request.Get("amount").Integer(amount) && amount >= 1 && amount <= 10)
                    {
                        const auto status = session.Begin(
                            { .Instance = static_cast<uint32>(instance), .Amount = static_cast<uint32>(amount) });
                        ok = status == s2::Status::Completed || status == s2::Status::Paused;
                    }
                }
                else if (action == "debug" && request.Fields({"version", "action", "request"}))
                {
                    std::string_view debug;
                    usize size = 0;
                    if (request.Get("request").String(debug))
                    {
                        ok = session.Control(debug, runtimeReply, size);
                    }
                }
                else if (action == "inspect" && request.Fields({"version", "action"}))
                {
                    ok = true;
                }
            }
        }
    }
    JsonWriter writer({reinterpret_cast<uint8*>(reply), sizeof(reply) - 1});
    writer.Raw("{\"ok\":");
    writer.Boolean(ok);
    writer.Raw(",\"error\":");
    writer.String(ok ? "" : session.World().Faulted ? session.Last().Message : reason);
    writer.Raw(",\"runtime\":");
    usize size = 0;
    if (session.Active() != nullptr && session.Control(InspectRequest(), runtimeReply, size))
    {
        while (size != 0 && (runtimeReply[size - 1] == '\n' || runtimeReply[size - 1] == '\r'))
        {
            --size;
        }
        writer.Raw({reinterpret_cast<const char*>(runtimeReply), size});
    }
    else
    {
        writer.Raw("null");
    }
    writer.Raw(",\"open\":[");
    writer.Boolean(session.World().Open[0]);
    writer.Raw(",");
    writer.Boolean(session.World().Open[1]);
    writer.Raw("],\"effects\":[");
    for (usize i = 0; i < session.World().OutcomeCount; ++i)
    {
        if (i != 0)
        {
            writer.Raw(",");
        }
        const auto& effect = session.World().Outcomes[i];
        writer.Raw("{\"operation\":200,\"instance\":");
        Id(writer, effect.Request.Instance);
        writer.Raw(",\"slot\":");
        writer.Integer(effect.Request.Target.Slot);
        writer.Raw(",\"power\":");
        writer.Integer(effect.Request.Power);
        writer.Raw(",\"token\":");
        writer.Integer(effect.Request.Token);
        writer.Raw(",\"result\":");
        writer.String(s1::ResultName(effect.Code));
        writer.Raw("}");
    }
    writer.Raw("],\"native_result\":");
    writer.String(s1::ResultName(static_cast<s1::Result>(session.Last().NativeStatus)));
    writer.Raw(",\"heap\":");
    writer.Integer(session.Heap());
    writer.Raw("}");
    std::span<const uint8> output;
    if (writer.Finish(output) != ParseStatus::Ok)
    {
        return "{\"ok\":false,\"error\":\"Reply capacity\"}";
    }
    usize length = output.size();
    while (length != 0 && (reply[length - 1] == '\n' || reply[length - 1] == '\r'))
    {
        --length;
    }
    reply[length] = 0;
    return reply;
}
} // namespace
} // namespace ludus::s3

#if defined(__EMSCRIPTEN__)
extern "C" EMSCRIPTEN_KEEPALIVE const char* S3Control(const char* input) noexcept
{
    return ludus::s3::Process(input == nullptr ? std::string_view{} : std::string_view{input});
}
int main()
{
    return 0;
}
#else
int main()
{
    using namespace ludus::foundation;
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
        return 2;
    }
    static char bytes[ludus::s3::MAX_INPUT + 1]{};
    usize used = 0;
    while (true)
    {
        const auto received = ::read(STDIN_FILENO, bytes + used, ludus::s3::MAX_INPUT - used);
        if (received < 0 && errno == EINTR)
        {
            continue;
        }
        if (received <= 0)
        {
            break;
        }
        used += static_cast<usize>(received);
        usize start = 0;
        for (usize i = 0; i < used; ++i)
        {
            if (bytes[i] != '\n')
            {
                continue;
            }
            const char* result = ludus::s3::Process({bytes + start, i - start});
            const usize size = std::strlen(result);
            usize sent = 0;
            while (sent < size)
            {
                const auto count = ::write(STDOUT_FILENO, result + sent, size - sent);
                if (count < 0 && errno == EINTR)
                {
                    continue;
                }
                if (count <= 0)
                {
                    ludus::s3::session.Close();
                    return 2;
                }
                sent += static_cast<usize>(count);
            }
            isize newline = 0;
            do
            {
                newline = ::write(STDOUT_FILENO, "\n", 1);
            } while (newline < 0 && errno == EINTR);
            if (newline != 1)
            {
                ludus::s3::session.Close();
                return 2;
            }
            start = i + 1;
        }
        std::memmove(bytes, bytes + start, used - start);
        used -= start;
        if (used == ludus::s3::MAX_INPUT)
        {
            ludus::s3::session.Close();
            return 2;
        }
    }
    ludus::s3::session.Close();
    LUDUS_ASSERT(ludus::s3::session.Heap() == 0);
    return used == 0 ? 0 : 2;
}
#endif
