#pragma once

// Ludus gameplay module function table (design section 6/7/8).
//
// The host fills a caller-owned GameApiTable with noexcept function pointers via
// the single exported entry LudusGetGameApi. Mandatory operations are
// Query/Create/Destroy/Update. Reload-capable modules additionally provide
// Quiesce/Resume, WriteCheckpoint, CreateCandidate and ValidateCandidate. Live
// editing adds DescribeProperties/ReadProperties/PrepareEdits/CommitEdits/
// DiscardEdits. Size negotiation is separate from writing bounded caller buffers.

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/config.h>

#include <ludus/runtime/game_api/abi.h>
#include <ludus/runtime/game_api/services.h>

namespace ludus::runtime::game_api
{
// Opaque module-owned instance. Returned only to the owning module's callbacks
// and invalid after Destroy. The host never dereferences it (design 6).
struct GameInstance;

// Opaque module-owned candidate prepared during staging (reload phase Stage).
struct GameCandidate;

// Opaque module-owned prepared edit plan (live-edit PrepareEdits).
struct GameEditPlan;

/// @brief A borrowed, bounded byte view.
///
/// Valid only for the duration of the call that received it; the host copies anything it retains. Wire/on-disk
/// lengths use explicit fixed-width integers with checked conversion (design 6).
struct ByteView final
{
    const uint8* Data = nullptr;
    usize Size = 0;
};

/// @brief A mutable, bounded byte span the module writes into.
///
/// The host owns the memory and the declared Capacity; the module reports BytesWritten.
struct ByteSpan final
{
    uint8* Data = nullptr;
    usize Capacity = 0;
};

/// @brief Module metadata returned by Query.
///
/// The host validates every field (lengths, ABI, identity) and compares it against the published generation
/// manifest before Create; query and published metadata must agree (requirement L04).
struct GameMetadata final
{
    uint32 StructSize = 0;
    uint32 AbiMajor = 0;
    uint32 AbiMinor = 0;
    uint32 Capabilities = 0; ///< Bitset of game_api::Capability.

    uint32 PropertySchemaVersion = 0;
    uint32 CheckpointSchemaVersion = 0;
    uint32 IdentityLength = 0; ///< Bytes of Identity actually used (<= kIdentityMax).
    uint32 Reserved = 0;

    /// @brief The build/SDK identity string this module was compiled against.
    ///
    /// Mirrors LUDUS_SDK_VARIANT plus compiler id/version (design 6/7). The host compares this exactly against
    /// its own identity before Create. Null-padded.
    char Identity[kIdentityMax] = {};
};

/// @brief Checkpoint envelope header (design section 8).
///
/// Checkpoints are explicit module-defined tagged records, never a memory dump. Body bytes follow in a separate
/// host-owned buffer; this header carries validation metadata.
struct CheckpointHeader final
{
    uint32 FormatVersion = 1;
    uint32 SchemaVersion = 0;
    uint64 ProjectId = 0;
    uint64 GameId = 0;
    uint64 ModuleBuildId = 0;
    uint64 BodyLength = 0; ///< Bytes of body; host bounds to 16 MiB (design 8).
    uint64 BodyDigest = 0; ///< FNV-1a of body; validated before migration use.
};

/// @brief Create parameters.
///
/// The host passes the service table and identity facts. The module returns an opaque instance pointer it owns
/// (design 6).
struct CreateInfo final
{
    uint32 StructSize = 0;
    uint32 Reserved = 0;
    const HostServices* Services = nullptr;
    uint64 ProjectId = 0;
    uint64 GameId = 0;
    uint64 ModuleGeneration = 0;
    /// Optional authored document bytes (copied saved tuning document). Borrowed.
    ByteView AuthoredDocument;
};

/// @brief The gameplay function table.
///
/// Every pointer is noexcept. Optional-capability entries are null when the module does not advertise the matching
/// capability; the host checks capabilities (never a null pointer) before calling.
struct GameApiTable final
{
    uint32 StructSize = 0; ///< sizeof(GameApiTable); host negotiates the common size.
    uint32 AbiMajor = 0;
    uint32 AbiMinor = 0;
    uint32 Reserved = 0;

    // --- Mandatory ---
    /// Fill metadata. May run before Create; validation is not sandboxing.
    Status (*Query)(GameMetadata* outMetadata) noexcept = nullptr;
    /// Construct the instance. Returns Ok and sets *outInstance, or a status.
    Status (*Create)(const CreateInfo* info, GameInstance** outInstance) noexcept = nullptr;
    /// Destroy the instance and release all module-owned resources for it.
    void (*Destroy)(GameInstance* instance) noexcept = nullptr;
    /// Advance one host frame. Reads input, writes render params. No exceptions.
    Status (*Update)(GameInstance* instance, const FrameInput* input, RenderParams* outRender) noexcept = nullptr;

