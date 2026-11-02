#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/graphics/rhi/compute_workload.hpp>

namespace ludus::graphics::rhi
{
/// Caller-owned semantic revisions. Scene includes geometry, instances, materials,
/// lights, environment and preprocessing settings; Model includes weights and layout.
/// Both must be nonzero. Reusing a key promises identical inputs, never pointer equality.
/// Camera and image dimensions need not enter a view-independent encoding's scene revision.
struct ComputeCacheKey final
{
    foundation::uint64 Scene = 0; ///< Complete source/preprocessing revision.
    foundation::uint64 Model = 0; ///< Complete learned weights/kernel/layout revision.
    /// Compare revision identities without inspecting source data.
    [[nodiscard]] LUDUS_INLINE bool operator==(const ComputeCacheKey&) const noexcept = default;
};
/// Explicit candidate state, separate from whether a previously published result exists.
enum class ComputeCachePhase : foundation::uint8
{
    Empty,              ///< No candidate; an active result may still exist.
    Preparing,          ///< Upload/shader/pipeline preparation is pending.
    Prepared,           ///< Candidate can build its one encoding graph.
    AwaitingSubmission, ///< Graph handed to caller; candidate cannot publish yet.
    Executing,          ///< Accepted submission is not yet known complete.
    Failed,             ///< Candidate failed; active result remains available.
};
class ComputeCache;
/// Candidate-attempt identity, scoped to one live cache. Capture it after building
/// a candidate graph; equal scene/model keys do not make separate attempts equal.
/// The originating cache must remain alive while callbacks hold this ticket.
struct ComputeCacheTicket final
{
private:
    const ComputeCache* OwnerOrNull = nullptr;
    foundation::uint64 Generation = 0;
    friend class ComputeCache;
};
/// Owner-thread, two-generation cache for view-independent GPU preprocessing.
/// Stage/encode/submit/Poll leaves the active buffers intact until a replacement
/// submission completes. No hidden execution, GPU wait, CPU readback or fallback.
/// Mutable workload output must not be modified after publication while consumers use it.
/// Call between frames and destroy before the device; normal RHI leases retire old uses.
class ComputeCache final
{
public:
    /// Construct an empty cache with no device or resource ownership.
    ComputeCache() noexcept = default;
    /// Candidate tickets require a stable cache address; copying is forbidden.
    ComputeCache(const ComputeCache&) = delete;
    /// Candidate tickets require a stable cache address; copy assignment is forbidden.
    ComputeCache& operator=(const ComputeCache&) = delete;
    /// Candidate tickets require a stable cache address; moving is forbidden.
    ComputeCache(ComputeCache&&) = delete;
    /// Candidate tickets require a stable cache address; move assignment is forbidden.
    ComputeCache& operator=(ComputeCache&&) = delete;
    /// Prepare a new candidate, preserving active and previous candidate on admission
    /// failure. Equal active/candidate keys reuse their result/state without uploading.
    /// Different accepted keys supersede pending work; caller must discard any unsubmitted
    /// graph of the superseded candidate. Input lifetimes follow ComputeWorkload::Initialize.
    /// Device must match every existing active/candidate generation until Reset.
    /// Candidate writable buffers must be newly owned: writable External buffers
    /// reject. Read-only External inputs must have owners outside this cache that
    /// outlive both generations; borrowing this cache's active owned buffers is invalid.
    [[nodiscard]] RasterStatus
    Stage(DeviceHandle device, ComputeCacheKey key, const ComputeWorkloadDescription& description) noexcept;
    /// Advance preparation or check submission completion and atomically publish a
    /// completed candidate. Ready means no pending candidate; it never means that
    /// unsubmitted GPU work has completed. Host drives PollLifetime as usual.
    /// A failed candidate remains inspectable and does not release active buffers.
    [[nodiscard]] RasterStatus Poll() noexcept;
    /// Build the prepared candidate's graph, then enter AwaitingSubmission. Caller
    /// compiles/executes it using normal RHI APIs. Failure preserves phase and output.
    [[nodiscard]] RasterStatus BuildCandidateGraph(OrderedGraph& outGraph) noexcept;
    /// Capture the current candidate attempt, or an invalid ticket when empty.
    /// A new accepted Stage uses a new attempt even when a cancelled key is reused.
    [[nodiscard]] ComputeCacheTicket GetCandidateTicket() const noexcept;
    /// Associate the exact attempt and submission that executed the candidate graph.
    /// Reject foreign/stale attempt callbacks, invalid tokens and calls outside AwaitingSubmission.
    /// @pre completion actually covers all candidate kernels on this device.
    [[nodiscard]] RasterStatus SetCandidateCompletion(ComputeCacheTicket ticket, SubmissionToken completion) noexcept;
    /// Drop candidate owners/state; submitted/retained graph work retires normally.
    /// Caller discards any unsubmitted candidate graph. Preserves the active result.
    void CancelCandidate() noexcept;
    /// Whether a completed GPU result is published.
    [[nodiscard]] bool HasActive() const noexcept;
    /// Active semantic revisions, or zero revisions when HasActive is false.
    [[nodiscard]] ComputeCacheKey GetActiveKey() const noexcept;
    /// Candidate state, independent of active readiness.
    [[nodiscard]] ComputeCachePhase GetPhase() const noexcept;
    /// Borrow an active result allocation; same lifetime contract as GetBuffer.
    /// NotReady before first publication. Failures preserve outBuffer.
    [[nodiscard]] RasterStatus GetActiveBuffer(foundation::usize index, BufferHandle& outBuffer) const noexcept;
    /// Release both generations and device association without waiting. Between frames.
    void Reset() noexcept;

private:
    DeviceHandle mDevice{};
    ComputeWorkload mActive;
    ComputeWorkload mCandidate;
    ComputeCacheKey mActiveKey{};
    ComputeCacheKey mCandidateKey{};
    SubmissionToken mCompletion{};
    ComputeCachePhase mPhase = ComputeCachePhase::Empty;
    RasterStatus mFailure = RasterStatus::NotReady;
    foundation::uint64 mNextCandidate = 1;
    foundation::uint64 mCandidateGeneration = 0;
};
} // namespace ludus::graphics::rhi
