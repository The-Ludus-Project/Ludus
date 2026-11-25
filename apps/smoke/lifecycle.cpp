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

bool Runner::validate() noexcept
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
        const Node& node = mPlan[position];
        PlanError error = PlanError::None;
        if (node.Id >= mPlan.size())
        {
            error = PlanError::InvalidId;
        }
        else if (mRecords[node.Id].Position != INVALID_NODE)
        {
            error = PlanError::DuplicateId;
        }
        else if (node.Name.empty() == true)
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
    return validateProviders();
}

bool Runner::validateProviders() noexcept
{
    // Before the attempt begins, the journal is scratch storage: entry i holds the
    // position of the last consumer that listed provider i, which finds repeated
    // edges in O(V + E) without allocation.
    for (usize id = 0; id < mPlan.size(); ++id)
    {
        mJournal[id] = INVALID_NODE;
    }
    for (usize position = 0; position < mPlan.size(); ++position)
    {
        const Node& node = mPlan[position];
        for (const usize provider : node.Providers)
        {
            PlanError error = PlanError::None;
            if (provider >= mPlan.size())
            {
                error = PlanError::InvalidProvider;
            }
            else if (provider == node.Id)
            {
                error = PlanError::SelfProvider;
            }
            else if (mJournal[provider] == position)
            {
                error = PlanError::DuplicateProvider;
            }
            else if (mRecords[provider].Position > position)
            {
                error = isProviderCycle(node, provider) == true ? PlanError::ProviderCycle : PlanError::ProviderOrder;
            }
            if (error != PlanError::None)
            {
                mPlanDiagnostic = { .Error = error, .NodeId = node.Id, .ProviderId = provider };
                return false;
            }
            mJournal[provider] = position;
        }
    }
    return true;
}

bool Runner::isProviderCycle(const Node& consumer, usize providerId) noexcept
{
    // Bounded diagnostic DFS: does the forward provider transitively depend on its
    // consumer? Each node is marked when pushed, so the journal-backed stack never
    // exceeds the plan size and the search is O(V + E). It distinguishes a cycle
    // from a merely misordered plan; it never repairs the plan.
    usize depth = 0;
    mJournal[depth++] = providerId;
    mRecords[providerId].State = NodeState::Starting;
    bool isCycle = false;
    while (depth > 0 && isCycle == false)
    {
        const usize id = mJournal[--depth];
        for (const usize provider : mPlan[mRecords[id].Position].Providers)
        {
            if (provider == consumer.Id)
            {
                isCycle = true;
                break;
            }
            // Later nodes are not validated yet; their invalid edges cannot close a cycle.
            if (provider < mPlan.size() && mRecords[provider].State == NodeState::Dormant)
            {
                mRecords[provider].State = NodeState::Starting;
                mJournal[depth++] = provider;
            }
        }
    }
    for (usize id = 0; id < mPlan.size(); ++id)
    {
        mRecords[id].State = NodeState::Dormant;
    }
    return isCycle;
}

BeginStatus Runner::Begin() noexcept
{
    if (mIsAdvancing == true || (mState != State::Idle && mState != State::Stopped))
    {
        return BeginStatus::Busy;
    }
    if (validate() == false)
    {
        return BeginStatus::InvalidPlan;
    }
    if (core::TryAdd(mGeneration, uint64{1}, mGeneration) == false)
    {
        return BeginStatus::GenerationExhausted;
    }
    mIsStopRequested.store(false, std::memory_order_release);
    mNext = 0;
    mJournalSize = 0;
    mAttemptedCount = 0;
    mOutcome = Outcome::None;
    mFailure = {};
    mCleanupFailure = {};
    mState = State::Starting;
    return BeginStatus::Started;
}

void Runner::RequestStop() noexcept
{
    mIsStopRequested.store(true, std::memory_order_release);
}

void Runner::enterCleanup() noexcept
{
    mState = mJournalSize == 0 ? State::Stopped : State::Stopping;
}

void Runner::observeStopRequest() noexcept
{
    if (mIsStopRequested.load(std::memory_order_acquire) == false)
    {
        return;
    }
    if (mState == State::Starting || mState == State::Ready || mState == State::Idle)
    {
        mOutcome = mState == State::Starting ? Outcome::Cancelled : Outcome::Completed;
        enterCleanup();
    }
}

bool Runner::ReportFailure(usize nodeId, ErrorCode error) noexcept
{
    if (mIsAdvancing == true || nodeId >= mPlan.size() || (mState != State::Starting && mState != State::Ready))
    {
        return false;
    }
    mFailure = { .NodeId = nodeId, .Source = FailureSource::Runtime, .Error = error };
    mOutcome = Outcome::RuntimeFailed;
    enterCleanup();
    return true;
}

