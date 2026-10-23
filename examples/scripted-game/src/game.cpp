// Original installed-SDK example. A generation owns its provider, checkpoint,
// safe debug copies and generated maps. GameHost's prepared reload protocol
// retires Destroy/VM callbacks before releasing the native module lease.
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/core.h>
#if !defined(LUDUS_GAME_STATIC_DISPATCH)
#    include <ludus/foundation/parsing/json.hpp>
#endif
#include <ludus/runtime/behavior/behavior.h>
#include <ludus/runtime/game_api/api.h>
#include <ludus/runtime/game_api/services.h>

#include <cstring>
#include <new>
#include <span>
#include <string_view>

#include "contract.h"
#if !defined(LUDUS_GAME_STATIC_DISPATCH)
#    include "debug_maps.h"
#endif
#include "package.h"

namespace
{
using namespace ludus::foundation;
using namespace ludus::runtime::game_api;
namespace behavior = ludus::runtime::behavior;
namespace project = ludus::sample;
/// Checkpoint body layout: tick, applied, previous actions and execution as
/// little-endian uint64 values, then the encoded behavior state.
constexpr usize CHECKPOINT_APPLIED_OFFSET = 8;
constexpr usize CHECKPOINT_PREVIOUS_ACTIONS_OFFSET = 16;
constexpr usize CHECKPOINT_EXECUTION_OFFSET = 24;
constexpr usize CHECKPOINT_STATE_OFFSET = 32;
#if !defined(LUDUS_GAME_STATIC_DISPATCH)
/// Highest script line a breakpoint request may name.
constexpr uint64 MAX_BREAKPOINT_LINE = 65536;
/// Largest amount one interact request may apply.
constexpr uint64 MAX_INTERACT_AMOUNT = 10;
#endif
struct Owner
{
    behavior::LuauProvider Scripts;
    CreateInfo Info;
    behavior::Record State;
    behavior::Diagnostic Last;
    uint64 Execution = 0;
    uint64 Tick = 0;
    uint64 Applied = 0;
    uint64 PreviousActions = 0;
    bool Quiesced = false;
};
Owner* AsOwner(GameInstance* instance) noexcept
{
    return reinterpret_cast<Owner*>(instance);
}
bool Alive(void*, behavior::EntityRef ref) noexcept
{
    return ref.Slot == 0 && ref.Generation == 1;
}
#if !defined(LUDUS_GAME_STATIC_DISPATCH)
void Id(parsing::JsonWriter& writer, uint64 value) noexcept
{
    constexpr char HEX[] = "0123456789abcdef";
    char bytes[16] = {};
    for (usize i = 0; i < 16; ++i)
    {
        bytes[15 - i] = HEX[(value >> (i * 4)) & 15U];
    }
    writer.String({bytes, sizeof(bytes)});
}
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
#endif
// Thanks to Glenn Fowler, Landon Curt Noll and Phong Vo, "FNV Hash",
// "FNV-1a alternate algorithm" and "Parameters of the FNV-1/FNV-1a hash":
// https://www.isthe.com/chongo/tech/comp/fnv/ . Adopt the XOR/multiply sequence
// and published constants for a non-cryptographic hash; collisions remain possible.
uint64 Digest(ByteView bytes) noexcept
{
    uint64 value = 14695981039346656037ULL;
    for (usize i = 0; i < bytes.Size; ++i)
    {
        value = (value ^ bytes.Data[i]) * 1099511628211ULL;
    }
    return value;
}
Status Query(GameMetadata* out) noexcept
{
    if (out == nullptr)
    {
        return Status::InvalidArgument;
    }
    *out =
    {
        .StructSize = sizeof(GameMetadata),
        .AbiMajor = kAbiMajor,
        .AbiMinor = kAbiMinor,
        .Capabilities = static_cast<uint32>(Capability::Reload)
#if !defined(LUDUS_GAME_STATIC_DISPATCH)
                        | static_cast<uint32>(Capability::ScriptDebug)
#endif
            ,
        .CheckpointSchemaVersion = 1,
        .IdentityLength = sizeof(LUDUS_EXAMPLE_IDENTITY) - 1,
    };
    std::memcpy(out->Identity, LUDUS_EXAMPLE_IDENTITY, out->IdentityLength);
    return Status::Ok;
}
Owner* Make(const CreateInfo* info) noexcept
{
    if (info == nullptr || info->StructSize < sizeof(CreateInfo) || info->ModuleGeneration == 0)
    {
        return nullptr;
    }
    auto* owner = new (std::nothrow) Owner;
    if (owner == nullptr)
    {
        return nullptr;
    }
    owner->Info = *info;
    owner->Info.AuthoredDocument = {};
    if (owner->Info.ProjectId == 0)
    {
        owner->Info.ProjectId = 1;
    }
    if (owner->Info.GameId == 0)
    {
        owner->Info.GameId = 1;
    }
    owner->Execution = info->ModuleGeneration;
    owner->State = project::MakeState({});
    if (owner->Scripts.Load(project::SCHEMA, project::PACKAGE) != behavior::Status::Completed)
    {
        delete owner;
        return nullptr;
    }
    return owner;
}
Status Create(const CreateInfo* info, GameInstance** out) noexcept
{
    if (out == nullptr)
    {
        return Status::InvalidArgument;
    }
    auto* owner = Make(info);
    if (owner == nullptr)
    {
        return Status::Internal;
    }
    *out = reinterpret_cast<GameInstance*>(owner);
    return Status::Ok;
}
void Destroy(GameInstance* instance) noexcept
{
    auto* owner = AsOwner(instance);
    if (owner == nullptr)
    {
        return;
    }
    (void)owner->Scripts.Close();
    LUDUS_REQUIRE(owner->Scripts.LiveBytes() == 0);
    if (owner->Info.Services != nullptr && owner->Info.Services->Log != nullptr)
    {
        constexpr char MESSAGE[] = "S5 behavior VM retired before native module lease release";
        owner->Info.Services->Log(owner->Info.Services->Context, LogSeverity::Info, MESSAGE, sizeof(MESSAGE) - 1);
    }
    delete owner;
}
Status Update(GameInstance* instance, const FrameInput* input, RenderParams* out) noexcept
{
    if (instance == nullptr || input == nullptr || out == nullptr)
    {
        return Status::InvalidArgument;
    }
    auto& owner = *AsOwner(instance);
    behavior::DebugSnapshot snapshot;
    if (owner.Scripts.Inspect(snapshot) && snapshot.Paused)
    {
        *out = {};
        return Status::ScriptPaused;
    }
    const bool interact = (input->HeldActions & 4U) != 0 && (owner.PreviousActions & 4U) == 0;
    owner.PreviousActions = input->HeldActions;
    if (interact && owner.Quiesced == false)
    {
        behavior::Invocation call
        {
            .Asset = 0x100,
            .Revision = 1,
            .Instance = 1,
            .World = owner.Info.ProjectId == 0 ? 1 : owner.Info.ProjectId,
            .Session = owner.Info.GameId == 0 ? 1 : owner.Info.GameId,
            .Execution = owner.Execution,
            .Tick = owner.Tick + 1,
            .Phase = 3,
            .Capabilities = 1,
        };
        call.Config = project::MakeConfig({});
        call.State = owner.State;
        call.Event = project::MakeEvent({ .Target = {call.World, call.Session, call.Execution, 0, 1}, .Amount = 1 });
        behavior::Outcome outcome;
        // The same input path uses real break/step in an editor generation and
        // the synchronous provider in the statically linked shipping player.
#if defined(LUDUS_GAME_STATIC_DISPATCH)
        const auto result = owner.Scripts.Invoke(call, {nullptr, Alive}, outcome, owner.Last);
#else
        const auto result = owner.Scripts.BeginDebug(call, {nullptr, Alive}, outcome, owner.Last);
#endif
        if (result == behavior::Status::Completed)
        {
            owner.State = outcome.State;
            owner.Applied += outcome.Count;
            ++owner.Tick;
        }
        else if (result == behavior::Status::Paused)
        {
            *out = {};
            return Status::ScriptPaused;
        }
        else
        {
            return Status::Internal;
        }
    }
    *out =
    {
        .ClearRed = owner.Applied == 0 ? 0.3F : 0.0F,
        .ClearGreen = owner.Applied == 0 ? 0.0F : 0.6F,
        .ClearBlue = 0.15F,
        .ClearAlpha = 1.0F,
    };
    return Status::Ok;
}
Status Quiesce(GameInstance* instance) noexcept
{
    if (instance == nullptr)
    {
        return Status::InvalidArgument;
    }
    AsOwner(instance)->Quiesced = true;
    return Status::Ok;
}
Status Resume(GameInstance* instance) noexcept
{
    if (instance == nullptr)
    {
        return Status::InvalidArgument;
    }
    AsOwner(instance)->Quiesced = false;
    return Status::Ok;
}
Status Size(GameInstance* instance, usize* out) noexcept
{
    if (instance == nullptr || out == nullptr)
    {
        return Status::InvalidArgument;
    }
    behavior::DebugSnapshot snapshot;
    if (AsOwner(instance)->Scripts.Inspect(snapshot) == false || snapshot.Paused)
    {
        return Status::MigrationUnsupported;
    }
    uint8 bytes[256] = {};
    usize written = 0;
    if (behavior::EncodeState(project::SCHEMA, AsOwner(instance)->State, bytes, written) == false)
    {
        return Status::Internal;
    }
    *out = CHECKPOINT_STATE_OFFSET + written;
    return Status::Ok;
}
Status Write(GameInstance* instance, CheckpointHeader* header, ByteSpan body, usize* written) noexcept
{
    if (instance == nullptr || header == nullptr || body.Data == nullptr || written == nullptr ||
        body.Capacity < CHECKPOINT_STATE_OFFSET)
    {
        return Status::InvalidArgument;
    }
    auto& owner = *AsOwner(instance);
    usize encoded = 0;
    auto bytes = std::span<uint8>(body.Data, body.Capacity);
    if (behavior::EncodeState(project::SCHEMA, owner.State, bytes.subspan(CHECKPOINT_STATE_OFFSET), encoded) == false)
    {
        return Status::Internal;
    }
    (void)TryWriteLittleEndian(owner.Tick, bytes);
    (void)TryWriteLittleEndian(owner.Applied, bytes.subspan(CHECKPOINT_APPLIED_OFFSET));
    (void)TryWriteLittleEndian(owner.PreviousActions, bytes.subspan(CHECKPOINT_PREVIOUS_ACTIONS_OFFSET));
    (void)TryWriteLittleEndian(owner.Execution, bytes.subspan(CHECKPOINT_EXECUTION_OFFSET));
    *written = encoded + CHECKPOINT_STATE_OFFSET;
    *header =
    {
        .FormatVersion = 1,
        .SchemaVersion = 1,
        .ProjectId = owner.Info.ProjectId,
        .GameId = owner.Info.GameId,
        .ModuleBuildId = owner.Info.ModuleGeneration,
        .BodyLength = *written,
        .BodyDigest = Digest({body.Data, *written}),
    };
    return Status::Ok;
}
Status Candidate(const CreateInfo* info, const CheckpointHeader* header, ByteView body, GameCandidate** out) noexcept
{
    if (info == nullptr || header == nullptr || out == nullptr || body.Data == nullptr ||
        body.Size < CHECKPOINT_STATE_OFFSET || header->FormatVersion != 1 || header->SchemaVersion != 1 ||
        header->ProjectId != info->ProjectId || header->GameId != info->GameId || header->BodyLength != body.Size ||
        header->BodyDigest != Digest(body))
    {
        return Status::InvalidArgument;
    }
    auto* owner = Make(info);
    if (owner == nullptr)
    {
        return Status::Internal;
    }
    const auto bytes = std::span<const uint8>(body.Data, body.Size);
    uint64 previous_execution = 0;
    if (behavior::DecodeState(project::SCHEMA, bytes.subspan(CHECKPOINT_STATE_OFFSET), owner->State) == false ||
        TryReadLittleEndian(bytes, owner->Tick) == false ||
        TryReadLittleEndian(bytes.subspan(CHECKPOINT_APPLIED_OFFSET), owner->Applied) == false ||
        TryReadLittleEndian(bytes.subspan(CHECKPOINT_PREVIOUS_ACTIONS_OFFSET), owner->PreviousActions) == false ||
        TryReadLittleEndian(bytes.subspan(CHECKPOINT_EXECUTION_OFFSET), previous_execution) == false ||
        previous_execution == ~uint64{0})
    {
        Destroy(reinterpret_cast<GameInstance*>(owner));
        return Status::MigrationUnsupported;
    }
    // Preserve the execution namespace across module replacement, including
    // earlier VM restarts, so their stale cursors cannot alias the candidate.
    if (owner->Execution <= previous_execution)
    {
        owner->Execution = previous_execution + 1;
    }
    owner->Quiesced = true;
    *out = reinterpret_cast<GameCandidate*>(owner);
    return Status::Ok;
}
Status Validate(GameCandidate* candidate) noexcept
{
    return candidate == nullptr ? Status::InvalidArgument : Status::Ok;
}
Status Commit(GameCandidate* candidate, GameInstance** out) noexcept
{
    if (candidate == nullptr || out == nullptr)
    {
        return Status::InvalidArgument;
    }
    *out = reinterpret_cast<GameInstance*>(candidate);
    return Status::Ok;
}
void Discard(GameCandidate* candidate) noexcept
{
    Destroy(reinterpret_cast<GameInstance*>(candidate));
}
#if !defined(LUDUS_GAME_STATIC_DISPATCH)
// Closed ABI 1.1 debugger vocabulary. Bytecode/source are never accepted here;
// code and node maps belong to the leased, paired-cooked native generation.
Status Control(GameInstance* instance, ByteView bytes, ByteSpan response, usize* written, uint32* paused) noexcept
{
    if (instance == nullptr || bytes.Data == nullptr || bytes.Size > 4096 || response.Capacity < 4096 ||
        response.Data == nullptr || written == nullptr || paused == nullptr)
    {
        return Status::InvalidArgument;
    }
    auto& owner = *AsOwner(instance);
    behavior::DebugSnapshot snapshot;
    if (owner.Scripts.Inspect(snapshot) == false)
    {
        return Status::InvalidArgument;
    }
    parsing::JsonDocument document;
    parsing::ParseError error;
    parsing::JsonLimits limits
    {
        .MaxInputBytes = 4096,
        .MaxWorkspaceBytes = 65536,
        .MaxDepth = 1,
        .MaxObjectMembers = 10,
        .MaxArrayElements = 0,
        .MaxValues = 12,
        .MaxStringBytes = 128,
    };
    if (document.Read({reinterpret_cast<const char*>(bytes.Data), bytes.Size}, error, limits) !=
        parsing::ParseStatus::Ok)
    {
        return Status::InvalidArgument;
    }
    const auto request = document.Root();
    std::string_view action;
    uint64 version = 0;
    if (request.Get("version").Integer(version) == false || version != 1 ||
        request.Get("action").String(action) == false)
    {
        return Status::InvalidArgument;
    }
    if (action == "inspect")
    {
        if (request.Fields({"version", "action"}) == false)
        {
            return Status::InvalidArgument;
        }
    }
    else
    {
        std::string_view session_text, execution_text, stop_text;
        uint64 session = 0, execution = 0, stop = 0;
        if (request.Get("session").String(session_text) == false ||
            request.Get("execution").String(execution_text) == false ||
            request.Get("stop").String(stop_text) == false || Hex(session_text, session) == false ||
            Hex(execution_text, execution) == false || Hex(stop_text, stop) == false || session != owner.Info.GameId ||
            execution != owner.Execution || stop != snapshot.Stop)
        {
            return Status::InvalidArgument;
        }
        if (action == "breakpoint")
        {
            uint64 asset = 0, line = 0;
            std::string_view asset_text;
            bool enabled = false;
            if (request.Fields({"version", "action", "session", "execution", "stop", "asset", "line", "enabled"}) ==
                    false ||
                request.Get("asset").String(asset_text) == false || Hex(asset_text, asset) == false ||
                request.Get("line").Integer(line) == false || line == 0 || line > MAX_BREAKPOINT_LINE ||
                request.Get("enabled").Boolean(enabled) == false ||
                owner.Scripts.Breakpoint(asset, static_cast<int32>(line), enabled) < 0)
            {
                return Status::InvalidArgument;
            }
        }
        else if (action == "restart")
        {
            if (request.Fields({"version", "action", "session", "execution", "stop"}) == false ||
                owner.Execution == ~uint64{0})
            {
                return Status::InvalidArgument;
            }
            (void)owner.Scripts.Close();
            ++owner.Execution;
            owner.State = project::MakeState({});
            owner.Tick = owner.Applied = owner.PreviousActions = 0;
            if (owner.Scripts.Load(project::SCHEMA, project::PACKAGE) != behavior::Status::Completed)
            {
                return Status::Internal;
            }
        }
        else
        {
            behavior::Outcome outcome;
            behavior::Status status = behavior::Status::InvalidInput;
            if (action == "interact")
            {
                uint64 asset = 0, amount = 0;
                std::string_view asset_text;
                if (request.Fields({"version", "action", "session", "execution", "stop", "asset", "amount"}) == false ||
                    snapshot.Paused || owner.Quiesced || request.Get("asset").String(asset_text) == false ||
                    Hex(asset_text, asset) == false || request.Get("amount").Integer(amount) == false || amount == 0 ||
                    amount > MAX_INTERACT_AMOUNT)
                {
                    return Status::InvalidArgument;
                }
                behavior::Invocation input
                {
                    .Asset = asset,
                    .Revision = 1,
                    .Instance = 1,
                    .World = owner.Info.ProjectId,
                    .Session = owner.Info.GameId,
                    .Execution = owner.Execution,
                    .Tick = owner.Tick + 1,
                    .Phase = 3,
                    .Capabilities = 1,
                };
                input.Config = project::MakeConfig({});
                input.State = owner.State;
                input.Event = project::MakeEvent(
                {
                    .Target = {input.World, input.Session, input.Execution, 0, 1},
                    .Amount = static_cast<uint32>(amount),
                });
                status = owner.Scripts.BeginDebug(input, {nullptr, Alive}, outcome, owner.Last);
            }
            else
            {
                if (request.Fields({"version", "action", "session", "execution", "stop"}) == false ||
                    (action != "continue" && action != "into" && action != "over" && action != "out"))
                {
                    return Status::InvalidArgument;
                }
                const auto mode = action == "continue" ? behavior::DebugMode::Continue
                                  : action == "into"   ? behavior::DebugMode::Into
                                  : action == "over"   ? behavior::DebugMode::Over
                                                       : behavior::DebugMode::Out;
                status = owner.Scripts.ResumeDebug(stop, mode, outcome, owner.Last);
            }
            if (status == behavior::Status::Completed)
            {
                owner.State = outcome.State;
                owner.Applied += outcome.Count;
                ++owner.Tick;
            }
            else if (status != behavior::Status::Paused && status != behavior::Status::ScriptFault &&
                     status != behavior::Status::Interrupted && status != behavior::Status::AllocationFailure)
            {
                return Status::InvalidArgument;
            }
        }
    }
    (void)owner.Scripts.Inspect(snapshot);
    *paused = snapshot.Paused ? 1U : 0U;
    parsing::JsonWriter writer({response.Data, response.Capacity});
    writer.Raw("{\"version\":1,\"provider\":\"luau\",\"session\":");
    Id(writer, owner.Info.GameId);
    writer.Raw(",\"package\":");
    writer.String(project::PACKAGE_KEY);
    writer.Raw(",\"execution\":");
    Id(writer, owner.Execution);
    writer.Raw(",\"stop\":");
    Id(writer, snapshot.Stop);
    writer.Raw(",\"partial\":");
    writer.Boolean(snapshot.Paused);
    writer.Raw(",\"tick\":");
    Id(writer, owner.Tick);
    writer.Raw(",\"applied\":");
    writer.Integer(owner.Applied);
    writer.Raw(",\"interactions\":");
    writer.Integer(owner.State.Items[0].Data.Scalar);
    writer.Raw(",\"faulted\":");
    writer.Boolean(owner.Last.Code == behavior::Status::ScriptFault || owner.Scripts.LiveBytes() == 0);
    writer.Raw(",\"diagnostic\":");
    writer.String(owner.Last.Message);
    writer.Raw(",\"frames\":[");
    for (uint32 i = 0; i < snapshot.FrameCount && i < 4; ++i)
    {
        if (i != 0)
        {
            writer.Raw(",");
        }
        writer.Raw("{\"source\":");
        writer.String(snapshot.Frames[i].Source);
        writer.Raw(",\"line\":");
        writer.Number(static_cast<float64>(snapshot.Frames[i].Line));
        writer.Raw(",\"node\":");
        std::string_view node;
        uint64 asset = 0;
        std::string_view source(snapshot.Frames[i].Source);
        if (source.starts_with('@') == true)
        {
            source.remove_prefix(1);
        }
        if (source.starts_with("asset/") && Hex(source.substr(6), asset))
        {
            for (const auto& span : project::SOURCE_SPANS)
            {
                if (span.Asset == asset && span.Line == snapshot.Frames[i].Line)
                {
                    node = span.Node;
                    break;
                }
            }
        }
        writer.String(node);
        writer.Raw("}");
    }
    writer.Raw("],\"locals\":[");
    for (uint32 i = 0; i < snapshot.LocalCount && i < 8; ++i)
    {
        if (i != 0)
        {
            writer.Raw(",");
        }
        writer.Raw("{\"name\":");
        writer.String(snapshot.Locals[i].Name);
        writer.Raw(",\"kind\":");
        writer.Integer(snapshot.Locals[i].Kind);
        writer.Raw(",\"number\":");
        writer.Number(snapshot.Locals[i].Number);
        writer.Raw(",\"boolean\":");
        writer.Boolean(snapshot.Locals[i].Boolean);
        writer.Raw(",\"bytes\":");
        writer.String(snapshot.Locals[i].Bytes);
        writer.Raw("}");
    }
    writer.Raw("]}");
    std::span<const uint8> reply;
    if (writer.Finish(reply) != parsing::ParseStatus::Ok)
    {
        return Status::Internal;
    }
    *written = reply.size();
    return Status::Ok;
}
#endif
} // namespace
// NOLINTBEGIN(bugprone-easily-swappable-parameters) -- fixed ABI signature
extern "C" LUDUS_GAME_API_EXPORT ludus::runtime::game_api::Status
LudusGetGameApi(ludus::foundation::uint32 hostAbiMajor,
                ludus::foundation::uint32 hostAbiMinor,
                ludus::runtime::game_api::GameApiTable* outTable) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    using namespace ludus::runtime::game_api;
    if (outTable == nullptr || outTable->StructSize < sizeof(GameApiTable) || hostAbiMajor != kAbiMajor ||
        hostAbiMinor < 1)
    {
        return Status::IncompatibleAbi;
    }
    *outTable =
    {
        .StructSize = sizeof(GameApiTable),
        .AbiMajor = kAbiMajor,
        .AbiMinor = kAbiMinor,
        .Query = Query,
        .Create = Create,
        .Destroy = Destroy,
        .Update = Update,
        .Quiesce = Quiesce,
        .Resume = Resume,
        .CheckpointSize = Size,
        .WriteCheckpoint = Write,
        .CreateCandidate = Candidate,
        .ValidateCandidate = Validate,
        .CommitCandidate = Commit,
        .DiscardCandidate = Discard,
#if !defined(LUDUS_GAME_STATIC_DISPATCH)
        .ProcessScriptDebug = Control,
#endif
    };
    return Status::Ok;
}
