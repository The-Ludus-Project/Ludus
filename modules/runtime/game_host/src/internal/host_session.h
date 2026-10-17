#pragma once

// Private host session: owns the loaded gameplay module, its instance, the host
// services, the control-channel framing and the per-frame command dispatch
// (project-live-reload design 3/4/7/10). One session per host process. Gameplay
// callbacks run on the host thread; the control fd is polled non-blocking each
// frame and validated commands are dispatched at the frame boundary.
//
// The reload transaction (Quiesce/Snapshot/Stage/Commit/Retire) and the live
// property/asset commands are implemented here so one owner serializes reload
// against property/pause/step commands and Stop supersedes queued work.

#include "internal/host_services.h"
#include "internal/module_loader.h"
#include "internal/protocol_codec.h"
#include "internal/timing_telemetry.h"

#include <ludus/runtime/game_api/api.h>
#include <ludus/runtime/game_host/host.h>
#include <ludus/runtime/game_host/protocol.h>

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/platform/frame_pacing.h>

#include <string>
#include <string_view>

namespace ludus::runtime::game_host
{
using ludus::foundation::int32;
using ludus::foundation::uint64;
using ludus::foundation::usize;

// PlayState mirrored from design section 4. The host owns transitions.
enum class PlayState : ludus::foundation::uint8
{
    Stopped = 0,
    Starting = 1,
    Running = 2,
    Paused = 3,
    Reloading = 4,
    Stopping = 5,
    Failed = 6,
    CleanupUnknown = 7
};

class FramePresenter;

/// Largest frame interval the host's diagnostic frame clock accepts: the frame-update 250 ms clamp.
inline constexpr uint64 HOST_MAXIMUM_FRAME_DELTA_NANOSECONDS = 250'000'000;

[[nodiscard]] std::string_view PlayStateName(PlayState state) noexcept;

// Run a statically linked gameplay implementation (shipping build, no dlopen,
// no reload). Builds the table from the in-process entry, runs the bounded
// frame loop, and retires the instance.
[[nodiscard]] RunResult RunStaticEntry(const HostConfig& config, StaticEntryFn entry) noexcept;

class HostSession final
{
public:
    // projectId/gameId/controlFd are a fixed construction signature.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    HostSession(uint64 projectId, uint64 gameId, int32 controlFd, uint64 projectEpoch = 1) noexcept;
    ~HostSession() noexcept;

    HostSession(const HostSession&) = delete;
    HostSession& operator=(const HostSession&) = delete;

    // Load the initial generation from an absolute leased path. Emits
    // ModuleReady on success. Returns false and sets Failed on rejection.
    [[nodiscard]] bool
    LoadInitial(std::string_view modulePath, uint64 generation = 1, game_api::ByteView authored = {}) noexcept;
    [[nodiscard]] bool
    PrepareInitialLoad(std::string_view path, uint64 generation, game_api::ByteView authored) noexcept;

    // Run the frame loop until Stop (or maxFrames, 0 = unbounded). Polls the
    // control channel each frame, dispatches validated commands at the boundary,
    // advances the active instance, and emits events. Returns the terminal
    // RunResult.
    [[nodiscard]] RunResult RunLoop(uint64 maxFrames, FramePresenter* presenter = nullptr) noexcept;

    [[nodiscard]] PlayState State() const noexcept
    {
        return State_;
    }

    [[nodiscard]] RunResult InitialFailure() const noexcept
    {
        return InitialFailure_;
    }

    [[nodiscard]] uint64 ActiveGeneration() const noexcept
    {
        return ActiveGeneration_;
    }

    [[nodiscard]] uint64 OutstandingHostAllocations() const noexcept
    {
        return Services_.OutstandingAllocations();
    }

    [[nodiscard]] uint64 OutstandingWorkLeases() const noexcept
    {
        return Services_.OutstandingWork();
    }

    // Advance the active instance one frame (deterministic test driver). No I/O.
    void AdvanceOneFrame() noexcept
    {
        StepFrame();
    }

    // Read the active instance's live property values into a caller buffer for
    // state-equality checks. Returns bytes written, or 0 on failure.
    [[nodiscard]] usize ReadActiveProperties(ludus::foundation::uint8* buffer, usize capacity) noexcept;

    // Capture the active instance's full checkpoint body into a caller buffer
    // for EXACT supported-state comparison (position/velocity/RNG/time/etc).
    // Returns bytes written, or 0 on failure. Test driver only.
    [[nodiscard]] usize CaptureCheckpoint(ludus::foundation::uint8* buffer, usize capacity) noexcept;

    // Simulation ticks advanced so far (advances only when not paused).
    [[nodiscard]] uint64 SimTicks() const noexcept
    {
        return SimTicks_;
    }

