#pragma once

// Host service table (design section 6).
//
// The gameplay module calls a passed engine service table, never host-exported
// C++ symbols. Services cover explicit diagnostics, bounded allocation,
// frame/input data, resource IDs and the sample's render parameters. The table
// is grown with concrete consumer requirements, not by forwarding every engine
// API speculatively. All callbacks are noexcept POD function pointers.
//
// Service handles carry the session and resource generation; the host revokes a
// stale table after reload so a cached pointer cannot outlive its generation.
// Host allocations are released only through matching host services; a module
// never cross-module delete()s host memory (design 6).

#include <ludus/foundation/base/types.h>

namespace ludus::runtime::game_api
{
using ludus::foundation::float32;
using ludus::foundation::float64;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

/// @brief Diagnostic severity mirrored across the ABI as an explicit integer.
///
/// The host maps these onto LUDUS_LOG_* so the module needs no logging headers. The 32-bit base is a fixed part of
/// the ABI wire layout.
// NOLINTNEXTLINE(performance-enum-size)
enum class LogSeverity : uint32
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4
};

/// @brief Per-frame data handed to Update.
///
/// Fixed-width, POD. Times are host-owned; the module does not advance wall-clock time itself (design 8). Input is
/// the current held state, normalized by the host from the Platform contract.
struct FrameInput final
{
    /// Update callback ordinal. Debug fast-forward can run several updates per presented image.
    uint64 FrameIndex = 0;
    /// Simulation timestep in seconds for an advancing update. Paused updates leave the simulation clock frozen.
    float64 DeltaSeconds = 0;
    /// Host-owned simulation seconds at the start of the update, excluding paused time.
    float64 ElapsedSeconds = 0;
    uint32 Width = 0;
    uint32 Height = 0;
    /// @brief Bitset of currently-held logical actions (sample vocabulary).
    ///
    /// The host maps bit 0 to Left (A/left arrow), bit 1 to Right (D/right arrow), and bit 2 to Space.
    /// Focus/reset suppression follows Ludus::Input.
    uint32 HeldActions = 0;
    uint8 Paused = 0; ///< 1 while the host has the session paused.
    uint8 Reserved0 = 0;
    uint8 Reserved1 = 0;
    uint8 Reserved2 = 0;
};

/// @brief Render parameters the module writes each frame.
///
/// The host owns the RHI session and performs the actual draw; the module only expresses a bounded, validated
/// description. The current public RHI supports frame clearing; colors are clamped by the host to [0,1].
/// Draw/resource APIs can extend this contract.
struct RenderParams final
{
    float32 ClearRed = 0;
    float32 ClearGreen = 0;
    float32 ClearBlue = 0;
    float32 ClearAlpha = 1;
    /// @brief Reserved scalar channels for a future draw path.
    ///
    /// The current host validates finiteness but does not render these channels.
    float32 Uniform0 = 0;
    float32 Uniform1 = 0;
    float32 Uniform2 = 0;
    float32 Uniform3 = 0;
};

// Opaque host context passed to every service call. Valid only for the lifetime
// of the generation that received it; the host revokes it on retire.
struct HostContext;

/// @brief The host service table.
///
/// One pointer is handed to Create and stays valid for that instance's generation. Each entry is noexcept. A module
/// must tolerate a null function pointer for optional services it did not request.
struct HostServices final
{
    uint32 StructSize = 0; ///< sizeof(HostServices) on the host; negotiated.
    uint32 Reserved = 0;
    HostContext* Context = nullptr;

    /// @brief Diagnostics: route a bounded UTF-8 message through host logging.
    ///
    /// The host never lets a logging failure propagate into gameplay (AGENTS.md).
    void (*Log)(HostContext* ctx, LogSeverity severity, const char* text, usize length) noexcept = nullptr;

    /// @brief Bounded allocation owned by the host.
    ///
    /// Freed only via FreeBytes with the same context. Returns nullptr on exhaustion; the module checks.
    void* (*AllocateBytes)(HostContext* ctx, usize size, usize alignment) noexcept = nullptr;
    void (*FreeBytes)(HostContext* ctx, void* ptr) noexcept = nullptr;

    /// @brief Resource identity: translate a logical asset ID (stable across reload) into the host's current
    /// resource generation handle.
    ///
    /// Returns 0 if unknown.
    uint64 (*ResolveResource)(HostContext* ctx, uint64 logicalAssetId) noexcept = nullptr;

    /// @brief Optional appended service entries.
    ///
    /// A deferred callback/job must acquire a lease before publishing any module-code reference, and release it
    /// after its final use. Quiesce must also join workers: a worker releasing its lease is not proof that it
    /// has returned out of module code. Acquisition is gated during Create/staging, pause and retirement; start
    /// work from Update or Resume after activation, never from Create/CreateCandidate.
    /// These two calls alone may run on worker threads; other services remain
    /// host-thread calls. Check StructSize before accessing this appended tail.
    bool (*AcquireWorkLease)(HostContext* ctx) noexcept = nullptr;
    bool (*ReleaseWorkLease)(HostContext* ctx) noexcept = nullptr;
};
} // namespace ludus::runtime::game_api
