#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <atomic>
#include <span>
#include <string_view>

/// @file
/// Private lifecycle runner of the smoke application.
///
/// The runner validates an authored startup plan, starts its nodes in array order,
/// journals every attempted start, and stops attempted nodes in reverse order on
/// completion, cancellation, startup failure or runtime failure. It reports the
/// primary outcome and the cleanup outcome as separate structured results.
/// The contract and its migration plan are owned by
/// `docs/architecture/module-lifecycle.md`. The runner stays private to this
/// application until a second host adopts it (engine-loop.md, finding 9); the
/// composition root and module adapters live in application.cpp.

namespace ludus::smoke::lifecycle
{
/// Sentinel for an absent node ID, plan position or provider ID.
inline constexpr foundation::usize INVALID_NODE = ~foundation::usize{0};

/// Error domain value that means "no error domain".
inline constexpr foundation::uint32 NO_ERROR_DOMAIN = 0;

/// Structured module error copied by value into runner records.
///
/// The domain identifies the adapter-owned error vocabulary (for example one
/// application's RHI startup errors) so equal codes from different modules are
/// not confused. The runner never interprets either field. Neither field borrows
/// module storage, so the value survives module teardown.
struct ErrorCode
{
    /// Adapter-defined error vocabulary; NO_ERROR_DOMAIN when no error was reported.
    foundation::uint32 Domain = NO_ERROR_DOMAIN;
    /// Code within Domain; 0 when no error was reported.
    foundation::uint32 Value = 0;
};

/// Result of a node's BeginStart or PollStart callback.
enum class StartStatus : foundation::uint8
{
    /// The node's documented service contract is usable by consumers.
    Ready,
    /// Startup continues; the runner polls the node on a later Advance.
    Pending,
    /// Startup failed. The node may still own partial resources and is stopped.
    Failed
};

/// Result of a node's BeginStop or PollStop callback.
enum class StopStatus : foundation::uint8
{
    /// Every resource acquired by the attempt is released and no activity remains.
    Stopped,
    /// Retirement continues; the runner polls the node on a later Advance.
    Pending,
    /// Cleanup cannot establish safety; the node and its providers are retained.
    Unsafe
};

/// Value returned by a start callback.
struct StartResult
{
    /// Startup progress of the node.
    StartStatus Status = StartStatus::Ready;
    /// Structured error; meaningful when Status is Failed.
    ErrorCode Error{};
    /// Adapter-defined reason for Pending, recorded for diagnostics; 0 for none.
    foundation::uint32 WaitReason = 0;
};

/// Value returned by a stop callback.
struct StopResult
{
    /// Retirement progress of the node.
    StopStatus Status = StopStatus::Stopped;
    /// Structured error; meaningful when Status is Unsafe.
    ErrorCode Error{};
    /// Adapter-defined reason for Pending, recorded for diagnostics; 0 for none.
    foundation::uint32 WaitReason = 0;
};

/// Start callback. Receives Node::ContextOrNull and must return promptly.
using StartCallback = StartResult (*)(void* contextOrNull) noexcept;
/// Stop callback. Receives Node::ContextOrNull and must return promptly.
using StopCallback = StopResult (*)(void* contextOrNull) noexcept;

/// One authored lifecycle node: a resource-owning service instance.
///
/// The position of a node in the plan array is its execution order. Its dense ID
/// (0 to plan size - 1) identifies its record independently of that order. All four
/// callbacks are required; a synchronous adapter returns Ready or Stopped from its
/// Begin callbacks and may share a stub for the Poll callbacks.
struct Node
{
    /// Dense unique ID in [0, plan size); indexes the runner's records.
    foundation::usize Id = INVALID_NODE;
    /// Nonempty readable name. The caller keeps its storage alive with the plan.
    std::string_view Name;
    /// Adapter context passed to every callback. The runner never dereferences it.
    void* ContextOrNull = nullptr;
    /// IDs of required providers. Each must precede this node in the plan, appear
    /// once, and differ from Id. Storage must outlive the runner.
    std::span<const foundation::usize> Providers;
    /// Called once per attempt before any other callback of this node.
    StartCallback BeginStart{};
    /// Called only while the node is Starting after BeginStart returned Pending.
    StartCallback PollStart{};
    /// Called once for every attempted node, including a Failed or Pending start.
    StopCallback BeginStop{};
    /// Called only while the node is Stopping after BeginStop returned Pending.
    StopCallback PollStop{};
};

/// Per-node state in the current attempt.
enum class NodeState : foundation::uint8
{
    /// Not attempted in this generation; it has received no callback.
    Dormant,
    /// BeginStart was called and startup has not completed.
    Starting,
    /// Startup completed; the node is usable by its consumers.
    Ready,
    /// Startup failed; the node is still journaled for cleanup.
    Failed,
    /// BeginStop was called and retirement has not completed.
    Stopping,
    /// Retirement completed; the node has left the journal.
    Stopped,
    /// Retirement is unsafe or deadline-blocked; the node is retained.
    CleanupBlocked
};

/// Bounded inspectable record of one node, indexed by node ID.
struct NodeRecord
{
    /// Position of the node in the plan, established by validation.
    foundation::usize Position = INVALID_NODE;
    /// Current state in this generation.
    NodeState State = NodeState::Dormant;
    /// Last structured error reported by the node's callbacks.
    ErrorCode Error{};
    /// Last wait reason reported by the node's callbacks.
    foundation::uint32 WaitReason = 0;
};

/// Runner state.
enum class State : foundation::uint8
{
    /// Constructed; no attempt has begun.
    Idle,
    /// Starting nodes in plan order.
    Starting,
    /// Every node is Ready.
    Ready,
    /// Stopping attempted nodes in reverse journal order.
    Stopping,
    /// The journal is empty; a new attempt may begin.
    Stopped,
    /// Cleanup is retained after an Unsafe stop or an owner deadline.
    CleanupBlocked
};

/// Primary outcome of an attempt, independent of the cleanup outcome.
enum class Outcome : foundation::uint8
{
    /// The attempt has not ended.
    None,
    /// A stop was requested after every node became Ready.
    Completed,
    /// A stop was requested before every node became Ready.
    Cancelled,
    /// A node's start callback returned Failed.
    StartFailed,
    /// The owner reported a runtime failure through ReportFailure.
    RuntimeFailed
};

/// Cleanup outcome of an attempt, independent of the primary outcome.
enum class CleanupOutcome : foundation::uint8
{
    /// No cleanup has been required yet.
    None,
    /// Attempted nodes remain in the journal and retirement continues.
    Pending,
    /// Every attempted node has Stopped.
    Complete,
    /// Cleanup is blocked; the remaining journal and its providers are retained.
    Blocked
};

/// Plan or storage defect found by validation before any callback runs.
enum class PlanError : foundation::uint8
{
    /// The plan is valid.
    None,
    /// The records or journal span is smaller than the plan.
    Capacity,
    /// A node ID is outside [0, plan size).
    InvalidId,
    /// Two nodes share an ID.
    DuplicateId,
    /// A node has an empty name.
    MissingName,
    /// A node lacks one of its four callbacks.
    MissingCallback,
    /// A provider ID is outside [0, plan size).
    InvalidProvider,
    /// A node lists itself as a provider.
    SelfProvider,
    /// A node lists the same provider more than once.
    DuplicateProvider,
    /// A provider follows its consumer in the plan, without a dependency cycle.
    ProviderOrder,
    /// A provider follows its consumer and transitively depends on that consumer.
    ProviderCycle
};

/// Named validation diagnostic for the first plan defect found.
struct PlanDiagnostic
{
    /// The defect, or None for a valid plan.
    PlanError Error = PlanError::None;
    /// ID of the defective or consuming node; INVALID_NODE for Capacity.
    foundation::usize NodeId = INVALID_NODE;
    /// ID of the offending provider for provider defects; otherwise INVALID_NODE.
    foundation::usize ProviderId = INVALID_NODE;
};

/// Operation that produced a recorded failure.
enum class FailureSource : foundation::uint8
{
    /// No failure was recorded.
    None,
    /// The node's BeginStart returned Failed.
    BeginStart,
    /// The node's PollStart returned Failed.
    PollStart,
    /// The node's BeginStop returned Unsafe.
    BeginStop,
    /// The node's PollStop returned Unsafe.
    PollStop,
    /// The owner reported a runtime failure through ReportFailure.
    Runtime,
    /// The owner blocked a pending stop through BlockCleanup, such as a deadline.
    Deadline
};

/// A structured failure attributed to one node and one operation.
struct Failure
{
    /// ID of the failing node; INVALID_NODE when no failure was recorded.
    foundation::usize NodeId = INVALID_NODE;
    /// Operation that produced the failure.
    FailureSource Source = FailureSource::None;
    /// Structured error reported by the node or the owner.
    ErrorCode Error{};
};

/// Status of Runner::Begin.
enum class BeginStatus : foundation::uint8
{
    /// A new attempt began; Advance starts its nodes.
    Started,
    /// An attempt is active, cleanup is blocked, or Begin was called from a callback.
    Busy,
    /// Validation rejected the plan; see Runner::GetPlanDiagnostic.
    InvalidPlan,
    /// The attempt generation counter cannot advance without wrapping.
    GenerationExhausted
};

/// Structured primary and cleanup results of the current or last attempt.
///
/// The primary failure is the first cause and is never overwritten by cleanup.
/// The cleanup failure is recorded separately. Values copy no borrowed module
/// storage and remain valid after the modules have been torn down.
struct Report
{
    /// Attempt generation; 0 before the first Begin.
    foundation::uint64 Generation = 0;
    /// Runner state when the report was taken.
    State RunnerState = State::Idle;
    /// Primary outcome of the attempt.
    Outcome Primary = Outcome::None;
    /// First startup or runtime failure; Source None for Completed or Cancelled.
    Failure PrimaryFailure{};
    /// Cleanup outcome of the attempt.
    CleanupOutcome Cleanup = CleanupOutcome::None;
    /// Most recent cleanup failure (Unsafe stop or owner deadline), if any.
    Failure CleanupFailure{};
    /// Nodes that received BeginStart in this attempt.
    foundation::usize AttemptedCount = 0;
    /// Attempted nodes that have Stopped.
    foundation::usize StoppedCount = 0;
    /// Attempted nodes still retained in the journal, including a blocked node.
    foundation::usize RemainingCount = 0;
};

/// Single owner-thread lifecycle coordinator over caller-owned storage.
///
/// Begin validates the whole plan before invoking any callback. Advance starts
/// nodes in plan order, appending each to the attempted-start journal before its
/// BeginStart, and never starts a node until its providers are Ready. On a stop
/// request, a startup failure or a reported runtime failure, it stops the
/// journaled nodes in reverse order, one at a time, so each node's providers stay
/// Ready until it reports Stopped. Never-attempted nodes receive no callbacks.
///
/// @note Only RequestStop may be called from another thread. Every other member,
///       and every callback, runs on the owner thread. Callbacks must not
///       recursively advance the runner; Begin and Advance reject reentry.
/// @warning The plan, its names and provider spans, the callback contexts, and the
///          records and journal spans must outlive the runner and every attempted
///          node. Do not mutate the plan while an attempt is active. The runner
///          allocates no memory and owns no module resources.
class Runner final
{
public:
    /// Binds the runner to caller-owned storage; performs no validation or callback.
    /// @param plan Authored nodes in execution order.
    /// @param records One record per node ID; at least plan.size() entries.
    /// @param journal Attempted-start journal; at least plan.size() entries.
    Runner(std::span<const Node> plan, std::span<NodeRecord> records, std::span<foundation::usize> journal) noexcept;
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    Runner(Runner&&) = delete;
    Runner& operator=(Runner&&) = delete;

