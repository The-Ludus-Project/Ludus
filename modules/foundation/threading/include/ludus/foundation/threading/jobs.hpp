#pragma once

#include <ludus/foundation/base/types.h>

/// @file
/// @brief Bounded CPU task graphs and an explicitly budgeted worker pool.
///
/// Usage, lifetime rules and validation evidence are described in
/// docs/modules/foundation/threading.md and docs/architecture/threading.md.

namespace ludus::foundation::threading
{
/// @brief Result of a graph or system operation. Every failure leaves outputs in a documented state.
enum class JobStatus : uint8
{
    Ok,                   ///< The operation succeeded.
    InvalidArgument,      ///< Invalid configuration or callback, or Wait/Cancel with no submitted graph.
    NotInitialized,       ///< The graph or system has not been successfully initialized.
    AlreadyInitialized,   ///< Initialize was called again; the existing storage is unchanged.
    OutOfMemory,          ///< Fixed storage could not be allocated; nothing was retained.
    CapacityExceeded,     ///< The graph's MaxJobs or MaxDependencies reservation is full.
    InvalidHandle,        ///< The handle belongs to another graph or to a generation invalidated by Reset.
    DuplicateDependency,  ///< The same prerequisite edge was already declared.
    CycleDetected,        ///< A self edge was declared, or Seal found a dependency cycle.
    NotSealed,            ///< Submit requires a graph that Seal accepted.
    Busy,                 ///< A graph is already in flight, or this graph is sealed/submitted and cannot change.
    InsideJob,            ///< Called from a job callback of any system; it would wait for or destroy itself.
    Unsupported,          ///< The request is unavailable on this target; browser builds require zero workers.
    ThreadCreationFailed, ///< Native synchronization or a worker could not start; started workers are joined.
    IdentityExhausted,    ///< Graph IDs or the graph's reset generation cannot be represented without wrapping.
};

/// @brief Execution state of one job, and the aggregate outcome reported by JobSystem::Wait.
enum class JobOutcome : uint8
{
    Pending,   ///< Not yet executed or resolved.
    Running,   ///< The callback is executing on a worker or a helping caller.
    Succeeded, ///< The callback returned Succeeded.
    Failed,    ///< The callback returned Failed or an invalid outcome.
    Cancelled, ///< The callback returned Cancelled, or JobSystem::Cancel skipped the job before it started.
    Blocked,   ///< A prerequisite did not succeed, so the callback was never called.
};

/// @brief Job callback. Runs to completion on any worker or a helping caller, with no thread affinity.
///
/// Only Succeeded, Failed and Cancelled are valid return values; any other value
/// is recorded as Failed. A callback must not block on other jobs or synchronous
/// I/O, and must not call JobSystem Initialize, Submit, Wait or Shutdown (they
/// return InsideJob). The code must stay loaded until the owning Wait returns.
using JobFunction = JobOutcome (*)(void* context) noexcept;

/// @brief Value identity of one job. Copying does not retain the job.
///
/// Valid only for the graph and reset generation that created it, within one
/// FoundationThreading runtime instance. A default-constructed handle is never valid.
struct JobHandle
{
    uint64 GraphId = 0;    ///< Graph identity, unique within this runtime; zero is never issued.
    uint64 Generation = 0; ///< Graph reset generation when the job was added.
    uint32 Index = 0;      ///< Job index in that generation.
};

/// @brief Optional identity of a job, given to JobGraph::Add and reported to trace hooks.
///
/// Neither field is interpreted by the scheduler. Add stores the label by value,
/// without allocating or copying the name. A default label is anonymous.
struct JobLabel
{
    /// @brief Borrowed NUL-terminated name, normally a string literal; never copied.
    ///
    /// It must stay valid until the Wait that releases every submission of the job
    /// returns. Trace hooks receive this pointer, not a copy.
    const char* NameOrNull = nullptr;
    /// @brief Caller-chosen call-site identifier; zero means none.
    ///
    /// The profiler adapter (`job_profiling.hpp`) uses it as a profiler site ID, so
    /// a label from `LUDUS_JOB_LABEL` resolves to its name in exported traces.
    uint32 SiteId = 0;
};

/// @brief Fixed graph capacity. Initialize reserves all storage; a graph never grows.
struct JobGraphConfig
{
    uint32 MaxJobs = 256;          ///< Maximum jobs per generation; range 1..1048576.
    uint32 MaxDependencies = 1024; ///< Maximum dependency edges per generation; range 0..1048576.
};

/// @brief WorkerIndex of a trace event reported on the thread that called Submit or Wait.
inline constexpr uint32 CALLING_THREAD_WORKER_INDEX = static_cast<uint32>(-1);
/// @brief EdgeIndex of a Started trace event for a job without prerequisites.
inline constexpr uint32 NO_JOB_EDGE = static_cast<uint32>(-1);

/// @brief Scheduler transition reported to JobSystemHooks::TraceOrNull.
///
/// For one traced submission, the owner reports Submitted before any job can start
/// and WaitEntered/WaitReturned from Wait. The thread executing a job reports Started,
/// then, after the callback returns Succeeded, one Signalled per outgoing dependency
/// edge, then Finished. Jobs skipped as Blocked or Cancelled are not reported.
enum class JobTraceKind : uint8
{
    Submitted,    ///< Submit admitted the graph; its ReadyDepth roots become ready after this report returns.
    Started,      ///< The job's callback is about to run on the reporting thread.
    Signalled,    ///< The succeeded job is about to satisfy EdgeIndex of DependentIndex.
    Finished,     ///< The callback returned Outcome; the outcome is published after this report returns.
    WaitEntered,  ///< The owner entered Wait for this submission.
    WaitReturned, ///< Wait released the submission with Outcome and is about to return.
};

/// @brief One scheduler transition of a traced submission. Fields not listed for a kind are zero or default.
///
/// Every value is copied out of scheduler state under the system gate; the report
/// itself runs outside the gate. Sequence numbers, edge indices and job indices
/// identify work within one SubmissionId only.
struct JobTraceEvent
{
    /// @brief Nonzero identity of the submission, unique within this FoundationThreading runtime.
    uint64 SubmissionId = 0;
    /// @brief JobHandle::GraphId of the submitted graph.
    uint64 GraphId = 0;
    /// @brief Started, Signalled and Finished: the label given to Add for the job.
    JobLabel Label{};
    /// @brief Started, Signalled and Finished: index of the job (JobHandle::Index).
    uint32 JobIndex = 0;
    /// @brief Started, Signalled and Finished: zero-based order in which the job became ready.
    ///
    /// Jobs without prerequisites are ready first, in Add order, so their sequence
    /// numbers are [0, ReadyDepth of Submitted).
    uint32 Sequence = 0;
    /// @brief Started: the dependency edge whose satisfaction made the job ready, or NO_JOB_EDGE
    ///        for a job without prerequisites. Signalled: the edge being satisfied.
    ///
    /// Edges are numbered in DependsOn order from zero within the graph generation.
    uint32 EdgeIndex = NO_JOB_EDGE;
    /// @brief Signalled: index of the dependent job.
    uint32 DependentIndex = 0;
    /// @brief Submitted: number of jobs without prerequisites. Started: jobs still waiting
    ///        in the ready queue after this one was taken.
    uint32 ReadyDepth = 0;
    /// @brief Submitted: number of jobs in the graph.
    uint32 JobCount = 0;
    /// @brief Zero-based worker index of the reporting thread, or CALLING_THREAD_WORKER_INDEX
    ///        for the thread that called Submit or Wait (including a helping Wait).
    uint32 WorkerIndex = CALLING_THREAD_WORKER_INDEX;
    /// @brief The reported transition.
    JobTraceKind Kind = JobTraceKind::Submitted;
    /// @brief Finished: the callback's outcome (an invalid value is reported as Failed).
    ///        WaitReturned: the graph outcome returned by Wait.
    JobOutcome Outcome = JobOutcome::Pending;
};

/// @brief Worker setup or teardown hook. Runs once on the worker thread it names.
/// @param contextOrNull JobSystemHooks::ContextOrNull.
/// @param workerIndex Zero-based worker index, below JobSystemConfig::WorkerCount.
using JobWorkerFunction = void (*)(void* contextOrNull, uint32 workerIndex) noexcept;
/// @brief Receives one trace event. Called concurrently from workers and the owner.
using JobTraceFunction = void (*)(void* contextOrNull, const JobTraceEvent& event) noexcept;
/// @brief Asked once per Submit, on the calling thread, whether to trace that submission.
using JobTraceFilterFunction = bool (*)(void* contextOrNull) noexcept;

/// @brief Optional observers of a JobSystem, fixed at Initialize.
///
/// Every hook runs outside the scheduler gate and must not block on jobs. A hook
/// that calls JobSystem Initialize, Submit, Wait or Shutdown receives InsideJob;
/// destroying a system or a submitted graph from a hook is a fatal contract
/// violation. Hooks must not fail the scheduler: report or drop their own errors.
/// The hook code and the data ContextOrNull references must outlive the system.
/// With no hooks the scheduler performs no extra work; with TraceOrNull set and the
/// filter declining a submission, each job costs one extra predictable branch.
struct JobSystemHooks
{
    /// @brief Runs once on each worker before it takes work; Initialize returns after every call returned.
    ///
    /// Use it to register threads with a profiler or to initialize thread-local state.
    JobWorkerFunction WorkerStartOrNull = nullptr;
    /// @brief Runs once on each worker after it stops taking work, before Shutdown joins it.
    JobWorkerFunction WorkerStopOrNull = nullptr;
    /// @brief Receives the transitions of each traced submission; null disables tracing.
    ///
    /// For a submission, every call returns before Wait releases it, except
    /// WaitReturned, which Wait reports after the release and before it returns.
    /// Started, Signalled and Finished are reported by the thread that runs the job,
    /// around its callback, so an event's Label is valid during the call.
    JobTraceFunction TraceOrNull = nullptr;
    /// @brief Decides per Submit whether TraceOrNull observes it; null traces every submission.
    JobTraceFilterFunction ShouldTraceOrNull = nullptr;
    /// @brief Passed to every hook. Not owned.
    void* ContextOrNull = nullptr;
};

/// @brief Worker budget and optional observers for a JobSystem.
struct JobSystemConfig
{
    /// @brief Explicit budget, excluding the calling thread, which may help in Wait. Range 0..64.
    ///
    /// Zero selects the serial executor, which runs every job inside Submit on the
    /// calling thread. Browser builds require zero. The engine owner must leave CPU
    /// time for simulation, driver, audio and I/O threads; logical CPU count is not
    /// a recommended value.
    uint32 WorkerCount = 0;
    /// @brief Worker setup/teardown and trace hooks; none by default.
    JobSystemHooks Hooks{};
};

/// @brief Per-outcome job counts of the in-flight graph, observed atomically under the system gate.
struct JobSnapshot
{
    uint32 Pending = 0;   ///< Jobs not yet executed or resolved.
    uint32 Running = 0;   ///< Callbacks currently executing.
    uint32 Succeeded = 0; ///< Jobs that returned Succeeded.
    uint32 Failed = 0;    ///< Jobs that returned Failed or an invalid outcome.
    uint32 Cancelled = 0; ///< Jobs that returned Cancelled or were skipped by Cancel.
    uint32 Blocked = 0;   ///< Jobs skipped because a prerequisite did not succeed.
};

namespace detail
{
struct GraphState;
struct SystemState;
} // namespace detail

class JobSystem;

// Thanks to Jon Parise, "Multithreaded Object Models," Game Engine Gems 1,
// ch. 21, pp. 371-379, for explicit ownership/buffer lifetimes, and Julien
// Hamaide, "Thread Communication Techniques," Game Engine Gems 2, ch. 29,
// pp. 459-467, for bounded communication. The host owns graph/context lifetimes;
// a mutex/atomic publication proof replaces historical volatile examples.
// See docs/architecture/threading.md for the reviewed sources and departures.
/// @brief Bounded dependency graph, built, sealed and reset on one owner thread.
///
/// Add jobs, declare edges with DependsOn, then Seal. A sealed graph can be
/// submitted repeatedly; each job keeps the context pointer given to Add, so
/// update the data it points to between Wait and the next Submit. Reset
/// invalidates every handle and permits rebuilding. Contexts and the data they
/// reference must outlive the Wait that releases a submission.
///
/// Build/reset and destruction are serialized by the owner and must not race a
/// submission. GetOutcome may run concurrently with execution.
class JobGraph final
{
public:
    /// @brief Creates an uninitialized graph that owns no storage.
    JobGraph() noexcept = default;
    /// @brief Drains an in-flight submission through its system, then frees storage.
    /// @warning Destroying a submitted graph from any job callback is a fatal contract violation.
    ~JobGraph() noexcept;
    /// @brief Not copyable: graph storage and identity are not transferable.
    JobGraph(const JobGraph&) = delete;
    /// @brief Not copy-assignable.
    JobGraph& operator=(const JobGraph&) = delete;
    /// @brief Not movable: graph storage and identity are not transferable.
    JobGraph(JobGraph&&) = delete;
    /// @brief Not move-assignable.
    JobGraph& operator=(JobGraph&&) = delete;

