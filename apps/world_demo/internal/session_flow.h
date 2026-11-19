#pragma once
#include "level.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/gameplay/world/entity_id.h>

namespace ludus::world_demo
{
using gameplay::world::Status;

/// Game flow states (docs/architecture/gameplay-foundations.md#session-flow-and-hierarchical-state-machines).
/// Playing, Paused and Result are children of the Session superstate; the others are top-level states.
enum class FlowState : uint8
{
    /// Services are starting; no world is shown.
    Boot,
    /// No session runs; a load request starts one.
    Frontend,
    /// A candidate world is being prepared; any retained session is held.
    Preparing,
    /// Session child: the active world simulates.
    Playing,
    /// Session child: the active world is held and may be stepped one tick at a time.
    Paused,
    /// Session child: a completed outcome is shown; the world is held.
    Result,
    /// An authoritative tick failed; the partial world stays inspectable until a load restarts it.
    Faulted,
    /// Terminal state; every later request is rejected.
    Shutdown,
};

/// Typed flow transition requests. The first group is admitted by the game; the last three are raised by the session
/// itself when it observes completed state at a boundary.
enum class FlowRequestKind : uint8
{
    /// Boot finished starting services.
    ServicesReady,
    /// Prepare a candidate world: start, replace or restart a session, or restart an in-flight load.
    Load,
    /// Hold the playing world.
    Pause,
    /// Resume a paused world.
    Resume,
    /// Show a completed outcome.
    Complete,
    /// Continue playing after a result without replacing the world.
    Continue,
    /// Leave the session or cancel a load and return to Frontend.
    Leave,
    /// Shut down; accepted from every state except Shutdown, with the highest priority.
    Quit,
    /// Observed: the candidate world is ready to activate.
    CandidateReady,
    /// Observed: the candidate world failed or was cancelled outside the flow.
    CandidateFailed,
    /// Observed: an authoritative tick failed.
    Fault,
};

/// Where a Load request reads its level source from.
enum class LevelSource : uint8
{
    /// A built-in room named by LevelReference::Builtin.
    Builtin,
    /// The source the session was given with Session::SetProvidedLevel.
    Provided,
};

/// Owned reference to a level source; the bytes are resolved at the boundary that preflights the load.
struct LevelReference final
{
    LevelSource Source = LevelSource::Builtin;
    /// Built-in room ID; ignored for LevelSource::Provided.
    Name Builtin{};
};

/// Reference a built-in room by ID.
/// @param id Built-in room ID; it is copied, so the view need not outlive the call.
/// @param outReference Receives the reference; unchanged on failure.
/// @return false when the ID is empty or longer than a Name holds.
[[nodiscard]] bool TryMakeBuiltinLevelReference(std::string_view id, LevelReference& outReference) noexcept;

/// One typed flow request with its owned payload.
struct FlowRequest final
{
    FlowRequestKind Kind = FlowRequestKind::ServicesReady;
    /// Level to prepare; used only by FlowRequestKind::Load.
    LevelReference Level{};
};

/// Outcome of one admitted or observed flow request; every request receives exactly one.
enum class FlowRequestResult : uint8
{
    /// The request's transition was applied at this boundary.
    Applied,
    /// The current state has no transition for the request, or its guard failed.
    Rejected,
    /// An eligible request with a higher priority, or an earlier one of equal priority, was selected instead.
    Superseded,
    /// The selected transition failed its preflight; the state did not change.
    PreflightFailed,
};

/// Published outcome of one flow request.
struct FlowRequestOutcome final
{
    /// Token returned at admission; observed requests receive their own tokens.
    uint64 Token = 0;
    FlowRequestKind Kind = FlowRequestKind::ServicesReady;
    FlowRequestResult Result = FlowRequestResult::Rejected;
};

/// How the active world advances in a flow state.
enum class SimulationPolicy : uint8
{
    /// No world advances.
    Stopped,
    /// The world is shown but not advanced by frame time.
    Held,
    /// Frame time advances the world through the tick driver.
    Running,
};

/// Which consumer owns player input in a flow state.
enum class InputRoute : uint8
{
    /// Menus, overlays and other interface controls.
    Interface,
    /// The gameplay input bridge.
    Gameplay,
};

/// The flow policy snapshot adapters apply at their owner boundary, derived from the flow state alone.
struct FlowPolicy final
{
    SimulationPolicy Simulation = SimulationPolicy::Stopped;
    InputRoute Input = InputRoute::Interface;
    /// Whether a single fixed tick may be stepped.
    bool CanStep = false;
    [[nodiscard]] constexpr bool operator==(const FlowPolicy&) const noexcept = default;
};

/// Record of the most recent applied transition, for observers that poll after a boundary.
struct FlowChange final
{
    /// Boundary number at which the transition was applied; zero before the first change.
    uint64 Boundary = 0;
    FlowState From = FlowState::Boot;
    FlowState To = FlowState::Boot;
    /// Kind of the request that caused it.
    FlowRequestKind Cause = FlowRequestKind::ServicesReady;
    /// Token of that request.
    uint64 Token = 0;
};

/// The transition selected at a boundary, before its preflight.
struct FlowTransition final
{
    /// Index of the selected request in the captured batch.
    usize Request = 0;
    FlowState From = FlowState::Boot;
    FlowState To = FlowState::Boot;
    FlowRequestKind Kind = FlowRequestKind::ServicesReady;
    LevelReference Level{};
    uint64 Token = 0;
};

/// Most game requests admitted between two boundaries; Quit has one reserved slot beyond these.
inline constexpr usize MAX_FLOW_REQUESTS = 8;
/// Most requests the session observes at one boundary.
inline constexpr usize MAX_FLOW_OBSERVATIONS = 2;
/// Most requests one boundary captures: game requests, the reserved Quit slot and observations.
inline constexpr usize MAX_FLOW_BATCH = MAX_FLOW_REQUESTS + 1 + MAX_FLOW_OBSERVATIONS;

/// Policy snapshot of a flow state.
[[nodiscard]] FlowPolicy GetFlowPolicy(FlowState state) noexcept;
/// Whether `state` is a child of the Session superstate (Playing, Paused or Result).
[[nodiscard]] bool IsSessionState(FlowState state) noexcept;
/// Selection priority of a request kind; a lower value wins. Quit has the highest priority, then Fault.
[[nodiscard]] uint8 GetFlowRequestPriority(FlowRequestKind kind) noexcept;

/// Request batching and transition selection of the session flow state machine.
///
/// SessionFlow owns the flow state, the request buffer, the transition table and the published outcomes; the
/// session that owns the worlds runs a boundary through it and performs the preflight and the entry/exit actions:
///
/// 1. BeginBoundary captures the admitted requests. Requests admitted afterwards, including any raised by entry or
///    exit actions, are eligible at the next boundary.
/// 2. Observe appends requests the session raises from completed state.
/// 3. SelectTransition picks at most one transition: the eligible request with the highest priority, then the
///    lowest admission sequence. A table row is eligible when its source state matches and its guard passes.
/// 4. The session preflights it, then calls ApplyTransition or RejectTransition.
/// 5. EndBoundary gives every other captured request exactly one outcome and publishes the outcome list.
///
/// Every operation is bounded and allocates nothing. Not thread-safe: the session calls it between ticks.
class SessionFlow final
{
public:
    /// Admit a game request for the next boundary.
    /// @param request The typed request; observed kinds (CandidateReady, CandidateFailed, Fault) are rejected.
    /// @param outToken Receives the request token; unchanged on failure.
    /// @return InvalidConfiguration for an observed kind; CapacityExceeded when MAX_FLOW_REQUESTS are pending,
    /// except for a Quit, which uses its reserved slot while that is free; IdentityExhausted when tokens would wrap.
    [[nodiscard]] Status Request(const FlowRequest& request, uint64& outToken) noexcept;
    /// Start a boundary: capture the admitted requests and clear the previous boundary's outcomes.
    /// @return InvalidPhase when a boundary is already open; IdentityExhausted when the boundary count would wrap.
    [[nodiscard]] Status BeginBoundary() noexcept;
    /// Append a request the session observed from completed state.
    /// @return InvalidPhase outside a boundary or after selection; CapacityExceeded beyond MAX_FLOW_OBSERVATIONS;
    /// IdentityExhausted when tokens would wrap.
    [[nodiscard]] Status Observe(const FlowRequest& request) noexcept;
    /// Select at most one transition from the captured batch.
    /// @return false when no captured request is eligible in the current state.
    [[nodiscard]] bool SelectTransition(FlowTransition& outTransition) noexcept;
    /// Apply a preflighted transition: change the state, keep the return state for Preparing and record the change.
    void ApplyTransition(const FlowTransition& transition) noexcept;
    /// Report that the selected transition failed its preflight; the state is unchanged.
    void RejectTransition(const FlowTransition& transition) noexcept;
    /// Give every unresolved captured request its outcome and close the boundary.
    void EndBoundary() noexcept;