    /// Validates the plan and begins a new attempt with a fresh generation.
    ///
    /// Allowed from Idle or Stopped. Validation resets every record and reports
    /// the first defect through GetPlanDiagnostic before any callback runs.
    /// @return Started on success; otherwise the reason no attempt began. A
    ///         rejected Begin leaves the previous attempt's report unchanged
    ///         except for the plan diagnostic and reset records.
    [[nodiscard]] BeginStatus Begin() noexcept;

    /// Advances startup or cleanup by up to callbackBudget callbacks.
    ///
    /// Yields immediately after a Pending result. Publishing Ready requires no
    /// extra callback. A pending stop request is observed before the first and
    /// after every start callback.
    /// @param callbackBudget Maximum callbacks to invoke; 0 only observes requests.
    /// @return The runner state after advancing. A reentrant call from a callback
    ///         invokes nothing and returns the current state.
    State Advance(foundation::usize callbackBudget = 1) noexcept;

    /// Requests that the current attempt stop. Thread-safe and coalescing.
    ///
    /// The owner thread observes the request on its next Advance. A request made
    /// while Idle or Stopped is observed by the next Advance after Begin.
    void RequestStop() noexcept;

    /// Records a runtime failure and enters cleanup.
    ///
    /// Neither cancellation nor a reported failure is evidence that a node has
    /// retired; cleanup still stops every attempted node.
    /// @param nodeId ID of the failing node.
    /// @param error Owner-supplied structured error.
    /// @return false, leaving the report unchanged, when nodeId is invalid, the
    ///         runner is not Starting or Ready, or the call comes from a callback.
    bool ReportFailure(foundation::usize nodeId, ErrorCode error) noexcept;