void Runner::blockCleanup(FailureSource source, ErrorCode error) noexcept
{
    const usize id = mPlan[mJournal[mJournalSize - 1]].Id;
    NodeRecord& record = mRecords[id];
    record.State = NodeState::CleanupBlocked;
    record.Error = error;
    mCleanupFailure = { .NodeId = id, .Source = source, .Error = error };
    mState = State::CleanupBlocked;
}

bool Runner::BlockCleanup(ErrorCode error) noexcept
{
    if (mIsAdvancing == true || mState != State::Stopping || mJournalSize == 0)
    {
        return false;
    }
    // A deadline can quarantine an already-issued stop, never skip BeginStop.
    if (mRecords[mPlan[mJournal[mJournalSize - 1]].Id].State != NodeState::Stopping)
    {
        return false;
    }
    blockCleanup(FailureSource::Deadline, error);
    return true;
}

bool Runner::ResumeCleanup() noexcept
{
    if (mIsAdvancing == true || mState != State::CleanupBlocked)
    {
        return false;
    }
    mRecords[mPlan[mJournal[mJournalSize - 1]].Id].State = NodeState::Stopping;
    mState = State::Stopping;
    return true;
}

void Runner::startOne() noexcept
{
    const Node& node = mPlan[mNext];
    NodeRecord& record = mRecords[node.Id];
    const bool isBegin = record.State == NodeState::Dormant;
    if (isBegin == true)
    {
        // Journal BEFORE the callback: a failed BeginStart may own resources.
        mJournal[mJournalSize++] = mNext;
        ++mAttemptedCount;
        record.State = NodeState::Starting;
    }
    const StartResult result =
        isBegin == true ? node.BeginStart(node.ContextOrNull) : node.PollStart(node.ContextOrNull);
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
        mFailure =
        {
            .NodeId = node.Id,
            .Source = isBegin == true ? FailureSource::BeginStart : FailureSource::PollStart,
            .Error = result.Error,
        };
        mOutcome = Outcome::StartFailed;
        enterCleanup();
    }
}

void Runner::stopOne() noexcept
{
    const Node& node = mPlan[mJournal[mJournalSize - 1]];
    NodeRecord& record = mRecords[node.Id];
    const bool isBegin = record.State != NodeState::Stopping;
    record.State = NodeState::Stopping;
    const StopResult result = isBegin == true ? node.BeginStop(node.ContextOrNull) : node.PollStop(node.ContextOrNull);
    record.Error = result.Error;
    record.WaitReason = result.WaitReason;
    if (result.Status == StopStatus::Stopped)
    {
        record.State = NodeState::Stopped;
        --mJournalSize;
        enterCleanup();
    }
    else if (result.Status == StopStatus::Unsafe)
    {
        blockCleanup(isBegin == true ? FailureSource::BeginStop : FailureSource::PollStop, result.Error);
    }
}

State Runner::Advance(usize callbackBudget) noexcept
{
    if (mIsAdvancing == true)
    {
        return mState;
    }
    mIsAdvancing = true;
    observeStopRequest();
    while (callbackBudget > 0)
    {
        if (mState == State::Starting)
        {
            if (mNext == mPlan.size())
            {
                mState = State::Ready;
                break;
            }
            const usize id = mPlan[mNext].Id;
            startOne();
            --callbackBudget;
            observeStopRequest();
            if (mState != State::Starting || mRecords[id].State == NodeState::Starting)
            {
                break;
            }
        }
        else if (mState == State::Stopping)
        {
            const usize before = mJournalSize;
            stopOne();
            --callbackBudget;
            if (mJournalSize == before)
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
    observeStopRequest();
    if (mState == State::Starting && mNext == mPlan.size())
    {
        mState = State::Ready;
    }
    mIsAdvancing = false;
    return mState;
}

std::span<const usize> Runner::GetRemainingCleanup() const noexcept
{
    return std::span<const usize>(mJournal).first(mJournalSize);
}

std::span<const NodeRecord> Runner::GetRecords() const noexcept
{
    return mRecords.size() > mPlan.size() ? mRecords.first(mPlan.size()) : mRecords;
}

Report Runner::GetReport() const noexcept
{
    CleanupOutcome cleanup = CleanupOutcome::None;
    if (mState == State::CleanupBlocked)
    {
        cleanup = CleanupOutcome::Blocked;
    }
    else if (mState == State::Stopping)
    {
        cleanup = CleanupOutcome::Pending;
    }
    else if (mState == State::Stopped)
    {
        cleanup = CleanupOutcome::Complete;
    }
    return
    {
        .Generation = mGeneration,
        .RunnerState = mState,
        .Primary = mOutcome,
        .PrimaryFailure = mFailure,
        .Cleanup = cleanup,
        .CleanupFailure = mCleanupFailure,
        .AttemptedCount = mAttemptedCount,
        .StoppedCount = mAttemptedCount - mJournalSize,
        .RemainingCount = mJournalSize,
    };
}
} // namespace ludus::smoke::lifecycle
