#include "internal/host_session.h"
#include "internal/frame_presenter.h"

#include "internal/identity.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/runtime/game_api/frame_clear.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <string>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ludus::runtime::game_host
{
namespace
{
LUDUS_DEFINE_LOG_CATEGORY(LOG_SESSION, "GameHost");

using game_api::ByteSpan;
using game_api::ByteView;
using game_api::CheckpointHeader;
using game_api::CreateInfo;
using game_api::FrameInput;
using game_api::GameCandidate;
using game_api::GameInstance;
using game_api::RenderParams;
using game_api::Status;
using protocol::CommandKind;
using protocol::CommandStatus;
using protocol::EventKind;
using protocol::Message;
using protocol::ReloadPhase;

// Host frame pacing: 60 Hz frame starts (16,666,667 ns, whole nanoseconds), a
// four-interval lateness bound before re-anchoring. The simulation is unaffected: it
// advances one fixed tick per frame (design 8).
constexpr uint64 HOST_FRAME_INTERVAL_NANOSECONDS = 16'666'667;
constexpr uint64 HOST_MAXIMUM_LATE_INTERVALS = 4;

[[nodiscard]] platform::PacingPolicy MakeHostPacingPolicy() noexcept
{
    return
    {
        .Interval = {HOST_FRAME_INTERVAL_NANOSECONDS},
        .MaximumLateness = {HOST_FRAME_INTERVAL_NANOSECONDS * HOST_MAXIMUM_LATE_INTERVALS},
    };
}

// Checkpoint body cap (design 8): 16 MiB.
constexpr usize kMaxCheckpointBody = static_cast<usize>(16) * 1024 * 1024;

// Thanks to Glenn Fowler, Landon Curt Noll and Phong Vo, "FNV Hash",
// "FNV-1a alternate algorithm" and "Parameters of the FNV-1/FNV-1a hash":
// https://www.isthe.com/chongo/tech/comp/fnv/ . Adopt the XOR/multiply sequence
// and published constants for a non-cryptographic hash; collisions remain possible.
// FNV-1a over the body for host-side envelope digest validation.
[[nodiscard]] uint64 Fnv1a(const ludus::foundation::uint8* data, usize size) noexcept
{
    uint64 hash = 0xCBF29CE484222325ULL;
    for (usize i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

// Validate the checkpoint envelope the module produced BEFORE trusting the body
// (design 8; review finding 6): bounded format version, non-zero schema, body
// length agreeing with what was written, and a matching digest. The host does
// not interpret the body (that is the module's schema) but it refuses to carry
// a self-inconsistent envelope across the boundary.
[[nodiscard]] bool
ValidateCheckpointEnvelope(const CheckpointHeader& header, const ludus::foundation::uint8* body, usize written) noexcept
{
    if (header.FormatVersion != 1 || header.SchemaVersion == 0)
    {
        return false;
    }
    if (header.BodyLength != written || written == 0 || written > kMaxCheckpointBody)
    {
        return false;
    }
    return Fnv1a(body, written) == header.BodyDigest;
}
} // namespace

std::string_view PlayStateName(PlayState state) noexcept
{
    switch (state)
    {
        case PlayState::Stopped:
            return "Stopped";
        case PlayState::Starting:
            return "Starting";
        case PlayState::Running:
            return "Running";
        case PlayState::Paused:
            return "Paused";
        case PlayState::Reloading:
            return "Reloading";
        case PlayState::Stopping:
            return "Stopping";
        case PlayState::Failed:
            return "Failed";
        case PlayState::CleanupUnknown:
            return "CleanupUnknown";
    }
    return "Unknown";
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
HostSession::HostSession(uint64 projectId, uint64 gameId, int32 controlFd, uint64 projectEpoch) noexcept
    : ProjectId_(projectId), GameId_(gameId), ProjectEpoch_(projectEpoch), ControlFd_(controlFd)
{
    SessionId_ = static_cast<uint64>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                 (static_cast<uint64>(::getpid()) << 32);
    if (SessionId_ == 0)
    {
        SessionId_ = 1;
    }
    // Reserve so warm emission does not allocate. A failed reservation is not
    // fatal here: queueing retries growth and latches a channel failure.
    (void)Output_.TryEnsureCapacity(protocol::kMaxPendingCommands + 2);
    (void)Results_.TryEnsureCapacity(protocol::kMaxRetainedResults);
}

HostSession::~HostSession() noexcept
{
    (void)RetireActive();
}

bool HostSession::RetireActive() noexcept
{
    if (ActiveRetired_)
    {
        return State_ != PlayState::CleanupUnknown;
    }
    ActiveRetired_ = true;
    Services_.GateWork();
    if (InstanceReady_ && Instance_ != nullptr && Active_.IsLoaded() && !Quiesced_ && ModuleHasReload())
    {
        Quiesced_ = Active_.Table().Quiesce != nullptr && Active_.Table().Quiesce(Instance_) == Status::Ok;
    }
    if ((InstanceReady_ && ModuleHasReload() && !Quiesced_) || Services_.OutstandingWork() != 0)
    {
        // A worker may still use the instance and execute its module code.
        // Leave all three alive; process exit is the only safe recovery.
        Active_.PinUntilProcessExit();
        Services_.PinUntilProcessExit();
        Instance_ = nullptr;
        State_ = PlayState::CleanupUnknown;
        return false;
    }
    Services_.Retire();
    if (Instance_ != nullptr && Active_.IsLoaded())
    {
        Active_.Table().Destroy(Instance_);
        Instance_ = nullptr;
    }
    if (Services_.OutstandingAllocations() != 0 || Services_.OutstandingWork() != 0)
    {
        Active_.PinUntilProcessExit();
        Services_.PinUntilProcessExit();
        State_ = PlayState::CleanupUnknown;
        return false;
    }
    if (!Active_.Close())
    {
        Services_.PinUntilProcessExit();
        State_ = PlayState::CleanupUnknown;
        return false;
    }
    return true;
}

void HostSession::Emit(const Message& message, bool priority) noexcept
{
    if (ControlFd_ < 0)
    {
        return;
    }
    Message event = message;
    event.SetHexId("epoch", ProjectEpoch_);
    const std::string payload = event.Serialize();
    ludus::foundation::Array<ludus::foundation::uint8> frame;
    if (protocol::EncodeFrame(payload, frame) == false)
    {
        if (payload.size() <= protocol::kMaxControlFrameBytes)
        {
            latchOutputAllocationFailure(); // An in-bound frame failed to allocate.
        }
        return;
    }
    constexpr usize kTerminalReserve = 4096;
    const usize budget = protocol::kMaxCommandQueueBytes - (priority ? 0 : kTerminalReserve);
    const usize frameBudget = protocol::kMaxPendingCommands + (priority ? 2 : 0);
    if (OutputBytes_ > budget || frame.GetSize() > budget - OutputBytes_ || Output_.GetSize() >= frameBudget)
    {
        // Never discard a partial frame or silently lose a mutation outcome.
        // Backpressure stops new mutations; Status/retained results reconcile.
        return;
    }
    const usize frameBytes = frame.GetSize();
    if (Output_.TryAdd({std::move(frame), 0}) == false)
    {
        latchOutputAllocationFailure();
        return;
    }
    OutputBytes_ += frameBytes;
    FlushOutput();
}

void HostSession::latchOutputAllocationFailure() noexcept
{
    // Dropping a whole event would silently lose an outcome; fail the channel
    // like a send error so the client observes the session end instead.
    LUDUS_LOG_ERROR(LOG_SESSION, "control-channel output allocation failed");
    ChannelFailed_ = true;
    StopLatched_ = true;
}

void HostSession::FlushOutput() noexcept
{
    while (Output_.IsEmpty() == false && ChannelFailed_ == false)
    {
        OutputFrame& frame = Output_.GetFirst();
        const ssize_t n = ::send(ControlFd_,
                                 frame.Bytes.GetData() + frame.Offset,
                                 frame.Bytes.GetSize() - frame.Offset,
                                 MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            ChannelFailed_ = true;
            StopLatched_ = true;
            return;
        }
        if (n == 0)
        {
            return;
        }
        const usize sent = static_cast<usize>(n);
        frame.Offset += sent;
        OutputBytes_ -= sent;
        if (frame.Offset == frame.Bytes.GetSize())
        {
            Output_.RemoveAt(0);
        }
    }
}

void HostSession::EmitReloadPhase(ReloadPhase phase) noexcept
{
    Message m;
    m.SetString("event", protocol::EventKindName(EventKind::ReloadPhase));
    m.SetUint("protocol", protocol::kProtocolVersion);
    m.SetHexId("session", SessionId_);
    m.SetString("phase", protocol::ReloadPhaseName(phase));
    m.SetHexId("generation", ActiveGeneration_);
    Emit(m);
}

void HostSession::EmitCommandResult(uint64 requestId, CommandStatus status, std::string_view message) noexcept
{
    Message m;
    m.SetString("event", protocol::EventKindName(EventKind::CommandResult));
    m.SetUint("protocol", protocol::kProtocolVersion);
    m.SetHexId("session", SessionId_);
    m.SetHexId("request", requestId);
    m.SetString("status", protocol::CommandStatusName(status));
    m.SetHexId("generation", ActiveGeneration_);
    m.SetUint("schema_epoch", SchemaEpoch_);
    m.SetString("state", PlayStateName(State_));
    if (!message.empty())
    {
        m.SetString("message", message);
    }
    if (RetainPending_ && requestId == PendingRequest_)
    {
        RetainPending_ = false;
        if (Results_.TryAdd({requestId, std::move(PendingPayload_), m}) == false)
        {
            // A mutation outcome that cannot be retained cannot be reconciled.
            latchOutputAllocationFailure();
            return;
        }
    }
    Emit(m, StopLatched_);
}

void HostSession::EmitReady() noexcept
{
    if (ReadyEmitted_)
    {
        return;
    }
    ReadyEmitted_ = true;
    Message ready;
    ready.SetString("event", protocol::EventKindName(EventKind::SessionReady));
    ready.SetUint("protocol", protocol::kProtocolVersion);
    ready.SetHexId("session", SessionId_);
    ready.SetString("identity", CurrentHostIdentity());
    ready.SetUint("abi_major", CurrentAbiMajor());
    ready.SetUint("abi_minor", CurrentAbiMinor());
    Emit(ready);
}

bool HostSession::PrepareInitialLoad(std::string_view path, uint64 generation, game_api::ByteView authored) noexcept
{
    if (ControlFd_ < 0 || !Services_.IsValid() || generation == 0 || path.empty() || State_ != PlayState::Stopped)
    {
        return false;
    }
    InitialPath_ = path;
    InitialGeneration_ = generation;
    InitialAuthored_ = authored; // Borrowed from RunMain for the entire Run call.
    AwaitingLoad_ = true;
    State_ = PlayState::Starting;
    EmitReady();
    return true;
}

bool HostSession::LoadInitial(std::string_view modulePath, uint64 generation, game_api::ByteView authored) noexcept
{
    if (!Services_.IsValid() || generation == 0 || Instance_ != nullptr)
    {
        InitialFailure_ = RunResult::Internal;
        State_ = PlayState::Failed;
        return false;
    }
    State_ = PlayState::Starting;
    ActiveGeneration_ = generation;
    const LoadStatus loadStatus =
        LoadModule(modulePath, ActiveGeneration_, CurrentHostIdentity(), CurrentAbiMajor(), CurrentAbiMinor(), Active_);
    if (loadStatus != LoadStatus::Ok)
    {
        LUDUS_LOG_ERROR(LOG_SESSION, "initial load rejected: {}", LoadStatusName(loadStatus));
        InitialFailure_ = loadStatus == LoadStatus::PathInvalid || loadStatus == LoadStatus::DlopenFailed
                              ? RunResult::ModuleLoadFailed
                              : RunResult::IncompatibleModule;
        State_ = PlayState::Failed;
        return false;
    }

    CreateInfo info = {};
    info.StructSize = static_cast<uint32>(sizeof(CreateInfo));
    info.Services = &Services_.Services();
    info.ProjectId = ProjectId_;
    info.GameId = GameId_;
    info.ModuleGeneration = ActiveGeneration_;
    info.AuthoredDocument = authored;
    const Status createStatus = Active_.Table().Create(&info, &Instance_);
    if (createStatus != Status::Ok || Instance_ == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_SESSION, "initial Create failed: {}", static_cast<uint32>(createStatus));
        InitialFailure_ = RunResult::IncompatibleModule;
        // A failed Create may still return an instance or own service memory.
        // Keep its code/table reachable until the session destructor destroys
        // it and proves allocation retirement, or pins both until process exit.
        State_ = PlayState::Failed;
        return false;
    }

    Services_.Activate();
    InstanceReady_ = true;
    State_ = PlayState::Running;

    // SessionReady then ModuleReady declare actual capabilities/build identity.
    EmitReady();
    {
        Message mod;
        mod.SetString("event", protocol::EventKindName(EventKind::ModuleReady));
        mod.SetUint("protocol", protocol::kProtocolVersion);
        mod.SetHexId("session", SessionId_);
        mod.SetHexId("generation", ActiveGeneration_);
        mod.SetUint("capabilities", Active_.Metadata().Capabilities);
        mod.SetUint("property_schema", Active_.Metadata().PropertySchemaVersion);
        mod.SetUint("checkpoint_schema", Active_.Metadata().CheckpointSchemaVersion);
        Emit(mod);
    }
    return true;
}

void HostSession::StepFrame() noexcept
{
    if (ScriptPaused_)
    {
        return;
    }
    if (Instance_ == nullptr || !Active_.IsLoaded() || (State_ != PlayState::Running && State_ != PlayState::Paused))
    {
        return;
    }
    // Advance the simulation iff Running, or paused with exactly one pending
    // Step. Simulation time is driven by advanced ticks only, so a pause does
    // not advance time and Resume does not catch up (design 8).
    const bool advancing = (State_ == PlayState::Running) || (State_ == PlayState::Paused && StepRequested_);

    constexpr ludus::foundation::float64 kFixedDt = 1.0 / 60.0;
    FrameInput input = {};
    input.FrameIndex = FrameIndex_;
    input.DeltaSeconds = kFixedDt;
    input.ElapsedSeconds = static_cast<ludus::foundation::float64>(SimTicks_) * kFixedDt;
    input.Width = 800;
    input.Height = 600;
    if (Presenter_ != nullptr)
    {
        Presenter_->FillInput(input);
    }
    // Paused==0 drives the module to advance one tick; the module advances on
    // input.Paused alone (a single Step while paused advances exactly one tick).
    input.Paused = advancing ? 0 : 1;

    RenderParams render = {};
    const game_api::Status status = Active_.Table().Update(Instance_, &input, &render);
    if (status == game_api::Status::ScriptPaused &&
        game_api::HasCapability(Active_.Metadata().Capabilities, game_api::Capability::ScriptDebug))
    {
        ScriptPaused_ = true;
        return; // Partial invocation: no simulation tick/frame/checkpoint advances.
    }
    if (status != game_api::Status::Ok)
    {
        // A module Update error is a host failure for this session, not a
        // silently ignored result (review finding 8). Latch Failed and stop.
        LUDUS_LOG_ERROR(LOG_SESSION, "module Update failed at frame {}: {}", FrameIndex_, static_cast<uint32>(status));
        State_ = PlayState::Failed;
        StopLatched_ = true;
        return;
    }
    LastRender_ = render;
    ++FrameIndex_; // frame counter is wall-clock frames
    if (advancing)
    {
        ++SimTicks_; // simulation ticks only advance when not paused
        Timing_.RecordTick();
    }
    StepRequested_ = false;
}

bool HostSession::PumpControl() noexcept
{
    FlushOutput();
    if (StopLatched_ || ChannelFailed_)
    {
        return false;
    }
    if (ControlFd_ < 0)
    {
        return true;
    }
    // Non-blocking poll so the frame loop is never starved.
    pollfd pfd = {};
    pfd.fd = ControlFd_;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, 0);
    if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP)) != 0)
    {
        std::array<ludus::foundation::uint8, 4096> buffer = {};
        const ssize_t n = ::recv(ControlFd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (n == 0)
        {
            // EOF cancels the session (design 10); it does not reconnect.
            LUDUS_LOG_INFO(LOG_SESSION, "control EOF: ending session");
            return false;
        }
        if (n > 0)
        {
            Reader_.Append(buffer.data(), static_cast<usize>(n));
        }
    }
    std::string payload;
    while (Reader_.Next(payload))
    {
        Message command;
        if (!Message::Parse(payload, command))
        {
            EmitCommandResult(0, CommandStatus::ProtocolError, "malformed frame");
            continue;
        }
        Dispatch(command);
        if (StopLatched_)
        {
            return false;
        }
    }
    if (Reader_.Failed())
    {
        LUDUS_LOG_ERROR(LOG_SESSION, "control framing failure: ending session");
        return false;
    }
    return true;
}