    [[nodiscard]] LUDUS_INLINE FlowState GetState() const noexcept
    {
        return mState;
    }
    /// State a failed load returns to: the session state or Faulted the load left, or Frontend.
    [[nodiscard]] LUDUS_INLINE FlowState GetReturnState() const noexcept
    {
        return mReturnState;
    }
    /// Whether a boundary is open.
    [[nodiscard]] LUDUS_INLINE bool IsInBoundary() const noexcept
    {
        return mIsInBoundary;
    }
    /// Outcomes published by the latest EndBoundary, in capture order; valid until the next BeginBoundary.
    [[nodiscard]] foundation::BorrowedView<const FlowRequestOutcome> GetOutcomes() const noexcept
    {
        return {mOutcomes.GetData(), mOutcomeCount};
    }
    /// The most recent applied transition.
    [[nodiscard]] LUDUS_INLINE const FlowChange& GetLastChange() const noexcept
    {
        return mLastChange;
    }
    /// Transitions applied so far.
    [[nodiscard]] LUDUS_INLINE uint64 GetChangeCount() const noexcept
    {
        return mChangeCount;
    }
    /// Boundaries begun so far.
    [[nodiscard]] LUDUS_INLINE uint64 GetBoundaryCount() const noexcept
    {
        return mBoundary;
    }

private:
    struct CapturedRequest final
    {
        uint64 Token = 0;
        FlowRequest Request{};
        FlowRequestResult Result = FlowRequestResult::Rejected;
        bool IsResolved = false;
    };
    struct PendingRequest final
    {
        uint64 Token = 0;
        FlowRequest Request{};
    };
    /// The state a request is judged from and the return state a failed load would resume.
    struct FlowPosition final
    {
        FlowState State = FlowState::Boot;
        FlowState ReturnState = FlowState::Frontend;
    };
    [[nodiscard]] static bool findTarget(FlowPosition position, FlowRequestKind kind, FlowState& outTarget) noexcept;
    void resolve(usize request, FlowRequestResult result) noexcept;
    foundation::StaticArray<PendingRequest, MAX_FLOW_REQUESTS> mPending{};
    PendingRequest mPendingQuit{};
    foundation::StaticArray<CapturedRequest, MAX_FLOW_BATCH> mCaptured{};
    foundation::StaticArray<FlowRequestOutcome, MAX_FLOW_BATCH> mOutcomes{};
    FlowChange mLastChange{};
    usize mPendingCount = 0;
    usize mCapturedCount = 0;
    usize mObservedCount = 0;
    usize mOutcomeCount = 0;
    uint64 mNextToken = 1;
    uint64 mBoundary = 0;
    uint64 mChangeCount = 0;
    FlowState mState = FlowState::Boot;
    FlowState mReturnState = FlowState::Frontend;
    /// State at BeginBoundary; eligibility of unselected requests is judged against it.
    FlowState mBoundaryState = FlowState::Boot;
    /// Return state at BeginBoundary, for the same judgement.
    FlowState mBoundaryReturnState = FlowState::Frontend;
    bool mHasPendingQuit = false;
    bool mIsInBoundary = false;
    bool mHasSelected = false;
};
} // namespace ludus::world_demo
