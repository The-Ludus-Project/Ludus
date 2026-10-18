#include "internal/lifecycle.h"

#include <ludus/foundation/base/checked_integer.hpp>

// Thanks to Jason Gregory, Game Engine Architecture, 3rd ed., section 6.1,
// "Subsystem Start-Up and Shut-Down", pp. 417-425, for explicit subsystem order;
// and Scott Bilas, "An Automatic Singleton Utility", Game Programming Gems,
// section 1.3, pp. 36-40, for separating controlled lifetime from access.
// This independently written coordinator uses caller-owned storage and a partial
// attempt journal; it does not adopt singleton registration or their sample code.
// Thanks to Julien Hamaide, "Multithread Job and Dependency System", Game
// Programming Gems 7, section 1.9, pp. 87-96, for explicit dependency completion;
// and Graham Wihlidal, "Responsive UI During Intensive Processing", Game Engine
// Toolset Development, chapter 37, pp. 423-430, for cooperative cancellation.
// We adapt completion into serial, nonblocking polls: a stop request cannot prove
// retirement, and a late start success must still be stopped.
// Sources consulted and departures: docs/architecture/module-lifecycle-reference-review.md.
namespace ludus::smoke::lifecycle
{
using namespace foundation;

Runner::Runner(std::span<const Node> plan, std::span<NodeRecord> records, std::span<usize> journal) noexcept
    : mPlan(plan), mRecords(records), mJournal(journal)
{
}

bool Runner::Validate() noexcept
{
    mPlanDiagnostic = {};
    if (mRecords.size() < mPlan.size() || mJournal.size() < mPlan.size())
    {
        mPlanDiagnostic.Error = PlanError::Capacity;
        return false;
    }
    for (usize id = 0; id < mPlan.size(); ++id)
    {
        mRecords[id] = {};
    }
    for (usize position = 0; position < mPlan.size(); ++position)
    {
        const auto& node = mPlan[position];
        PlanError error = PlanError::None;
        if (node.Id >= mPlan.size())
        {
            error = PlanError::InvalidId;
        }
        else if (mRecords[node.Id].Position != InvalidNode)
        {
            error = PlanError::DuplicateId;
        }
        else if (node.Name.empty())
        {
            error = PlanError::MissingName;
        }
        else if (node.BeginStart == nullptr || node.PollStart == nullptr || node.BeginStop == nullptr ||
                 node.PollStop == nullptr)
        {
            error = PlanError::MissingCallback;
        }
        if (error != PlanError::None)
        {
            mPlanDiagnostic = { .Error = error, .NodeId = node.Id };
            return false;
        }
        mRecords[node.Id].Position = position;
    }
    for (usize position = 0; position < mPlan.size(); ++position)
    {
        const auto& node = mPlan[position];
        for (const usize provider : node.Providers)
        {
            const auto error = provider >= mPlan.size()                  ? PlanError::InvalidProvider
                               : mRecords[provider].Position >= position ? PlanError::ProviderOrder
                                                                         : PlanError::None;
            if (error != PlanError::None)
            {
                mPlanDiagnostic = { .Error = error, .NodeId = node.Id, .ProviderId = provider };
                return false;
            }
        }
    }
    return true;
}

BeginStatus Runner::Begin() noexcept
{
    if (mAdvancing || (mState != State::Idle && mState != State::Stopped))
    {
        return BeginStatus::Busy;
    }
    if (Validate() == false)
    {
        return BeginStatus::InvalidPlan;
    }
    if (core::TryAdd(mGeneration, uint64{1}, mGeneration) == false)
    {
        return BeginStatus::GenerationExhausted;
    }
    mStopRequested.store(false, std::memory_order_release);
    mNext = 0;
    mAttempted = 0;
    mOutcome = Outcome::None;
    mFailure = {};
    mCleanupFailure = {};
    mState = State::Starting;
    return BeginStatus::Started;
}

void Runner::RequestStop() noexcept
{
    mStopRequested.store(true, std::memory_order_release);
}

void Runner::ObserveStopRequest() noexcept
{
    if (mStopRequested.load(std::memory_order_acquire) == false)
    {
        return;
    }
    if (mState == State::Starting || mState == State::Ready || mState == State::Idle)
    {
        mOutcome = mState == State::Starting ? Outcome::Cancelled : Outcome::Completed;
        mState = mAttempted == 0 ? State::Stopped : State::Stopping;
    }
}

bool Runner::ReportFailure(usize nodeId, uint32 error) noexcept
{
    if (mAdvancing || nodeId >= mPlan.size() || (mState != State::Starting && mState != State::Ready))
    {
        return false;
    }
    mFailure = { .NodeId = nodeId, .Error = error };
    mOutcome = Outcome::RuntimeFailed;
    mState = mAttempted == 0 ? State::Stopped : State::Stopping;
    return true;
}

bool Runner::BlockCleanup(uint32 error) noexcept
{
    if (mAdvancing || mState != State::Stopping || mAttempted == 0)
    {
        return false;
    }
    auto& record = mRecords[mPlan[mJournal[mAttempted - 1]].Id];
    // A deadline can quarantine an already-issued stop, never skip BeginStop.
    if (record.State != NodeState::Stopping)
    {
        return false;
    }
    record.State = NodeState::CleanupBlocked;
    record.Error = error;
    mCleanupFailure = { .NodeId = mPlan[mJournal[mAttempted - 1]].Id, .Error = error };
    mState = State::CleanupBlocked;
    return true;
}

bool Runner::ResumeCleanup() noexcept
{
    if (mAdvancing || mState != State::CleanupBlocked)
    {
        return false;
    }
    mRecords[mPlan[mJournal[mAttempted - 1]].Id].State = NodeState::Stopping;
    mState = State::Stopping;
    return true;
}

void Runner::StartOne() noexcept
{
    const auto& node = mPlan[mNext];
    auto& record = mRecords[node.Id];
    const bool begin = record.State == NodeState::Dormant;
    if (begin)
    {
        // Journal BEFORE the callback: a failed BeginStart may own resources.
        mJournal[mAttempted++] = mNext;
        record.State = NodeState::Starting;
    }
    const auto result = begin ? node.BeginStart(node.Context) : node.PollStart(node.Context);
    record.Error = result.Error;
    record.WaitReason = result.WaitReason;
    if (result.Status == StartStatus::Ready)
    {
        record.State = NodeState::Ready;
        ++mNext;
    }
    else if (result.Status == StartStatus::Failed)
    {
        record.State = NodeState::Failed;
        mFailure = { .NodeId = node.Id, .Error = result.Error };
        mOutcome = Outcome::StartFailed;
        mState = State::Stopping;
    }
}

void Runner::StopOne() noexcept
{
    const auto& node = mPlan[mJournal[mAttempted - 1]];
    auto& record = mRecords[node.Id];
    const bool begin = record.State != NodeState::Stopping;
    record.State = NodeState::Stopping;
    const auto result = begin ? node.BeginStop(node.Context) : node.PollStop(node.Context);
    record.Error = result.Error;
    record.WaitReason = result.WaitReason;
    if (result.Status == StopStatus::Stopped)
    {
        record.State = NodeState::Stopped;
        --mAttempted;
        if (mAttempted == 0)
        {
            mState = State::Stopped;
        }
    }
    else if (result.Status == StopStatus::Unsafe)
    {
        record.State = NodeState::CleanupBlocked;
        mCleanupFailure = { .NodeId = node.Id, .Error = result.Error };
        mState = State::CleanupBlocked;
    }
}

State Runner::Advance(usize callbackBudget) noexcept
{
    if (mAdvancing)
    {
        return mState;
    }
    mAdvancing = true;
    ObserveStopRequest();
    while (callbackBudget > 0)
    {
        if (mState == State::Starting)
        {
            if (mNext == mPlan.size())
            {
                mState = State::Ready;
                break;
            }
            const auto id = mPlan[mNext].Id;
            StartOne();
            --callbackBudget;
            ObserveStopRequest();
            if (mState != State::Starting || mRecords[id].State == NodeState::Starting)
            {
                break;
            }
        }
        else if (mState == State::Stopping)
        {
            const auto before = mAttempted;
            StopOne();
            --callbackBudget;
            if (mAttempted == before)
            {
                break;
            }
        }
        else
        {
            break;
        }
    }
    // Publishing Ready requires no extra callback or owner-loop iteration.
    ObserveStopRequest();
    if (mState == State::Starting && mNext == mPlan.size())
    {
        mState = State::Ready;
    }
    mAdvancing = false;
    return mState;
}
} // namespace ludus::smoke::lifecycle
