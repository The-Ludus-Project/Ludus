#pragma once

// Private module loader (project-live-reload design 5/6/7).
//
// Loads a gameplay module from an absolute canonical path inside a leased
// generation directory. On Linux and macOS it uses dlopen with immediate resolution and
// local scope (RTLD_NOW | RTLD_LOCAL); on Windows, LoadLibraryExW with dependencies
// resolved beside the module and in system directories (D6c). It resolves exactly one
// public entry symbol
// (LudusGetGameApi), negotiates the common ABI table size, fills a host-owned
// table and validates the module's embedded Query identity against the host
// identity BEFORE any Create. Two generations (A and B) can be loaded
// concurrently from distinct paths; each has its own handle and symbol scope.
//
// RTLD_LOCAL (or a separate DLL handle) is a scoping choice, not isolation (design 5): the loader never
// claims OS unmapping as proof of retirement. Load/Query failures are ordinary
// explicit errors, distinct from native crashes which are host failures.

#include <ludus/runtime/game_api/api.h>

#include <ludus/foundation/base/types.h>

#include <string>
#include <string_view>

namespace ludus::runtime::game_host
{
using ludus::foundation::uint32;
using ludus::foundation::uint64;

// Explicit, non-crash outcome of a load attempt. These are all recoverable:
// A keeps running when a candidate B fails to load (design 7).
// NOLINTNEXTLINE(performance-enum-size)
enum class LoadStatus : uint32
{
    Ok = 0,
    PathInvalid = 1,
    DlopenFailed = 2,
    EntryMissing = 3,
    AbiRejected = 4,         // AbiMajor mismatch or unusable table size.
    QueryFailed = 5,         // Query returned non-Ok.
    IdentityRejected = 6,    // Embedded identity != host identity.
    MetadataInconsistent = 7 // Query metadata disagrees with the manifest.
};

[[nodiscard]] std::string_view LoadStatusName(LoadStatus status) noexcept;

// One loaded module generation. Owns the dlopen handle and a host-owned copy of
// the negotiated function table and metadata. Non-copyable; move-only ownership
// of the OS handle. Closing releases the loader reference (NOT a proof of
// unmapping); the owner must have retired all module objects first (design 8).
class LoadedModule final
{
public:
    LoadedModule() noexcept = default;
    ~LoadedModule() noexcept;

    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;
    LoadedModule(LoadedModule&& other) noexcept;
    LoadedModule& operator=(LoadedModule&& other) noexcept;

    [[nodiscard]] bool IsLoaded() const noexcept
    {
        return Handle_ != nullptr;
    }
    [[nodiscard]] const game_api::GameApiTable& Table() const noexcept
    {
        return Table_;
    }
    [[nodiscard]] const game_api::GameMetadata& Metadata() const noexcept
    {
        return Metadata_;
    }
    [[nodiscard]] std::string_view Path() const noexcept
    {
        return Path_;
    }
    [[nodiscard]] uint64 Generation() const noexcept
    {
        return Generation_;
    }

    // Release the loader reference. Idempotent. Does not run module Destroy; the
    // caller retires module objects first (design 8). Returns false if dlclose
    // reported an error (residency uncertain -> caller requires restart); a
    // not-loaded module returns true (nothing to release).
    bool Close() noexcept;

    // Uncertain live references: intentionally retain this OS reference until
    // process exit. A session may pin at most one retired generation.
    void PinUntilProcessExit() noexcept
    {
        Pinned_ = true;
    }

private:
    friend LoadStatus LoadModule(std::string_view path,
                                 uint64 generation,
                                 std::string_view hostIdentity,
                                 uint32 hostAbiMajor,
                                 uint32 hostAbiMinor,
                                 LoadedModule& outModule) noexcept;

    void* Handle_ = nullptr;
    game_api::GameApiTable Table_ = {};
    game_api::GameMetadata Metadata_ = {};
    std::string Path_;
    uint64 Generation_ = 0;
    bool Pinned_ = false;
};

// Load a module and validate it to the point just before Create. The host
// identity is the full compatibility key; a module whose embedded identity
// differs is rejected with IdentityRejected (no Create attempted).
[[nodiscard]] LoadStatus LoadModule(std::string_view path,
                                    uint64 generation,
                                    std::string_view hostIdentity,
                                    uint32 hostAbiMajor,
                                    uint32 hostAbiMinor,
                                    LoadedModule& outModule) noexcept;
} // namespace ludus::runtime::game_host