void HostSession::Dispatch(const Message& command) noexcept
{
    std::string type;
    if (!command.GetString("command", type))
    {
        EmitCommandResult(0, CommandStatus::ProtocolError, "missing command");
        return;
    }
    uint64 requestId = 0;
    uint64 version = 0;
    uint64 session = 0;
    uint64 epoch = 0;
    if (!command.GetUint("protocol", version) || version != protocol::kProtocolVersion ||
        !command.GetHexId("request", requestId) || requestId == 0 || !command.GetHexId("session", session) ||
        session != SessionId_ || !command.GetHexId("epoch", epoch) || epoch != ProjectEpoch_)
    {
        EmitCommandResult(requestId, CommandStatus::ProtocolError, "protocol/request/session/epoch mismatch");
        return;
    }
    RetainPending_ = false;
    bool knownFields = false;
    if (type == "Ack")
    {
        knownFields = command.HasOnly({"command", "protocol", "request", "session", "epoch", "acknowledge"});
    }
    else if (type == "Status")
    {
        knownFields = command.HasOnly({"command", "protocol", "request", "session", "epoch", "reconcile"});
    }
    else if (type == "Stop" || type == "Hello")
    {
        knownFields = command.HasOnly({"command", "protocol", "request", "session", "epoch"});
    }
    else if (type == "Reload")
    {
        knownFields = command.HasOnly(
            {"command", "protocol", "request", "session", "epoch", "expected_generation", "module_path", "generation"});
    }
    else if (type == "Load")
    {
        knownFields = command.HasOnly({"command", "protocol", "request", "session", "epoch", "generation"});
    }
    else if (type == "ReloadAsset")
    {
        knownFields = command.HasOnly({"command",
                                       "protocol",
                                       "request",
                                       "session",
                                       "epoch",
                                       "expected_generation",
                                       "asset_id",
                                       "artifact",
                                       "digest"});
    }
    else if (type == "ScriptDebug")
    {
        knownFields = command.HasOnly(
            {"command", "protocol", "request", "session", "epoch", "expected_generation", "extension", "payload"});
    }
    else if (type == "ApplyEdits")
    {
        knownFields = command.HasOnly({"command",
                                       "protocol",
                                       "request",
                                       "session",
                                       "epoch",
                                       "expected_generation",
                                       "schema_epoch",
                                       "schema_version",
                                       "edits"});
    }
    else if (type == "Pause" || type == "Resume" || type == "Step" || type == "ReadProperties")
    {
        knownFields = command.HasOnly({"command", "protocol", "request", "session", "epoch", "expected_generation"});
    }
    if (!knownFields)
    {
        EmitCommandResult(requestId, CommandStatus::InvalidRequest, "unknown command or fields");
        return;
    }
    if (type == "Ack")
    {
        uint64 acknowledged = 0;
        if (!command.GetHexId("acknowledge", acknowledged))
        {
            EmitCommandResult(requestId, CommandStatus::InvalidRequest, "missing acknowledge id");
            return;
        }
        for (usize i = 0; i < Results_.GetSize(); ++i)
        {
            if (Results_[i].Request == acknowledged)
            {
                Results_.RemoveAt(i);
                break;
            }
        }
        EmitCommandResult(requestId, CommandStatus::Ok, "acknowledged");
        return;
    }

    if (type == protocol::CommandKindName(CommandKind::Stop))
    {
        StopLatched_ = true;
        if (State_ != PlayState::CleanupUnknown && State_ != PlayState::Failed)
        {
            State_ = PlayState::Stopping;
        }
        EmitCommandResult(requestId, CommandStatus::Ok, "stopping");
        return;
    }
    if (type == "Load")
    {
        uint64 generation = 0;
        if (!AwaitingLoad_ || !HelloReceived_ || State_ != PlayState::Starting ||
            !command.GetHexId("generation", generation) || generation != InitialGeneration_)
        {
            EmitCommandResult(requestId,
                              CommandStatus::InvalidRequest,
                              "Hello and the selected initial generation are required");
            return;
        }
        AwaitingLoad_ = false; // No retry may create a second instance.
        if (!LoadInitial(InitialPath_, InitialGeneration_, InitialAuthored_))
        {
            StopLatched_ = true;
            EmitCommandResult(requestId, CommandStatus::IncompatibleModule, "initial load failed");
        }
        else
        {
            EmitCommandResult(requestId, CommandStatus::Ok, "initial generation loaded");
        }
        return;
    }
    const bool mutation = type != protocol::CommandKindName(CommandKind::Status) && type != "Hello" &&
                          type != protocol::CommandKindName(CommandKind::ReadProperties);
    if (mutation)
    {
        if (State_ == PlayState::CleanupUnknown || State_ == PlayState::Failed)
        {
            EmitCommandResult(requestId, CommandStatus::RestartRequired, "retirement is uncertain; restart the host");
            return;
        }
        const std::string payload = command.Serialize();
        for (const auto& retained : Results_)
        {
            if (retained.Request == requestId)
            {
                if (retained.Payload == payload)
                {
                    Emit(retained.Result);
                }
                else
                {
                    EmitCommandResult(requestId, CommandStatus::InvalidRequest, "request id payload mismatch");
                }
                return;
            }
        }
        if (requestId <= HighestMutationRequest_)
        {
            EmitCommandResult(requestId,
                              CommandStatus::InvalidRequest,
                              "outcome no longer retained; reconcile with Status");
            return;
        }
        if (Results_.GetSize() >= protocol::kMaxRetainedResults || OutputBytes_ > protocol::kMaxCommandQueueBytes / 2 ||
            Output_.GetSize() >= protocol::kMaxPendingCommands / 2)
        {
            EmitCommandResult(requestId, CommandStatus::Busy, "acknowledge results or drain output before mutation");
            return;
        }
        PendingRequest_ = requestId;
        HighestMutationRequest_ = requestId;
        PendingPayload_ = payload;
        RetainPending_ = true;
    }
    if (mutation || type == "ReadProperties")
    {
        uint64 expected = 0;
        if (!command.GetHexId("expected_generation", expected) || expected != ActiveGeneration_)
        {
            EmitCommandResult(requestId, CommandStatus::SchemaChanged, "module generation changed; refresh values");
            return;
        }
    }
    if (type == "Hello" || type == protocol::CommandKindName(CommandKind::Status))
    {
        if (type == "Hello")
        {
            HelloReceived_ = true;
        }
        Message m;
        m.SetString("event", protocol::EventKindName(EventKind::CommandResult));
        m.SetUint("protocol", protocol::kProtocolVersion);
        m.SetHexId("session", SessionId_);
        m.SetHexId("request", requestId);
        m.SetString("status", protocol::CommandStatusName(CommandStatus::Ok));
        m.SetHexId("generation", ActiveGeneration_);
        m.SetUint("schema_epoch", SchemaEpoch_);
        m.SetString("state", PlayStateName(State_));
        m.SetUint("sim_ticks", SimTicks_);
        m.SetBool("script_paused", ScriptPaused_);
        m.SetUint("frame_index", FrameIndex_);
        m.SetUint("presented_frames", Presenter_ == nullptr ? 0 : Presenter_->PresentedFrames());
        m.SetBool("windowed", Presenter_ != nullptr && Presenter_->Windowed());
        m.SetHexId("clear_asset_generation", Presenter_ == nullptr ? 0 : Presenter_->ClearConfigurationGeneration());
        m.SetUint("work_leases", Services_.OutstandingWork());
        m.SetUint("host_allocations", Services_.OutstandingAllocations());
        m.SetUint("retained_results", Results_.GetSize());
        m.SetString("identity", CurrentHostIdentity());
        m.SetUint("capabilities", Active_.Metadata().Capabilities);
        m.SetUint("schema_version", Active_.Metadata().PropertySchemaVersion);
        // Runtime-computed intervals and counts only; the editor never compares clocks.
        Message timing;
        WriteTimingRecord(Timing_.GetReport(), timing);
        m.SetRecords(TIMING_RECORD_KEY, {&timing, 1});
        uint64 reconcile = 0;
        if (command.Has("reconcile"))
        {
            if (!command.GetHexId("reconcile", reconcile))
            {
                EmitCommandResult(requestId, CommandStatus::InvalidRequest, "invalid reconciliation id");
                return;
            }
            m.SetHexId("reconciled_request", reconcile);
            m.SetString("reconciled_status", "NotRetained");
            for (const auto& retained : Results_)
            {
                if (retained.Request == reconcile)
                {
                    std::string status;
                    (void)retained.Result.GetString("status", status);
                    m.SetString("reconciled_status", status);
                    break;
                }
            }
        }
        if (Results_.IsEmpty() == false)
        {
            m.SetHexId("last_request", Results_.GetLast().Request);
            std::string status;
            (void)Results_.GetLast().Result.GetString("status", status);
            m.SetString("last_status", status);
        }
        Emit(m);
        return;
    }
    if (type == "ScriptDebug")
    {
        uint64 extension = 0;
        std::string payload;
        if (State_ != PlayState::Running || !command.GetUint("extension", extension) || extension != 1 ||
            !command.GetString("payload", payload) || payload.empty() || payload.size() > 4096 ||
            !protocol::ValidUtf8(payload) ||
            !game_api::HasCapability(Active_.Metadata().Capabilities, game_api::Capability::ScriptDebug))
        {
            EmitCommandResult(requestId,
                              CommandStatus::InvalidRequest,
                              "script-debug capability/extension/payload unavailable");
            return;
        }
        ludus::foundation::uint8 output[4096] = {};
        usize written = 0;
        uint32 paused = ScriptPaused_ ? 1 : 0;
        const auto status = Active_.Table().ProcessScriptDebug(
            Instance_,
            {reinterpret_cast<const ludus::foundation::uint8*>(payload.data()), payload.size()},
            {output, sizeof(output)},
            &written,
            &paused);
        if (status != Status::Ok || written > sizeof(output) || paused > 1 ||
            !protocol::ValidUtf8({reinterpret_cast<const char*>(output), written}))
        {
            EmitCommandResult(requestId, CommandStatus::InvalidRequest, "script-debug request rejected");
            return;
        }
        ScriptPaused_ = paused != 0;
        EmitCommandResult(requestId, CommandStatus::Ok, {reinterpret_cast<const char*>(output), written});
        return;
    }
    if (ScriptPaused_)
    {
        EmitCommandResult(requestId, CommandStatus::Busy, "partial script tick; use script-debug resume or Stop");
        return;
    }
    if (type == protocol::CommandKindName(CommandKind::ReloadAsset))
    {
        uint64 assetId = 0;
        uint64 digest = 0;
        std::string encoded;
        if (Presenter_ == nullptr || !command.GetHexId("asset_id", assetId) ||
            assetId != game_api::kFrameClearAssetId || !command.GetHexId("digest", digest) ||
            !command.GetString("artifact", encoded) || encoded.size() != game_api::kFrameClearArtifactBytes * 2)
        {
            EmitCommandResult(requestId,
                              CommandStatus::InvalidRequest,
                              "only the host frame-clear configuration is supported");
            return;
        }
        std::array<ludus::foundation::uint8, game_api::kFrameClearArtifactBytes> bytes = {};
        for (usize i = 0; i < encoded.size(); ++i)
        {
            const char digit = encoded[i];
            if ((digit < '0' || digit > '9') && (digit < 'a' || digit > 'f'))
            {
                EmitCommandResult(requestId, CommandStatus::InvalidRequest, "invalid cooked artifact encoding");
                return;
            }
            const auto nibble = static_cast<ludus::foundation::uint8>(digit <= '9' ? digit - '0' : digit - 'a' + 10);
            bytes[i / 2] = static_cast<ludus::foundation::uint8>((bytes[i / 2] << 4U) | nibble);
        }
        if (!Services_.PrepareResource(assetId) ||
            !Presenter_->ReplaceClearConfiguration({bytes.data(), bytes.size()}, digest))
        {
            EmitCommandResult(requestId,
                              CommandStatus::ReloadRejected,
                              "configuration validation failed; previous asset retained");
            return;
        }
        // PrepareResource completed all fallible storage work before commit.
        const bool resourceSet = Services_.SetResource(assetId, digest);
        LUDUS_REQUIRE(resourceSet);
        EmitCommandResult(requestId, CommandStatus::Ok, "frame-clear configuration replaced");
        return;
    }
    if (type == protocol::CommandKindName(CommandKind::Pause))
    {
        if (State_ == PlayState::Running)
        {
            Services_.GateWork();
            // Quiesce is a reload-capability callback; a mandatory-only module
            // may lack it. Host-level pause still stops simulation advance via
            // the Paused state even when the module has no Quiesce hook.
            if (ModuleHasReload() && Active_.Table().Quiesce != nullptr)
            {
                if (Active_.Table().Quiesce(Instance_) != game_api::Status::Ok)
                {
                    Services_.Activate();
                    EmitCommandResult(requestId, CommandStatus::InvalidRequest, "quiesce failed");
                    return;
                }
            }
            if (Services_.OutstandingWork() != 0)
            {
                Services_.Activate();
                if (ModuleHasReload() && Active_.Table().Resume != nullptr &&
                    Active_.Table().Resume(Instance_) != Status::Ok)
                {
                    Services_.GateWork();
                    State_ = PlayState::CleanupUnknown;
                    EmitCommandResult(requestId, CommandStatus::RestartRequired, "resume after busy pause failed");
                    return;
                }
                EmitCommandResult(requestId, CommandStatus::Busy, "work leases have not drained");
                return;
            }
            Quiesced_ = true;
            State_ = PlayState::Paused;
            if (Presenter_ != nullptr)
            {
                Presenter_->ResetInput();
            }
        }
        EmitCommandResult(requestId, CommandStatus::Ok, PlayStateName(State_));
        return;
    }
    if (type == protocol::CommandKindName(CommandKind::Resume))
    {
        if (State_ == PlayState::Paused)
        {
            Services_.Activate();
            if (ModuleHasReload() && Active_.Table().Resume != nullptr)
            {
                if (Active_.Table().Resume(Instance_) != game_api::Status::Ok)
                {
                    Services_.GateWork();
                    Quiesced_ = false;
                    State_ = PlayState::CleanupUnknown;
                    EmitCommandResult(requestId, CommandStatus::RestartRequired, "resume failed");
                    return;
                }
            }
            Quiesced_ = false;
            State_ = PlayState::Running;
            if (Presenter_ != nullptr)
            {
                Presenter_->ResetInput();
            }
        }
        EmitCommandResult(requestId, CommandStatus::Ok, PlayStateName(State_));
        return;
    }
    if (type == protocol::CommandKindName(CommandKind::Step))
    {
        if (State_ == PlayState::Paused)
        {
            StepRequested_ = true;
            StepFrame();
            EmitCommandResult(requestId,
                              State_ == PlayState::Failed ? CommandStatus::InvalidRequest : CommandStatus::Ok,
                              State_ == PlayState::Failed ? "step failed" : "stepped");
        }
        else
        {
            EmitCommandResult(requestId, CommandStatus::InvalidRequest, "step only while paused");
        }
        return;
    }
    if (type == protocol::CommandKindName(CommandKind::Reload))
    {
        std::string path;
        uint64 generation = 0;
        if (!command.GetString("module_path", path) || !command.GetHexId("generation", generation))
        {
            EmitCommandResult(requestId, CommandStatus::InvalidRequest, "reload needs module_path+generation");
            return;
        }
        const CommandStatus status = ReloadTo(path, generation);
        EmitCommandResult(requestId, status, protocol::CommandStatusName(status));
        return;
    }

    if (type == "ReadProperties")
    {
        const auto status = PublishProperties(requestId);
        EmitCommandResult(requestId, status, protocol::CommandStatusName(status));
        return;
    }
    if (type == "ApplyEdits")
    {
        const auto status = ApplyPropertyEdits(command);
        EmitCommandResult(requestId, status, protocol::CommandStatusName(status));
        if (status == CommandStatus::Ok || status == CommandStatus::StaleRevision)
        {
            // The retained result proves the mutation outcome. A bounded value
            // transfer can be retried independently if a slow reader fills it.
            (void)PublishProperties(requestId);
        }
        return;
    }

    EmitCommandResult(requestId, CommandStatus::InvalidRequest, "unsupported command");
}

