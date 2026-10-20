#include <ludus/foundation/threading/jobs.hpp>

#include "internal/native.hpp"

#include <atomic>
#include <new>

// Thanks to Julien Hamaide, "Multithread Job and Dependency System," Game
// Programming Gems 7, ch. 1.9, pp. 87-96, for dependency counters and versioned
// identity; Brad Werth, "Holistic Task Parallelism for Common Game Architecture
// Patterns," Game Engine Gems 1, ch. 22, pp. 381-390, for continuations/helping;
// and Jean-François Dubé, "Efficient and Scalable Multi-Core Programming," Game
// Programming Gems 8, ch. 4.3, pp. 373-384, for sleeping workers and allocation
// discipline. This implementation uses a bounded gate-protected queue and C++23
// publication, with distinct failure/cancellation outcomes, rather than their
// historical volatile/spinning examples. Independently written code; full source
// review and departures: docs/architecture/threading.md.
namespace ludus::foundation::threading
{
namespace detail
{
constexpr uint32 INVALID_INDEX = static_cast<uint32>(-1);
constexpr uint32 MAX_WORKERS = 64;
constexpr uint32 MAX_RECORDS = 1u << 20;
thread_local bool isInsideJob = false;

// Marks a hook call: lifecycle calls from a hook return InsideJob, exactly as
// they do from a job callback. Hooks always run outside the system gate.
class HookScope final
{
public:
    HookScope() noexcept : mIsCallerInsideJob(isInsideJob)
    {
        isInsideJob = true;
    }
    ~HookScope() noexcept
    {
        isInsideJob = mIsCallerInsideJob;
    }
    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;

private:
    bool mIsCallerInsideJob;
};

struct Node
{
    JobFunction Function = nullptr;
    void* ContextOrNull = nullptr;
    JobLabel Label{};
    uint32 FirstEdge = INVALID_INDEX;
    // The edge whose satisfaction made this node ready in the current submission.
    uint32 ReleasingEdge = INVALID_INDEX;
    uint32 Incoming = 0;
    uint32 Remaining = 0;
    bool HasFailedPrerequisite = false;
    std::atomic<JobOutcome> Outcome{JobOutcome::Pending};
};
struct Edge
{
    uint32 To = 0;
    uint32 Next = INVALID_INDEX;
};
struct GraphState
{
    // Initialize allocates every array, including a zero-length edge array, so
    // these pointers are nonnull for the state's lifetime.
    GraphState(Node* nodes, Edge* edges, uint32* ready, JobGraphConfig config, uint64 id) noexcept
        : Nodes(nodes), Edges(edges), Ready(ready), Config(config), Id(id)
    {
    }
    ~GraphState() noexcept
    {
        delete[] Nodes;
        delete[] Edges;
        delete[] Ready;
    }
    GraphState(const GraphState&) = delete;
    GraphState& operator=(const GraphState&) = delete;

    Node* Nodes;
    Edge* Edges;
    uint32* Ready;
    JobGraphConfig Config;
    uint64 Id;
    uint64 Generation = 1;
    uint32 JobCount = 0;
    uint32 EdgeCount = 0;
    // Each submission pushes every node at most once and starts Head at zero, so
    // the ring never wraps within a submission: position Head is the popped node's
    // ready sequence number, and Ready[0, RootCount) always holds the roots in Add
    // order (Seal and Submit both push them first, in index order).
    uint32 Head = 0;
    uint32 ReadyCount = 0;
    uint32 Unfinished = 0;
    bool IsSealed = false;
    bool IsCancelRequested = false;
    bool HasFailure = false;
    bool HasCancellation = false;
    std::atomic<JobSystem*> OwnerOrNull{nullptr};