    // Reload the active module to a new generation at a frame boundary. Keeps A
    // alive until B validates; recoverable rejection resumes A unchanged
    // (design 7). Returns the command status.
    [[nodiscard]] protocol::CommandStatus ReloadTo(std::string_view newModulePath, uint64 newGeneration) noexcept;

private:
    // Emit an event frame on the control channel (no-op if no control fd).
    void Emit(const protocol::Message& message, bool priority = false) noexcept;
    void FlushOutput() noexcept;
    // Fails the control channel after an output/result allocation failure.
    void latchOutputAllocationFailure() noexcept;
    void EmitReloadPhase(protocol::ReloadPhase phase) noexcept;
    void EmitCommandResult(uint64 requestId, protocol::CommandStatus status, std::string_view message) noexcept;
    void EmitReady() noexcept;

    // Poll + dispatch any pending control commands. Returns false to stop.
    [[nodiscard]] bool PumpControl() noexcept;
    void Dispatch(const protocol::Message& command) noexcept;
    [[nodiscard]] protocol::CommandStatus PublishProperties(uint64 requestId) noexcept;
    [[nodiscard]] protocol::CommandStatus ApplyPropertyEdits(const protocol::Message& command) noexcept;

    // One Update tick of the active instance (unless paused/stepping gate it).
    void StepFrame() noexcept;

    // Whether the active module advertises the Reload capability (gates calls to
    // the optional Quiesce/Resume/checkpoint/candidate callbacks).
    [[nodiscard]] bool ModuleHasReload() const noexcept;
    [[nodiscard]] bool RetireActive() noexcept;
    // Drop the frame-clock baseline and pacing schedule after session replacement.
    void resetFrameTiming() noexcept;

    FramePresenter* Presenter_ = nullptr;
    uint64 ProjectId_ = 0;
    uint64 GameId_ = 0;
    uint64 ProjectEpoch_ = 1;
    int32 ControlFd_ = -1;

    PlayState State_ = PlayState::Stopped;
    RunResult InitialFailure_ = RunResult::IncompatibleModule;
    bool StopLatched_ = false;
    bool ActiveRetired_ = false;
    bool InstanceReady_ = false;
    bool Quiesced_ = false;
    bool StepRequested_ = false;
    bool ScriptPaused_ = false;
    bool ReadyEmitted_ = false;
    bool HelloReceived_ = false;
    bool AwaitingLoad_ = false;
    std::string InitialPath_;
    uint64 InitialGeneration_ = 0;
    game_api::ByteView InitialAuthored_;
    uint64 FrameIndex_ = 0;
    uint64 SimTicks_ = 0;                    // Simulation ticks; advance only when not paused (no catch-up).
    game_api::RenderParams LastRender_ = {}; // Last render params from Update (asset/visible-change checks).
    // Frame timing (docs/architecture/high-resolution-time.md, "Runtime timing
    // telemetry"): RunLoop paces each frame start, samples one timestamp per
    // frame and reports the aggregate on Status. Simulation still advances one
    // fixed tick per frame; the samples are diagnostics, not simulation debt.
    platform::FramePacer Pacer_;
    platform::PacingClock PacingClock_ = platform::GetSystemPacingClock();
    ludus::foundation::time::FrameClock FrameClock_{
        ludus::foundation::time::Duration{HOST_MAXIMUM_FRAME_DELTA_NANOSECONDS}};
    TimingTelemetry Timing_;
    uint64 SessionId_ = 1;
    uint64 ActiveGeneration_ = 0;
    uint64 SchemaEpoch_ = 0;

    LoadedModule Active_;
    HostServiceProvider Services_;
    LoadedModule Retired_;
    HostServiceProvider RetiredServices_;
    game_api::GameInstance* Instance_ = nullptr;

    protocol::FrameReader Reader_;
    struct OutputFrame
    {
        ludus::foundation::Array<ludus::foundation::uint8> Bytes;
        usize Offset = 0;
    };
    // Capacity is reserved at construction; an allocation failure while
    // queueing latches the channel as failed instead of dropping a frame.
    ludus::foundation::Array<OutputFrame> Output_;
    usize OutputBytes_ = 0;
    bool ChannelFailed_ = false;
    struct RetainedResult
    {
        uint64 Request = 0;
        std::string Payload;
        protocol::Message Result;
    };
    ludus::foundation::Array<RetainedResult> Results_;
    uint64 PendingRequest_ = 0;
    std::string PendingPayload_;
    bool RetainPending_ = false;
    uint64 HighestMutationRequest_ = 0;
};
} // namespace ludus::runtime::game_host