CommandStatus HostSession::ReloadTo(std::string_view newModulePath, uint64 newGeneration) noexcept
{
    if (ScriptPaused_)
    {
        return CommandStatus::Busy;
    }
    if (Instance_ == nullptr || !Active_.IsLoaded() || (State_ != PlayState::Running && State_ != PlayState::Paused) ||
        newGeneration <= ActiveGeneration_)
    {
        return CommandStatus::InvalidRequest;
    }
    if (!game_api::HasCapability(Active_.Metadata().Capabilities, game_api::Capability::Reload))
    {
        return CommandStatus::RestartRequired;
    }

    if (Presenter_ != nullptr)
    {
        Presenter_->ResetInput();
    }
    const PlayState priorState = State_;
    State_ = PlayState::Reloading;

    // Phase 1 Validate: load B from a distinct immutable path and copy its
    // Query table. A keeps running on load/query failure (design 7).
    EmitReloadPhase(ReloadPhase::Validate);
    LoadedModule candidateModule;
    const LoadStatus loadStatus = LoadModule(newModulePath,
                                             newGeneration,
                                             CurrentHostIdentity(),
                                             CurrentAbiMajor(),
                                             CurrentAbiMinor(),
                                             candidateModule);
    if (loadStatus != LoadStatus::Ok)
    {
        State_ = priorState;
        EmitReloadPhase(ReloadPhase::Rejected);
        return loadStatus == LoadStatus::IdentityRejected ? CommandStatus::IncompatibleModule
                                                          : CommandStatus::ReloadRejected;
    }
    if (!game_api::HasCapability(candidateModule.Metadata().Capabilities, game_api::Capability::Reload))
    {
        State_ = priorState;
        EmitReloadPhase(ReloadPhase::Rejected);
        return CommandStatus::RestartRequired;
    }

    const bool wasPaused = (priorState == PlayState::Paused);

    // Recoverable rejection: discard the candidate module, restore A to its
    // EXACT prior pause state (do NOT resume a previously-paused A), re-emit the
    // phase and return the status. A keeps its instance/state/resources.
    auto reject = [&](CommandStatus status) -> CommandStatus {
        if (!wasPaused)
        {
            Services_.Activate();
            // A was running: it was quiesced for the attempt, so resume it. A
            // Resume failure means A's liveness is uncertain -> restart.
            if (ModuleHasReload() && Active_.Table().Resume != nullptr &&
                Active_.Table().Resume(Instance_) != Status::Ok)
            {
                Services_.GateWork();
                Quiesced_ = false;
                State_ = PlayState::CleanupUnknown;
                EmitReloadPhase(ReloadPhase::Rejected);
                return CommandStatus::RestartRequired;
            }
            Quiesced_ = false;
            State_ = PlayState::Running;
        }
        else
        {
            // A was already paused: leave it quiesced/paused, no Resume.
            State_ = PlayState::Paused;
        }
        EmitReloadPhase(ReloadPhase::Rejected);
        return status;
    };

    // Phase 2 Quiesce: gate dispatch and pause A if it was running. A paused A
    // is already quiesced; do not double-quiesce.
    EmitReloadPhase(ReloadPhase::Quiesce);
    Services_.GateWork();
    if (!wasPaused)
    {
        if (Active_.Table().Quiesce(Instance_) != Status::Ok)
        {
            // Quiesce failed without mutating A: restore running state directly.
            Services_.Activate();
            State_ = PlayState::Running;
            EmitReloadPhase(ReloadPhase::Rejected);
            return CommandStatus::ReloadRejected;
        }
    }
    if (Services_.OutstandingWork() != 0)
    {
        return reject(CommandStatus::Busy);
    }
    Quiesced_ = true;

    // Phase 3 Snapshot: write A's read-only checkpoint into bounded host storage,
    // then validate the host envelope (format/schema/length/digest) before it is
    // trusted (design 8; review finding 6).
    EmitReloadPhase(ReloadPhase::Snapshot);
    usize bodySize = 0;
    if (Active_.Table().CheckpointSize == nullptr ||
        Active_.Table().CheckpointSize(Instance_, &bodySize) != Status::Ok || bodySize == 0 ||
        bodySize > kMaxCheckpointBody)
    {
        return reject(CommandStatus::ReloadRejected);
    }
    ludus::foundation::Array<ludus::foundation::uint8> body;
    if (body.TryResize(bodySize) == false)
    {
        return reject(CommandStatus::ReloadRejected); // A keeps running unchanged.
    }
    CheckpointHeader header = {};
    ByteSpan span = {body.GetData(), body.GetSize()};
    usize written = 0;
    if (Active_.Table().WriteCheckpoint(Instance_, &header, span, &written) != Status::Ok || written != bodySize)
    {
        return reject(CommandStatus::ReloadRejected);
    }
    if (ValidateCheckpointEnvelope(header, body.GetData(), written) == false || header.ProjectId != ProjectId_ ||
        header.GameId != GameId_ || header.ModuleBuildId != ActiveGeneration_ ||
        header.SchemaVersion != Active_.Metadata().CheckpointSchemaVersion)
    {
        return reject(CommandStatus::ReloadRejected);
    }

    // Phase 4 Stage: create and validate B from the checkpoint in a staging
    // context. Candidate B gets a staging-scoped service provider (its own
    // allocation ledger and resource generation), so staging cannot mutate A's
    // active resources (design 7; review finding 6). All fallible work happens
    // here, BEFORE commit. B is created paused.
    EmitReloadPhase(ReloadPhase::Stage);
    HostServiceProvider stagingServices;
    if (!stagingServices.IsValid())
    {
        return reject(CommandStatus::ReloadRejected);
    }
    if (!stagingServices.CopyResourcesFrom(Services_))
    {
        return reject(CommandStatus::ReloadRejected);
    }
    CreateInfo info = {};
    info.StructSize = static_cast<uint32>(sizeof(CreateInfo));
    info.Services = &stagingServices.Services();
    info.ProjectId = ProjectId_;
    info.GameId = GameId_;
    info.ModuleGeneration = newGeneration;
    GameCandidate* candidate = nullptr;
    auto discard = [&](CommandStatus status) -> CommandStatus {
        if (candidate != nullptr)
        {
            stagingServices.Retire();
            candidateModule.Table().DiscardCandidate(candidate);
            candidate = nullptr;
        }
        const CommandStatus result = reject(status);
        if (stagingServices.OutstandingAllocations() != 0 || stagingServices.OutstandingWork() != 0)
        {
            Retired_ = std::move(candidateModule);
            RetiredServices_ = std::move(stagingServices);
            Retired_.PinUntilProcessExit();
            RetiredServices_.PinUntilProcessExit();
            State_ = PlayState::CleanupUnknown;
            return CommandStatus::RestartRequired;
        }
        return result;
    };
    ByteView bodyView = {body.data(), body.size()};
    const Status createStatus = candidateModule.Table().CreateCandidate(&info, &header, bodyView, &candidate);
    if (createStatus != Status::Ok || candidate == nullptr)
    {
        return discard(createStatus == Status::MigrationUnsupported ? CommandStatus::RestartRequired
                                                                    : CommandStatus::ReloadRejected);
    }
    if (candidateModule.Table().ValidateCandidate(candidate) != Status::Ok)
    {
        return discard(CommandStatus::ReloadRejected);
    }

    // Promote the candidate to B's live instance (still pre-commit from the
    // host's perspective: all fallible work is done, the host swap below is
    // allocation-free and non-failing). A CommitCandidate failure here is still
    // a recoverable rejection because nothing in the host has changed yet.
    GameInstance* newInstance = nullptr;
    if (candidateModule.Table().CommitCandidate(candidate, &newInstance) != Status::Ok || newInstance == nullptr)
    {
        return discard(CommandStatus::ReloadRejected);
    }

    // Phase 5 Commit: the allocation-free, non-failing host swap. From here on
    // there is no rollback; B is live. No user callback or allocation runs
    // inside the swap itself.
    EmitReloadPhase(ReloadPhase::Commit);
    GameInstance* oldInstance = Instance_;
    Retired_ = std::move(Active_);
    RetiredServices_ = std::move(Services_);
    Instance_ = newInstance;
    Active_ = std::move(candidateModule);
    Services_ = std::move(stagingServices);
    Quiesced_ = true;
    ActiveGeneration_ = newGeneration;
    ++SchemaEpoch_;
    resetFrameTiming();

    // Phase 6 Retire: destroy A with its own code in a retired context, then
    // release A's loader reference (only after its instance is destroyed and its
    // services drained). Uncertain retirement disables further reload.
    EmitReloadPhase(ReloadPhase::Retire);
    RetiredServices_.Retire();
    Retired_.Table().Destroy(oldInstance);
    if (RetiredServices_.OutstandingAllocations() != 0 || RetiredServices_.OutstandingWork() != 0)
    {
        // A left host allocations outstanding: residency is uncertain. Keep B
        // active but require restart rather than release code with live refs.
        State_ = PlayState::CleanupUnknown;
        Retired_.PinUntilProcessExit();
        RetiredServices_.PinUntilProcessExit();
        EmitReloadPhase(ReloadPhase::Rejected);
        return CommandStatus::RestartRequired;
    }
    if (!Retired_.Close())
    {
        RetiredServices_.PinUntilProcessExit();
        // dlclose failed: the old image's residency is uncertain (design 8).
        State_ = PlayState::CleanupUnknown;
        EmitReloadPhase(ReloadPhase::Rejected);
        return CommandStatus::RestartRequired;
    }

    // Resume B only if A was running before the reload (preserve prior pause).
    if (wasPaused)
    {
        State_ = PlayState::Paused;
    }
    else
    {
        Services_.Activate();
        if (Active_.Table().Resume != nullptr && Active_.Table().Resume(Instance_) != Status::Ok)
        {
            Services_.GateWork();
            Quiesced_ = false;
            State_ = PlayState::CleanupUnknown;
            return CommandStatus::RestartRequired;
        }
        Quiesced_ = false;
        State_ = PlayState::Running;
    }

    EmitReloadPhase(ReloadPhase::Done);
    return CommandStatus::Ok;
}

