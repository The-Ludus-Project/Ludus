#include "internal/module_loader.h"

#include <ludus/foundation/base/core.h>

#include <cstring>
#include <utility>

#include <dlfcn.h>

namespace ludus::runtime::game_host
{
namespace
{
using game_api::GameApiTable;
using game_api::GameMetadata;
using game_api::GetGameApiFn;
using game_api::Status;
using ludus::foundation::uint8;
using ludus::foundation::usize;

// Minimum usable table size: everything through the mandatory Update entry.
// A module reporting a smaller table cannot provide the mandatory operations.
constexpr usize kMinTableSize = offsetof(GameApiTable, Quiesce);

// A field is present in a negotiated table only if the reported StructSize
// covers all of its bytes. A capability callback beyond StructSize is treated
// as absent even if the host struct would place a pointer there.
template <typename FieldPtr>
[[nodiscard]] bool FieldWithin(uint32 structSize, FieldPtr GameApiTable::* member) noexcept
{
    const GameApiTable probe{};
    const auto* base = reinterpret_cast<const uint8*>(&probe);
    const auto* field = reinterpret_cast<const uint8*>(&(probe.*member));
    const usize offset = static_cast<usize>(field - base);
    return offset + sizeof(FieldPtr) <= structSize;
}

// Validate that every callback a capability needs is both within the negotiated
// table size and non-null. A module advertising a capability with a null or
// truncated callback is rejected before any use (review finding 2).
[[nodiscard]] bool CapabilityComplete(const GameApiTable& t, game_api::Capability cap) noexcept
{
    switch (cap)
    {
        case game_api::Capability::Reload:
            return FieldWithin(t.StructSize, &GameApiTable::DiscardCandidate) && t.Quiesce != nullptr &&
                   t.Resume != nullptr && t.CheckpointSize != nullptr && t.WriteCheckpoint != nullptr &&
                   t.CreateCandidate != nullptr && t.ValidateCandidate != nullptr && t.CommitCandidate != nullptr &&
                   t.DiscardCandidate != nullptr;
        case game_api::Capability::Properties:
            return FieldWithin(t.StructSize, &GameApiTable::DiscardEdits) && t.DescribePropertiesSize != nullptr &&
                   t.DescribeProperties != nullptr && t.ReadProperties != nullptr && t.PrepareEdits != nullptr &&
                   t.CommitEdits != nullptr && t.DiscardEdits != nullptr;
        case game_api::Capability::AssetReload:
            return FieldWithin(t.StructSize, &GameApiTable::ReloadAsset) && t.ReloadAsset != nullptr;
        case game_api::Capability::ScriptDebug:
            return t.AbiMinor >= 1 && FieldWithin(t.StructSize, &GameApiTable::ProcessScriptDebug) &&
                   t.ProcessScriptDebug != nullptr;
        case game_api::Capability::None:
            return true;
    }
    return false;
}

// Compare two bounded identity strings exactly. The module's Identity is a
// null-padded fixed buffer of IdentityLength used bytes.
[[nodiscard]] bool IdentityMatches(const GameMetadata& metadata, std::string_view hostIdentity) noexcept
{
    if (metadata.IdentityLength != hostIdentity.size())
    {
        return false;
    }
    if (metadata.IdentityLength > game_api::kIdentityMax)
    {
        return false;
    }
    return std::memcmp(metadata.Identity, hostIdentity.data(), hostIdentity.size()) == 0;
}
} // namespace

std::string_view LoadStatusName(LoadStatus status) noexcept
{
    switch (status)
    {
        case LoadStatus::Ok:
            return "Ok";
        case LoadStatus::PathInvalid:
            return "PathInvalid";
        case LoadStatus::DlopenFailed:
            return "DlopenFailed";
        case LoadStatus::EntryMissing:
            return "EntryMissing";
        case LoadStatus::AbiRejected:
            return "AbiRejected";
        case LoadStatus::QueryFailed:
            return "QueryFailed";
        case LoadStatus::IdentityRejected:
            return "IdentityRejected";
        case LoadStatus::MetadataInconsistent:
            return "MetadataInconsistent";
    }
    return "Unknown";
}

LoadedModule::~LoadedModule() noexcept
{
    Close();
}

LoadedModule::LoadedModule(LoadedModule&& other) noexcept
    : Handle_(other.Handle_), Table_(other.Table_), Metadata_(other.Metadata_), Path_(std::move(other.Path_)),
      Generation_(other.Generation_), Pinned_(other.Pinned_)
{
    other.Handle_ = nullptr;
    other.Table_ = {};
    other.Metadata_ = {};
    other.Generation_ = 0;
}

LoadedModule& LoadedModule::operator=(LoadedModule&& other) noexcept
{
    if (this != &other)
    {
        Close();
        Handle_ = other.Handle_;
        Table_ = other.Table_;
        Metadata_ = other.Metadata_;
        Path_ = std::move(other.Path_);
        Generation_ = other.Generation_;
        Pinned_ = other.Pinned_;
        other.Handle_ = nullptr;
        other.Table_ = {};
        other.Metadata_ = {};
        other.Generation_ = 0;
    }
    return *this;
}

bool LoadedModule::Close() noexcept
{
    if (Pinned_)
    {
        return false;
    }
    bool ok = true;
    if (Handle_ != nullptr)
    {
        // Releasing the loader reference. This is logical retirement; it does
        // not prove the OS unmapped every page (design 8). The owner must have
        // destroyed all module instances/candidates first. A dlclose error is
        // surfaced so the caller can require a restart rather than assume the
        // image was retired.
        ok = (::dlclose(Handle_) == 0);
        if (ok == false)
        {
            Pinned_ = true;
            return false;
        }
        Handle_ = nullptr;
    }
    Table_ = {};
    Metadata_ = {};
    Generation_ = 0;
    return ok;
}

LoadStatus LoadModule(std::string_view path,
                      uint64 generation,
                      std::string_view hostIdentity,
                      uint32 hostAbiMajor,
                      uint32 hostAbiMinor,
                      LoadedModule& outModule) noexcept
{
    if (outModule.Close() == false)
    {
        return LoadStatus::MetadataInconsistent;
    }

    if (path.size() > 4096 || path.find('\0') != std::string_view::npos || path.empty() || path.front() != '/')
    {
        // Load only from an absolute canonical path inside a leased generation.
        return LoadStatus::PathInvalid;
    }

    // Null-terminate the path for dlopen without heap churn beyond the string.
    const std::string pathZ(path);

    // Immediate resolution + local scope: no lazy binding surprises, and the
    // module does not export its symbols into the global namespace. We also
    // ask for a fresh mapping (NODELETE is intentionally NOT set so a close can
    // retire the generation when the OS chooses to unmap).
    void* handle = ::dlopen(pathZ.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
        return LoadStatus::DlopenFailed;
    }

    // Resolve exactly the one public entry symbol.
    void* symbol = ::dlsym(handle, LUDUS_GAME_ENTRY_SYMBOL);
    if (symbol == nullptr)
    {
        ::dlclose(handle);
        return LoadStatus::EntryMissing;
    }

    // Fill a host-owned table. The host declares its own StructSize so the
    // module writes only the common prefix (size negotiation, design 6).
    GameApiTable table = {};
    table.StructSize = static_cast<uint32>(sizeof(GameApiTable));
    table.AbiMajor = hostAbiMajor;
    table.AbiMinor = hostAbiMinor;

    auto entry = reinterpret_cast<GetGameApiFn>(symbol);
    const Status fillStatus = entry(hostAbiMajor, hostAbiMinor, &table);
    if (fillStatus != Status::Ok)
    {
        ::dlclose(handle);
        return LoadStatus::AbiRejected;
    }

    // The module must report a compatible ABI major, a minor it does not exceed
    // the host's, and a usable, in-range table size (size negotiation, design 6).
    if (table.AbiMajor != hostAbiMajor || table.AbiMinor > hostAbiMinor || table.StructSize < kMinTableSize ||
        table.StructSize > sizeof(GameApiTable))
    {
        ::dlclose(handle);
        return LoadStatus::AbiRejected;
    }
    // The mandatory callbacks must be present and within the negotiated size.
    if (FieldWithin(table.StructSize, &GameApiTable::Update) == false || table.Query == nullptr ||
        table.Create == nullptr || table.Destroy == nullptr || table.Update == nullptr)
    {
        ::dlclose(handle);
        return LoadStatus::AbiRejected;
    }

    // Read embedded metadata via Query (may run before Create; not sandboxing).
    GameMetadata metadata = {};
    metadata.StructSize = static_cast<uint32>(sizeof(GameMetadata));
    const Status queryStatus = table.Query(&metadata);
    if (queryStatus != Status::Ok)
    {
        ::dlclose(handle);
        return LoadStatus::QueryFailed;
    }
    // Query must agree with the table on ABI, report a consistent metadata size,
    // and a bounded identity. Published and query metadata must agree (L04).
    if (metadata.StructSize != sizeof(GameMetadata) || metadata.AbiMajor != hostAbiMajor ||
        metadata.AbiMinor != table.AbiMinor || metadata.IdentityLength > game_api::kIdentityMax ||
        metadata.Reserved != 0 || table.Reserved != 0 || (metadata.Capabilities & ~uint32{15}) != 0)
    {
        ::dlclose(handle);
        return LoadStatus::MetadataInconsistent;
    }
    // Every advertised capability must have all its callbacks present and
    // non-null within the negotiated table (review finding 2): a module that
    // advertises Reload/Properties/AssetReload with a null or truncated callback
    // is rejected before any use.
    for (const auto cap : {game_api::Capability::Reload,
                           game_api::Capability::Properties,
                           game_api::Capability::AssetReload,
                           game_api::Capability::ScriptDebug})
    {
        if (game_api::HasCapability(metadata.Capabilities, cap) && CapabilityComplete(table, cap) == false)
        {
            ::dlclose(handle);
            return LoadStatus::MetadataInconsistent;
        }
    }

    // Reject a module whose embedded identity differs from the host identity
    // (SDK variant / compiler / target / ABI). No Create is attempted.
    if (IdentityMatches(metadata, hostIdentity) == false)
    {
        ::dlclose(handle);
        return LoadStatus::IdentityRejected;
    }

    outModule.Handle_ = handle;
    outModule.Table_ = table;
    outModule.Metadata_ = metadata;
    outModule.Path_ = pathZ;
    outModule.Generation_ = generation;
    return LoadStatus::Ok;
}
} // namespace ludus::runtime::game_host