    // --- Reload (Capability::Reload) ---
    /// @brief Pause/resume simulation at a frame boundary.
    ///
    /// Quiesce must leave the instance resumable; it may not irreversibly mutate state (design 7).
    Status (*Quiesce)(GameInstance* instance) noexcept = nullptr;
    Status (*Resume)(GameInstance* instance) noexcept = nullptr;
    /// Size negotiation then bounded write of a read-only checkpoint.
    Status (*CheckpointSize)(GameInstance* instance, usize* outBodySize) noexcept = nullptr;
    Status (*WriteCheckpoint)(GameInstance* instance,
                              CheckpointHeader* outHeader,
                              ByteSpan body,
                              usize* outBytesWritten) noexcept = nullptr;
    /// @brief Create and validate a candidate from a checkpoint during staging.
    ///
    /// Staging must not mutate active resources or perform side effects (design 7).
    Status (*CreateCandidate)(const CreateInfo* info,
                              const CheckpointHeader* header,
                              ByteView body,
                              GameCandidate** outCandidate) noexcept = nullptr;
    Status (*ValidateCandidate)(GameCandidate* candidate) noexcept = nullptr;
    /// @brief Promote a validated candidate to the live instance (non-failing swap in the module's own storage;
    /// the host commit is allocation-free).
    ///
    /// Returns the new instance pointer; the old instance is destroyed separately by Destroy.
    Status (*CommitCandidate)(GameCandidate* candidate, GameInstance** outInstance) noexcept = nullptr;
    /// Discard a candidate during recoverable rejection. Resume A unchanged.
    void (*DiscardCandidate)(GameCandidate* candidate) noexcept = nullptr;

    // --- Live properties (Capability::Properties) ---
    Status (*DescribePropertiesSize)(GameInstance* instance, usize* outByteSize) noexcept = nullptr;
    Status (*DescribeProperties)(GameInstance* instance, ByteSpan out, usize* outBytesWritten) noexcept = nullptr;
    Status (*ReadProperties)(GameInstance* instance, ByteSpan out, usize* outBytesWritten) noexcept = nullptr;
    Status (*PrepareEdits)(GameInstance* instance, ByteView edits, GameEditPlan** outPlan) noexcept = nullptr;
    Status (*CommitEdits)(GameInstance* instance, GameEditPlan* plan) noexcept = nullptr;
    void (*DiscardEdits)(GameInstance* instance, GameEditPlan* plan) noexcept = nullptr;

    // --- Supported asset reload (Capability::AssetReload) ---
    /// @brief Swap one supported logical asset to a validated immutable artifact at the
    /// owning subsystem's safe point; old resource disposal is deferred (design 12).
    Status (*ReloadAsset)(GameInstance* instance,
                          uint64 logicalAssetId,
                          ByteView cookedArtifact,
                          uint64 artifactDigest) noexcept = nullptr;

    /// Process a versioned script-debug request on the instance's owner thread.
    /// @param request Borrowed UTF-8 extension JSON, at most 4096 bytes.
    /// @param response Caller-owned response storage, at most 4096 bytes.
    /// @param outBytesWritten Exact response length on Ok; zero on rejection.
    /// @param outPaused One when the authoritative tick remains partial/stopped,
    /// zero when normal Update may resume. The next Update must account for the
    /// completed pending tick before starting another. Never save a partial tick.
    /// @return Explicit status; the module must validate revision/execution/stop
    /// tokens before mutation, bound inspection, and perform no watch evaluation.
    /// @note Optional ABI 1.1 tail, gated by Capability::ScriptDebug. Unsupported
    /// requests preserve simulation. Destroy cancels partial effects and retires
    /// every VM/native closure before the host releases the module image.
    Status (*ProcessScriptDebug)(GameInstance* instance,
                                 ByteView request,
                                 ByteSpan response,
                                 usize* outBytesWritten,
                                 uint32* outPaused) noexcept = nullptr;
};

// The single exported entry symbol. Spelled as a macro so host and module agree.
#define LUDUS_GAME_ENTRY_SYMBOL "LudusGetGameApi"

/// @brief Entry function signature.
///
/// The module fills *outTable up to the host-provided StructSize and returns Ok, or IncompatibleAbi when AbiMajor
/// disagrees.
using GetGameApiFn = Status (*)(uint32 hostAbiMajor, uint32 hostAbiMinor, GameApiTable* outTable) noexcept;
} // namespace ludus::runtime::game_api

// Visibility of the single public entry. A gameplay module builds with hidden
// default visibility and marks EXACTLY this symbol visible, so dlsym resolves
// the one entry and nothing else (design 5/6). A Windows DLL exports nothing by
// default, so the entry is exported explicitly there (D6c). Without either this
// expands to nothing and the explicit linker export policy applies instead.
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
#    define LUDUS_GAME_API_EXPORT __declspec(dllexport)
#elif LUDUS_HAS_ATTRIBUTE(visibility)
#    define LUDUS_GAME_API_EXPORT __attribute__((visibility("default")))
#else
#    define LUDUS_GAME_API_EXPORT
#endif

// The exported symbol is unmangled C linkage with a platform-default calling
// convention (design 6). A module defines exactly this and nothing else public.
// The hostAbiMajor/hostAbiMinor parameter order is the fixed ABI entry
// signature; a module definition should carry the same NOLINT.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
extern "C" LUDUS_GAME_API_EXPORT ::ludus::runtime::game_api::Status
LudusGetGameApi(::ludus::foundation::uint32 hostAbiMajor,
                ::ludus::foundation::uint32 hostAbiMinor,
                ::ludus::runtime::game_api::GameApiTable* outTable) noexcept;
// NOLINTEND(bugprone-easily-swappable-parameters)