    /// @brief Reserves all job, edge and ready-queue storage and assigns a graph ID.
    /// @param config Fixed capacities; MaxJobs must be nonzero.
    /// @return Ok; AlreadyInitialized; InvalidArgument for out-of-range capacities;
    ///         OutOfMemory; or IdentityExhausted. On failure the graph stays uninitialized.
    [[nodiscard]] JobStatus Initialize(JobGraphConfig config = {}) noexcept;

    /// @brief Appends a job to an unsealed graph.
    /// @param function Callback; must not be null.
    /// @param contextOrNull Value passed to every invocation of the callback. Not owned.
    /// @param outHandle Receives the new job's handle, or a default (invalid) handle on failure.
    /// @param label Optional identity reported to trace hooks; stored by value without
    ///        allocation. The name is borrowed (see JobLabel::NameOrNull).
    /// @return Ok; NotInitialized; Busy when sealed or submitted; InvalidArgument; or CapacityExceeded.
    [[nodiscard]] JobStatus
    Add(JobFunction function, void* contextOrNull, JobHandle& outHandle, JobLabel label = {}) noexcept;

    /// @brief Declares that @p dependent runs only after @p prerequisite succeeds.
    ///
    /// The edge orders execution and publishes the prerequisite's writes to the
    /// dependent. If the prerequisite fails or is cancelled, the dependent and its
    /// descendants become Blocked without being called.
    /// @return Ok; NotInitialized; Busy when sealed or submitted; InvalidHandle;
    ///         CycleDetected for a self edge; DuplicateDependency; or CapacityExceeded.
    [[nodiscard]] JobStatus DependsOn(JobHandle dependent, JobHandle prerequisite) noexcept;