    /// Quarantines the pending stop of the current cleanup node, such as at a deadline.
    ///
    /// A deadline can only block an already issued stop; it never skips BeginStop.
    /// @param error Owner-supplied structured error recorded as the cleanup failure.
    /// @return false when the runner is not Stopping, the current cleanup node has
    ///         not returned Pending from a stop callback, or the call comes from a callback.
    bool BlockCleanup(ErrorCode error) noexcept;

    /// Resumes polling the blocked cleanup node after explicit safe recovery.
    /// @return false when cleanup is not blocked or the call comes from a callback.
    bool ResumeCleanup() noexcept;

    /// @return The current runner state.
    [[nodiscard]] LUDUS_INLINE State GetState() const noexcept
    {
        return mState;
    }
    /// @return The primary outcome of the current or last attempt.
    [[nodiscard]] LUDUS_INLINE Outcome GetOutcome() const noexcept
    {
        return mOutcome;
    }
    /// @return The first startup or runtime failure of the current or last attempt.
    [[nodiscard]] LUDUS_INLINE Failure GetFailure() const noexcept
    {
        return mFailure;
    }
    /// @return The most recent cleanup failure of the current or last attempt.
    [[nodiscard]] LUDUS_INLINE Failure GetCleanupFailure() const noexcept
    {
        return mCleanupFailure;
    }
    /// @return The diagnostic of the most recent validation.
    [[nodiscard]] LUDUS_INLINE PlanDiagnostic GetPlanDiagnostic() const noexcept
    {
        return mPlanDiagnostic;
    }
    /// @return The generation of the current or last attempt; 0 before the first.
    [[nodiscard]] LUDUS_INLINE foundation::uint64 GetGeneration() const noexcept
    {
        return mGeneration;
    }
    /// @return The authored plan.
    [[nodiscard]] LUDUS_INLINE std::span<const Node> GetPlan() const noexcept
    {
        return mPlan;
    }
    /// Plan positions still journaled for cleanup, oldest first. The last entry is
    /// the next node to stop; every entry and its providers remain live.
    /// @return A view into the runner's journal storage, valid until the next mutation.
    [[nodiscard]] std::span<const foundation::usize> GetRemainingCleanup() const noexcept;
    /// Per-node records indexed by node ID. Remains safe to inspect after
    /// rejecting undersized storage.
    /// @return A view into the caller's record storage.
    [[nodiscard]] std::span<const NodeRecord> GetRecords() const noexcept;
    /// @return The structured primary and cleanup results of the current or last attempt.
    [[nodiscard]] Report GetReport() const noexcept;

private:
    bool validate() noexcept;
    bool validateProviders() noexcept;
    bool isProviderCycle(const Node& consumer, foundation::usize providerId) noexcept;
    void observeStopRequest() noexcept;
    void enterCleanup() noexcept;
    void startOne() noexcept;
    void stopOne() noexcept;
    void blockCleanup(FailureSource source, ErrorCode error) noexcept;

    std::span<const Node> mPlan;
    std::span<NodeRecord> mRecords;
    std::span<foundation::usize> mJournal;
    std::atomic<bool> mIsStopRequested{false};
    State mState = State::Idle;
    Outcome mOutcome = Outcome::None;
    Failure mFailure;
    Failure mCleanupFailure;
    PlanDiagnostic mPlanDiagnostic;
    foundation::uint64 mGeneration = 0;
    foundation::usize mNext = 0;
    foundation::usize mJournalSize = 0;
    foundation::usize mAttemptedCount = 0;
    bool mIsAdvancing = false;
};
} // namespace ludus::smoke::lifecycle