RunResult RunStaticEntry(const HostConfig& config, StaticEntryFn entry) noexcept
{
    // Fill the table in-process (static dispatch replaces only the module
    // lookup; design 2). No identity check is needed — the implementation is
    // compiled into this binary against this SDK.
    game_api::GameApiTable table = {};
    table.StructSize = static_cast<uint32>(sizeof(game_api::GameApiTable));
    table.AbiMajor = CurrentAbiMajor();
    table.AbiMinor = CurrentAbiMinor();
    const Status fillStatus = entry(CurrentAbiMajor(), CurrentAbiMinor(), &table);
    if (fillStatus != Status::Ok || table.Create == nullptr || table.Update == nullptr || table.Destroy == nullptr)
    {
        return RunResult::IncompatibleModule;
    }

    HostServiceProvider services;
    CreateInfo info = {};
    info.StructSize = static_cast<uint32>(sizeof(CreateInfo));
    info.Services = &services.Services();
    info.ProjectId = config.ProjectId;
    info.GameId = config.GameId;
    info.ModuleGeneration = 1;
    info.AuthoredDocument = config.AuthoredDocument;
    GameInstance* instance = nullptr;
    if (table.Create(&info, &instance) != Status::Ok || instance == nullptr)
    {
        services.Retire();
        if (instance != nullptr)
        {
            table.Destroy(instance);
        }
        return RunResult::Internal;
    }
    services.Activate();

    FramePresenter presenter;
    const auto started = presenter.Start(config.Mode);
    if (started != RunResult::Ok)
    {
        services.Retire();
        table.Destroy(instance);
        return started;
    }
    RunResult result = RunResult::Ok;
    platform::FramePacer pacer;
    if (pacer.Configure(MakeHostPacingPolicy()) != platform::PacingStatus::Ok)
    {
        services.Retire();
        table.Destroy(instance);
        return RunResult::Internal;
    }
    const platform::PacingClock pacingClock = platform::GetSystemPacingClock();
    for (uint64 i = 0; config.MaxFrames == 0 || i < config.MaxFrames; ++i)
    {
        // The same 60 Hz absolute schedule as the play session, without telemetry.
        (void)pacer.Pace(pacingClock);
        if (!presenter.Poll())
        {
            break;
        }
        FrameInput input = {};
        input.FrameIndex = i;
        input.DeltaSeconds = 1.0 / 60.0;
        input.ElapsedSeconds = static_cast<ludus::foundation::float64>(i) / 60.0;
        input.Width = 800;
        input.Height = 600;
        presenter.FillInput(input);
        RenderParams render = {};
        if (table.Update(instance, &input, &render) != Status::Ok)
        {
            result = RunResult::Internal;
            break;
        }
        result = presenter.Present(render);
        if (result != RunResult::Ok)
        {
            break;
        }
    }
    services.GateWork();
    if ((table.Quiesce != nullptr && table.Quiesce(instance) != Status::Ok) || services.OutstandingWork() != 0)
    {
        services.PinUntilProcessExit();
        return RunResult::Internal;
    }
    services.Retire();
    table.Destroy(instance);
    return services.OutstandingAllocations() == 0 && services.OutstandingWork() == 0 ? result : RunResult::Internal;
}