    /// @brief Validates the graph as acyclic in O(jobs + edges) without allocation and freezes it.
    /// @return Ok; NotInitialized; Busy while submitted; or CycleDetected, in which case
    ///         the graph stays unsealed and must be Reset to remove the cycle.
    [[nodiscard]] JobStatus Seal() noexcept;

    /// @brief Removes every job and edge and advances the generation, invalidating all handles.
    /// @return Ok; NotInitialized; Busy until Wait releases a submission; or
    ///         IdentityExhausted when the generation cannot advance without wrapping.
    [[nodiscard]] JobStatus Reset() noexcept;

    /// @brief Reads one job's published outcome with acquire ordering.
    ///
    /// A Succeeded result makes that job's writes visible to the caller. Safe to call
    /// concurrently with execution, but never concurrently with Reset, rebuild or destruction.
    /// @param handle Job handle returned by Add in the current generation.
    /// @param outOutcome Receives the outcome; Pending on failure.
    /// @return Ok; NotInitialized; or InvalidHandle for a foreign or stale handle.
    [[nodiscard]] JobStatus GetOutcome(JobHandle handle, JobOutcome& outOutcome) const noexcept;

private:
    friend class JobSystem;
    detail::GraphState* mStateOrNull = nullptr;
};

/// @brief Explicitly budgeted pool that executes one sealed graph at a time.
///
/// Initialize, Submit, Wait, Shutdown and destruction are serialized by the
/// application owner. Cancel and Snapshot may run concurrently with execution and
/// with the owner's Wait, including from job callbacks, while the system stays
/// initialized. Systems are independent; there is no hidden global pool.
/// Platform, RHI and audio callback work does not belong in this pool.
class JobSystem final
{
public:
    /// @brief Creates an uninitialized system with no threads.
    JobSystem() noexcept = default;
    /// @brief Equivalent to Shutdown.
    /// @warning Destroying a system from any job callback is a fatal contract violation.
    ~JobSystem() noexcept;
    /// @brief Not copyable: workers reference the system state.
    JobSystem(const JobSystem&) = delete;
    /// @brief Not copy-assignable.
    JobSystem& operator=(const JobSystem&) = delete;
    /// @brief Not movable: workers reference the system state.
    JobSystem(JobSystem&&) = delete;
    /// @brief Not move-assignable.
    JobSystem& operator=(JobSystem&&) = delete;

