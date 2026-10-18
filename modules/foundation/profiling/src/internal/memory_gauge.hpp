#pragma once

// Encoding of allocation-domain gauge samples in the trace event stream
// (memory profiling phase 1). A MemoryGauge event carries one metric of one
// domain: SiteId packs the registered domain ID with the metric index and Aux
// holds the value. The exporter regroups consecutive events of one sample.

#include <ludus/foundation/base/types.h>

#include <string_view>

namespace ludus::foundation::profiling::internal
{
enum class MemoryGaugeMetric : uint8
{
    Validity = 0,
    LiveRequestedBytes,
    LiveBlocks,
    PeakRequestedBytes,
    AllocatedBytes,
    FreedBytes,
    AllocationCount,
    FreeCount,
    FailureCount,
    RejectedCount,
};

inline constexpr uint32 MEMORY_GAUGE_METRIC_COUNT = 10;
inline constexpr uint32 MEMORY_GAUGE_METRIC_BITS = 4;
inline constexpr uint32 MEMORY_GAUGE_METRIC_MASK = (1U << MEMORY_GAUGE_METRIC_BITS) - 1U;
static_assert(MEMORY_GAUGE_METRIC_COUNT <= MEMORY_GAUGE_METRIC_MASK + 1U, "Metric index must fit its bits");

// Version of the "ludus-memory-gauges" export metadata object.
inline constexpr uint32 MEMORY_GAUGE_SCHEMA_VERSION = 1;

[[nodiscard]] constexpr uint32 EncodeMemoryGaugeSite(uint32 domainId, MemoryGaugeMetric metric) noexcept
{
    return (domainId << MEMORY_GAUGE_METRIC_BITS) | static_cast<uint32>(metric);
}

[[nodiscard]] constexpr uint32 DecodeMemoryGaugeDomain(uint32 siteId) noexcept
{
    return siteId >> MEMORY_GAUGE_METRIC_BITS;
}

[[nodiscard]] constexpr uint32 DecodeMemoryGaugeMetric(uint32 siteId) noexcept
{
    return siteId & MEMORY_GAUGE_METRIC_MASK;
}

[[nodiscard]] constexpr std::string_view GetMemoryGaugeMetricName(uint32 metricIndex) noexcept
{
    switch (static_cast<MemoryGaugeMetric>(metricIndex))
    {
        case MemoryGaugeMetric::Validity:
            return "Validity";
        case MemoryGaugeMetric::LiveRequestedBytes:
            return "LiveRequestedBytes";
        case MemoryGaugeMetric::LiveBlocks:
            return "LiveBlocks";
        case MemoryGaugeMetric::PeakRequestedBytes:
            return "PeakRequestedBytes";
        case MemoryGaugeMetric::AllocatedBytes:
            return "AllocatedBytes";
        case MemoryGaugeMetric::FreedBytes:
            return "FreedBytes";
        case MemoryGaugeMetric::AllocationCount:
            return "AllocationCount";
        case MemoryGaugeMetric::FreeCount:
            return "FreeCount";
        case MemoryGaugeMetric::FailureCount:
            return "FailureCount";
        case MemoryGaugeMetric::RejectedCount:
            return "RejectedCount";
    }
    return "Unknown";
}
} // namespace ludus::foundation::profiling::internal