usize HostSession::CaptureCheckpoint(ludus::foundation::uint8* buffer, usize capacity) noexcept
{
    if (ScriptPaused_)
    {
        return 0;
    }
    if (Instance_ == nullptr || !Active_.IsLoaded() || Active_.Table().CheckpointSize == nullptr ||
        Active_.Table().WriteCheckpoint == nullptr || buffer == nullptr)
    {
        return 0;
    }
    usize bodySize = 0;
    if (Active_.Table().CheckpointSize(Instance_, &bodySize) != Status::Ok || bodySize == 0 || bodySize > capacity)
    {
        return 0;
    }
    CheckpointHeader header = {};
    ByteSpan span = {buffer, capacity};
    usize written = 0;
    if (Active_.Table().WriteCheckpoint(Instance_, &header, span, &written) != Status::Ok || written != bodySize)
    {
        return 0;
    }
    return written;
}

void HostSession::resetFrameTiming() noexcept
{
    Pacer_.Reset();
    FrameClock_.Reset();
}

bool HostSession::ModuleHasReload() const noexcept
{
    return Active_.IsLoaded() && game_api::HasCapability(Active_.Metadata().Capabilities, game_api::Capability::Reload);
}

usize HostSession::ReadActiveProperties(ludus::foundation::uint8* buffer, usize capacity) noexcept
{
    if (Instance_ == nullptr || !Active_.IsLoaded() || Active_.Table().ReadProperties == nullptr || buffer == nullptr)
    {
        return 0;
    }
    ByteSpan span = {buffer, capacity};
    usize written = 0;
    if (Active_.Table().ReadProperties(Instance_, span, &written) != Status::Ok)
    {
        return 0;
    }
    return written;
}

