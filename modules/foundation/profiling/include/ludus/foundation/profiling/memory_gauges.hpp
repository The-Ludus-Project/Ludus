#pragma once

/// @file
/// @brief Allocation-domain gauges on the CPU trace timeline (memory profiling phase 1).
///
/// A non-ubiquitous control header: the application coordinator calls it at a
/// boundary it chooses, typically once per frame or around a load. Ordinary code
/// never includes it. Metric and coverage contracts are in
/// docs/development/memory-phase-1.md.

#include <ludus/foundation/base/types.h>

namespace ludus::foundation::profiling
{
/// @brief Records one sample of every registered allocation domain on the calling thread's trace timeline.
///
/// Each registered FoundationMemory domain contributes one timestamped group of
/// counter events. `ExportPerfettoTrace` writes each group as one Chrome trace
/// counter event (`"ph":"C"`) named `Memory/<domain>` whose arguments are the
/// domain's `Validity`, `LiveRequestedBytes`, `LiveBlocks`,
/// `PeakRequestedBytes`, `AllocatedBytes`, `FreedBytes`, `AllocationCount`,
/// `FreeCount`, `FailureCount` and `RejectedCount`. A domain whose accounting is
/// no longer valid contributes only `Validity` (1 saturated, 2 accounting error),
/// never plausible-looking byte values. The export's `metadata` object carries
/// the coverage manifest: the sampled domains and the allocation paths outside
/// domain accounting.
///
/// Values are relaxed per-field samples. They reconcile exactly only when the
/// caller has established that no thread is inside an allocation or free in
/// any registered domain for the duration of this call; this function cannot
/// detect a violation. Per-domain peaks are separate histories; viewers must
/// not sum them into a process peak.
///
/// Gauge events share the capture's bounded event pool: a dropped event omits
/// its metric from that sample and is counted in `TraceHealth::EventsDropped`.
/// Never allocates and never blocks beyond the recorder's per-chunk lock.
/// @return True when a capture is active and every event of the sample was
/// recorded; false when no capture is active, in a profiling-disabled browser
/// build, or when any event was dropped.
/// @note Call from a thread registered with `RegisterThreadForTrace` so the
/// sample is attributed and flushed. Domain accounting itself is independent of
/// `LUDUS_PROFILING_ENABLED` and of whether a capture is active.
[[nodiscard]] bool RecordMemoryGauges() noexcept;
} // namespace ludus::foundation::profiling
