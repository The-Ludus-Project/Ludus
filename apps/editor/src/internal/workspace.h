#pragma once

// WorkspaceState and the pure transition function.
//
// Private editor header (not installed). WorkspaceState is the single owner of
// saved/draft settings, descriptor path/digest, project epoch, operation phase,
// last result, discovered targets and the active job. The transition function
// is tested independently of any widget or tool (design.md sections 3, 6).
//
// It uses private Qt value types but creates no widgets and runs no tools.

#include "internal/engine_pin.h"
#include "internal/project_descriptor.h"

#include <ludus/foundation/base/types.h>

#include <QString>
#include <QStringList>

#include <optional>

namespace ludus::editor
{
using foundation::uint32;
using foundation::uint64;

// The owned operation phase. A single enum, never overlapping booleans
// (design section 6). CleanupUnknown is terminal for operations: no new
// operation and no automatic close until explicit operator recovery.
enum class Phase : foundation::uint8
{
    Idle,
    Starting,
    Configuring,
    Building,
    Publishing,
    Launching,
    Running,
    Debugging,
    Stopping,
    CleanupUnknown,
};

// Document facts, orthogonal to Phase.
enum class DocumentState : foundation::uint8
{
    NoProject,
    ProjectLoaded,
};

// Last result outcome; separate from Idle so a failure is not erased merely
// because the operation becomes idle (design section 6).
enum class Outcome : foundation::uint8
{
    None,
    Success,
    Failed,
    Cancelled,
    CleanupUnknown,
};

// A persisted/last operation result with enough detail to display and to copy.
struct LastResult
{
    Outcome Kind = Outcome::None;
    Phase Stage = Phase::Idle; // stage reached when the result was produced
    ResultCode Code = ResultCode::Ok;
    QString Message; // bounded, user-readable
    std::optional<foundation::int32> ExitCode;
    std::optional<foundation::int32> Signal;
    bool CleanupConfirmed = true;
};

// The typed actions the UI can request. Each is validated centrally by the
// controller/transition function; invalid requests are rejected, not applied.
enum class ActionKind : foundation::uint8
{
    OpenProject,
    EditDraft,
    Save,
    Reload,
    Configure,
    Build,
    BuildRun,
    BuildDebug,
    ProjectCheck,
    ProjectSetup,
    ProjectCreate,
    ReleaseInit,
    Package,
    Stop,
    ClearOutput,
    CopyJobDetails,
    Close,
};

// Typed events delivered back from tooling/time, tagged with the owning job id.
enum class EventKind : foundation::uint8
{
    JobStarted,   // adapter ready + request sent
    PhaseChanged, // phase: configuring/building/launching/running/stopping
    TargetsDiscovered,
    RuntimeStarted, // the only event that may confirm Launching -> Running
    JobResult,      // terminal result (after cleanup)
    ProtocolError,
    SupervisorLost, // bridge died without a terminal result
};

// Immutable identity for the whole workspace state. The transition function
// takes a state and an input and returns the next state; it mutates nothing in
// place (so stale callbacks cannot corrupt a newer state).
struct WorkspaceState
{
    DocumentState Document = DocumentState::NoProject;
    QString DescriptorPath;  // absolute path of the open descriptor
    QString SavedDigest;     // digest of the saved document on disk
    ProjectDescriptor Saved; // last successfully saved settings
    ProjectDescriptor Draft; // in-progress edits
    bool HasSaved = false;

    uint64 ProjectEpoch = 0; // bumped on every successful Open/Reload
    uint64 ActiveJob = 0;    // 0 when no job is owned
    uint64 NextJob = 1;      // monotonic; never wraps silently
    Phase OperationPhase = Phase::Idle;
    ActionKind ActiveAction = ActionKind::Configure;
    bool StopLatched = false;      // Stop accepted; wins before runtime spawn
    QStringList DiscoveredTargets; // executable targets from last Configure
    QString DiscoveredPreset;      // preset the discovery cache belongs to
    QString SetupStatus;           // read-only CMake setup result for the saved project
    QString SetupProblem;          // last setup failure; retained until a successful check or repair
    EnginePinStatus EnginePin;     // selected engine install versus this editor's engine (last check)
    LastResult Result;

    // Dirty is the semantic inequality of saved and draft (design section 4).
    [[nodiscard]] bool Dirty() const noexcept
    {
        return HasSaved == false || (Saved == Draft) == false;
    }

    // True while an operation is owned (not Idle/CleanupUnknown is "busy").
    [[nodiscard]] bool Busy() const noexcept
    {
        return OperationPhase != Phase::Idle && OperationPhase != Phase::CleanupUnknown;
    }
};

// Capability flags derived purely from state (design sections 5-6). The UI
// toggles controls from these; it never computes enablement independently.
struct Capabilities
{
    bool CanOpen = false;
    bool CanCloseProject = false;
    bool CanEdit = false;
    bool CanSave = false;
    bool CanReload = false;
    bool CanConfigure = false;
    bool CanBuild = false;
    bool CanBuildRun = false;
    bool CanBuildDebug = false;
    bool CanProjectCheck = false;
    bool CanProjectSetup = false;
    bool CanProjectCreate = false;
    bool CanReleaseInit = false;
    bool CanPackage = false;
    bool CanStop = false;
    bool CanClearOutput = true;
    bool CanCopyJobDetails = false;
    bool CanCloseImmediately = true; // false while busy (needs Stop and Close)
};

// Compute capabilities from a state. Centralized so capability logic has one
// source of truth and cannot drift across widgets.
[[nodiscard]] Capabilities ComputeCapabilities(const WorkspaceState& state);

// Whether a job of this kind may legally start now. Encapsulates "exactly one
// owned operation" and "a clean saved document is required for build/run".
[[nodiscard]] bool CanStartJob(const WorkspaceState& state, ActionKind kind);

// Phase a given start action enters first (always Starting for a job start).
[[nodiscard]] Phase StartingPhaseFor(ActionKind kind);

// Apply a phase event tagged with jobId. Returns the next state. A phase/job
// mismatch, a late Running after Stop, or a disallowed transition leaves the
// state unchanged (ignored stale event). Only RuntimeStarted confirms Running.
[[nodiscard]] WorkspaceState
ApplyPhaseEvent(const WorkspaceState& state, uint64 jobId, Phase requested, bool runtimeConfirmed);

// Only a typed debugger confirmation can establish an external RAD session.
[[nodiscard]] WorkspaceState ApplyDebuggerStarted(const WorkspaceState& state, uint64 jobId);

// Latch a Stop for the current job. Idempotent. Once latched the state never
// transitions back to Running or launches another child.
[[nodiscard]] WorkspaceState LatchStop(const WorkspaceState& state);

// Apply a terminal result for jobId. A result for a non-current/retired job, or
// a second terminal result for the same job, is ignored. Clears ActiveJob and
// moves OperationPhase to Idle (or CleanupUnknown when cleanup is unconfirmed),
// while preserving the LastResult (an error is a result, not a busy state).
[[nodiscard]] WorkspaceState ApplyResult(const WorkspaceState& state, uint64 jobId, const LastResult& result);

// Begin a job: assign the next job id, advance to Starting, clear StopLatched,
// and reset the live result. Caller must have checked CanStartJob first.
[[nodiscard]] WorkspaceState BeginJob(const WorkspaceState& state, ActionKind kind);

} // namespace ludus::editor