    /// @brief Allocates scheduler state and starts every worker once.
    ///
    /// When JobSystemHooks::WorkerStartOrNull is set, returns Ok only after it has
    /// run on every worker.
    /// @param config Worker budget and hooks; see JobSystemConfig.
    /// @return Ok; InsideJob; AlreadyInitialized; InvalidArgument when WorkerCount exceeds 64;
    ///         Unsupported for nonzero workers on browser builds; OutOfMemory; or
    ///         ThreadCreationFailed, after joining any worker already started.
    ///         On failure the system stays uninitialized and Initialize may be retried.
    [[nodiscard]] JobStatus Initialize(JobSystemConfig config = {}) noexcept;

    /// @brief Admits a sealed graph and publishes its jobs without prerequisites.
    ///
    /// The graph stays reserved, and cannot be edited, reset or submitted elsewhere,
    /// until Wait returns. With zero workers every job runs on the caller before
    /// Submit returns; Wait is still required to release the graph.
    /// @param inoutGraph Graph whose per-job execution state is reset for this run.
    /// @return Ok; InsideJob; NotInitialized for an uninitialized system or graph;
    ///         Busy when this system already has a graph in flight or another system
    ///         holds the graph; or NotSealed. There is no queue of pending graphs.
    [[nodiscard]] JobStatus Submit(JobGraph& inoutGraph) noexcept;