    [[nodiscard]] bool IsValid(JobHandle handle) const noexcept
    {
        return handle.GraphId == Id && handle.Generation == Generation && handle.Index < JobCount;
    }
    void PushReady(uint32 index) noexcept
    {
        LUDUS_REQUIRE(ReadyCount < Config.MaxJobs);
        Ready[(Head + ReadyCount) % Config.MaxJobs] = index;
        ++ReadyCount;
    }
    [[nodiscard]] uint32 PopReady() noexcept
    {
        const uint32 index = Ready[Head];
        Head = (Head + 1) % Config.MaxJobs;
        --ReadyCount;
        return index;
    }
};
struct SystemState;
struct WorkerContext
{
    SystemState* SystemOrNull = nullptr;
    uint32 Index = 0;
};
struct SystemState
{
    NativeGate Gate{};
    // FoundationThreading depends only on FoundationBase, so the fixed worker
    // tables stay raw arrays rather than Containers StaticArrays.
    NativeThread Threads[MAX_WORKERS]{};
    WorkerContext Workers[MAX_WORKERS]{};
    JobSystemHooks Hooks{};
    uint32 WorkerCount = 0;
    uint32 StartedWorkerCount = 0;
    GraphState* ActiveOrNull = nullptr;
    // Identity of the active submission while IsTraced; written under the gate by Submit.
    uint64 SubmissionId = 0;
    bool IsTraced = false;
    bool ShouldStop = false;
};

namespace
{
std::atomic<uint64> gNextGraphId{1};
// Traced submissions only; a 64-bit counter cannot wrap in practice.
std::atomic<uint64> gNextSubmissionId{1};
// Returns zero once the identity space is exhausted; it never wraps.
[[nodiscard]] uint64 AllocateGraphId() noexcept
{
    uint64 id = gNextGraphId.load(std::memory_order_relaxed);
    while (id != static_cast<uint64>(-1))
    {
        if (gNextGraphId.compare_exchange_weak(id, id + 1, std::memory_order_relaxed) == true)
        {
            return id;
        }
    }
    return 0;
}

// Called with the system gate held. Every node enters Ready exactly once.
void CompleteJob(SystemState& inoutSystem, uint32 index, JobOutcome outcome) noexcept
{
    GraphState& graph = *inoutSystem.ActiveOrNull;
    Node& node = graph.Nodes[index];
    node.Outcome.store(outcome, std::memory_order_release);
    graph.HasFailure |= outcome == JobOutcome::Failed;
    graph.HasCancellation |= outcome == JobOutcome::Cancelled;
    const bool wasReadyEmpty = graph.ReadyCount == 0;
    for (uint32 edge = node.FirstEdge; edge != INVALID_INDEX; edge = graph.Edges[edge].Next)
    {
        Node& dependent = graph.Nodes[graph.Edges[edge].To];
        dependent.HasFailedPrerequisite |= outcome != JobOutcome::Succeeded;
        LUDUS_REQUIRE(dependent.Remaining > 0);
        if (--dependent.Remaining == 0)
        {
            dependent.ReleasingEdge = edge;
            graph.PushReady(graph.Edges[edge].To);
        }
    }
    --graph.Unfinished;
    // One predicate/gate for ready work, completion and stop. Publish while
    // holding the gate so sleepers cannot miss a transition before sleeping.
    if ((wasReadyEmpty == true && graph.ReadyCount > 0) || graph.Unfinished == 0)
    {
        inoutSystem.Gate.WakeAll();
    }
}

// Resolves one ready job without calling it: an explicit Cancel takes
// precedence over a failed prerequisite.
[[nodiscard]] JobOutcome GetSkippedOutcome(const GraphState& graph, const Node& node) noexcept
{
    if (graph.IsCancelRequested == true)
    {
        return JobOutcome::Cancelled;
    }
    return node.HasFailedPrerequisite == true ? JobOutcome::Blocked : JobOutcome::Pending;
}

void ReportTrace(const SystemState& system, const JobTraceEvent& event) noexcept
{
    const HookScope scope{};
    system.Hooks.TraceOrNull(system.Hooks.ContextOrNull, event);
}

// The caller holds the gate on entry and return. User code and trace hooks run
// outside it. Every report for a job precedes the publication of its outcome, so
// Wait cannot release the graph (or its labels) while a report is running.
bool ExecuteOneJob(SystemState& inoutSystem, uint32 workerIndex) noexcept
{
    GraphState* graphOrNull = inoutSystem.ActiveOrNull;
    if (graphOrNull == nullptr || graphOrNull->ReadyCount == 0)
    {
        return false;
    }
    const uint32 sequence = graphOrNull->Head;
    const uint32 index = graphOrNull->PopReady();
    Node& node = graphOrNull->Nodes[index];
    const JobOutcome skippedOutcome = GetSkippedOutcome(*graphOrNull, node);
    if (skippedOutcome != JobOutcome::Pending)
    {
        CompleteJob(inoutSystem, index, skippedOutcome);
        return true;
    }
    node.Outcome.store(JobOutcome::Running, std::memory_order_release);
    const bool isTraced = inoutSystem.IsTraced;
    JobTraceEvent event{};
    if (isTraced == true)
    {
        event =
        {
            .SubmissionId = inoutSystem.SubmissionId,
            .GraphId = graphOrNull->Id,
            .Label = node.Label,
            .JobIndex = index,
            .Sequence = sequence,
            .EdgeIndex = node.ReleasingEdge,
            .ReadyDepth = graphOrNull->ReadyCount,
            .WorkerIndex = workerIndex,
            .Kind = JobTraceKind::Started,
        };
    }
    inoutSystem.Gate.Unlock();
    if (isTraced == true)
    {
        ReportTrace(inoutSystem, event);
    }
    isInsideJob = true;
    JobOutcome outcome = node.Function(node.ContextOrNull);
    isInsideJob = false;
    if (outcome != JobOutcome::Succeeded && outcome != JobOutcome::Failed && outcome != JobOutcome::Cancelled)
    {
        outcome = JobOutcome::Failed;
    }
    if (isTraced == true)
    {
        // The sealed edge list is immutable while the graph is submitted.
        event.ReadyDepth = 0;
        if (outcome == JobOutcome::Succeeded)
        {
            event.Kind = JobTraceKind::Signalled;
            for (uint32 edge = node.FirstEdge; edge != INVALID_INDEX; edge = graphOrNull->Edges[edge].Next)
            {
                event.EdgeIndex = edge;
                event.DependentIndex = graphOrNull->Edges[edge].To;
                ReportTrace(inoutSystem, event);
            }
        }
        event.Kind = JobTraceKind::Finished;
        event.EdgeIndex = NO_JOB_EDGE;
        event.DependentIndex = 0;
        event.Outcome = outcome;
        ReportTrace(inoutSystem, event);
    }
    inoutSystem.Gate.Lock();
    CompleteJob(inoutSystem, index, outcome);
    return true;
}

void RunWorker(void* context) noexcept
{
    const WorkerContext& worker = *static_cast<const WorkerContext*>(context);
    LUDUS_REQUIRE(worker.SystemOrNull != nullptr);
    SystemState& system = *worker.SystemOrNull;
    if (system.Hooks.WorkerStartOrNull != nullptr)
    {
        const HookScope scope{};
        system.Hooks.WorkerStartOrNull(system.Hooks.ContextOrNull, worker.Index);
    }
    system.Gate.Lock();
    if (system.Hooks.WorkerStartOrNull != nullptr)
    {
        ++system.StartedWorkerCount;
        system.Gate.WakeAll();
    }
    while (system.ShouldStop == false)
    {
        if (ExecuteOneJob(system, worker.Index) == false)
        {
            system.Gate.Wait();
        }
    }
    system.Gate.Unlock();
    if (system.Hooks.WorkerStopOrNull != nullptr)
    {
        const HookScope scope{};
        system.Hooks.WorkerStopOrNull(system.Hooks.ContextOrNull, worker.Index);
    }
}

// Called with the gate held after the graph's roots are published.
void ExecuteSerially(SystemState& inoutSystem) noexcept
{
    if (inoutSystem.WorkerCount == 0)
    {
        while (ExecuteOneJob(inoutSystem, CALLING_THREAD_WORKER_INDEX) == true)
        {
        }
    }
}
} // namespace
} // namespace detail

using detail::GateLock;
using detail::GraphState;
using detail::INVALID_INDEX;
using detail::Node;

JobGraph::~JobGraph() noexcept
{
    if (mStateOrNull != nullptr)
    {
        JobSystem* ownerOrNull = mStateOrNull->OwnerOrNull.load(std::memory_order_acquire);
        if (ownerOrNull != nullptr)
        {
            // Destruction on a callback stack cannot safely drain that callback.
            LUDUS_REQUIRE(detail::isInsideJob == false);
            JobOutcome outcome{};
            (void)ownerOrNull->Wait(outcome);
        }
        delete mStateOrNull;
    }
}

JobStatus JobGraph::Initialize(JobGraphConfig config) noexcept
{
    if (mStateOrNull != nullptr)
    {
        return JobStatus::AlreadyInitialized;
    }
    if (config.MaxJobs == 0 || config.MaxJobs > detail::MAX_RECORDS || config.MaxDependencies > detail::MAX_RECORDS)
    {
        return JobStatus::InvalidArgument;
    }
    Node* nodesOrNull = new (std::nothrow) Node[config.MaxJobs];
    // A zero-length array new still returns a unique nonnull pointer.
    detail::Edge* edgesOrNull = new (std::nothrow) detail::Edge[config.MaxDependencies];
    uint32* readyOrNull = new (std::nothrow) uint32[config.MaxJobs];
    if (nodesOrNull == nullptr || edgesOrNull == nullptr || readyOrNull == nullptr)
    {
        delete[] nodesOrNull;
        delete[] edgesOrNull;
        delete[] readyOrNull;
        return JobStatus::OutOfMemory;
    }
    GraphState* stateOrNull = new (std::nothrow) GraphState(nodesOrNull, edgesOrNull, readyOrNull, config, 0);
    if (stateOrNull == nullptr)
    {
        delete[] nodesOrNull;
        delete[] edgesOrNull;
        delete[] readyOrNull;
        return JobStatus::OutOfMemory;
    }
    stateOrNull->Id = detail::AllocateGraphId();
    if (stateOrNull->Id == 0)
    {
        delete stateOrNull;
        return JobStatus::IdentityExhausted;
    }
    mStateOrNull = stateOrNull;
    return JobStatus::Ok;
}

JobStatus JobGraph::Add(JobFunction function, void* contextOrNull, JobHandle& outHandle, JobLabel label) noexcept
{
    outHandle = {};
    if (mStateOrNull == nullptr)
    {
        return JobStatus::NotInitialized;
    }
    GraphState& graph = *mStateOrNull;
    if (graph.OwnerOrNull.load(std::memory_order_acquire) != nullptr || graph.IsSealed == true)
    {
        return JobStatus::Busy;
    }
    if (function == nullptr)
    {
        return JobStatus::InvalidArgument;
    }
    if (graph.JobCount == graph.Config.MaxJobs)
    {
        return JobStatus::CapacityExceeded;
    }
    const uint32 index = graph.JobCount++;
    Node& node = graph.Nodes[index];
    node.Function = function;
    node.ContextOrNull = contextOrNull;
    node.Label = label;
    outHandle = { .GraphId = graph.Id, .Generation = graph.Generation, .Index = index };
    return JobStatus::Ok;
}

JobStatus JobGraph::DependsOn(JobHandle dependent, JobHandle prerequisite) noexcept
{
    if (mStateOrNull == nullptr)
    {
        return JobStatus::NotInitialized;
    }
    GraphState& graph = *mStateOrNull;
    if (graph.OwnerOrNull.load(std::memory_order_acquire) != nullptr || graph.IsSealed == true)
    {
        return JobStatus::Busy;
    }
    if (graph.IsValid(dependent) == false || graph.IsValid(prerequisite) == false)
    {
        return JobStatus::InvalidHandle;
    }
    if (dependent.Index == prerequisite.Index)
    {
        return JobStatus::CycleDetected;
    }
    Node& from = graph.Nodes[prerequisite.Index];
    for (uint32 edge = from.FirstEdge; edge != INVALID_INDEX; edge = graph.Edges[edge].Next)
    {
        if (graph.Edges[edge].To == dependent.Index)
        {
            return JobStatus::DuplicateDependency;
        }
    }
    if (graph.EdgeCount == graph.Config.MaxDependencies)
    {
        return JobStatus::CapacityExceeded;
    }
    graph.Edges[graph.EdgeCount] = { .To = dependent.Index, .Next = from.FirstEdge };
    from.FirstEdge = graph.EdgeCount++;
    ++graph.Nodes[dependent.Index].Incoming;
    return JobStatus::Ok;
}

JobStatus JobGraph::Seal() noexcept
{
    if (mStateOrNull == nullptr)
    {
        return JobStatus::NotInitialized;
    }
    GraphState& graph = *mStateOrNull;
    if (graph.OwnerOrNull.load(std::memory_order_acquire) != nullptr)
    {
        return JobStatus::Busy;
    }
    // Kahn validation reuses the runtime ready ring and dependency counters.
    graph.Head = 0;
    graph.ReadyCount = 0;
    for (uint32 index = 0; index < graph.JobCount; ++index)
    {
        graph.Nodes[index].Remaining = graph.Nodes[index].Incoming;
        if (graph.Nodes[index].Incoming == 0)
        {
            graph.PushReady(index);
        }
    }
    uint32 visited = 0;
    while (graph.ReadyCount > 0)
    {
        const uint32 index = graph.PopReady();
        ++visited;
        for (uint32 edge = graph.Nodes[index].FirstEdge; edge != INVALID_INDEX; edge = graph.Edges[edge].Next)
        {
            if (--graph.Nodes[graph.Edges[edge].To].Remaining == 0)
            {
                graph.PushReady(graph.Edges[edge].To);
            }
        }
    }
    graph.IsSealed = visited == graph.JobCount;
    return graph.IsSealed == true ? JobStatus::Ok : JobStatus::CycleDetected;
}

JobStatus JobGraph::Reset() noexcept
{
    if (mStateOrNull == nullptr)
    {
        return JobStatus::NotInitialized;
    }
    GraphState& graph = *mStateOrNull;
    if (graph.OwnerOrNull.load(std::memory_order_acquire) != nullptr)
    {
        return JobStatus::Busy;
    }
    if (graph.Generation == static_cast<uint64>(-1))
    {
        return JobStatus::IdentityExhausted;
    }
    for (uint32 index = 0; index < graph.JobCount; ++index)
    {
        Node& node = graph.Nodes[index];
        node.Function = nullptr;
        node.ContextOrNull = nullptr;
        node.Label = {};
        node.FirstEdge = INVALID_INDEX;
        node.Incoming = 0;
        node.Outcome.store(JobOutcome::Pending, std::memory_order_relaxed);
    }
    ++graph.Generation;
    graph.JobCount = 0;
    graph.EdgeCount = 0;
    graph.IsSealed = false;
    return JobStatus::Ok;
}

JobStatus JobGraph::GetOutcome(JobHandle handle, JobOutcome& outOutcome) const noexcept
{
    outOutcome = JobOutcome::Pending;
    if (mStateOrNull == nullptr)
    {
        return JobStatus::NotInitialized;
    }
    if (mStateOrNull->IsValid(handle) == false)
    {
        return JobStatus::InvalidHandle;
    }
    outOutcome = mStateOrNull->Nodes[handle.Index].Outcome.load(std::memory_order_acquire);
    return JobStatus::Ok;
}

JobSystem::~JobSystem() noexcept
{
    LUDUS_REQUIRE(detail::isInsideJob == false);
    (void)Shutdown();
}

JobStatus JobSystem::Initialize(JobSystemConfig config) noexcept
{
    if (detail::isInsideJob == true)
    {
        return JobStatus::InsideJob;
    }
    if (mStateOrNull != nullptr)
    {
        return JobStatus::AlreadyInitialized;
    }
    if (config.WorkerCount > detail::MAX_WORKERS)
    {
        return JobStatus::InvalidArgument;
    }
#if defined(LUDUS_PLATFORM_WEB)
    if (config.WorkerCount != 0)
    {
        return JobStatus::Unsupported;
    }
#endif
    detail::SystemState* stateOrNull = new (std::nothrow) detail::SystemState{};
    if (stateOrNull == nullptr)
    {
        return JobStatus::OutOfMemory;
    }
    if (stateOrNull->Gate.Initialize() == false)
    {
        delete stateOrNull;
        return JobStatus::ThreadCreationFailed;
    }
    stateOrNull->Hooks = config.Hooks;
    mStateOrNull = stateOrNull;
    for (uint32 index = 0; index < config.WorkerCount; ++index)
    {
        stateOrNull->Workers[index] = { .SystemOrNull = stateOrNull, .Index = index };
        if (stateOrNull->Threads[index].Start(detail::RunWorker, &stateOrNull->Workers[index]) == false)
        {
            (void)Shutdown();
            return JobStatus::ThreadCreationFailed;
        }
        ++stateOrNull->WorkerCount;
    }
    if (config.Hooks.WorkerStartOrNull != nullptr)
    {
        // Worker setup hooks complete before any graph can be submitted.
        GateLock lock(stateOrNull->Gate);
        while (stateOrNull->StartedWorkerCount < stateOrNull->WorkerCount)
        {
            stateOrNull->Gate.Wait();
        }
    }
    return JobStatus::Ok;
}

JobStatus JobSystem::Submit(JobGraph& inoutGraph) noexcept
{
    if (detail::isInsideJob == true)
    {
        return JobStatus::InsideJob;
    }
    if (mStateOrNull == nullptr || inoutGraph.mStateOrNull == nullptr)
    {
        return JobStatus::NotInitialized;
    }
    detail::SystemState& system = *mStateOrNull;
    bool shouldTrace = system.Hooks.TraceOrNull != nullptr;
    if (shouldTrace == true && system.Hooks.ShouldTraceOrNull != nullptr)
    {
        const detail::HookScope scope{};
        shouldTrace = system.Hooks.ShouldTraceOrNull(system.Hooks.ContextOrNull);
    }
    GraphState& graph = *inoutGraph.mStateOrNull;
    system.Gate.Lock();
    if (system.ActiveOrNull != nullptr)
    {
        system.Gate.Unlock();
        return JobStatus::Busy;
    }
    if (graph.IsSealed == false)
    {
        system.Gate.Unlock();
        return JobStatus::NotSealed;
    }
    JobSystem* expectedOrNull = nullptr;
    if (graph.OwnerOrNull.compare_exchange_strong(expectedOrNull, this, std::memory_order_acq_rel) == false)
    {
        system.Gate.Unlock();
        return JobStatus::Busy;
    }
    graph.Head = 0;
    graph.ReadyCount = 0;
    graph.Unfinished = graph.JobCount;
    graph.IsCancelRequested = false;
    graph.HasFailure = false;
    graph.HasCancellation = false;
    for (uint32 index = 0; index < graph.JobCount; ++index)
    {
        Node& node = graph.Nodes[index];
        node.Remaining = node.Incoming;
        node.ReleasingEdge = INVALID_INDEX;
        node.HasFailedPrerequisite = false;
        node.Outcome.store(JobOutcome::Pending, std::memory_order_relaxed);
        if (node.Remaining == 0)
        {
            graph.PushReady(index);
        }
    }
    system.ActiveOrNull = &graph;
    system.IsTraced = shouldTrace;
    if (shouldTrace == true)
    {
        // Admit the graph without ready work, report it outside the gate, then
        // publish the roots: no job can start before Submitted is reported.
        const uint32 rootCount = graph.ReadyCount;
        graph.ReadyCount = 0;
        system.SubmissionId = detail::gNextSubmissionId.fetch_add(1, std::memory_order_relaxed);
        const JobTraceEvent event
        {
            .SubmissionId = system.SubmissionId,
            .GraphId = graph.Id,
            .ReadyDepth = rootCount,
            .JobCount = graph.JobCount,
            .Kind = JobTraceKind::Submitted,
        };
        system.Gate.Unlock();
        detail::ReportTrace(system, event);
        system.Gate.Lock();
        graph.ReadyCount = rootCount;
    }
    system.Gate.WakeAll();
    detail::ExecuteSerially(system);
    system.Gate.Unlock();
    return JobStatus::Ok;
}

JobStatus JobSystem::Wait(JobOutcome& outOutcome, bool shouldHelp) noexcept
{
    outOutcome = JobOutcome::Pending;
    if (detail::isInsideJob == true)
    {
        return JobStatus::InsideJob;
    }
    if (mStateOrNull == nullptr)
    {
        return JobStatus::NotInitialized;
    }
    detail::SystemState& system = *mStateOrNull;
    system.Gate.Lock();
    GraphState* graphOrNull = system.ActiveOrNull;
    if (graphOrNull == nullptr)
    {
        system.Gate.Unlock();
        return JobStatus::InvalidArgument;
    }
    // Submit and Wait are serialized by the owner, so the active submission
    // cannot change while the gate is released for the reports below.
    const bool isTraced = system.IsTraced;
    JobTraceEvent event
    {
        .SubmissionId = system.SubmissionId,
        .GraphId = graphOrNull->Id,
        .Kind = JobTraceKind::WaitEntered,
    };
    if (isTraced == true)
    {
        system.Gate.Unlock();
        detail::ReportTrace(system, event);
        system.Gate.Lock();
    }
    while (graphOrNull->Unfinished > 0)
    {
        if (shouldHelp == false || detail::ExecuteOneJob(system, CALLING_THREAD_WORKER_INDEX) == false)
        {
            system.Gate.Wait();
        }
    }
    JobOutcome outcome = JobOutcome::Succeeded;
    if (graphOrNull->HasFailure == true)
    {
        outcome = JobOutcome::Failed;
    }
    else if (graphOrNull->HasCancellation == true)
    {
        outcome = JobOutcome::Cancelled;
    }
    system.ActiveOrNull = nullptr;
    system.IsTraced = false;
    graphOrNull->OwnerOrNull.store(nullptr, std::memory_order_release);
    system.Gate.Unlock();
    if (isTraced == true)
    {
        event.Kind = JobTraceKind::WaitReturned;
        event.Outcome = outcome;
        detail::ReportTrace(system, event);
    }
    outOutcome = outcome;
    return JobStatus::Ok;
}

JobStatus JobSystem::Cancel() noexcept
{
    if (mStateOrNull == nullptr)
    {
        return JobStatus::NotInitialized;
    }
    GateLock lock(mStateOrNull->Gate);
    if (mStateOrNull->ActiveOrNull == nullptr)
    {
        return JobStatus::InvalidArgument;
    }
    mStateOrNull->ActiveOrNull->IsCancelRequested = true;
    mStateOrNull->Gate.WakeAll();
    return JobStatus::Ok;
}

uint32 JobSystem::GetWorkerCount() const noexcept
{
    // Written only by Initialize, before it returns to the owner; never changed afterwards.
    return mStateOrNull == nullptr ? 0 : mStateOrNull->WorkerCount;
}

JobStatus JobSystem::Snapshot(JobSnapshot& outSnapshot) const noexcept
{
    outSnapshot = {};
    if (mStateOrNull == nullptr)
    {
        return JobStatus::NotInitialized;
    }
    GateLock lock(mStateOrNull->Gate);
    const GraphState* graphOrNull = mStateOrNull->ActiveOrNull;
    if (graphOrNull == nullptr)
    {
        return JobStatus::Ok;
    }
    for (uint32 index = 0; index < graphOrNull->JobCount; ++index)
    {
        switch (graphOrNull->Nodes[index].Outcome.load(std::memory_order_relaxed))
        {
            case JobOutcome::Pending:
                ++outSnapshot.Pending;
                break;
            case JobOutcome::Running:
                ++outSnapshot.Running;
                break;
            case JobOutcome::Succeeded:
                ++outSnapshot.Succeeded;
                break;
            case JobOutcome::Failed:
                ++outSnapshot.Failed;
                break;
            case JobOutcome::Cancelled:
                ++outSnapshot.Cancelled;
                break;
            case JobOutcome::Blocked:
                ++outSnapshot.Blocked;
                break;
        }
    }
    return JobStatus::Ok;
}

JobStatus JobSystem::Shutdown() noexcept
{
    if (detail::isInsideJob == true)
    {
        return JobStatus::InsideJob;
    }
    if (mStateOrNull == nullptr)
    {
        return JobStatus::Ok;
    }
    JobOutcome outcome{};
    (void)Wait(outcome);
    {
        GateLock lock(mStateOrNull->Gate);
        mStateOrNull->ShouldStop = true;
        mStateOrNull->Gate.WakeAll();
    }
    for (uint32 index = 0; index < mStateOrNull->WorkerCount; ++index)
    {
        mStateOrNull->Threads[index].Join();
    }
    mStateOrNull->Gate.Destroy();
    delete mStateOrNull;
    mStateOrNull = nullptr;
    return JobStatus::Ok;
}
} // namespace ludus::foundation::threading