RunResult HostSession::RunLoop(uint64 maxFrames, FramePresenter* presenter) noexcept
{
    if (State_ != PlayState::Running && State_ != PlayState::Paused && State_ != PlayState::Starting)
    {
        return RunResult::Internal;
    }
    Presenter_ = presenter;
    RunResult result = RunResult::Ok;
    const platform::PacingPolicy pacing = MakeHostPacingPolicy();
    if (Pacer_.Configure(pacing) == platform::PacingStatus::Ok)
    {
        Timing_.SetPacingInterval(pacing.Interval);
    }
    FrameClock_.Reset();
    for (;;)
    {
        // Pace before the frame reads its one timestamp; absolute targets keep
        // the rate when frame work varies (high-resolution-time.md, "Native
        // frame pacing"). No busy spin and no pause/reload catch-up.
        const platform::PaceResult pace = Pacer_.Pace(PacingClock_);
        Timing_.Record(FrameClock_.Sample(ludus::foundation::time::Now()), pace);
        if ((presenter != nullptr && !presenter->Poll()) || !PumpControl())
        {
            break;
        }
        StepFrame();
        if (StopLatched_)
        {
            break;
        }
        if (presenter != nullptr && Instance_ != nullptr)
        {
            const auto presentedBefore = presenter->PresentedFrames();
            result = presenter->Present(LastRender_);
            if (presentedBefore == 0 && presenter->PresentedFrames() == 1)
            {
                Message frame;
                frame.SetString("event", protocol::EventKindName(EventKind::FramePresented));
                frame.SetUint("protocol", protocol::kProtocolVersion);
                frame.SetHexId("session", SessionId_);
                frame.SetHexId("generation", ActiveGeneration_);
                frame.SetUint("presented_frames", 1);
                Emit(frame);
            }
            if (result != RunResult::Ok)
            {
                State_ = PlayState::Failed;
                break;
            }
        }
        if (maxFrames != 0 && FrameIndex_ >= maxFrames)
        {
            break;
        }
    }

    Presenter_ = nullptr;
    // Stop: retire the instance before releasing the loader reference.
    if (!RetireActive())
    {
        LUDUS_LOG_ERROR(LOG_SESSION, "module retirement uncertain; process restart required");
    }
    if (State_ != PlayState::CleanupUnknown && State_ != PlayState::Failed)
    {
        State_ = PlayState::Stopped;
    }

    {
        Message ended;
        ended.SetString("event", protocol::EventKindName(EventKind::SessionEnded));
        ended.SetUint("protocol", protocol::kProtocolVersion);
        ended.SetHexId("session", SessionId_);
        ended.SetString("state", PlayStateName(State_));
        Emit(ended, true);
    }
    // Bounded terminal flush; a client that never reads cannot hang Stop.
    for (int i = 0; i < 100 && Output_.IsEmpty() == false && ChannelFailed_ == false; ++i)
    {
        FlushOutput();
        if (Output_.IsEmpty() == false)
        {
            pollfd pfd = {ControlFd_, POLLOUT, 0};
            (void)::poll(&pfd, 1, 2);
        }
    }

    if (State_ == PlayState::CleanupUnknown || State_ == PlayState::Failed)
    {
        return result == RunResult::Ok ? RunResult::Internal : result;
    }
    State_ = PlayState::Stopped;
    return result;
}
} // namespace ludus::runtime::game_host