    /// @brief Waits until every job of the in-flight graph is resolved, then releases it.
    ///
    /// Acts as a lifetime fence: on return all callback writes are visible and no
    /// callback still references graph contexts. With @p shouldHelp, the caller also
    /// executes ready jobs; pass false when thread identity, thread-local state or
    /// responsiveness make that inappropriate.
    /// @param outOutcome Failed if any job failed, otherwise Cancelled if any job was
    ///        cancelled, otherwise Succeeded. Blocked jobs follow from one of the former.
    ///        Pending when the call fails.
    /// @param shouldHelp Whether the calling thread executes ready jobs while it waits.
    /// @return Ok; InsideJob; NotInitialized; or InvalidArgument when no graph is submitted.
    [[nodiscard]] JobStatus Wait(JobOutcome& outOutcome, bool shouldHelp = true) noexcept;

    /// @brief Requests that jobs which have not started are skipped and reported Cancelled.
    ///
    /// Running callbacks finish and keep their actual outcomes; side effects are not
    /// rolled back. Once requested, every job that has not started reports Cancelled,
    /// including one that a failed prerequisite would otherwise report Blocked.
    /// Cancelling after the last job resolved does not change the graph outcome.
    /// @return Ok; NotInitialized; or InvalidArgument when no graph is submitted.
    [[nodiscard]] JobStatus Cancel() noexcept;

    /// @brief Counts the in-flight graph's job outcomes. O(jobs); do not poll per task.
    /// @param outSnapshot Receives the counts; all zero when no graph is submitted or on failure.
    /// @return Ok or NotInitialized.
    [[nodiscard]] JobStatus Snapshot(JobSnapshot& outSnapshot) const noexcept;

    /// @brief Reports the worker budget accepted by Initialize, excluding a helping caller.
    ///
    /// Lets a consumer that receives a borrowed system size its graphs to the owner's
    /// budget without a second, possibly disagreeing configuration value. Called by the
    /// owner, or by a consumer the owner has lent the system to, never concurrently with
    /// Initialize or Shutdown.
    /// @return JobSystemConfig::WorkerCount of a successful Initialize; zero while
    ///         uninitialized (including after Shutdown) and for the serial executor.
    [[nodiscard]] uint32 GetWorkerCount() const noexcept;

    /// @brief Drains accepted work, stops and joins every worker, and frees scheduler state.
    /// @return Ok, including when not initialized; or InsideJob, leaving the system running.
    [[nodiscard]] JobStatus Shutdown() noexcept;

private:
    detail::SystemState* mStateOrNull = nullptr;
};
} // namespace ludus::foundation::threading
