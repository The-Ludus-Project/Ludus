#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::foundation::profiling
{
/// One completed device-clock duration, attributed to its CPU submission marker.
/// No CPU/GPU execution-clock alignment is implied. All times/durations are nanoseconds.
struct GpuTraceSample final
{
    /// Monotonic CPU timestamp recorded immediately before the originating submission.
    uint64 SubmissionTicks = 0;
    /// CPU timestamp when the nonblocking poll observed this result.
    uint64 CollectionTicks = 0;
    /// Nonnegative measured device interval; a quantized zero is valid.
    uint64 DurationNanoseconds = 0;
    /// Nonzero producer submission sequence, independent of presentation.
    uint64 FrameId = 0;
    /// Nonzero device lifetime generation; distinguishes restarted devices.
    uint64 DeviceSession = 0;
    /// Empty for the frame probe; otherwise a nonempty UTF-8 pass name borrowed until serialization returns.
    BorrowedView<const char> PassName{};
    /// Nonzero producer-local identity for a named pass; zero for a frame probe.
    uint64 ScopeIdentity = 0;
    /// Physical operation ordinal within the named scope; distinguishes multi-encoder scopes.
    uint32 ScopePassIndex = 0;
    /// Number of logical operations covered by this physical scope; at least one.
    uint32 LogicalPassCount = 1;
    /// True for a compute encoder/dispatch, false for a render encoder/pass.
    bool IsCompute = false;
};

/// Coverage of the GPU sampling window. Missing results never become zero-duration samples.
struct GpuTraceCoverage final
{
    /// GPU recording was requested, including windows with no eligible submissions.
    bool IsGpuRequested = false;
    /// Native per-pass sampling was admitted; false explicitly records unsupported/failed admission.
    bool IsPassTimingSupported = false;
    /// Frames submitted while GPU recording admitted work.
    uint64 EligibleFrames = 0;
    /// Samples rejected due to clock discontinuity or native failure.
    uint64 InvalidSamples = 0;
    /// Completed samples discarded because the bounded capture storage was full.
    uint64 DroppedSamples = 0;
    /// Submitted samples still unresolved at the finalization deadline or device teardown.
    uint64 UnresolvedSamples = 0;
    /// Physical passes omitted because the bounded native query batch was full.
    uint64 QueryDroppedPasses = 0;
    /// Submitted pass batches unresolved at finalization or device teardown.
    uint64 UnresolvedPassBatches = 0;
};

/// Serialize a stopped recording into owned Chrome/Perfetto JSON bytes, off the hot path.
/// @param outBytes Receives the complete file bytes only on success; unchanged on failure.
/// @param samples Borrowed completed GPU samples, consumed during this call. IDs must be nonzero
/// and CollectionTicks must not precede SubmissionTicks. PassName must be valid UTF-8 without
/// embedded zero; ScopeIdentity must be nonzero exactly when PassName is nonempty. No native objects are retained.
/// @param coverage GPU sampling coverage included in the exported artifact.
/// @param shouldIncludeCpu When true, drains the most recent stopped CPU recording. When false,
/// does not drain CPU events or serialize CPU thread metadata, allowing GPU-only export
/// without enabling CPU scopes.
/// @return False for invalid samples, active CPU capture, empty data, disabled profiling or
/// output-buffer allocation failure. Drained CPU events are consumed if a later step fails.
/// Flattening and JSON assembly retain the existing exporter's allocation policy.
/// @note Serialize capture lifecycle/export calls. Preserve successful bytes for I/O retry;
/// never start a new CPU capture until the application's unexported recording is resolved.
[[nodiscard]] bool SerializePerfettoTrace(Array<uint8>& outBytes,
                                          BorrowedView<const GpuTraceSample> samples = {},
                                          const GpuTraceCoverage& coverage = {},
                                          bool shouldIncludeCpu = true) noexcept;
} // namespace ludus::foundation::profiling
