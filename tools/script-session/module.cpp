// Original S2 ownership fixture: a VM is an instance-owned resource. Thanks to
// Ludus's GameHost prepared reload protocol (§7/8); Destroy closes both banks
// before dlclose. No VM callback or worker escapes this synchronous owner.
#include <ludus/foundation/base/core.h>
#include <ludus/runtime/game_api/api.h>
#include <ludus/runtime/game_api/services.h>

#include <cstring>
#include <new>
#include <span>
#include <string_view>

#include "packages.h"
#include "session.h"

namespace
{
using namespace ludus::foundation;
using namespace ludus::runtime::game_api;
using ludus::s2::Session;
struct Owner
{
    Session Scripts;
    CreateInfo Info;
    bool Quiesced = false;
    bool Armed = false;
};
Owner* AsOwner(GameInstance* instance) noexcept
{
    return reinterpret_cast<Owner*>(instance);
}
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
        .Capabilities = static_cast<uint32>(Capability::Reload) | static_cast<uint32>(Capability::ScriptDebug),
        .CheckpointSchemaVersion = 2,
        .IdentityLength = sizeof(LUDUS_FIXTURE_IDENTITY) - 1,
    };
    std::memcpy(out->Identity, LUDUS_FIXTURE_IDENTITY, out->IdentityLength);
    return Status::Ok;
}
void Destroy(GameInstance* instance) noexcept
{
    auto* owner = AsOwner(instance);
    if (owner == nullptr)
    {
        return;
    }
    owner->Scripts.Close();
    LUDUS_ASSERT(owner->Scripts.Heap() == 0);
    if (owner->Info.Services != nullptr && owner->Info.Services->Log != nullptr)
    {
        constexpr char MESSAGE[] = "S2 VM retired before native module release";
        owner->Info.Services->Log(owner->Info.Services->Context, LogSeverity::Info, MESSAGE, sizeof(MESSAGE) - 1);
    }
    delete owner;
}
Owner* Make(const CreateInfo* info, const ludus::s2::Package& package) noexcept
{
    if (info == nullptr || info->StructSize < sizeof(CreateInfo) || info->ModuleGeneration == 0 ||
        info->ModuleGeneration > 10000)
    {
        return nullptr;
    }
    auto* owner = new (std::nothrow) Owner;
    if (owner == nullptr)
    {
        return nullptr;
    }
    owner->Info = *info;
    // AuthoredDocument is borrowed by Create; never retain it.
    owner->Info.AuthoredDocument = {};
    if (owner->Scripts.Initialize(package) == false)
    {
        delete owner;
        return nullptr;
    }
    owner->Scripts.World().Execution += info->ModuleGeneration - 1;
    return owner;
}
Status Create(const CreateInfo* info, GameInstance** out) noexcept
{
    if (out == nullptr)
    {
        return Status::InvalidArgument;
    }
    auto* owner = Make(info, ludus::s2::BASE);
    if (owner == nullptr)
    {
        return Status::Internal;
    }
    *out = reinterpret_cast<GameInstance*>(owner);
    return Status::Ok;
}
Status Update(GameInstance* instance, const FrameInput* /*input*/, RenderParams* out) noexcept
{
    auto* owner = AsOwner(instance);
    if (owner == nullptr || out == nullptr)
    {
        return Status::InvalidArgument;
    }
    *out = {};
    if (owner->Scripts.World().Faulted)
    {
        return Status::Internal;
    }
    if (owner->Scripts.Pending())
    {
        return Status::ScriptPaused;
    }
    if (owner->Quiesced == false && owner->Armed)
    {
        owner->Armed = false;
        const auto status = owner->Scripts.Begin();
        if (status == ludus::runtime::scripting::Status::Paused)
        {
            return Status::ScriptPaused;
        }
        if (status != ludus::runtime::scripting::Status::Completed)
        {
            return Status::Internal;
        }
    }
    return Status::Ok;
}
Status Quiesce(GameInstance* instance) noexcept
{
    if (instance == nullptr)
    {
        return Status::InvalidArgument;
    }
    AsOwner(instance)->Quiesced = true; // Keep a paused invocation owned and resumable until Destroy.
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
    if (instance == nullptr || out == nullptr || AsOwner(instance)->Scripts.SafePoint() == false)
    {
        return Status::InvalidArgument;
    }
    *out = 168;
    return Status::Ok;
}
Status Write(GameInstance* instance, CheckpointHeader* header, ByteSpan body, usize* written) noexcept
{
    if (instance == nullptr || header == nullptr || body.Data == nullptr || written == nullptr)
    {
        return Status::InvalidArgument;
    }
    auto* owner = AsOwner(instance);
    if (owner->Scripts.Checkpoint({body.Data, body.Capacity}, *written) == false)
    {
        return Status::Internal;
    }
    *header =
    {
        .FormatVersion = 1,
        .SchemaVersion = 2,
        .ProjectId = owner->Info.ProjectId,
        .GameId = owner->Info.GameId,
        .ModuleBuildId = owner->Info.ModuleGeneration,
        .BodyLength = *written,
        .BodyDigest = Digest({body.Data, *written}),
    };
    return Status::Ok;
}
Status Candidate(const CreateInfo* info, const CheckpointHeader* header, ByteView body, GameCandidate** out) noexcept
{
    if (info == nullptr || header == nullptr || out == nullptr || body.Data == nullptr || body.Size != 168 ||
        header->FormatVersion != 1 || header->SchemaVersion != 2 || header->ProjectId != info->ProjectId ||
        header->GameId != info->GameId || header->BodyLength != body.Size || header->BodyDigest != Digest(body))
    {
        return Status::InvalidArgument;
    }
    auto* owner = Make(info, ludus::s2::REPLACEMENT);
    if (owner == nullptr)
    {
        return Status::Internal;
    }
    if (owner->Scripts.Restore({body.Data, body.Size}) == false)
    {
        Destroy(reinterpret_cast<GameInstance*>(owner));
        return Status::MigrationUnsupported;
    }
    owner->Quiesced = true;
    *out = reinterpret_cast<GameCandidate*>(owner);
    return Status::Ok;
}
Status Validate(GameCandidate* candidate) noexcept
{
    return candidate != nullptr && reinterpret_cast<Owner*>(candidate)->Scripts.SafePoint() ? Status::Ok
                                                                                            : Status::InvalidArgument;
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
Status Control(GameInstance* instance, ByteView request, ByteSpan response, usize* written, uint32* paused) noexcept
{
    if (instance == nullptr || request.Data == nullptr || response.Data == nullptr || written == nullptr ||
        paused == nullptr)
    {
        return Status::InvalidArgument;
    }
    *written = 0;
    auto* owner = AsOwner(instance);
    const std::string_view text{reinterpret_cast<const char*>(request.Data), request.Size};
    if (owner->Scripts.Control(text, {response.Data, response.Capacity}, *written) == false)
    {
        return Status::InvalidArgument;
    }
    *paused = owner->Scripts.Pending() ? 1u : 0u;
    // The closed fixture arms exactly one event after a breakpoint is configured.
    if (text.find("\"breakpoint\"") != std::string_view::npos)
    {
        owner->Armed = true;
    }
    return Status::Ok;
}
} // namespace
// NOLINTBEGIN(bugprone-easily-swappable-parameters) -- fixed ABI signature
extern "C" LUDUS_GAME_API_EXPORT ludus::runtime::game_api::Status
LudusGetGameApi(ludus::foundation::uint32 hostAbiMajor,
                ludus::foundation::uint32 hostAbiMinor,
                ludus::runtime::game_api::GameApiTable* outTable) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    using namespace ludus::runtime::game_api;
    if (outTable == nullptr || outTable->StructSize < sizeof(GameApiTable))
    {
        return Status::InvalidArgument;
    }
    if (hostAbiMajor != kAbiMajor || hostAbiMinor < 1)
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
        .ProcessScriptDebug = Control,
    };
    return Status::Ok;
}
