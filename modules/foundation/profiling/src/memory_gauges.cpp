// Allocation-domain gauge sampling onto the CPU trace timeline (memory
// profiling phase 1; docs/development/memory-phase-1.md). FoundationMemory owns
// the counters and registry; Profiling only samples them outside allocation
// hooks and transports the values. Memory never calls back into Profiling.

#include <ludus/foundation/profiling/memory_gauges.hpp>

#include "internal/memory_gauge.hpp"
#include "internal/recorder.hpp"
#include "internal/trace_event.hpp"

#include <ludus/foundation/memory/allocation_statistics.hpp>
#include <ludus/foundation/profiling/clock.hpp>

namespace ludus::foundation::profiling
{
namespace
{
using internal::MemoryGaugeMetric;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): ticks, domain ID and value are distinct encoded fields
[[nodiscard]] bool EmitGauge(uint64 ticks, uint32 domainId, MemoryGaugeMetric metric, uint64 value) noexcept
{
    internal::TraceEvent event;
    event.Ticks = ticks;
    event.SiteId = internal::EncodeMemoryGaugeSite(domainId, metric);
    event.Kind = static_cast<uint16>(internal::TraceEventKind::MemoryGauge);
    event.Flags = internal::TRACE_FLAG_NONE;
    event.Aux = value;
    return internal::TraceRecorder::Instance().Emit(event);
}

[[nodiscard]] bool EmitDomainSample(uint64 ticks, uint32 domainId, const AllocationStatistics& statistics) noexcept
{
    bool isComplete = EmitGauge(ticks, domainId, MemoryGaugeMetric::Validity, static_cast<uint64>(statistics.Validity));
    if (statistics.Validity != AccountingValidity::Valid)
    {
        return isComplete;
    }
    isComplete =
        EmitGauge(ticks, domainId, MemoryGaugeMetric::LiveRequestedBytes, statistics.LiveRequestedBytes) && isComplete;
    isComplete = EmitGauge(ticks, domainId, MemoryGaugeMetric::LiveBlocks, statistics.LiveBlocks) && isComplete;
    isComplete =
        EmitGauge(ticks, domainId, MemoryGaugeMetric::PeakRequestedBytes, statistics.PeakRequestedBytes) && isComplete;
    isComplete = EmitGauge(ticks, domainId, MemoryGaugeMetric::AllocatedBytes, statistics.AllocatedBytes) && isComplete;
    isComplete = EmitGauge(ticks, domainId, MemoryGaugeMetric::FreedBytes, statistics.FreedBytes) && isComplete;
    isComplete =
        EmitGauge(ticks, domainId, MemoryGaugeMetric::AllocationCount, statistics.AllocationCount) && isComplete;
    isComplete = EmitGauge(ticks, domainId, MemoryGaugeMetric::FreeCount, statistics.FreeCount) && isComplete;
    isComplete = EmitGauge(ticks, domainId, MemoryGaugeMetric::FailureCount, statistics.FailureCount) && isComplete;
    isComplete = EmitGauge(ticks, domainId, MemoryGaugeMetric::RejectedCount, statistics.RejectedCount) && isComplete;
    return isComplete;
}
} // namespace

bool RecordMemoryGauges() noexcept
{
    if (internal::IsCaptureActive() == false)
    {
        return false;
    }
    // One timestamp per sample lets the exporter regroup each domain's events.
    const uint64 ticks = NowTicks();
    bool isComplete = true;
    const usize domainCount = GetRegisteredAllocationDomainCount();
    for (usize index = 0; index < domainCount; ++index)
    {
        RegisteredAllocationDomain entry;
        if (TryGetRegisteredAllocationDomain(index, entry) == false || entry.DomainOrNull == nullptr)
        {
            continue;
        }
        const AllocationStatistics statistics = GetAllocationStatistics(*entry.DomainOrNull);
        isComplete = EmitDomainSample(ticks, entry.Id, statistics) && isComplete;
    }
    return isComplete;
}
} // namespace ludus::foundation::profiling
