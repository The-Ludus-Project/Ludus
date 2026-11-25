#include "internal/session_flow.h"

#include <limits>

// Thanks to Ron Barbosa, "The Game State Observer Pattern", Game Engine Gems 1, chapter 26, pp. 415-431, and Charles
// Farris, "Function Pointer-Based, Embedded Finite-State Machines", Game Programming Gems 3, 3.3, pp. 256-267, for
// explicit state propagation and deferred begin/end hooks. This table keeps their deferred transition and state
// snapshot ideas with typed requests and plain enums instead of function-pointer states; see
// docs/architecture/gameplay-foundations-gems-review.md#flow-and-trigger-semantics.
namespace ludus::world_demo
{
namespace
{
/// Condition a transition row checks against flow state before it is eligible. Guards read state only.
enum class FlowGuard : uint8
{
    None,
    /// The load came from a session or a faulted world that a failure returns to.
    HasRetainedSession,
};
/// One row of the transition table.
struct FlowRow final
{
    FlowState From = FlowState::Boot;
    FlowRequestKind Kind = FlowRequestKind::ServicesReady;
    FlowGuard Guard = FlowGuard::None;
    /// Whether the target is the return state rather than To.
    bool IsReturn = false;
    FlowState To = FlowState::Boot;
};
/// Rows in FLOW_TRANSITIONS.
constexpr usize FLOW_TRANSITION_COUNT = 29;
/// The flow transition table; the first row matching state, kind and guard wins. Self transitions are explicit:
/// only Preparing restarts itself, for a load that supersedes the in-flight candidate.
constexpr foundation::StaticArray<FlowRow, FLOW_TRANSITION_COUNT> FLOW_TRANSITIONS{{
    { .From = FlowState::Boot, .Kind = FlowRequestKind::ServicesReady, .To = FlowState::Frontend },
    { .From = FlowState::Boot, .Kind = FlowRequestKind::Quit, .To = FlowState::Shutdown },
    { .From = FlowState::Frontend, .Kind = FlowRequestKind::Load, .To = FlowState::Preparing },
    { .From = FlowState::Frontend, .Kind = FlowRequestKind::Quit, .To = FlowState::Shutdown },
    { .From = FlowState::Preparing, .Kind = FlowRequestKind::CandidateReady, .To = FlowState::Playing },
    {
        .From = FlowState::Preparing,
        .Kind = FlowRequestKind::CandidateFailed,
        .Guard = FlowGuard::HasRetainedSession,
        .IsReturn = true,
    },
    { .From = FlowState::Preparing, .Kind = FlowRequestKind::CandidateFailed, .To = FlowState::Frontend },
    { .From = FlowState::Preparing, .Kind = FlowRequestKind::Load, .To = FlowState::Preparing },
    { .From = FlowState::Preparing, .Kind = FlowRequestKind::Leave, .To = FlowState::Frontend },
    { .From = FlowState::Preparing, .Kind = FlowRequestKind::Quit, .To = FlowState::Shutdown },
    { .From = FlowState::Playing, .Kind = FlowRequestKind::Pause, .To = FlowState::Paused },
    { .From = FlowState::Playing, .Kind = FlowRequestKind::Complete, .To = FlowState::Result },
    { .From = FlowState::Playing, .Kind = FlowRequestKind::Load, .To = FlowState::Preparing },
    { .From = FlowState::Playing, .Kind = FlowRequestKind::Leave, .To = FlowState::Frontend },
    { .From = FlowState::Playing, .Kind = FlowRequestKind::Fault, .To = FlowState::Faulted },
    { .From = FlowState::Playing, .Kind = FlowRequestKind::Quit, .To = FlowState::Shutdown },
    { .From = FlowState::Paused, .Kind = FlowRequestKind::Resume, .To = FlowState::Playing },
    { .From = FlowState::Paused, .Kind = FlowRequestKind::Load, .To = FlowState::Preparing },
    { .From = FlowState::Paused, .Kind = FlowRequestKind::Leave, .To = FlowState::Frontend },
    { .From = FlowState::Paused, .Kind = FlowRequestKind::Fault, .To = FlowState::Faulted },
    { .From = FlowState::Paused, .Kind = FlowRequestKind::Quit, .To = FlowState::Shutdown },
    { .From = FlowState::Result, .Kind = FlowRequestKind::Continue, .To = FlowState::Playing },
    { .From = FlowState::Result, .Kind = FlowRequestKind::Load, .To = FlowState::Preparing },
    { .From = FlowState::Result, .Kind = FlowRequestKind::Leave, .To = FlowState::Frontend },
    { .From = FlowState::Result, .Kind = FlowRequestKind::Fault, .To = FlowState::Faulted },
    { .From = FlowState::Result, .Kind = FlowRequestKind::Quit, .To = FlowState::Shutdown },
    { .From = FlowState::Faulted, .Kind = FlowRequestKind::Load, .To = FlowState::Preparing },
    { .From = FlowState::Faulted, .Kind = FlowRequestKind::Leave, .To = FlowState::Frontend },
    { .From = FlowState::Faulted, .Kind = FlowRequestKind::Quit, .To = FlowState::Shutdown },
}};
/// Priority ranks; a lower rank wins.
enum class FlowPriority : uint8
{
    Quit,
    Fault,
    Candidate,
    Leave,
    Load,
    Outcome,
    Pause,
    Startup,
};
[[nodiscard]] bool IsObservedKind(FlowRequestKind kind) noexcept
{
    return kind == FlowRequestKind::CandidateReady || kind == FlowRequestKind::CandidateFailed ||
           kind == FlowRequestKind::Fault;
}
} // namespace
bool TryMakeBuiltinLevelReference(std::string_view id, LevelReference& outReference) noexcept
{
    LevelReference reference{ .Source = LevelSource::Builtin };
    if (id.empty() == true || id.size() > sizeof(reference.Builtin.Text))
    {
        return false;
    }
    for (usize index = 0; index < id.size(); ++index)
    {
        reference.Builtin.Text[index] = id[index];
    }
    outReference = reference;
    return true;
}
FlowPolicy GetFlowPolicy(FlowState state) noexcept
{
    switch (state)
    {
        case FlowState::Playing:
            return { .Simulation = SimulationPolicy::Running, .Input = InputRoute::Gameplay };
        case FlowState::Paused:
            return { .Simulation = SimulationPolicy::Held, .Input = InputRoute::Interface, .CanStep = true };
        case FlowState::Preparing:
        case FlowState::Result:
            return { .Simulation = SimulationPolicy::Held, .Input = InputRoute::Interface };
        case FlowState::Boot:
        case FlowState::Frontend:
        case FlowState::Faulted:
        case FlowState::Shutdown:
            return { .Simulation = SimulationPolicy::Stopped, .Input = InputRoute::Interface };
    }
    return {};
}
bool IsSessionState(FlowState state) noexcept
{
    return state == FlowState::Playing || state == FlowState::Paused || state == FlowState::Result;
}
uint8 GetFlowRequestPriority(FlowRequestKind kind) noexcept
{
    FlowPriority priority = FlowPriority::Startup;
    switch (kind)
    {
        case FlowRequestKind::Quit:
            priority = FlowPriority::Quit;
            break;
        case FlowRequestKind::Fault:
            priority = FlowPriority::Fault;
            break;
        case FlowRequestKind::CandidateReady:
        case FlowRequestKind::CandidateFailed:
            priority = FlowPriority::Candidate;
            break;
        case FlowRequestKind::Leave:
            priority = FlowPriority::Leave;
            break;
        case FlowRequestKind::Load:
            priority = FlowPriority::Load;
            break;
        case FlowRequestKind::Complete:
        case FlowRequestKind::Continue:
            priority = FlowPriority::Outcome;
            break;
        case FlowRequestKind::Pause:
        case FlowRequestKind::Resume:
            priority = FlowPriority::Pause;
            break;
        case FlowRequestKind::ServicesReady:
            priority = FlowPriority::Startup;
            break;
    }
    return static_cast<uint8>(priority);
}
Status SessionFlow::Request(const FlowRequest& request, uint64& outToken) noexcept
{
    if (IsObservedKind(request.Kind) == true)
    {
        return Status::InvalidConfiguration;
    }
    if (mNextToken == std::numeric_limits<uint64>::max())
    {
        return Status::IdentityExhausted;
    }
    const bool isReservedQuit = request.Kind == FlowRequestKind::Quit && mHasPendingQuit == false;
    if (isReservedQuit == false && mPendingCount == MAX_FLOW_REQUESTS)
    {
        return Status::CapacityExceeded;
    }
    const PendingRequest pending{ .Token = mNextToken++, .Request = request };
    if (isReservedQuit == true)
    {
        mPendingQuit = pending;
        mHasPendingQuit = true;
    }
    else
    {
        mPending[mPendingCount++] = pending;
    }
    outToken = pending.Token;
    return Status::Success;
}
Status SessionFlow::BeginBoundary() noexcept
{
    if (mIsInBoundary == true)
    {
        return Status::InvalidPhase;
    }
    if (mBoundary == std::numeric_limits<uint64>::max())
    {
        return Status::IdentityExhausted;
    }
    ++mBoundary;
    mCapturedCount = 0;
    for (usize index = 0; index < mPendingCount; ++index)
    {
        mCaptured[mCapturedCount++] = { .Token = mPending[index].Token, .Request = mPending[index].Request };
    }
    if (mHasPendingQuit == true)
    {
        mCaptured[mCapturedCount++] = { .Token = mPendingQuit.Token, .Request = mPendingQuit.Request };
    }
    mPendingCount = 0;
    mHasPendingQuit = false;
    mObservedCount = 0;
    mOutcomeCount = 0;
    mBoundaryState = mState;
    mBoundaryReturnState = mReturnState;
    mHasSelected = false;
    mIsInBoundary = true;
    return Status::Success;
}
Status SessionFlow::Observe(const FlowRequest& request) noexcept
{
    if (mIsInBoundary == false || mHasSelected == true)
    {
        return Status::InvalidPhase;
    }
    if (mObservedCount == MAX_FLOW_OBSERVATIONS)
    {
        return Status::CapacityExceeded;
    }
    if (mNextToken == std::numeric_limits<uint64>::max())
    {
        return Status::IdentityExhausted;
    }
    mCaptured[mCapturedCount++] = { .Token = mNextToken++, .Request = request };
    ++mObservedCount;
    return Status::Success;
}
bool SessionFlow::findTarget(FlowPosition position, FlowRequestKind kind, FlowState& outTarget) noexcept
{
    for (const FlowRow& row : FLOW_TRANSITIONS)
    {
        if (row.From != position.State || row.Kind != kind)
        {
            continue;
        }
        if (row.Guard == FlowGuard::HasRetainedSession && position.ReturnState == FlowState::Frontend)
        {
            continue;
        }
        outTarget = row.IsReturn == true ? position.ReturnState : row.To;
        return true;
    }
    return false;
}
bool SessionFlow::SelectTransition(FlowTransition& outTransition) noexcept
{
    if (mIsInBoundary == false || mHasSelected == true)
    {
        return false;
    }
    mHasSelected = true;
    bool hasCandidate = false;
    usize selected = 0;
    FlowState selectedTarget = mState;
    for (usize index = 0; index < mCapturedCount; ++index)
    {
        FlowState target = mState;
        if (findTarget(
                {
                    .State = mState,
                    .ReturnState = mReturnState,
                },
                mCaptured[index].Request.Kind,
                target) == false)
        {
            continue;
        }
        // Capture order is admission order, so a strict comparison keeps the earlier request on equal priority.
        if (hasCandidate == false || GetFlowRequestPriority(mCaptured[index].Request.Kind) <
                                         GetFlowRequestPriority(mCaptured[selected].Request.Kind))
        {
            hasCandidate = true;
            selected = index;
            selectedTarget = target;
        }
    }
    if (hasCandidate == false)
    {
        return false;
    }
    outTransition =
    {
        .Request = selected,
        .From = mState,
        .To = selectedTarget,
        .Kind = mCaptured[selected].Request.Kind,
        .Level = mCaptured[selected].Request.Level,
        .Token = mCaptured[selected].Token,
    };
    return true;
}
void SessionFlow::resolve(usize request, FlowRequestResult result) noexcept
{
    if (request >= mCapturedCount || mCaptured[request].IsResolved == true)
    {
        return;
    }
    mCaptured[request].Result = result;
    mCaptured[request].IsResolved = true;
}
void SessionFlow::ApplyTransition(const FlowTransition& transition) noexcept
{
    resolve(transition.Request, FlowRequestResult::Applied);
    if (transition.To == FlowState::Preparing && transition.From != FlowState::Preparing)
    {
        mReturnState = transition.From == FlowState::Frontend ? FlowState::Frontend : transition.From;
    }
    else if (transition.To != FlowState::Preparing)
    {
        mReturnState = FlowState::Frontend;
    }
    mState = transition.To;
    mLastChange =
    {
        .Boundary = mBoundary,
        .From = transition.From,
        .To = transition.To,
        .Cause = transition.Kind,
        .Token = transition.Token,
    };
    ++mChangeCount;
}
void SessionFlow::RejectTransition(const FlowTransition& transition) noexcept
{
    resolve(transition.Request, FlowRequestResult::PreflightFailed);
}
void SessionFlow::EndBoundary() noexcept
{
    if (mIsInBoundary == false)
    {
        return;
    }
    for (usize index = 0; index < mCapturedCount; ++index)
    {
        CapturedRequest& captured = mCaptured[index];
        if (captured.IsResolved == false)
        {
            FlowState target = mBoundaryState;
            captured.Result = findTarget({ .State = mBoundaryState, .ReturnState = mBoundaryReturnState },
                                         captured.Request.Kind,
                                         target) == true
                                  ? FlowRequestResult::Superseded
                                  : FlowRequestResult::Rejected;
            captured.IsResolved = true;
        }
        mOutcomes[index] = { .Token = captured.Token, .Kind = captured.Request.Kind, .Result = captured.Result };
    }
    mOutcomeCount = mCapturedCount;
    mCapturedCount = 0;
    mIsInBoundary = false;
}
} // namespace ludus::world_demo
